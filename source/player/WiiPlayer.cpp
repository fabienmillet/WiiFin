/*
 * WiiPlayer.cpp — in-process MPlayer CE integration for WiiFin.
 *
 * Build requirements:
 *   - mplayer-ce must be built as a static library (libmplayer.a) using the
 *     patches in libs/mplayer-ce/wii_player_patch.diff.
 *   - The library path must be passed to the linker (see Makefile).
 *
 * How it works:
 *   runMplayer() saves a setjmp context, then calls mplayer_main().  The
 *   patched mplayer.c replaces exit() with longjmp(g_mplayer_jmp, rc) so
 *   that when MPlayer finishes (EOF / quit / error), control returns here
 *   instead of killing the whole process.
 *
 *   Video sessions run on a dedicated thread (wii_player_start) so WiiFin's
 *   main loop keeps drawing the UI; frames come through vo_wiifin.c.
 *   Audio sessions (wii_player_play_audio) block the caller and drive the
 *   music UI from a background thread.
 */

#include "WiiPlayer.h"
#include "../core/ExitZone.h"

#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#include <malloc.h>
#include <string>
#include <ogc/system.h>    /* SYS_STDIO_Report */
#include <ogc/lwp.h>       /* LWP_CreateThread / LWP_JoinThread */
#include <wiiuse/wpad.h>   /* WPAD_ScanPads, WPAD_Data */
#include <unistd.h>        /* usleep */
#include <network.h>       /* net_close — socket cleanup after MPlayer longjmp */

/* -----------------------------------------------------------------------
 * Symbols exported by the patched mplayer.c (in libmplayer.a)
 * ----------------------------------------------------------------------- */
extern "C" {
    jmp_buf g_mplayer_jmp;                       /* we define this */

    int mplayer_main(int argc, char** argv);     /* renamed main() */
    void register_mpegts_demuxer(void);          /* register MPEG-TS lavf demuxer */
    extern volatile int async_quit_request;      /* set 1 to stop  */

    /* MPlayer input command queue (protected by a mutex in MPlayer CE) */
    struct mp_cmd_t;
    struct mp_cmd_t* mp_input_parse_cmd(char* str);
    void             mp_input_queue_cmd(struct mp_cmd_t* cmd);
    /* Key FIFO / command queue — flushed before each mplayer_main call */
    int  mplayer_get_key(int fd);
    struct mp_cmd_t* mp_input_get_cmd(int time, int paused, int peek_only);
    void             mp_cmd_free(struct mp_cmd_t* cmd);

    /* Declared in getch2-gekko.c; when set, MPlayer does not scan the
     * Wiimote itself (WiiFin's main loop owns input during video). */
    extern volatile int g_wiifin_gx_active;
}

/* -----------------------------------------------------------------------
 * WiiFin / MPlayer shared state — referenced as extern by mplayer.c
 * ----------------------------------------------------------------------- */
extern "C" {
    volatile float g_mplayer_time_pos  = 0.0f;
    volatile float g_mplayer_duration  = 0.0f;
    volatile int   g_mplayer_paused    = 0;

    /* Control requests, consumed once per main-loop iteration by mplayer.c */
    volatile float g_wiifin_seek_secs  = -1.0f;
    volatile int   g_wiifin_vol_delta  = 0;

    volatile float g_wiifin_ss_secs = 0.0f;
}

volatile int   g_player_stop_reason       = PLAYER_STOP_EOF;
volatile float g_wiifin_known_duration    = 0.0f;
volatile int   g_wiifin_loading_active    = 0;
float          g_wiifin_loading_start_pos = -1.0f;

/* Wii power-off / reset flags — defined in App.cpp */
extern volatile bool g_app_powerOff;
extern volatile bool g_app_reset;

/* -----------------------------------------------------------------------
 * Shared helpers
 * ----------------------------------------------------------------------- */
static void resetControlState()
{
    async_quit_request   = 0;
    g_player_stop_reason = PLAYER_STOP_EOF;
    g_mplayer_time_pos   = 0.0f;
    g_mplayer_duration   = 0.0f;
    g_mplayer_paused     = 0;
    g_wiifin_seek_secs   = -1.0f;
    g_wiifin_vol_delta   = 0;
}

static void logUrl(const char* tag, const char* url)
{
    std::string safeUrl(url);
    size_t kp = safeUrl.find("ApiKey=");
    if (kp != std::string::npos) {
        size_t ve = safeUrl.find('&', kp + 7);
        safeUrl.replace(kp + 7,
            (ve == std::string::npos ? safeUrl.size() : ve) - kp - 7, "***");
    }
    SYS_Report("[WiiPlayer] %s: %s\n", tag, safeUrl.c_str());
}

/* Heap use around each MPlayer run: it is left with longjmp, so whatever it
 * does not free on the way out stays allocated. */
static void logMemory(const char* when)
{
    struct mallinfo mi = mallinfo();
    /* (uordblks is meaningless here: the heap jumps from MEM1 to MEM2) */
    SYS_Report("[mem] %s: free in heap %u KB, MEM1 left %u KB, MEM2 left %u KB\n",
               when, (unsigned)(mi.fordblks / 1024),
               (unsigned)(SYS_GetArena1Size() / 1024), (unsigned)(SYS_GetArena2Size() / 1024));
}

/* Run mplayer_main until it longjmps back.  Returns the setjmp code. */
static int runMplayer(int argc, const char** argv)
{
    int rc = setjmp(g_mplayer_jmp);
    if (rc == 0) {
        /* Drop stale key events (e.g. the A press that launched playback);
         * otherwise mp_input_check_interrupt() kills the stream early. */
        while (mplayer_get_key(0) != -3 /* MP_INPUT_NOTHING */) { /* drain */ }
        /* Commands queued too late for the previous session (e.g. a pause
         * sent just before a restart) would otherwise run in this one. */
        for (int i = 0; i < 64 && !async_quit_request; ++i) {
            struct mp_cmd_t* cmd = mp_input_get_cmd(0, 1, 0);
            if (!cmd) break;
            mp_cmd_free(cmd);
        }
        register_mpegts_demuxer();
        mplayer_main(argc, const_cast<char**>(argv));
    }
    return rc;
}

/* MPlayer's stream and cache threads cannot unwind through the setjmp
 * boundary, leaving their IOS socket handles open; the cache thread keeps
 * calling net_read() on them and floods the IOS network queue, which makes
 * WiiFin's next HTTPS request fail.  WiiFin closes its own sockets inside
 * each request, so anything still open here belongs to MPlayer.  IOS allows
 * at most 24 sockets; net_close() on an invalid FD just returns EBADF. */
static void closeLeakedSockets()
{
    wii_player_abort_io();
    usleep(200000);   /* let IOS process the closures */
}

void wii_player_abort_io(void)
{
    for (s32 fd = 0; fd < 24; ++fd) net_close(fd);
}

/* -----------------------------------------------------------------------
 * Video session thread
 * ----------------------------------------------------------------------- */
static const u32     PLAY_STACK_SIZE = 512 * 1024;
static u8*           s_play_stack    = nullptr;
static lwp_t         s_play_thread   = LWP_THREAD_NULL;
static volatile int  s_play_running  = 0;
static std::string   s_play_url;

static void* playThreadFunc(void*)
{
    const char* argv[64];
    int argc = 0;
    auto addArg = [&](const char* a) { argv[argc++] = a; };
    addArg("mplayer");
    addArg("-noconsolecontrols");
    addArg("-v");
    addArg("-msglevel"); addArg("demux=4");
    addArg("-fs");
    addArg("-demuxer"); addArg("lavf"); /* MPlayer's native TS demuxer fails to find
                                        * the video PID in Jellyfin live-transcoded
                                        * streams; FFmpeg's lavf parses PAT/PMT. */
    addArg("-lavfdopts"); addArg("format=mpegts:probesize=32768:analyzeduration=1");
    addArg("-vo"); addArg("gx");        /* vo_wiifin.c */
    addArg("-ao"); addArg("gekko");
    /* 8 MB cache, playback starts (and resumes after an underrun) once 8%
     * ≈ 650 KB ≈ 3 s at 1.6 Mb/s is buffered: enough to ride out Wi-Fi hiccups
     * and a server transcoding barely faster than real time. */
    addArg("-cache"); addArg("8192");
    addArg("-cache-min"); addArg("8");
    addArg("-cache-seek-min"); addArg("5");
    addArg("-autosync"); addArg("10");
    addArg("-mc"); addArg("15");         /* large initial A/V gap on mid-stream resumes */
    addArg("-delay"); addArg("0.3");
    addArg("-lavdopts"); addArg("fast:skiploopfilter=all:skipidct=nonref:skipframe=nonref");
    addArg("-hardframedrop");
    /* Discard the 3 s RESUME_PAD back-off at demuxer level so output starts
     * at the exact target position. */
    char ssBuf[16];
    if (g_wiifin_ss_secs > 0.0f) {
        snprintf(ssBuf, sizeof(ssBuf), "%.1f", (double)g_wiifin_ss_secs);
        addArg("-ss"); addArg(ssBuf);
    }
    addArg(s_play_url.c_str());
    argv[argc] = nullptr;

    SYS_STDIO_Report(true);
    logUrl("play", s_play_url.c_str());
    logMemory("before play");

    g_wiifin_gx_active = 1;
    int rc = runMplayer(argc, argv);
    g_wiifin_gx_active = 0;

    closeLeakedSockets();

    /* Premature EOF: nothing was played (time < 2 s, duration still 0).
     * Usually the Jellyfin transcoder session expired while the stream was
     * stuck in 504 retries; report an error so the caller re-acquires a
     * fresh session and retries. */
    if (g_player_stop_reason == PLAYER_STOP_EOF &&
        g_mplayer_time_pos < 2.0f && g_mplayer_duration == 0.0f)
        g_player_stop_reason = PLAYER_STOP_ERROR;

    SYS_Report("[WiiPlayer] mplayer exited rc=%d reason=%d time=%.1f\n",
               rc, (int)g_player_stop_reason, (double)g_mplayer_time_pos);
    logMemory("after play");
    s_play_running = 0;
    return nullptr;
}

int wii_player_start(const char* url)
{
    if (s_play_thread != LWP_THREAD_NULL) return 0;
    if (!s_play_stack) {
        s_play_stack = (u8*)memalign(32, PLAY_STACK_SIZE);
        if (!s_play_stack) return 0;
    }

    resetControlState();
    g_wiifin_loading_active    = 1;
    g_wiifin_loading_start_pos = -1.0f;
    s_play_url     = url;
    s_play_running = 1;

    /* Below the main thread (64) so the UI stays responsive, below the
     * cache thread (70) so stream prefetch is never starved. */
    if (LWP_CreateThread(&s_play_thread, playThreadFunc, nullptr,
                         s_play_stack, PLAY_STACK_SIZE, 60) < 0) {
        s_play_thread  = LWP_THREAD_NULL;
        s_play_running = 0;
        return 0;
    }
    return 1;
}

int wii_player_is_running(void)
{
    return s_play_running;
}

int wii_player_wait(void)
{
    if (s_play_thread != LWP_THREAD_NULL) {
        LWP_JoinThread(s_play_thread, nullptr);
        s_play_thread = LWP_THREAD_NULL;
    }
    g_wiifin_loading_active = 0;
    return (int)g_player_stop_reason;
}

void wii_player_request_stop(int reason)
{
    g_player_stop_reason = reason;
    async_quit_request   = 1;
}

/* -----------------------------------------------------------------------
 * Controls
 * ----------------------------------------------------------------------- */
void wii_player_pause_toggle(void)
{
    mp_input_queue_cmd(mp_input_parse_cmd(const_cast<char*>("pause")));
}

void wii_player_seek_abs(float seconds)
{
    if (seconds < 0.0f) seconds = 0.0f;
    /* Show the loading indicator until playback resumes after the seek. */
    g_wiifin_loading_start_pos = -1.0f;
    g_wiifin_loading_active    = 1;
    g_wiifin_seek_secs         = seconds;
}

void wii_player_seek_rel(float delta)
{
    wii_player_seek_abs(g_mplayer_time_pos + delta);
}

void wii_player_vol_up(void)   { g_wiifin_vol_delta = 1; }
void wii_player_vol_down(void) { g_wiifin_vol_delta = -1; }

/* -----------------------------------------------------------------------
 * Music (audio-only) path
 * ----------------------------------------------------------------------- */
static void (*s_music_tick_cb)(int paused, uint32_t btnsDown, uint32_t btnsHeld) = nullptr;
static void (*s_audio_render_cb)() = nullptr;

static lwp_t        s_bg_thread = LWP_THREAD_NULL;
static volatile int s_bg_stop   = 0;
static uint8_t      s_bg_stack[48 * 1024] DEAD_AT_EXIT __attribute__((aligned(32)));

/* With -vo null MPlayer never scans the Wiimote, so this thread does:
 * it feeds MusicPlayerView's tick callback and renders its UI at 60 Hz. */
static void* audioThreadFunc(void*)
{
    WPAD_ScanPads();
    WPADData* wd0 = WPAD_Data(WPAD_CHAN_0);
    u32 prevHeld = wd0 ? wd0->btns_h : 0;
    while (!s_bg_stop) {
        if (g_app_powerOff || g_app_reset) {
            g_player_stop_reason = PLAYER_STOP_EOF;
            async_quit_request   = 1;
        }

        WPAD_ScanPads();
        WPADData* wd = WPAD_Data(WPAD_CHAN_0);
        u32 held = wd ? wd->btns_h : 0;
        u32 down = held & ~prevHeld;
        prevHeld = held;

        if (s_music_tick_cb)
            s_music_tick_cb(g_mplayer_paused, down, held);

        /* GRRLIB_Render() inside the callback waits for vsync (60 Hz pacing) */
        if (s_audio_render_cb) s_audio_render_cb();
        else                   usleep(16000);
    }
    return nullptr;
}

int wii_player_play_audio(const char* url)
{
    const char* argv[] = {
        "mplayer",
        "-noconsolecontrols",
        "-v",
        "-fs",
        "-vo", "null",              /* audio-only: GRRLIB stays active */
        "-ao", "gekko",
        "-cache", "2048",
        "-cache-min", "5",
        "-autosync", "30",
        "-demuxer", "audio",        /* lavf can't probe non-seekable HTTP MP3 */
        url,
        nullptr
    };
    const int argc = (int)(sizeof(argv) / sizeof(argv[0])) - 1;

    resetControlState();

    s_bg_stop = 0;
    LWP_CreateThread(&s_bg_thread, audioThreadFunc, nullptr,
                     s_bg_stack, sizeof(s_bg_stack), 40);

    SYS_STDIO_Report(true);
    logUrl("play_audio", url);

    int rc = runMplayer(argc, argv);

    s_bg_stop = 1;
    if (s_bg_thread != LWP_THREAD_NULL) {
        LWP_JoinThread(s_bg_thread, nullptr);
        s_bg_thread = LWP_THREAD_NULL;
    }
    s_music_tick_cb   = nullptr;
    s_audio_render_cb = nullptr;

    SYS_Report("[WiiPlayer] play_audio exited rc=%d reason=%d\n",
               rc, (int)g_player_stop_reason);
    return (int)g_player_stop_reason;
}

void wii_player_stop(void)
{
    async_quit_request = 1;
}

void wii_player_set_music_tick(
    void (*cb)(int paused, uint32_t btnsDown, uint32_t btnsHeld))
{
    s_music_tick_cb = cb;
}

void wii_player_set_audio_render_cb(void (*cb)())
{
    s_audio_render_cb = cb;
}
