/* Video playback session: the player, its menus and everything chained from it. */
#include "AppInternal.h"
#include "Input.h"
#include "MusicBGM.h"
#include "SoundFX.h"
#include "Log.h"
#include "../ui/Ui.h"
#include "../ui/LibraryView.h"
#include "../jellyfin/JellyfinClient.h"
#include "../jellyfin/RemoteControl.h"
#include "../player/WiiPlayer.h"
#include "../player/PlayerView.h"
#include "../player/Trickplay.h"
#include "../player/Subtitles.h"
#include "../player/VideoSurface.h"
#include "../player/vo_wiifin.h"
#include "../player/stream_wiifin.h"
#include "ExitZone.h"
#include <grrlib.h>
#include <math.h>
#include <stdio.h>
#include <unistd.h>
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
    int            audioIndex;
    int            subIndex;
    const char*    playMethod;   /* "DirectPlay" or "Transcode" */
} s_pendingReport;

static void onStreamOpened() {
    s_pendingReport.client->reportPlaybackStart(
        s_pendingReport.serverUrl,
        s_pendingReport.auth,
        s_pendingReport.itemId,
        s_pendingReport.mediaSourceId,
        s_pendingReport.playSessionId,
        s_pendingReport.playMethod, s_pendingReport.audioIndex, s_pendingReport.subIndex);
}

/* -----------------------------------------------------------------------
 * Progress reports: position and pause state every 10 s, and at once on a
 * pause or a resume, so the server's dashboard and "resume" follow.  Sent
 * from a thread of their own (the player never waits on the network), and
 * only while a stream plays: MPlayer closes every socket when it stops one.
 * ----------------------------------------------------------------------- */
static volatile bool      s_progQuit   = false;
static volatile bool      s_progWant   = false;
static volatile long long s_progTicks  = 0;
static volatile bool      s_progPaused = false;
static lwp_t              s_progThread = LWP_THREAD_NULL;
static u8                 s_progStack[32 * 1024] DEAD_AT_EXIT ATTRIBUTE_ALIGN(32);

static void* progressMain(void*) {
    while (!s_progQuit) {
        if (s_progWant) {
            s_progWant = false;
            s_pendingReport.client->reportPlaybackProgress(
                s_pendingReport.serverUrl, s_pendingReport.auth, s_pendingReport.itemId,
                s_pendingReport.mediaSourceId, s_pendingReport.playSessionId,
                s_progTicks, s_progPaused, s_pendingReport.playMethod,
                s_pendingReport.audioIndex, s_pendingReport.subIndex);
        }
        usleep(100 * 1000);
    }
    return nullptr;
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
    /* the server's remote control drives this session (see the loop) */
    Remote::setPlayerActive(true);
    struct RemoteIdle { ~RemoteIdle() { Remote::setPlayerActive(false); } } remoteIdle;

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
    /* the file as it is, or a transcode; a direct play that fails or that
     * the Wii can't keep up with goes on transcoded (allowDirect, per item) */
    JellyfinClient::PlaybackChoice how = lv.pendingPlayHow;
    bool allowDirect = true;

    /* ao_gekko needs the DSP: no menu music while a video plays. */
    bool musicWasRunning = MusicBGM::isRunning();
    MusicBGM::pause();

    PlayerView view(font, cursorTex, ringTex);
    Trickplay trickplay;
    view.setTrickplay(&trickplay);
    ir_t ir;
    ir.valid = false;

    /* Subtitles: text tracks are drawn by WiiFin (subs, fetched as SRT), so
     * the video may play as it is; picture tracks (PGS, VobSub) are burned
     * in by the server.  subIdx is the track shown, burnSub the one the
     * stream burns in (-1: none). */
    Subtitles subs;
    int subLoaded = -1;                       /* track held in subs, -1 none */
    auto isTextSub = [&](int idx) {
        for (const MediaStream& s : subStreams) if (s.index == idx) return s.isText;
        return false;
    };
    auto burnIndex = [&](int idx) { return idx >= 0 && !isTextSub(idx) ? idx : -1; };
    /* the SRT of track idx into subs (idx < 0: none); false when it failed */
    auto loadSub = [&](int idx) {
        if (idx < 0) { subs.clear(); subLoaded = -1; return true; }
        if (subLoaded == idx) return true;
        std::string srt;
        bool ok = false;
        runWithPlayerUI(view, ir, "Loading subtitles...", [&]() {
            ok = client.getSubtitleSrt(serverUrl, auth, itemId, mediaSourceId, idx, srt);
        });
        view.setBusy("");   /* may be during playback: the player goes on */
        if (ok && subs.load(srt)) { subLoaded = idx; return true; }
        SYS_Report("[Subtitles] track %d unusable, burning it in instead\n", idx);
        subs.clear();
        subLoaded = -1;
        return false;
    };
    int burnSub = burnIndex(subIdx);          /* as LibraryView asked for the stream */
    if (subIdx >= 0 && burnSub < 0 && !loadSub(subIdx)) {
        /* no SRT after all: a stream with the track burned in */
        std::string newUrl, newSession;
        bool ok = false;
        runWithPlayerUI(view, ir, "Loading...", [&]() {
            client.deleteActiveEncoding(serverUrl, auth, playSessionId);
            ok = client.getPlaybackUrl(serverUrl, auth, itemId, mediaSourceId, audioIdx, subIdx,
                                       startTicks, newUrl, newSession, how);
        });
        if (ok) { url = newUrl; playSessionId = newSession; burnSub = subIdx; }
    }

    /* Adaptive quality: repeated rebuffering on this session lowers the
     * bitrate for the rest of it (the saved setting is left untouched). */
    const int savedQuality = client.videoQuality;
    std::vector<u64> rebufferTimes;
    float qualityRestartAt = -1.0f;
    IntroInfo intro;
    bool introFetched = false;
    int  retries      = 0;
    bool everPlayed   = false;   /* this item showed something */
    bool pictureStuck = false;   /* the last stream: data came, the picture did not move */
    int  stuckTries   = 0;       /* streams of this item stuck like that */
    std::string failWhy;         /* set when giving up: shown to the user */
    int  reason       = PLAYER_STOP_EOF;
    bool reported     = false;   /* reportPlaybackStopped already sent */
    long long posTicks = 0;

    for (;;) {
        if (!episodes.empty() && episodeIdx < (int)episodes.size())
            title = episodeTitle(episodes[episodeIdx]);
        /* the parts to skip: episodes (intro, credits) and films (credits) */
        if (!introFetched) {
            runWithPlayerUI(view, ir, "Loading...", [&]() {
                intro = IntroInfo();
                client.getIntroTimestamps(serverUrl, auth, itemId, intro);
            });
            introFetched = true;
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
        const bool itemTimestamps = burnSub >= 0 || how.direct;   /* a file: its own */
        ctx.streamOrigin = itemTimestamps ? 0.0f : streamOriginSecs(startTicks);
        /* Skip the RESUME_PAD at demuxer level so output starts on target */
        ctx.startSkip    = !itemTimestamps && startTicks > RESUME_PAD_TICKS ? 3.0f : 0.0f;
        ctx.runtime      = (float)(runtimeTicks / 10000000.0);
        view.setContext(ctx);
        view.setSubtitles(subLoaded >= 0 ? &subs : nullptr, subIdx);
        if (trickplay.item() != itemId)
            trickplay.start(client, serverUrl, auth, itemId, mediaSourceId);

        /* Fallback stream length when the MPEG-TS demuxer can't find it */
        g_wiifin_known_duration = ctx.runtime > ctx.streamOrigin
                                  ? ctx.runtime - ctx.streamOrigin : 0.0f;
        /* a file as it is: MPlayer seeks to the start position itself */
        g_wiifin_ss_secs = how.direct ? (float)(startTicks / 10000000.0) : ctx.startSkip;
        g_wiifin_direct  = how.direct ? 1 : 0;
        snprintf(g_wiifin_demuxer, sizeof(g_wiifin_demuxer), "%s", how.demuxer.c_str());
        g_wiifin_burned_subs = burnSub >= 0;
        g_wiifin_aid = how.direct ? how.aid : -1;
        g_wiifin_fps = how.direct ? 0.0f : how.fps;   /* a file keeps its own */
        g_wiifin_stream_tls_verify = client.sslVerify;

        s_pendingReport.client        = &client;
        s_pendingReport.serverUrl     = serverUrl;
        s_pendingReport.auth          = auth;
        s_pendingReport.itemId        = itemId;
        s_pendingReport.mediaSourceId = mediaSourceId;
        s_pendingReport.playSessionId = playSessionId;
        s_pendingReport.audioIndex    = audioIdx;
        s_pendingReport.subIndex      = subIdx;
        s_pendingReport.playMethod    = how.direct ? "DirectPlay" : "Transcode";
        g_stream_opened_cb = onStreamOpened;
        SYS_Report("[runPlay] %s\n", how.direct ? "direct play" : "transcode");

        view.setBusy("");
        client.dropConnection();   /* MPlayer closes every socket when it stops */
        if (!wii_player_start(url.c_str())) break;

        /* ---- Playback: UI and MPlayer run side by side ---- */
        bool stopRequested = false;
        bool reachedEnd    = false;   /* the watchdog ended it at the credits */
        auto requestStop = [&](int r, const char* msg) {
            if (stopRequested) return;
            SYS_Report("[runPlay] stop (%d) at %.1f s: %s\n", r, (double)view.position(), msg);
            wii_player_request_stop(r);
            stopRequested = true;
            view.setBusy(msg);
        };
        /* Watchdog: if MPlayer stops making progress near the end (or for a
         * long time anywhere), end or restart the stream ourselves. */
        u64   lastProgressMs = ticks_to_millisecs(gettime());
        float lastTimePos    = -1.0f;
        unsigned long long bytesAtProgress = g_wiifin_stream_bytes;
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
        u64   lastProgressAt = ticks_to_millisecs(gettime());
        bool  lastPaused     = false;
        /* direct play: frames out per second of video, over 10 s */
        float    perfFrom   = -1.0f;   /* video time the count started at, -2 done */
        unsigned perfFrames = 0;
        /* direct play: where a seek (or the resume) was meant to land.  An
         * AVI seek goes on to the next keyframe, often 10 s later; seeking
         * back from there lands on the keyframe before the target. */
        float seekWant  = how.direct && startTicks > 0 ? (float)(startTicks / 10000000.0) : -1.0f;
        bool  seekFixed = false;
        /* MPlayer may show a frame or two from before a seek, its loading
         * indicator already off: a seek is done once the position moved */
        float seekFrom  = 0.0f;
        u64   seekAt    = ticks_to_millisecs(gettime());
        s_progQuit = false;
        s_progWant = false;
        LWP_CreateThread(&s_progThread, progressMain, nullptr, s_progStack, sizeof(s_progStack), 40);
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
                allowDirect = false;   /* a file's bitrate can't go down: transcode */
                rebufferTimes.clear();
                qualityRestartAt = view.position();
                SYS_Report("[runPlay] slow link, lowering quality to %s\n",
                           JellyfinClient::videoQualityName(client.effectiveQuality()));
                requestStop(PLAYER_STOP_SEEK, "Slow connection: lowering quality...");
            }

            float timePos = g_mplayer_time_pos;
            if (g_wiifin_test_freeze > 0.0f && view.position() > g_wiifin_test_freeze)
                timePos = lastTimePos;   /* tests: a picture that stopped there */
            if (timePos != lastTimePos || g_mplayer_paused || g_wiifin_loading_active) {
                lastTimePos    = timePos;
                if (timePos > 0.5f) everPlayed = true;
                lastProgressMs = now;
                bytesAtProgress = g_wiifin_stream_bytes;
            } else if (!stopRequested && now - lastProgressMs > 3000) {
                bool atEnd = ctx.runtime > 0.0f && view.position() >= ctx.runtime - 10.0f;
                if (atEnd) {
                    reachedEnd = true;
                    requestStop(PLAYER_STOP_EOF, "");
                }
                else if (now - lastProgressMs > 20000) {
                    /* the data still comes in (or sits in the cache) while
                     * the picture does not move: not the connection */
                    unsigned long long got = g_wiifin_stream_bytes - bytesAtProgress;
                    pictureStuck = got > 256 * 1024 || cache_fill_status >= 50.0f;
                    SYS_Report("[runPlay] no progress for 20 s: %llu KB received meanwhile, cache %.0f%%: %s\n",
                               got / 1024, (double)cache_fill_status,
                               pictureStuck ? "the picture is stuck" : "no data");
                    requestStop(ctx.runtime > 0.0f ? PLAYER_STOP_ERROR : PLAYER_STOP_EOF,
                                pictureStuck ? "The picture is stuck: trying again..." : "Reconnecting...");
                }
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

            if (seekWant >= 0.0f && !stopRequested && !g_wiifin_loading_active &&
                g_mplayer_time_pos > 0.0f &&
                (fabsf(g_mplayer_time_pos - seekFrom) > 1.0f || now - seekAt > 3000)) {
                if (!seekFixed && g_mplayer_time_pos > seekWant + 1.5f) {
                    SYS_Report("[DirectPlay] landed at %.1f s for %.1f s: back to the keyframe before\n",
                               (double)g_mplayer_time_pos, (double)seekWant);
                    seekFrom  = g_mplayer_time_pos;
                    seekAt    = now;
                    wii_player_seek_by(seekWant - g_mplayer_time_pos);
                    seekFixed = true;
                } else {
                    seekWant = -1.0f;
                }
            }

            /* Direct play the Wii can't keep up with (MPlayer drops frames
             * behind schedule): on transcoded from here */
            if (how.direct && how.fps > 1.0f && perfFrom > -2.0f && !stopRequested &&
                !g_mplayer_paused && !g_wiifin_loading_active && g_mplayer_time_pos > 0.0f) {
                if (perfFrom < 0.0f) {
                    perfFrom   = g_mplayer_time_pos;
                    perfFrames = wiifin_video_frames();
                } else if (g_mplayer_time_pos - perfFrom >= 10.0f) {
                    float rate = (wiifin_video_frames() - perfFrames) / (g_mplayer_time_pos - perfFrom);
                    SYS_Report("[DirectPlay] %.1f of %.1f frames a second shown\n",
                               (double)rate, (double)how.fps);
                    perfFrom = -2.0f;
                    if (rate < 0.85f * how.fps) {
                        allowDirect      = false;
                        qualityRestartAt = view.position();
                        requestStop(PLAYER_STOP_SEEK, "Too heavy for the Wii: transcoding...");
                    }
                }
            }

            if (!stopRequested && !g_wiifin_loading_active && !s_progWant &&
                (now - lastProgressAt >= 10000 || g_mplayer_paused != lastPaused)) {
                lastProgressAt = now;
                lastPaused     = g_mplayer_paused;
                s_progTicks    = (long long)(view.position() * 10000000.0f);
                s_progPaused   = lastPaused;
                s_progWant     = true;
            }

            /* thumbnails load once the picture is there, never while the
             * stream opens or stops; ahead of time with 6 s of video cached */
            trickplay.setOnline(!stopRequested && !g_wiifin_loading_active,
                                fill < 0.0f || fill >= 15.0f);

            /* the server's remote control: as the Wii Remote's buttons (a
             * volume change reported at once: lastProgressAt); something else
             * to play ("Play on..."): the library starts it */
            if (!stopRequested && Remote::hasPlay()) requestStop(PLAYER_STOP_EOF, "Stopping...");
            Remote::PlayRequest queued;   /* "Play next" during a video: music only */
            if (Remote::takeQueue(queued)) {
                SYS_Report("[Remote] queue during a video: not supported\n");
                view.toast("Play next / Add to queue: music only", 2500);
            }
            for (Remote::Command rc; !stopRequested && Remote::poll(rc); ) {
                switch (rc.cmd) {
                case Remote::Cmd::Pause:     if (!g_mplayer_paused) wii_player_pause_toggle(); break;
                case Remote::Cmd::Unpause:   if (g_mplayer_paused)  wii_player_pause_toggle(); break;
                case Remote::Cmd::PlayPause: wii_player_pause_toggle(); break;
                case Remote::Cmd::Stop:      requestStop(PLAYER_STOP_EOF, "Stopping..."); break;
                case Remote::Cmd::Seek:      view.remoteSeek((float)(rc.ticks / 10000000.0)); break;
                case Remote::Cmd::FastForward: view.remoteNudge(+30.0f); break;
                case Remote::Cmd::Rewind:      view.remoteNudge(-10.0f); break;
                case Remote::Cmd::Next:
                    if (episodeIdx + 1 < (int)episodes.size()) requestStop(PLAYER_STOP_NEXT, "Loading next episode...");
                    else view.toast("No next episode");
                    break;
                case Remote::Cmd::Prev:
                    if (!episodes.empty() && episodeIdx > 0) requestStop(PLAYER_STOP_PREV, "Loading previous episode...");
                    else view.toast("No previous episode");
                    break;
                case Remote::Cmd::VolumeUp:   wii_player_vol_up();   view.toast("Volume +", 1000); lastProgressAt = 0; break;
                case Remote::Cmd::VolumeDown: wii_player_vol_down(); view.toast("Volume -", 1000); lastProgressAt = 0; break;
                case Remote::Cmd::SetVolume: {
                    wii_player_set_volume(rc.value);
                    char t[24]; snprintf(t, sizeof(t), "Volume %d", wii_player_volume());
                    view.toast(t, 1000);
                    lastProgressAt = 0;
                    break;
                }
                case Remote::Cmd::Mute:       wii_player_set_mute(1); view.toast("Sound off", 1500); lastProgressAt = 0; break;
                case Remote::Cmd::Unmute:     wii_player_set_mute(0); view.toast("Sound on", 1500);  lastProgressAt = 0; break;
                case Remote::Cmd::ToggleMute:
                    wii_player_set_mute(!wii_player_muted());
                    view.toast(wii_player_muted() ? "Sound off" : "Sound on", 1500);
                    lastProgressAt = 0;
                    break;
                case Remote::Cmd::AudioTrack:    view.remoteTrack(true,  rc.value); break;
                case Remote::Cmd::SubtitleTrack: view.remoteTrack(false, rc.value); break;
                default: break;
                }
            }

            switch (view.update(Input::rawDown(), ir)) {
            case PlayerView::Action::Back:   requestStop(PLAYER_STOP_EOF,   "Stopping...");             break;
            case PlayerView::Action::Next:   requestStop(PLAYER_STOP_NEXT,  "Loading next episode..."); break;
            case PlayerView::Action::Prev:   requestStop(PLAYER_STOP_PREV,  "Loading previous episode..."); break;
            case PlayerView::Action::Audio:  requestStop(PLAYER_STOP_AUDIO, "Switching audio track..."); break;
            case PlayerView::Action::Sub: {
                /* to a text track or none, on a stream burning none: no
                 * restart, the new track is drawn over this stream */
                int newSub = view.chosenSub();
                if (burnSub < 0 && burnIndex(newSub) < 0 && loadSub(newSub)) {
                    subIdx = newSub;
                    s_pendingReport.subIndex = subIdx;
                    view.setSubtitles(subLoaded >= 0 ? &subs : nullptr, subIdx);
                } else {
                    requestStop(PLAYER_STOP_SUB, "Switching subtitles...");
                }
                break;
            }
            case PlayerView::Action::SeekTo:
                if (how.direct) {   /* in the same file: MPlayer seeks */
                    seekFrom  = g_mplayer_time_pos;
                    seekAt    = ticks_to_millisecs(gettime());
                    wii_player_seek_by(view.seekTarget() - view.position());
                    view.seekInPlace();
                    seekWant  = view.seekTarget();
                    seekFixed = false;
                    if (perfFrom > -2.0f) perfFrom = -1.0f;   /* count again from there */
                } else {
                    requestStop(PLAYER_STOP_SEEK, "Seeking...");
                }
                break;
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
        trickplay.setOnline(false, false);
        reason = wii_player_wait();
        s_progQuit = true;
        LWP_JoinThread(s_progThread, nullptr);
        s_progThread = LWP_THREAD_NULL;
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
        if (how.direct && allowDirect && (reason == PLAYER_STOP_ERROR || dropped)) {
            SYS_Report("[DirectPlay] the file did not play, transcoding instead\n");
            allowDirect = false;
        }
        const int httpFail = g_wiifin_stream_fail_status;
        if (httpFail == 500) client.forceReencode = true;
        const int maxRetries = httpFail >= 400 ? 1 : 3;
        /* Stuck with the data there: once more another way (the file
         * transcoded, or a lower quality: smaller pictures to decode), then
         * say what happens instead of blaming the connection. */
        bool stuckGiveUp = false;
        if (pictureStuck && reason == PLAYER_STOP_ERROR) {
            pictureStuck = false;
            if (++stuckTries == 1 && (how.direct || client.effectiveQuality() > 0)) {
                if (!how.direct) client.videoQuality = client.effectiveQuality() - 1;
                SYS_Report("[runPlay] picture stuck: again %s\n", how.direct ? "transcoded" :
                           JellyfinClient::videoQualityName(client.effectiveQuality()));
            } else {
                stuckGiveUp = true;
            }
        }
        if (stuckGiveUp) {
            failWhy = "The video comes in, but the Wii can't show it: the picture stays stuck.";
            SYS_Report("[runPlay] giving up: %s\n", failWhy.c_str());
        } else if ((reason == PLAYER_STOP_ERROR || dropped) && retries < maxRetries) {
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
            /* a text track is drawn: the stream burns in only picture ones */
            int newBurn  = burnIndex(newSub);
            if (newSub >= 0 && newBurn < 0 && !loadSub(newSub)) newBurn = newSub;
            if (newSub < 0) loadSub(-1);
            long long fromTicks = (long long)(restartAt * 10000000.0);
            std::string newUrl, newSession;
            bool ok = false;
            runWithPlayerUI(view, ir, "Loading...", [&]() {
                client.deleteActiveEncoding(serverUrl, auth, playSessionId);
                ok = client.getPlaybackUrl(serverUrl, auth, itemId, mediaSourceId,
                                           newAudio, newBurn, fromTicks,
                                           newUrl, newSession, how, allowDirect);
            });
            if (ok) {
                url           = newUrl;
                playSessionId = newSession;
                audioIdx      = newAudio;
                subIdx        = newSub;
                burnSub       = newBurn;
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
                /* its tracks as the server picks them (as on its page) */
                int nextAudio = 0, nextSub = -1, nextBurn = -1;
                runWithPlayerUI(view, ir, "Loading episode...", [&]() {
                    if (posTicks > 0)
                        client.reportPlaybackStopped(serverUrl, auth, itemId, mediaSourceId,
                                                     playSessionId, posTicks);
                    client.deleteActiveEncoding(serverUrl, auth, playSessionId);
                    ok = client.getItemDetail(serverUrl, auth, episodes[nextIdx].id, nextDetail);
                    if (ok) {
                        if (nextDetail.defaultAudioIndex >= 0) nextAudio = nextDetail.defaultAudioIndex;
                        for (const MediaStream& s : nextDetail.subtitleStreams)
                            if (s.index == nextDetail.defaultSubIndex) {
                                nextSub  = s.index;
                                nextBurn = s.isText ? -1 : s.index;   /* text: drawn by WiiFin */
                            }
                    }
                    ok = ok && client.getPlaybackUrl(serverUrl, auth,
                                                     episodes[nextIdx].id, episodes[nextIdx].id,
                                                     nextAudio, nextBurn, 0, nextUrl, nextSession, how);
                });
                reported = true;
                SYS_Report("[runPlay] %s episode: %d of %d\n",
                           reason == PLAYER_STOP_NEXT ? "next" : "previous", nextIdx + 1, (int)episodes.size());
                if (ok) {
                    episodeIdx    = nextIdx;
                    itemId        = episodes[nextIdx].id;
                    mediaSourceId = itemId;
                    playSessionId = nextSession;
                    url           = nextUrl;
                    audioStreams  = nextDetail.audioStreams;
                    subStreams    = nextDetail.subtitleStreams;
                    audioIdx      = nextAudio;
                    subIdx        = nextSub;
                    burnSub       = nextBurn;
                    subs.clear();
                    subLoaded     = -1;
                    if (subIdx >= 0 && burnSub < 0 && !loadSub(subIdx)) subIdx = -1;
                    startTicks    = 0;
                    runtimeTicks  = nextDetail.runtimeTicks;
                    introFetched  = false;
                    allowDirect   = true;
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
