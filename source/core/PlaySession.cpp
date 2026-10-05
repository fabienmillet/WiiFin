/* Video playback session: the player, its menus and everything chained from it. */
#include "AppInternal.h"
#include "Input.h"
#include "MusicBGM.h"
#include "SoundFX.h"
#include "Log.h"
#include "../ui/Ui.h"
#include "../ui/LibraryView.h"
#include "../jellyfin/JellyfinClient.h"
#include "../player/WiiPlayer.h"
#include "../player/PlayerView.h"
#include "../player/VideoSurface.h"
#include "../player/vo_wiifin.h"
#include "../player/stream_wiifin.h"
#include "ExitZone.h"
#include <grrlib.h>
#include <stdio.h>
#include <functional>
#include <ogc/lwp_watchdog.h>

/* State captured before starting a stream so the MPlayer callback can send
 * POST /Sessions/Playing once the stream is opened on the server. */
struct {
    JellyfinClient* client;
    std::string serverUrl;
    JellyfinAuth   auth;
    std::string    itemId;
    std::string    mediaSourceId;
    std::string    playSessionId;
} s_pendingReport;

static void onStreamOpened() {
    s_pendingReport.client->reportPlaybackStart(
        s_pendingReport.serverUrl,
        s_pendingReport.auth,
        s_pendingReport.itemId,
        s_pendingReport.mediaSourceId,
        s_pendingReport.playSessionId);
}

/* -----------------------------------------------------------------------
 * runWithPlayerUI — run a blocking Jellyfin call on a worker thread while
 * the player keeps drawing (last frame + spinner + msg).  The worker runs
 * below the main thread, i.e. while GRRLIB_Render() waits for vsync.
 * ----------------------------------------------------------------------- */
static u8                    s_playWorkStack[64 * 1024] DEAD_AT_EXIT ATTRIBUTE_ALIGN(32);
static volatile bool         s_playWorkDone;
static std::function<void()> s_playWorkFn;

static void* playWorker(void*) {
    s_playWorkFn();
    s_playWorkDone = true;
    return nullptr;
}

static void runWithPlayerUI(PlayerView& view, ir_t& ir, const char* msg,
                            std::function<void()> fn)
{
    view.setBusy(msg);
    s_playWorkFn   = std::move(fn);
    s_playWorkDone = false;
    lwp_t thread;
    LWP_CreateThread(&thread, playWorker, nullptr,
                     s_playWorkStack, sizeof(s_playWorkStack), 50);
    while (!s_playWorkDone) {
        Input::update();
        Input::readIR(ir);
        view.render(ir);
        GRRLIB_Render();
        VideoSurface::endFrame();
    }
    LWP_JoinThread(thread, nullptr);
    s_playWorkFn = nullptr;
}

/* getTranscodingUrl() starts the transcode 3 s before the requested position
 * (RESUME_PAD) once that position is past 3 s, otherwise at 0. */
static const long long RESUME_PAD_TICKS = 30000000LL;

static float streamOriginSecs(long long startTicks) {
    return startTicks > RESUME_PAD_TICKS
           ? (float)((startTicks - RESUME_PAD_TICKS) / 10000000.0)
           : 0.0f;
}

static std::string episodeTitle(const JellyfinEpisode& ep) {
    char buf[32];
    snprintf(buf, sizeof(buf), "S%d E%d - ", ep.seasonNumber, ep.indexNumber);
    return buf + ep.name;
}

/* -----------------------------------------------------------------------
 * runPlaySession — plays lv.pendingPlay* and everything chained from it:
 * next/previous episode, audio/subtitle switches, seeks, automatic retries.
 *
 * GRRLIB stays up the whole time: MPlayer decodes on its own thread and
 * PlayerView draws the frames together with the player UI.
 * lastItemId receives the item that was playing when the session ended.
 * Returns true if the user chose "Wii Menu" or "Reset" from the HOME menu.
 * ----------------------------------------------------------------------- */
bool runPlaySession(JellyfinClient& client,
                    const JellyfinAuth& auth,
                    const std::string& serverUrl,
                    LibraryView& lv,
                    GRRLIB_ttfFont* font,
                    GRRLIB_texImg* btnTex,
                    GRRLIB_texImg* cursorTex,
                    GRRLIB_texImg* ringTex,
                    std::string& lastItemId)
{
    /* Working copies — updated on next/prev/track change/seek */
    std::string itemId        = lv.pendingPlayItemId;
    std::string mediaSourceId = lv.pendingPlayMediaSourceId;
    std::string playSessionId = lv.pendingPlaySessionId;
    std::string url           = lv.pendingPlayUrl;
    std::string title         = lv.pendingPlayTitle;
    std::vector<JellyfinEpisode> episodes     = lv.pendingPlayEpisodes;
    int                          episodeIdx   = lv.pendingPlayEpisodeIdx;
    std::vector<MediaStream>     audioStreams = lv.pendingPlayAudioStreams;
    std::vector<MediaStream>     subStreams   = lv.pendingPlaySubStreams;
    int audioIdx = lv.pendingPlayAudioIdx;
    int subIdx   = lv.pendingPlaySubIdx;
    long long startTicks   = lv.pendingPlayStartTimeTicks;
    long long runtimeTicks = lv.pendingPlayRuntimeTicks;

    /* ao_gekko needs the DSP: no menu music while a video plays. */
    bool musicWasRunning = MusicBGM::isRunning();
    MusicBGM::pause();

    PlayerView view(font, cursorTex, ringTex);
    ir_t ir;
    ir.valid = false;

    /* Adaptive quality: repeated rebuffering on this session lowers the
     * bitrate for the rest of it (the saved setting is left untouched). */
    const int savedQuality = client.videoQuality;
    std::vector<u64> rebufferTimes;
    float qualityRestartAt = -1.0f;
    IntroInfo intro;
    bool introFetched = false;
    int  retries      = 0;
    bool everPlayed   = false;   /* this item showed something */
    std::string failWhy;         /* set when giving up: shown to the user */
    int  reason       = PLAYER_STOP_EOF;
    bool reported     = false;   /* reportPlaybackStopped already sent */
    long long posTicks = 0;

    for (;;) {
        if (!episodes.empty() && episodeIdx < (int)episodes.size()) {
            title = episodeTitle(episodes[episodeIdx]);
            if (!introFetched) {
                runWithPlayerUI(view, ir, "Loading...", [&]() {
                    intro = IntroInfo();
                    client.getIntroTimestamps(serverUrl, auth, itemId, intro);
                });
                introFetched = true;
            }
        }

        PlayerViewContext ctx;
        ctx.title        = title;
        ctx.episodes     = episodes;
        ctx.episodeIdx   = episodeIdx;
        ctx.audioStreams = audioStreams;
        ctx.subStreams   = subStreams;
        ctx.currentAudio = audioIdx;
        ctx.currentSub   = subIdx;
        ctx.intro        = intro;
        /* Burned-in subtitles are transcoded with CopyTimestamps (see
         * getTranscodingUrl): the stream keeps the item's own timestamps, so
         * the position needs no origin, and MPlayer's -ss would land before
         * the stream: playback then starts at the RESUME_PAD, 3 s early. */
        const bool itemTimestamps = subIdx >= 0;
        ctx.streamOrigin = itemTimestamps ? 0.0f : streamOriginSecs(startTicks);
        /* Skip the RESUME_PAD at demuxer level so output starts on target */
        ctx.startSkip    = !itemTimestamps && startTicks > RESUME_PAD_TICKS ? 3.0f : 0.0f;
        ctx.runtime      = (float)(runtimeTicks / 10000000.0);
        view.setContext(ctx);

        /* Fallback stream length when the MPEG-TS demuxer can't find it */
        g_wiifin_known_duration = ctx.runtime > ctx.streamOrigin
                                  ? ctx.runtime - ctx.streamOrigin : 0.0f;
        g_wiifin_ss_secs = ctx.startSkip;
        g_wiifin_burned_subs = subIdx >= 0;
        g_wiifin_stream_tls_verify = client.sslVerify;

        s_pendingReport.client        = &client;
        s_pendingReport.serverUrl     = serverUrl;
        s_pendingReport.auth          = auth;
        s_pendingReport.itemId        = itemId;
        s_pendingReport.mediaSourceId = mediaSourceId;
        s_pendingReport.playSessionId = playSessionId;
        g_stream_opened_cb = onStreamOpened;

        view.setBusy("");
        client.dropConnection();   /* MPlayer closes every socket when it stops */
        if (!wii_player_start(url.c_str())) break;

        /* ---- Playback: UI and MPlayer run side by side ---- */
        bool stopRequested = false;
        bool reachedEnd    = false;   /* the watchdog ended it at the credits */
        auto requestStop = [&](int r, const char* msg) {
            if (stopRequested) return;
            wii_player_request_stop(r);
            stopRequested = true;
            view.setBusy(msg);
        };
        /* Watchdog: if MPlayer stops making progress near the end (or for a
         * long time anywhere), end or restart the stream ourselves. */
        u64   lastProgressMs = ticks_to_millisecs(gettime());
        float lastTimePos    = -1.0f;
        u64   stopAtMs       = 0;
        bool  ioAborted      = false;

        /* Rebuffering: after its cache ran dry MPlayer resumes as soon as a
         * few bytes arrive, which stutters endlessly on a link slower than
         * the stream.  Pause it before the cache is empty and hold it until
         * a real margin is buffered.
         * Any command reaching this MPlayer build while it is blocked on an
         * empty cache aborts the stream, so "pause" is only sent while some
         * data is left (REBUF_SAFE..REBUF_LOW), and since it is a toggle,
         * every step acts on the observed g_mplayer_paused. */
        enum class Rebuf { Idle, PauseSent, Waiting, ResumeSent };
        const float REBUF_TARGET = 25.0f;   /* % of the 8 MB cache ≈ 10 s      */
        const float REBUF_LOW    = 3.0f;    /* ≈ 1 s of video ahead: pause now */
        const float REBUF_SAFE   = 0.5f;    /* below: MPlayer may be blocked   */
        Rebuf rebuf   = Rebuf::Idle;
        u64   rebufMs = 0;
        while (wii_player_is_running()) {
            Input::update();
            Input::readIR(ir);
            if (g_app_powerOff || g_app_reset) requestStop(PLAYER_STOP_EOF, "Stopping...");

            u64 now = ticks_to_millisecs(gettime());

            float fill = cache_fill_status;
            switch (rebuf) {
            case Rebuf::Idle:
                if (g_mplayer_paused && rebufMs && now - rebufMs < 5000) {
                    rebuf = Rebuf::Waiting;   /* our pause landed after all */
                    break;
                }
                /* Running low while still playing */
                if (!stopRequested && !g_mplayer_paused && !g_wiifin_loading_active &&
                    !view.buffering() && fill >= REBUF_SAFE && fill < REBUF_LOW) {
                    SYS_Report("[rebuf] cache %.1f%% at %.1f s: pausing\n",
                               (double)fill, (double)view.position());
                    wii_player_pause_toggle();
                    rebuf   = Rebuf::PauseSent;
                    rebufMs = now;
                    rebufferTimes.push_back(now);
                }
                break;
            case Rebuf::PauseSent:
                if (g_mplayer_paused)                                rebuf = Rebuf::Waiting;
                else if (!view.buffering() && now - rebufMs > 1500) {
                    SYS_Report("[rebuf] pause was dropped\n");
                    rebuf = Rebuf::Idle;
                }
                break;
            case Rebuf::Waiting:
                if (!g_mplayer_paused) { rebuf = Rebuf::Idle; break; }   /* user resumed */
                if (fill < 0.0f || fill >= REBUF_TARGET || now - rebufMs > 60000) {
                    SYS_Report("[rebuf] cache %.1f%% after %llu ms: resuming\n",
                               (double)fill, now - rebufMs);
                    wii_player_pause_toggle();
                    rebuf   = Rebuf::ResumeSent;
                    rebufMs = now;
                }
                break;
            case Rebuf::ResumeSent:
                if (!g_mplayer_paused)              { rebuf = Rebuf::Idle; rebufMs = 0; }
                else if (now - rebufMs > 1500) {
                    SYS_Report("[rebuf] still paused, resending\n");
                    wii_player_pause_toggle();
                    rebufMs = now;
                }
                break;
            }
            int pct = (int)((fill < 0.0f ? REBUF_TARGET : fill) * 100.0f / REBUF_TARGET);
            view.setRebuffering(rebuf == Rebuf::PauseSent || rebuf == Rebuf::Waiting
                                ? (pct > 99 ? 99 : pct) : -1);

            /* Two rebuffers within two minutes: the link can't sustain this
             * bitrate, restart one quality step lower from here. */
            while (!rebufferTimes.empty() && now - rebufferTimes.front() > 120000)
                rebufferTimes.erase(rebufferTimes.begin());
            if (!stopRequested && rebufferTimes.size() >= 2 && client.effectiveQuality() > 0) {
                client.videoQuality = client.effectiveQuality() - 1;
                rebufferTimes.clear();
                qualityRestartAt = view.position();
                SYS_Report("[runPlay] slow link, lowering quality to %s\n",
                           JellyfinClient::videoQualityName(client.effectiveQuality()));
                requestStop(PLAYER_STOP_SEEK, "Slow connection: lowering quality...");
            }

            if (g_mplayer_time_pos != lastTimePos || g_mplayer_paused || g_wiifin_loading_active) {
                lastTimePos    = g_mplayer_time_pos;
                if (g_mplayer_time_pos > 0.5f) everPlayed = true;
                lastProgressMs = now;
            } else if (!stopRequested && now - lastProgressMs > 3000) {
                bool atEnd = ctx.runtime > 0.0f && view.position() >= ctx.runtime - 10.0f;
                if (atEnd) {
                    reachedEnd = true;
                    requestStop(PLAYER_STOP_EOF, "");
                }
                else if (now - lastProgressMs > 20000)
                    requestStop(ctx.runtime > 0.0f ? PLAYER_STOP_ERROR : PLAYER_STOP_EOF,
                                "Reconnecting...");
            }
            /* MPlayer waits for its cache thread before quitting; unblock
             * it if it is stuck in a network read. */
            if (stopRequested) {
                if (!stopAtMs) stopAtMs = now;
                else if (!ioAborted && now - stopAtMs > 1500) {
                    SYS_Report("[runPlay] MPlayer slow to stop, closing its sockets\n");
                    wii_player_abort_io();
                    ioAborted = true;
                }
            }

            switch (view.update(Input::rawDown(), ir)) {
            case PlayerView::Action::Back:   requestStop(PLAYER_STOP_EOF,   "Stopping...");             break;
            case PlayerView::Action::Next:   requestStop(PLAYER_STOP_NEXT,  "Loading next episode..."); break;
            case PlayerView::Action::Prev:   requestStop(PLAYER_STOP_PREV,  "Loading previous episode..."); break;
            case PlayerView::Action::Audio:  requestStop(PLAYER_STOP_AUDIO, "Switching audio track..."); break;
            case PlayerView::Action::Sub:    requestStop(PLAYER_STOP_SUB,   "Switching subtitles...");  break;
            case PlayerView::Action::SeekTo: requestStop(PLAYER_STOP_SEEK,  "Seeking...");              break;
            case PlayerView::Action::Home: {
                bool wasPaused = g_mplayer_paused;
                if (!wasPaused) wii_player_pause_toggle();
                if (doShowHomeOverlay(font, btnTex, cursorTex, false))
                    requestStop(PLAYER_STOP_WIIMENU, "Stopping...");
                else if (!wasPaused)
                    wii_player_pause_toggle();
                break;
            }
            case PlayerView::Action::None:
                break;
            }

            view.render(ir);
            GRRLIB_Render();
            VideoSurface::endFrame();
        }
        reason = wii_player_wait();
        wiifin_video_report("session");
        g_stream_opened_cb = nullptr;
        client.dropConnection();
        float posSecs = view.position();
        posTicks = (long long)(posSecs * 10000000.0);

        /* The stream died on its own (transcoder session expired, network
         * drop): pick up where it stopped, up to 3 times. */
        bool dropped = !stopRequested && reason == PLAYER_STOP_EOF &&
                       ctx.runtime > 0.0f && posSecs < ctx.runtime - 30.0f;
        /* The episode played to its end (not stopped with B): go on with the
         * next one of the list, season order or shuffled, as Jellyfin does. */
        bool finished = reason == PLAYER_STOP_EOF && !dropped && everPlayed &&
                        (!stopRequested || reachedEnd);
        if (finished && episodeIdx + 1 < (int)episodes.size()) {
            SYS_Report("[runPlay] episode finished, playing the next one\n");
            reason = PLAYER_STOP_NEXT;
        }
        /* If nothing played, retry from where this stream was meant to start */
        float startSecs = (float)(startTicks / 10000000.0);
        float restartAt = posSecs > startSecs ? posSecs : startSecs;
        /* The server refused the stream: one retry (re-encoding everything
         * after a 500, see forceReencode), not three slow ones. */
        const int httpFail = g_wiifin_stream_fail_status;
        if (httpFail == 500) client.forceReencode = true;
        const int maxRetries = httpFail >= 400 ? 1 : 3;
        if ((reason == PLAYER_STOP_ERROR || dropped) && retries < maxRetries) {
            ++retries;
            SYS_Report("[runPlay] stream ended early at %.1f s, retry %d/%d\n",
                       (double)posSecs, retries, maxRetries);
            reason = PLAYER_STOP_SEEK;
        } else if (reason == PLAYER_STOP_ERROR || dropped) {
            if (httpFail >= 400) {
                char buf[96];
                snprintf(buf, sizeof(buf), "The server could not start this video (HTTP %d).", httpFail);
                failWhy = buf;
            } else if (!everPlayed) {
                failWhy = "The video stream could not be opened.";
            } else {
                failWhy = "The connection to the server keeps dropping.";
            }
            SYS_Report("[runPlay] giving up: %s\n", failWhy.c_str());
        } else if (reason == PLAYER_STOP_SEEK) {
            restartAt = qualityRestartAt >= 0.0f ? qualityRestartAt : view.seekTarget();
            qualityRestartAt = -1.0f;
        }

        /* ---- Same item, new transcode (seek / track switch / retry) ---- */
        if (reason == PLAYER_STOP_SEEK || reason == PLAYER_STOP_AUDIO ||
            reason == PLAYER_STOP_SUB) {
            int newAudio = (reason == PLAYER_STOP_AUDIO) ? view.chosenAudio() : audioIdx;
            int newSub   = (reason == PLAYER_STOP_SUB)   ? view.chosenSub()   : subIdx;
            long long fromTicks = (long long)(restartAt * 10000000.0);
            std::string newUrl, newSession;
            bool ok = false;
            runWithPlayerUI(view, ir, "Loading...", [&]() {
                client.deleteActiveEncoding(serverUrl, auth, playSessionId);
                ok = client.getTranscodingUrl(serverUrl, auth, itemId, mediaSourceId,
                                              newAudio, newSub, fromTicks,
                                              newUrl, newSession);
            });
            if (ok) {
                url           = newUrl;
                playSessionId = newSession;
                audioIdx      = newAudio;
                subIdx        = newSub;
                startTicks    = fromTicks;
                continue;
            }
            failWhy = "The server did not answer: " + client.lastError();
            SYS_Report("[runPlay] giving up: %s\n", failWhy.c_str());
            reason = PLAYER_STOP_EOF;
        }

        /* ---- Next / previous episode ---- */
        if (reason == PLAYER_STOP_NEXT || reason == PLAYER_STOP_PREV) {
            int nextIdx = (reason == PLAYER_STOP_NEXT) ? episodeIdx + 1 : episodeIdx - 1;
            if (nextIdx >= 0 && nextIdx < (int)episodes.size()) {
                JellyfinItemDetail nextDetail;
                std::string nextUrl, nextSession;
                bool ok = false;
                runWithPlayerUI(view, ir, "Loading episode...", [&]() {
                    if (posTicks > 0)
                        client.reportPlaybackStopped(serverUrl, auth, itemId, mediaSourceId,
                                                     playSessionId, posTicks);
                    client.deleteActiveEncoding(serverUrl, auth, playSessionId);
                    ok = client.getItemDetail(serverUrl, auth, episodes[nextIdx].id, nextDetail) &&
                         client.getTranscodingUrl(serverUrl, auth,
                                                  episodes[nextIdx].id, episodes[nextIdx].id,
                                                  0, -1, 0, nextUrl, nextSession);
                });
                reported = true;
                if (ok) {
                    episodeIdx    = nextIdx;
                    itemId        = episodes[nextIdx].id;
                    mediaSourceId = itemId;
                    playSessionId = nextSession;
                    url           = nextUrl;
                    audioStreams  = nextDetail.audioStreams;
                    subStreams    = nextDetail.subtitleStreams;
                    audioIdx      = 0;
                    subIdx        = -1;
                    startTicks    = 0;
                    runtimeTicks  = nextDetail.runtimeTicks;
                    introFetched  = false;
                    retries       = 0;
                    everPlayed    = false;
                    reported      = false;
                    wiifin_video_clear();   /* don't show the old episode while loading */
                    continue;
                }
            }
        }
        break;
    }

    /* ---- Leave the player ---- */
    client.videoQuality = savedQuality;
    lastItemId = itemId;
    /* Bring the menu music back now so it plays during the final requests. */
    MusicBGM::stop();
    MusicBGM::init(musicWasRunning);
    runWithPlayerUI(view, ir, "Stopping...", [&]() {
        if (g_wiifin_stream_fail_status >= 500) client.logTranscodeFailure(serverUrl, auth);
        if (!reported && posTicks > 0)
            client.reportPlaybackStopped(serverUrl, auth, itemId, mediaSourceId,
                                         playSessionId, posTicks);
        client.deleteActiveEncoding(serverUrl, auth, playSessionId);
    });
    wiifin_video_clear();
    if (!failWhy.empty() && !g_app_powerOff && !g_app_reset) {
        std::string detail = g_wiifin_stream_fail_status >= 400 && g_wiifin_stream_fail_body[0]
            ? std::string("Server says: ") + g_wiifin_stream_fail_body
            : std::string("Details are in wiifin.log on the SD card.");
        if (detail.size() > 70) detail = detail.substr(0, 67) + "...";
        if (g_wiifin_stream_fail_status == 500)
            showErrorScreen("Can't play this video", failWhy,
                            "Check the server's FFmpeg log (Dashboard > Logs).", false, ir);
        else
            showErrorScreen("Can't play this video", failWhy, detail, false, ir);
    }
    return reason == PLAYER_STOP_WIIMENU;
}
