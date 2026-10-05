#include "App.h"
#include "ExitZone.h"
#include "Text.h"
#include "../ui/Ui.h"
#include "Utils.h"
#include "Input.h"
#include "MusicBGM.h"
#include "SoundFX.h"
#include "../ui/ConnectView.h"
#include "../ui/MusicPlayerView.h"
#include "../ui/ProfileView.h"
#include "../ui/SettingsView.h"
#include "../ui/LibraryView.h"
#include "../jellyfin/JellyfinClient.h"

#include <grrlib.h>
#include <wiiuse/wpad.h>
#include <fat.h>
#include <sdcard/wiisd_io.h>
#include <errno.h>
#include <stdio.h>
#include <gccore.h>
#include <sys/iosupport.h>

#include <unistd.h>
#include <sys/stat.h>
#include <string>
#include "../player/WiiPlayer.h"
#include "../player/PlayerView.h"
#include "../player/VideoSurface.h"
#include "Log.h"
#include "../version.h"
#include <ogc/ios.h>
#include "../player/vo_wiifin.h"
#include "../player/stream_wiifin.h"
#include <functional>
#include <ogc/lwp_watchdog.h>

/* Ring spinner PNG embedded asset (music player transition frames) */
extern unsigned char data_ring_png[];
extern unsigned int  data_ring_png_len;

/* Forward declaration — defined later in this file, before App::loop() */
static bool doShowHomeOverlay(GRRLIB_ttfFont* font, GRRLIB_texImg* btnTex,
                               GRRLIB_texImg* cursorPointerTex, bool musicEnabled);

volatile bool g_app_powerOff = false;
volatile bool g_app_reset    = false;
static volatile bool s_restartApp = false;
static void onPower() { g_app_powerOff = true; }
static void onReset(u32, void*) { g_app_reset = true; }

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

/* Full-screen error card: title, two lines of explanation.  With retry, A
 * retries (returns true) and B goes back; without, A or B closes it. */
static bool showErrorScreen(const char* title, const std::string& line1,
                            const std::string& line2, bool retry, ir_t& ir)
{
    const Ui::Palette& p = Ui::pal();
    for (;;) {
        Input::update();
        Input::readIR(ir);
        if (g_app_powerOff || g_app_reset) return false;
        if (Input::isBackPressed() || (!retry && Input::isAJustPressed())) return false;
        if (retry && Input::isAJustPressed()) { SoundFX::play(SoundFX::FX::Start); return true; }
        Ui::background(false);
        Ui::card(80, 150, 480, 130, 18, 0.0f);
        Ui::circle(114, 182, 13, p.danger);
        Ui::textCentered(114, 172, "!", 18, 0xFFFFFFFF);
        Ui::text(138, 170, title, 20, p.danger);
        Ui::text(100, 210, line1.c_str(), 15, p.text);
        Ui::text(100, 236, line2.c_str(), 13, p.textDim);
        if (retry) {
            const Ui::Hint l[] = { { "A", "Retry" } };
            const Ui::Hint r[] = { { "B", "Back" } };
            Ui::bottomBar(l, 1, r, 1);
        } else {
            const Ui::Hint r[] = { { "A", "OK" } };
            Ui::bottomBar(nullptr, 0, r, 1);
        }
        GRRLIB_Render();
    }
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
static bool runPlaySession(JellyfinClient& client,
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
        ctx.streamOrigin = streamOriginSecs(startTicks);
        /* Skip the RESUME_PAD at demuxer level so output starts on target */
        ctx.startSkip    = startTicks > RESUME_PAD_TICKS ? 3.0f : 0.0f;
        ctx.runtime      = (float)(runtimeTicks / 10000000.0);
        view.setContext(ctx);

        /* Fallback stream length when the MPEG-TS demuxer can't find it */
        g_wiifin_known_duration = ctx.runtime > ctx.streamOrigin
                                  ? ctx.runtime - ctx.streamOrigin : 0.0f;
        g_wiifin_ss_secs = ctx.startSkip;
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
            if (!stopRequested && rebufferTimes.size() >= 2 && client.videoQuality > 0) {
                --client.videoQuality;
                rebufferTimes.clear();
                qualityRestartAt = view.position();
                SYS_Report("[runPlay] slow link, lowering quality to %s\n",
                           JellyfinClient::videoQualityName(client.videoQuality));
                requestStop(PLAYER_STOP_SEEK, "Slow connection: lowering quality...");
            }

            if (g_mplayer_time_pos != lastTimePos || g_mplayer_paused || g_wiifin_loading_active) {
                lastTimePos    = g_mplayer_time_pos;
                if (g_mplayer_time_pos > 0.5f) everPlayed = true;
                lastProgressMs = now;
            } else if (!stopRequested && now - lastProgressMs > 3000) {
                bool atEnd = ctx.runtime > 0.0f && view.position() >= ctx.runtime - 10.0f;
                if (atEnd)
                    requestStop(PLAYER_STOP_EOF, "");
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

            switch (view.update(WPAD_ButtonsDown(0), ir)) {
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

extern unsigned char data_logo_wiifin_png[];
extern unsigned int data_logo_wiifin_png_len;
extern unsigned char data_button_start_png[];
extern unsigned int data_button_start_png_len;
extern unsigned char data_cursors_PointerP1_64_png[];
extern unsigned int data_cursors_PointerP1_64_png_len;
extern unsigned char data_cursors_HandOpenP1_64_png[];
extern unsigned int data_cursors_HandOpenP1_64_png_len;
extern unsigned char data_cursors_HandClosedP1_64_png[];
extern unsigned int data_cursors_HandClosedP1_64_png_len;
extern unsigned char data_wii_font_ttf[];
extern unsigned int data_wii_font_ttf_len;
extern unsigned char data_jp_font_ttf[];
extern unsigned int data_jp_font_ttf_len;

#define logo_wiifin_png      data_logo_wiifin_png
#define logo_wiifin_png_len  data_logo_wiifin_png_len
#define button_start_png      data_button_start_png
#define button_start_png_len  data_button_start_png_len
#define wii_font_ttf      data_wii_font_ttf
#define wii_font_ttf_len  data_wii_font_ttf_len
#define jp_font_ttf       data_jp_font_ttf
#define jp_font_ttf_len   data_jp_font_ttf_len

// --- Button layout constants ---
// btnTex is 512x128 (power-of-2 required by GX/GRRLIB).
// Display at 280x70 (4:1 ratio preserved, sx=sy=0.547).
static const int BX        = 170;
static const int BW        = 300;
static const int BH        = 58;
static const int BY_START  = 168;
static const int B_SPACING = 74;

void App::init(const char* argv0) {
    SYS_SetPowerCallback(onPower);
    SYS_SetResetCallback(onReset);

    SYS_Report("[WiiFin] init start\n");

    {
        VIDEO_Init();
        SYS_Report("[WiiFin] VIDEO_Init done\n");
        GXRModeObj* m = VIDEO_GetPreferredMode(NULL);
        /* TV standard (0 NTSC, 1 PAL 50 Hz, 2 MPAL, 5 PAL 60 Hz) and sizes */
        SYS_Report("[WiiFin] video mode: tv=%u fb=%ux%u efb=%u vi=%ux%u%s\n",
                   (unsigned)(m->viTVMode >> 2), (unsigned)m->fbWidth, (unsigned)m->xfbHeight,
                   (unsigned)m->efbHeight, (unsigned)m->viWidth, (unsigned)m->viHeight,
                   (m->viTVMode & 3) == VI_PROGRESSIVE ? " progressive" : "");
        void* xfb = MEM_K0_TO_K1(SYS_AllocateFramebuffer(m));
        VIDEO_ClearFrameBuffer(m, xfb, 0x00800080); // YCbCr black — prevents green garbage frame
        VIDEO_Configure(m);
        VIDEO_SetNextFramebuffer(xfb);
        VIDEO_SetBlack(false);  // unblank so Dolphin's renderer connects to VI output
        VIDEO_Flush();
        SYS_Report("[WiiFin] VIDEO_Flush done, waiting 50ms\n");
        usleep(50000);
    }

    {
        static u8 s_pre_fifo[256 * 1024] DEAD_AT_EXIT ATTRIBUTE_ALIGN(32);
        SYS_Report("[WiiFin] GX_Init start\n");
        GX_Init(s_pre_fifo, sizeof(s_pre_fifo));
        SYS_Report("[WiiFin] GX_AbortFrame start\n");
        GX_AbortFrame();
        GX_Flush();
        SYS_Report("[WiiFin] GX pre-init done\n");
    }

    SYS_Report("[WiiFin] GRRLIB_Init start\n");
    GRRLIB_Init();
    SYS_Report("[WiiFin] GRRLIB_Init done\n");

    // Clear both framebuffers to black immediately to avoid green garbage frame
    GRRLIB_FillScreen(0x000000FF);
    GRRLIB_Render();
    GRRLIB_FillScreen(0x000000FF);
    GRRLIB_Render();

    WiiUtils::detectAspect();
    Ui::initScreen(WiiUtils::widescreen);

    // Load textures and fonts from embedded data (no file I/O, always fast)
    logoTex   = GRRLIB_LoadTexture(logo_wiifin_png);
    btnTex    = GRRLIB_LoadTexture(button_start_png);
    cursorPointerTex    = GRRLIB_LoadTexture(data_cursors_PointerP1_64_png);
    font   = GRRLIB_LoadTTF(wii_font_ttf, wii_font_ttf_len);
    jpFont = GRRLIB_LoadTTF(jp_font_ttf, jp_font_ttf_len);
    Ui::setFont(font);
    ringTex = GRRLIB_LoadTexture(data_ring_png);

    // Show a splash frame immediately so the user sees something during init.
    if (logoTex) {
        Ui::background(false);
        float ls = 0.60f;
        int lw = (int)(logoTex->w * ls);
        GRRLIB_DrawImg((640 - lw) / 2, (480 - (int)(logoTex->h * ls)) / 2,
                       logoTex, 0, ls, ls, 0xFFFFFFFF);
        GRRLIB_Render();
    } else {
        // Logo failed to load — show a plain coloured screen so we know init reached this point
        GRRLIB_FillScreen(0x1E3A5FFF);  // steel-blue diagnostic fallback
        GRRLIB_Render();
    }

    // Now init filesystem and input (may take a moment on first call)
    fatInitDefault();
    // fatMountSimple was removed: calling it after fatInitDefault() overwrites
    // the devoptab entry for "sd" with a stub that has open_r=NULL → errno=88.
    // fatInitDefault() alone correctly registers "sd" with a working FAT driver.

    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);
    WPAD_SetVRes(WPAD_CHAN_0, 640, 480);

    // Try argv0, all known prefixes, and NO-prefix (default libfat device)
    if (argv0 && argv0[0]) {
        std::string p(argv0);
        size_t slash = p.rfind('/');
        if (slash != std::string::npos)
            argvPath = p.substr(0, slash + 1) + "wiifin.cfg";
    }
    const char* probes[] = {
        argvPath.empty() ? nullptr : argvPath.c_str(),
        "/apps/WiiFin/wiifin.cfg",        // no prefix = default device
        "sd:/apps/WiiFin/wiifin.cfg",
        "fat:/apps/WiiFin/wiifin.cfg",
        "fat0:/apps/WiiFin/wiifin.cfg",
        "fat1:/apps/WiiFin/wiifin.cfg",
        "usb:/apps/WiiFin/wiifin.cfg",
    };
    // mkdirp: create each component of `dir` (e.g. "sd:/apps/WiiFin") only
    // if the device supports mkdir_r.  mkdir() only creates one level, so we
    // walk forward from the first '/' after the device prefix.
    auto mkdirp = [](const std::string& dir) {
        const char* colon = strchr(dir.c_str(), ':');
        if (!colon) return;
        std::string devname(dir.c_str(), colon - dir.c_str());
        bool devOk = false;
        for (int j = 0; j < STD_MAX; j++) {
            if (!devoptab_list[j] || !devoptab_list[j]->name) continue;
            if (devname == devoptab_list[j]->name && devoptab_list[j]->mkdir_r) {
                devOk = true; break;
            }
        }
        if (!devOk) return;
        // Walk each path component and mkdir incrementally
        std::string cur;
        const char* p = dir.c_str();
        while (*p) {
            const char* slash = strchr(p + 1, '/');
            if (slash) {
                cur.assign(dir.c_str(), slash);
                mkdir(cur.c_str(), 0777); /* ignore errors (EEXIST ok) */
                p = slash;
            } else {
                mkdir(dir.c_str(), 0777);
                break;
            }
        }
    };

    settingsPath = "";
    for (int i = 0; i < 7; i++) {
        if (!probes[i]) continue;
        // Create parent directory tree before probing
        {
            std::string p(probes[i]);
            size_t slash = p.rfind('/');
            if (slash != std::string::npos && slash > 0)
                mkdirp(p.substr(0, slash));
        }
        errno = 0;
        FILE* f = fopen(probes[i], "a");
        if (f) { fclose(f); settingsPath = probes[i]; break; }
    }

    if (!settingsPath.empty())
        Log::open(settingsPath.substr(0, settingsPath.rfind('/') + 1));
    SYS_Report("[WiiFin] v%s, IOS%d v%d, %s, MEM1 %u KB / MEM2 %u KB free, settings %s\n",
               WIIFIN_VERSION, (int)IOS_GetVersion(), (int)IOS_GetRevision(),
               WiiUtils::widescreen ? "16:9" : "4:3",
               (unsigned)(SYS_GetArena1Size() / 1024), (unsigned)(SYS_GetArena2Size() / 1024),
               settingsPath.empty() ? "(none)" : settingsPath.c_str());
    loadSettings();
    /* DHCP takes a few seconds: get it going while the menus show */
    jellyfinClient.startNetwork();
    MusicBGM::init(musicEnabled);
    SoundFX::init();
}

/* -----------------------------------------------------------------------
 * doShowHomeOverlay — full-screen Wii-style HOME menu with confirmation popup.
 * Can be called from App::loop() (normal UI) or from runPlaySession() (after
 * the player stops with PLAYER_STOP_HOME) so both contexts share identical UX.
 * Returns true if the user wants to exit (Wii Menu or Reset).
 * Sets s_restartApp = true when "Reset" is chosen.
 * Pauses BGM on entry; resumes only if musicEnabled is true (pass false
 * when calling from the player, since BGM is already paused for playback).
 * ----------------------------------------------------------------------- */
static bool doShowHomeOverlay(GRRLIB_ttfFont* font, GRRLIB_texImg* btnTex,
                               GRRLIB_texImg* cursorPointerTex, bool musicEnabled)
{
    ir_t ir;
    ir.valid = false;
        int   hmSel     = 0;    /* 0 = Wii Menu, 1 = Reset */
        int   state     = 0;    /* 0 = home menu, 1 = confirm popup */
        int   confirmFor = 0;   /* which button triggered the popup */

        /* Main button layout */
        const int BTN_W = 230, BTN_H = 80;
        const int BTN_Y = 195;
        const int B0X   =  65;
        const int B1X   = 345;
        float bcx[2] = { B0X + BTN_W * 0.5f, B1X + BTN_W * 0.5f };
        float bcy    = BTN_Y + BTN_H * 0.5f;

        /* Confirmation dialog layout — centred on 640×480 */
        const int DW = 370, DH = 218;
        const int DX = (640 - DW) / 2;       /* 135 */
        const int DY = (480 - DH) / 2;       /* 131 */
        const int PBW = 150, PBH = 56;
        const int PBY = DY + DH - PBH - 20;
        const int PB0X = DX + 24;
        const int PB1X = DX + DW - PBW - 24;
        float pbcx[2] = { PB0X + PBW * 0.5f, PB1X + PBW * 0.5f };
        float pbcy    = PBY + PBH * 0.5f;

        /* Animation state */
        float openAnim      = 0.0f;
        float hoverSc[2]    = { 1.0f, 1.0f };
        float popAnim       = 0.0f;
        int   popSel        = 1;             /* default: "No" */
        float popHoverSc[2] = { 1.0f, 1.0f };

        /* Hover-change trackers for Select sound */
        int prevHoverMain = -1;
        int prevHoverPop  = -1;

        MusicBGM::pause();
        SoundFX::play(SoundFX::FX::MenuEnter);

        while (true) {
            Input::update();
            Input::readIR(ir);
            orient_t orient; WPAD_Orientation(WPAD_CHAN_0, &orient);
            if (g_app_powerOff || g_app_reset) return true;

            float irX = ir.valid ? ir.x : -1.f;
            float irY = ir.valid ? ir.y : -1.f;

            /* Per-frame hover results (reset each frame) */
            int hover    = -1;
            int popHover = -1;

            /* ---- Input ---- */
            if (state == 0) {
                if (Input::isBPressed()) { if (musicEnabled) MusicBGM::resume(); return false; }

                if (ir.valid) {
                    for (int i = 0; i < 2; i++) {
                        float hw = BTN_W * hoverSc[i] * 0.5f;
                        float hh = BTN_H * hoverSc[i] * 0.5f;
                        if (irX >= bcx[i] - hw && irX <= bcx[i] + hw &&
                            irY >= bcy    - hh && irY <= bcy    + hh)
                            hover = i;
                    }
                }
                /* Select sound: fire once when IR cursor first enters a button */
                if (hover >= 0 && hover != prevHoverMain)
                    SoundFX::play(SoundFX::FX::Select);
                prevHoverMain = hover;

                if (hover >= 0) hmSel = hover;    /* one highlight: the pointer's */
                /* With the pointer on screen only a button under it counts;
                 * the d-pad selection is used when pointing away. */
                if (Input::isAJustPressed() && (!ir.valid || hover >= 0)) {
                    SoundFX::play(SoundFX::FX::Start); /* clicking Wii Menu / Reset */
                    confirmFor = (hover >= 0) ? hover : hmSel;
                    state      = 1;
                    popAnim    = 0.0f;
                    popSel     = 1;
                }
                if (Input::isLeftPressed()  || Input::isUpPressed())   hmSel = 0;
                if (Input::isRightPressed() || Input::isDownPressed())  hmSel = 1;
                for (int i = 0; i < 2; i++) {
                    float t = (hover == i) ? 1.10f : 1.0f;
                    hoverSc[i] += (t - hoverSc[i]) * 0.18f;
                }
            } else {
                /* popup state */
                if (Input::isBPressed()) { state = 0; }
                if (ir.valid) {
                    for (int i = 0; i < 2; i++) {
                        if (irX >= pbcx[i] - PBW * 0.5f && irX <= pbcx[i] + PBW * 0.5f &&
                            irY >= pbcy    - PBH * 0.5f && irY <= pbcy    + PBH * 0.5f)
                            popHover = i;
                    }
                }
                /* Select sound: fire once when IR cursor first enters a popup button */
                if (popHover >= 0 && popHover != prevHoverPop)
                    SoundFX::play(SoundFX::FX::Select);
                prevHoverPop = popHover;

                if (popHover >= 0) popSel = popHover;
                if (Input::isAJustPressed() && (!ir.valid || popHover >= 0)) {
                    int sel = (popHover >= 0) ? popHover : popSel;
                    if (sel == 0) { /* Yes */
                        SoundFX::play(SoundFX::FX::MenuExit);
                        SoundFX::waitDone(SoundFX::FX::MenuExit);
                        if (confirmFor == 1) s_restartApp = true;
                        return true;
                    } else {        /* No */
                        SoundFX::play(SoundFX::FX::Back);
                        state = 0;
                    }
                }
                if (Input::isLeftPressed()  || Input::isUpPressed())   popSel = 0;
                if (Input::isRightPressed() || Input::isDownPressed())  popSel = 1;
                for (int i = 0; i < 2; i++) {
                    float t = (popHover == i) ? 1.08f : 1.0f;
                    popHoverSc[i] += (t - popHoverSc[i]) * 0.18f;
                }
            }

            /* ---- Update animations ---- */
            if (openAnim < 1.0f) openAnim += 0.075f;
            if (openAnim > 1.0f) openAnim = 1.0f;
            float oc = openAnim * openAnim * (3.0f - 2.0f * openAnim); /* smoothstep */

            if (state == 1) {
                if (popAnim < 1.0f) popAnim += 0.12f;
                if (popAnim > 1.0f) popAnim = 1.0f;
            }
            float pc = popAnim * popAnim * (3.0f - 2.0f * popAnim);

            int slideH = (int)((1.0f - oc) * 90);
            int slideF = (int)((1.0f - oc) * 60);

            /* ---- Draw HOME menu ---- */
            const Ui::Palette& P = Ui::pal();
            Ui::background(false);

            /* Top bar slides down, bottom bar (clock) slides up */
            Ui::pushOffset(0, -slideH);
            Ui::shadow(Ui::screenLeft() - 20, -30, Ui::screenWidth() + 40, 92, 22, 8.0f, P.shadow);
            Ui::roundRect(Ui::screenLeft() - 20, -30, Ui::screenWidth() + 40, 92, 22, P.barTop, P.barBottom);
            Ui::roundBorder(Ui::screenLeft() - 20, -30, Ui::screenWidth() + 40, 92, 22, 1.5f, P.barBorder);
            Ui::text(28, 17, "HOME Menu", 26, P.text);
            {
                const Ui::Hint close = { "B", "Close" };
                Ui::hint(626 - Ui::hintWidth(close), 22, close);
            }
            Ui::popOffset();

            Ui::pushOffset(0, slideF);
            {
                const Ui::Hint l[] = { { "A", "Confirm" } };
                const Ui::Hint r[] = { { "B", "Close" } };
                Ui::bottomBar(l, 1, r, 1);
            }
            Ui::popOffset();

            /* Wii Menu / Reset buttons */
            for (int i = 0; i < 2; i++) {
                float sc = hoverSc[i];
                float dw = BTN_W * sc, dh = BTN_H * sc;
                bool  sel = (hmSel == i);
                Ui::button(bcx[i] - dw * 0.5f, bcy - dh * 0.5f, dw, dh,
                           i == 0 ? "Wii Menu" : "Reset", (int)(22 * sc),
                           sel ? Ui::pulse() : 0.0f);
            }

            /* ---- Draw confirmation popup (state == 1) ---- */
            if (state == 1 && pc > 0.01f) {
                GRRLIB_Rectangle(Ui::screenLeft(), 0, Ui::screenWidth(), 480, Ui::alpha(P.dim, pc), 1);

                /* Pop-in: scale from 0.82 → 1.0 around screen centre */
                float psc = 0.82f + 0.18f * pc;
                float adw = DW * psc, adh = DH * psc;
                float adx = 320 - adw * 0.5f, ady = 240 - adh * 0.5f;
                Ui::card(adx, ady, adw, adh, 20 * psc, 0.0f);

                const char* line1 = (confirmFor == 0)
                    ? "Return to the Wii Menu?"
                    : "Reset the application?";
                Ui::textCentered(320, ady + 32 * psc, line1, (int)(20 * psc), P.text);
                Ui::textCentered(320, ady + 62 * psc, "(Anything not saved will be lost.)",
                                 (int)(14 * psc), P.textDim);

                /* Yes / No buttons */
                for (int i = 0; i < 2; i++) {
                    float bsc = popHoverSc[i] * psc;
                    float bdw = PBW * bsc, bdh = PBH * bsc;
                    /* Scale positions relative to screen centre */
                    float bx = 320 + (pbcx[i] - 320.0f) * psc - bdw * 0.5f;
                    float by = 240 + (pbcy    - 240.0f) * psc - bdh * 0.5f;
                    bool  bsel = (popSel == i);
                    Ui::button(bx, by, bdw, bdh, i == 0 ? "Yes" : "No", (int)(19 * psc),
                               bsel ? Ui::pulse() : 0.0f);
                }
            }

            /* Fade in from black while opening */
            if (oc < 1.0f)
                GRRLIB_Rectangle(Ui::screenLeft(), 0, Ui::screenWidth(), 480, (u32)((1.0f - oc) * 255.0f), 1);

            /* IR cursor — always on top */
            if (ir.valid && cursorPointerTex)
                GRRLIB_DrawImg((int)irX - 10, (int)irY - 4,
                               cursorPointerTex, orient.roll, 1, 1, 0xFFFFFFFF);

            GRRLIB_Render();
        }
}

void App::loop() {
    ir_t ir;
    int  selectedIndex = 0;
    bool irMode        = false;
    int  prevIrBtn     = -1;  /* last button index hovered via IR; -1 = none */
    const int MENU_COUNT = 3;
    float menuFocus[MENU_COUNT] = {};
    const std::string menuItems[] = {
        "Connect To Jellyfin",
        "Settings",
        "Exit"
    };

    /* Thin wrapper so existing call sites don't need to change. */
    auto showHomeOverlay = [&]() -> bool {
        return doShowHomeOverlay(font, btnTex, cursorPointerTex, musicEnabled);
    };

    /* ---- Helper: wait for the network (started at boot), offering a retry
     * when it failed.  Returns false when the user backs out. ---- */
    auto waitForNetwork = [&]() -> bool {
        const Ui::Palette& p = Ui::pal();
        for (;;) {
            jellyfinClient.startNetwork();
            /* B leaves: the attempt keeps going in the background (IOS can
             * take very long to answer) and the next try picks it up */
            u64 t0 = ticks_to_millisecs(gettime());
            while (jellyfinClient.networkBusy()) {
                Input::update();
                if (g_app_powerOff || g_app_reset) { running = false; return false; }
                if (Input::isBackPressed()) return false;
                bool slow = ticks_to_millisecs(gettime()) - t0 > 10000;
                Ui::background(false);
                Ui::spinner(ringTex, 320, 220);
                Ui::textCentered(320, 274, "Connecting to the network...", 18, p.textDim);
                if (slow)
                    Ui::textCentered(320, 300, "This is taking long: the Wii's network may need a restart.", 13, p.textDim);
                const Ui::Hint r[] = { { "B", "Back" } };
                Ui::bottomBar(nullptr, 0, r, 1);
                GRRLIB_Render();
            }
            if (jellyfinClient.takeNetworkResult()) return true;
            if (!showErrorScreen("No network connection",
                                 "Check the Wii's Internet settings, then try again.",
                                 jellyfinClient.lastError(), true, ir)) {
                if (g_app_powerOff || g_app_reset) running = false;
                return false;
            }
        }
    };

    /* ---- Helper: launch LibraryView for a saved profile ---- */
    auto runLibraryWithProfile = [&](const SavedProfile& p) {
        JellyfinAuth auth;
        auth.userId      = p.userId;
        auth.accessToken = p.accessToken;
        auth.serverName  = p.serverName;
        {
            /* server kind only: the log may be posted publicly */
            const std::string& u = p.serverUrl;
            size_t hs = u.find("://"); hs = hs == std::string::npos ? 0 : hs + 3;
            std::string host = u.substr(hs, u.find_first_of(":/", hs) - hs);
            bool ip = !host.empty() && host.find_first_not_of("0123456789.") == std::string::npos;
            Log::addPrivate(host);
            SYS_Report("[WiiFin] open profile: %s, %s host%s%s\n",
                       u.compare(0, 8, "https://") == 0 ? "https" : "http",
                       ip ? "IP" : "name", ip ? "" : " .", ip ? "" :
                       (host.rfind('.') == std::string::npos ? "(none)" : host.substr(host.rfind('.') + 1).c_str()));
        }
        if (!waitForNetwork()) return; /* DNS won't work without this */
        LibraryView lv(font, jpFont, cursorPointerTex, ringTex,
                       jellyfinClient, auth, p.serverUrl);
        lv.setUserName(p.username);
        for (;;) {
            while (true) {
                Input::update();
                Input::readIR(ir);
                if (g_app_powerOff || g_app_reset) { running = false; break; }
                if (Input::isHomePressed() && showHomeOverlay()) { running = false; break; }
                if (lv.update(ir)) break;
                lv.render(ir);
                GRRLIB_Render();
            }
            if (!running) return;
            if (lv.pendingPlayIsMusic) {
                lv.pendingPlayIsMusic = false;
                SoundFX::play(SoundFX::FX::Start);
                MusicPlayerView mpv(font, jellyfinClient, auth, p.serverUrl);
                mpv.setCursorTex(cursorPointerTex);
                mpv.setTracks(lv.pendingMusicTracks, lv.pendingMusicTrackIdx);
                bool wantsExit = mpv.run();
                if (g_app_powerOff || g_app_reset) { running = false; return; }
                SoundFX::play(SoundFX::FX::Back);
                if (wantsExit) { running = false; return; }
                lv.onPlaybackFinished("");
            } else if (!lv.pendingPlayUrl.empty()) {
                std::string lastItemId;
                bool wantsExit = runPlaySession(jellyfinClient, auth, p.serverUrl, lv,
                                                font, btnTex, cursorPointerTex, ringTex,
                                                lastItemId);
                if (wantsExit || g_app_powerOff || g_app_reset) { running = false; return; }
                saveSettings();   /* the zoom may have changed during playback */
                lv.onPlaybackFinished(lastItemId);
            } else {
                break; // user navigated back (B from libraries grid) — no play requested
            }
        }
    };

    /* ---- Helper: open ConnectView, on success add/update profile + library ---- */
    auto runConnect = [&]() {
        ConnectView cv(btnTex, cursorPointerTex, font, jellyfinClient);
        ConnectResult res = ConnectResult::None;
        while (res == ConnectResult::None && running) {
            Input::update();
            Input::readIR(ir);
            if (g_app_powerOff || g_app_reset) { running = false; break; }
            if (Input::isHomePressed() && showHomeOverlay()) { running = false; break; }
            res = cv.update(ir);
            cv.render(ir);
            GRRLIB_Render();
        }
        if (res == ConnectResult::Success) {
            SavedProfile p;
            p.serverUrl   = cv.serverUrl;
            p.username    = cv.username;
            p.serverName  = cv.auth.serverName;
            p.userId      = cv.auth.userId;
            p.accessToken = cv.auth.accessToken;
            /* Update existing profile if same userId (token refresh/re-auth), otherwise append.
             * Only deduplicate when both sides have a known userId — if userId is empty
             * (should never happen) always append to avoid silently overwriting profiles. */
            bool found = false;
            if (!p.userId.empty()) {
                for (auto& existing : profiles) {
                    if (!existing.userId.empty() && existing.userId == p.userId) {
                        existing = p; found = true; break;
                    }
                }
            }
            if (!found) profiles.push_back(p);
            saveSettings();
            runLibraryWithProfile(p);
        }
    };

    /* ---- Helper: profile picker loop ---- */
    auto runProfilePicker = [&]() {
        while (running) {
            if (profiles.empty()) {
                runConnect();
                return;
            }
            ProfileView pv(font, cursorPointerTex, profiles);
            ProfileResult res = ProfileResult::None;
            while (res == ProfileResult::None && running) {
                Input::update();
                Input::readIR(ir);
                if (g_app_powerOff || g_app_reset) { running = false; return; }
                if (Input::isHomePressed() && showHomeOverlay()) { running = false; return; }
                res = pv.update(ir);
                pv.render(ir);
                GRRLIB_Render();
            }
            if (!running || res == ProfileResult::Back) return;
            if (res == ProfileResult::DeleteOne) {
                int idx = pv.selectedIdx;
                if (idx >= 0 && idx < (int)profiles.size()) {
                    profiles.erase(profiles.begin() + idx);
                    saveSettings();
                }
                continue;
            }
            if (res == ProfileResult::AddNew) { runConnect(); continue; }
            if (res == ProfileResult::Selected) {
                runLibraryWithProfile(profiles[pv.selectedIdx]);
                continue; /* re-show picker on return */
            }
        }
    };

    while (running) {
        Input::update();   // calls WPAD_ScanPads() internally
        Input::readIR(ir);

        // --- Input: D-pad navigation ---
        if (g_app_reset) running = false;
        else if (Input::isHomePressed() && showHomeOverlay()) running = false;
        if (g_app_powerOff) running = false;
        if (ir.valid) irMode = true;
        if (Input::isUpPressed())   { selectedIndex = (selectedIndex - 1 + MENU_COUNT) % MENU_COUNT; irMode = false; }
        if (Input::isDownPressed()) { selectedIndex = (selectedIndex + 1) % MENU_COUNT; irMode = false; }

        // --- IR hover updates selection (no action yet) ---
        bool irHovered = false;
        if (ir.valid) {
            for (int i = 0; i < MENU_COUNT; ++i) {
                int by = BY_START + i * B_SPACING;
                if (ir.x >= BX && ir.x <= BX + BW &&
                    ir.y >= by && ir.y <= by + BH) {
                    selectedIndex = i;
                    irHovered = true;
                    irMode = true;
                    break;
                }
            }
        }

        // --- Select sound: fire once when IR cursor first enters a button ---
        {
            int curIrBtn = irHovered ? selectedIndex : -1;
            if (curIrBtn >= 0 && curIrBtn != prevIrBtn)
                SoundFX::play(SoundFX::FX::Select);
            prevIrBtn = curIrBtn;
        }

        // --- Single action dispatch on A press ---
        if (Input::isAJustPressed() && (irHovered || (!ir.valid && !irMode))) {
            SoundFX::play(SoundFX::FX::Start);
            switch (selectedIndex) {
                case 0: {
                    runProfilePicker();
                    break;
                }
                case 1: {
                    // Launch Settings view
                    SettingsView sv(btnTex, font, jellyfinClient, musicEnabled);
                    while (true) {
                        Input::update();
                        Input::readIR(ir);
                        if (g_app_powerOff || g_app_reset) { running = false; break; }
                        if (Input::isHomePressed() && showHomeOverlay()) { running = false; break; }
                        if (sv.update(ir)) break;
                        sv.render(ir);
                        if (ir.valid && cursorPointerTex) {
                            orient_t orient; WPAD_Orientation(WPAD_CHAN_0, &orient);
                            GRRLIB_DrawImg((int)ir.x - 20, (int)ir.y - 4, cursorPointerTex, orient.roll, 1, 1, 0xFFFFFFFF);
                        }
                        GRRLIB_Render();
                    }
                    saveSettings();
                    MusicBGM::setEnabled(musicEnabled);
                    break;
                }
                case 2: running = false; break;
            }
        }

        // ===================== RENDER =====================
        Ui::background(false);

        if (logoTex) {
            float ls = 0.62f;
            int lw = (int)(logoTex->w * ls);
            GRRLIB_DrawImg((640 - lw) / 2, 40, logoTex, 0, ls, ls, 0xFFFFFFFF);
        }

        for (int i = 0; i < MENU_COUNT; ++i) {
            int by = BY_START + i * B_SPACING;
            bool hover = ir.valid && ir.x >= BX && ir.x <= BX + BW && ir.y >= by && ir.y <= by + BH;
            bool focus = ir.valid ? hover : (i == selectedIndex);
            menuFocus[i] = Ui::approach(menuFocus[i], focus ? 1.0f : 0.0f);
            float grow = 8.0f * menuFocus[i];
            Ui::button(BX - grow, by - grow * 0.25f, BW + grow * 2, BH + grow * 0.5f,
                       menuItems[i].c_str(), 22, menuFocus[i]);
        }

        {
            static const Ui::Hint left[]  = { { "A", "Select" } };
            static const Ui::Hint right[] = { { "HOME", "Menu" } };
            Ui::bottomBar(left, 1, right, 1);
        }

        // --- IR Cursor: rendered last ---
        if (ir.valid && cursorPointerTex) {
            orient_t orient;
            WPAD_Orientation(WPAD_CHAN_0, &orient);
            GRRLIB_DrawImg(
                (int)ir.x - 20,
                (int)ir.y - 4,
                cursorPointerTex, orient.roll, 1, 1, 0xFFFFFFFF);
        }

        GRRLIB_Render();
    }

    // --- Cleanup ---
    saveSettings();
    Log::close();        // its writer thread must not touch the card past here
    MusicBGM::pause();   // stop audio thread/ASND callbacks before tearing down GX
    // Blank the VI output before GX teardown to avoid purple/pink artefact frame
    VIDEO_SetBlack(true);
    VIDEO_Flush();
    VIDEO_WaitVSync();
    GRRLIB_FreeTexture(logoTex);
    GRRLIB_FreeTexture(btnTex);
    GRRLIB_FreeTexture(cursorPointerTex);
    GRRLIB_FreeTexture(ringTex);
    Text::clearCache();
    GRRLIB_FreeTTF(font);
    GRRLIB_FreeTTF(jpFont);
    GRRLIB_Exit();
    WPAD_Shutdown();
    if (g_app_powerOff)        SYS_ResetSystem(SYS_POWEROFF,    0, 0);
    else if (s_restartApp) exit(0);  // HBC catches exit(0) and reloads the app
    else {
        /* Restore the NAND-loader stub bytes at their installed VA (0x80804000)
         * before returning to the System Menu.  The stub was zeroed at startup
         * by the DOL BSS initialiser (BSS spans 0x806f7e0c–0x80ae64dc, which
         * includes the stub zone) and may have been overwritten again by
         * MPlayer's stream-cache buffer during playback.  Without this restore
         * SYS_RETURNTOMENU results in a DSI exception in the return trampoline. */
        {
            extern char __stub_zone_start[], __stub_zone_end[];
            size_t sz = (size_t)(__stub_zone_end - __stub_zone_start);
            if (sz > 0) {
                memcpy((void*)0x80804000u, __stub_zone_start, sz);
                DCFlushRange((void*)0x80804000u, sz);
            }
        }
        SYS_ResetSystem(SYS_RETURNTOMENU, 0, 0);
    }
}

void App::run() {
    loop();
}

void App::loadSettings() {
    if (settingsPath.empty()) return;
    FILE* f = fopen(settingsPath.c_str(), "r");
    if (!f) return;

    profiles.clear();
    int profileCount = 0;
    SavedProfile legacyProfile;
    bool hasLegacy = false;

    char line[512];
    while (fgets(line, sizeof(line), f)) {
        char* nl = strchr(line, '\n'); if (nl) *nl = '\0';
        char* eq = strchr(line, '=');
        if (!eq) continue;
        *eq = '\0';
        const char* key = line;
        const char* val = eq + 1;

        if (strcmp(key, "ssl_verify") == 0) {
            jellyfinClient.sslVerify = atoi(val) != 0;
        } else if (strcmp(key, "music_enabled") == 0) {
            musicEnabled = atoi(val) != 0;
        } else if (strcmp(key, "ui_theme") == 0) {
            {
                int t = atoi(val);
                Ui::setTheme(t >= 0 && t < Ui::THEME_COUNT ? (Ui::Theme)t : Ui::Theme::Dark);
                /* settings saved before home_layout existed: Flix meant rows
                 * (home_layout, written after ui_theme, overrides this) */
                if (Ui::theme() == Ui::Theme::Flix) Ui::setHomeLayout(Ui::HomeLayout::Rows);
            }
        } else if (strcmp(key, "smooth_motion") == 0) {
            g_wiifin_smooth_motion = atoi(val) != 0;
        } else if (strcmp(key, "video_zoom") == 0) {
            VideoSurface::setZoom(atoi(val) == 1 ? VideoSurface::Zoom::Fill : VideoSurface::Zoom::Fit);
        } else if (strcmp(key, "library_view") == 0) {
            int v = atoi(val);
            if (v >= 0 && v < Ui::LIBRARY_STYLE_COUNT) Ui::setLibraryStyle((Ui::LibraryStyle)v);
        } else if (strcmp(key, "home_layout") == 0) {
            Ui::setHomeLayout(atoi(val) == 1 ? Ui::HomeLayout::Rows : Ui::HomeLayout::Grid);
        } else if (strcmp(key, "safe_area") == 0) {
            int l = 0, t = 0, r = 0, b = 0;
            if (sscanf(val, "%d,%d,%d,%d", &l, &t, &r, &b) == 4) Ui::setSafeArea(l, t, r, b);
        } else if (strcmp(key, "video_quality") == 0) {
            int q = atoi(val);
            if (q >= 0 && q < JellyfinClient::VIDEO_QUALITY_COUNT) jellyfinClient.videoQuality = q;
        } else if (strcmp(key, "profile_count") == 0) {
            profileCount = atoi(val);
            if (profileCount > 0 && profileCount <= 32) profiles.resize((size_t)profileCount);
        } else if (strncmp(key, "profile.", 8) == 0) {
            /* profile.N.field=value */
            int idx = atoi(key + 8);
            if (idx < 0 || idx >= (int)profiles.size()) continue;
            const char* dot = strchr(key + 8, '.');
            if (!dot) continue;
            const char* field = dot + 1;
            if (strcmp(field, "server_url")   == 0) {
                profiles[idx].serverUrl = val;
                while (profiles[idx].serverUrl.size() > 1 && profiles[idx].serverUrl.back() == '/')
                    profiles[idx].serverUrl.pop_back();
            }
            if (strcmp(field, "username")      == 0) profiles[idx].username    = val;
            if (strcmp(field, "server_name")   == 0) profiles[idx].serverName  = val;
            if (strcmp(field, "user_id")       == 0) profiles[idx].userId      = val;
            if (strcmp(field, "access_token")  == 0) profiles[idx].accessToken = val;
        } else {
            /* Legacy single-profile keys — migrate on first load */
            if (strcmp(key, "server_url")   == 0) {
                legacyProfile.serverUrl = val;
                while (legacyProfile.serverUrl.size() > 1 && legacyProfile.serverUrl.back() == '/')
                    legacyProfile.serverUrl.pop_back();
                hasLegacy = true;
            }
            if (strcmp(key, "username")      == 0) { legacyProfile.username    = val; }
            if (strcmp(key, "user_id")       == 0) { legacyProfile.userId      = val; }
            if (strcmp(key, "access_token")  == 0) { legacyProfile.accessToken = val; }
            if (strcmp(key, "server_name")   == 0) { legacyProfile.serverName  = val; }
        }
    }
    fclose(f);

    /* Migrate legacy single-profile format (no profile_count key present) */
    if (hasLegacy && profiles.empty() && !legacyProfile.accessToken.empty())
        profiles.push_back(legacyProfile);
}

static std::string sanitizeConfigValue(const std::string& s) {
    std::string out;
    out.reserve(s.size());
    for (char c : s)
        if (c != '\n' && c != '\r') out += c;
    return out;
}

void App::saveSettings() {
    if (settingsPath.empty()) return;
    FILE* f = fopen(settingsPath.c_str(), "w");
    if (!f) return;
    fprintf(f, "ssl_verify=%d\n",      jellyfinClient.sslVerify ? 1 : 0);
    fprintf(f, "music_enabled=%d\n",    musicEnabled ? 1 : 0);
    fprintf(f, "video_quality=%d\n",    jellyfinClient.videoQuality);
    fprintf(f, "ui_theme=%d\n",         (int)Ui::theme());
    fprintf(f, "home_layout=%d\n",      (int)Ui::homeLayout());
    fprintf(f, "library_view=%d\n",     (int)Ui::libraryStyle());
    fprintf(f, "smooth_motion=%d\n",    g_wiifin_smooth_motion ? 1 : 0);
    fprintf(f, "video_zoom=%d\n",       (int)VideoSurface::zoom());
    {
        int l, t, r, b;
        Ui::safeArea(l, t, r, b);
        fprintf(f, "safe_area=%d,%d,%d,%d\n", l, t, r, b);
    }
    fprintf(f, "profile_count=%d\n",   (int)profiles.size());
    for (int i = 0; i < (int)profiles.size(); i++) {
        const SavedProfile& p = profiles[i];
        fprintf(f, "profile.%d.server_url=%s\n",  i, sanitizeConfigValue(p.serverUrl).c_str());
        fprintf(f, "profile.%d.username=%s\n",     i, sanitizeConfigValue(p.username).c_str());
        fprintf(f, "profile.%d.server_name=%s\n",  i, sanitizeConfigValue(p.serverName).c_str());
        fprintf(f, "profile.%d.user_id=%s\n",      i, sanitizeConfigValue(p.userId).c_str());
        fprintf(f, "profile.%d.access_token=%s\n", i, sanitizeConfigValue(p.accessToken).c_str());
    }
    fflush(f);
    fclose(f);
}
