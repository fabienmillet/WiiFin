/*
 * WiiPlayer.cpp — in-process MPlayer CE integration for WiiFin.
 *
 * Build requirements:
 *   - mplayer-ce must be built as a static library (libmplayer.a) using the
 *     patches in libs/mplayer-ce/wii_player_patch.diff.
 *   - The library path must be passed to the linker (see Makefile).
 *
 * How it works:
 *   wii_player_play() saves a setjmp context, then calls mplayer_main().
 *   The patched mplayer.c replaces exit() with longjmp(g_mplayer_jmp, rc)
 *   so that when mplayer finishes (EOF / quit / error), control returns here
 *   instead of calling posix exit() and killing the whole process.
 *
 *   A background LWP thread polls Wii-remote input every ~33 ms and ticks
 *   the PlayerOverlay for the in-player control HUD.
 */

#include "WiiPlayer.h"
#include "PlayerOverlay.h"
#include "../core/Utils.h"

#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#include <math.h>
#include <ogc/system.h>    /* SYS_STDIO_Report */
#include <ogc/video.h>     /* VIDEO_GetPreferredMode */
#include <ogc/gx.h>        /* GX_SetDrawDoneCallback */
#include <ogc/lwp.h>       /* LWP_CreateThread / LWP_JoinThread */
#include <wiiuse/wpad.h>   /* WPAD_ScanPads, WPAD_ButtonsDown, … */
#include <unistd.h>        /* usleep */
#include <network.h>       /* net_close — socket cleanup after MPlayer longjmp */

/* gx_supp globals used by vo_gx / mpgxInit */
extern "C" {
    extern GXRModeObj *vmode;
    extern int screenwidth;
    extern int screenheight;
    /* GX overlay callback registered by vo_gx preinit; cleared on player exit */
    extern void (*g_wiifin_gx_overlay_cb)(void);

    /* GX frame-pipeline functions from gx_supp.c — used to keep the
     * GX overlay (cursor, buttons) live while MPlayer is paused.
     * During pause_loop, MPlayer's flip_page is not called so these
     * are never called from that side; calling them from bgThread is safe. */
    void mpgxWaitDrawDone(void);
    void mpgxRunOverlay(void);
    void mpgxPushFrame(void);
}

/* -----------------------------------------------------------------------
 * Symbols exported by the patched mplayer.c / mixer.c (in libmplayer.a)
 * ----------------------------------------------------------------------- */
extern "C" {
    jmp_buf g_mplayer_jmp;                       /* we define this */

    int mplayer_main(int argc, char** argv);     /* renamed main() */
    void register_mpegts_demuxer(void);          /* register MPEG-TS lavf demuxer */
    extern volatile int async_quit_request;      /* set 1 to stop  */

    /* MPlayer input command queue (thread-safe, protected by mutex) */
    struct mp_cmd_t;
    struct mp_cmd_t* mp_input_parse_cmd(char* str);
    void             mp_input_queue_cmd(struct mp_cmd_t* cmd);
    /* Key FIFO — flush stale input before each mplayer_main call */
    int  mplayer_get_key(int fd);
}

/* -----------------------------------------------------------------------
 * WiiFin / MPlayer shared state — defined here, referenced as extern in
 * the patched mplayer.c and gx_supp.c (same pattern as g_mplayer_jmp).
 * ----------------------------------------------------------------------- */
extern "C" {
    /* Position / pause state: updated by the patched mplayer.c main loop */
    volatile float g_mplayer_time_pos  = 0.0f;
    volatile float g_mplayer_duration  = 0.0f;
    volatile int   g_mplayer_paused    = 0;

    /* Control requests: written by background thread, consumed by mplayer.c */
    volatile float g_wiifin_seek_secs  = -1.0f;
    volatile int   g_wiifin_vol_delta  = 0;

    /* Texture overlay callback: defined here, called from gx_supp.c flip_page */
    void (*g_wiifin_overlay_cb)(uint8_t*, int, int) = nullptr;

    /* Set to 1 by vo_gx.c ov_tick() when button A is handled by the GX
     * overlay so that PlayerOverlay::tick() skips its own A-press logic. */
    volatile int g_wiifin_gx_btn_consumed = 0;

    /* Set to 1 by PlayerOverlay::tick() while the HOME menu is open.
     * vo_gx.c ov_tick() reads this to suppress the GX overlay so the
     * HOME menu's A-press is not consumed by the GX overlay. */
    volatile int g_wiifin_home_menu_active = 0;

    /* Cursor PNG texture (PointerP1) decoded by GRRLIB and registered via
     * wii_player_set_cursor_tex() before playback starts.  vo_gx.c reads
     * g_wiifin_cursor_tex_valid to decide whether to use this texture or
     * fall back to the built-in crosshair cursor.                          */
    GXTexObj     g_wiifin_cursor_tex_obj;
    volatile int g_wiifin_cursor_tex_valid = 0;

    /* Audio / sub track lists — populated by App before wii_player_play().
     * The GX overlay (vo_gx.c) reads these to draw the track pickers and
     * writes g_wiifin_selected_audio / g_wiifin_selected_sub on confirm.   */
    WiiTrackInfo  g_wiifin_audio_tracks[WIIFIN_TRACK_MAX];
    int           g_wiifin_audio_count    = 0;
    int           g_wiifin_current_audio  = 0;
    volatile int  g_wiifin_selected_audio = 0;

    WiiTrackInfo  g_wiifin_sub_tracks[WIIFIN_TRACK_MAX];
    int           g_wiifin_sub_count      = 0;
    int           g_wiifin_current_sub    = -1;
    volatile int  g_wiifin_selected_sub   = -1;

    /* Seconds to pass to MPlayer -ss (skip at demuxer level).
     * Set by App before wii_player_play() when a RESUME_PAD was applied. */
    volatile float g_wiifin_ss_secs = 0.0f;
}

/* -----------------------------------------------------------------------
 * Stop-reason variable
 * ----------------------------------------------------------------------- */
volatile int g_player_stop_reason = PLAYER_STOP_EOF;

/* Set to 1 by PlayerOverlay the instant a stop is requested (before
 * wii_player_stop / async_quit_request).  vo_gx.c ov_gx_draw() reads
 * this to black-fill the screen immediately so there is no video blink. */
volatile int g_wiifin_stopping = 0;

/* Duration (seconds) known from Jellyfin's RunTimeTicks before playback.
 * App sets this; patched mplayer.c uses it as fallback when the demuxer
 * cannot determine the stream length (live-transcoded MPEG-TS). */
volatile float g_wiifin_known_duration = 0.0f;

/* 1 from the start of mplayer_main() until the patched mplayer.c hook
 * clears it once playback has progressed > 1 s from the first decoded
 * frame.  PlayerOverlay::onFrame shows a loading screen while set.
 * Stays 0 for track-switch restarts (g_wiifin_track_switch was 1). */
volatile int g_wiifin_loading_active = 0;

/* Set to 1 by App.cpp before calling wii_player_play() for an
 * audio/subtitle track-switch restart; consumed at start of play. */
volatile int g_wiifin_track_switch = 0;

/* Declared in getch2-gekko.c; when set, the getch2 WPAD scan is skipped so
 * ov_tick owns the Wiimote input and cannot have its pause_loop disturbed by
 * spurious 'sub_alignment' commands from the A button's 'a' key binding. */
extern "C" volatile int g_wiifin_gx_active;

/* Tracks the PTS of the first decoded frame so the loading indicator
 * is cleared after 1 s of actual playback progress (not absolute PTS). */
float g_wiifin_loading_start_pos = -1.0f;

/* XFB spinner: draws directly into the raw VIDEO framebuffer during
 * the pre-GX phase.  Set to 1 by wii_player_play(), cleared by
 * mpgxInit() when GX takes over VIDEO. */
volatile int g_wiifin_xfb_spinner_active = 0;
void* g_wiifin_xfb_spinner_ptr = nullptr;

/* bgThread loading watchdog — file scope so wii_player_play() can reset it
 * between sessions.  Otherwise the static counter survives and fires
 * prematurely on the second playback. */
static int s_loading_watchdog = 0;

/* GRRLIB loading spinner — App.cpp provides a render callback that draws
 * ring.png via GRRLIB_DrawImg, and a cleanup callback that does
 * GRRLIB_Exit().  bgThread calls the render callback every frame while
 * it is non-null.  mpgxInit() nulls render, waits, then calls cleanup
 * so GX is free for MPlayer. */
void (*g_wiifin_grrlib_render_cb)(void)  = nullptr;
void (*g_wiifin_grrlib_cleanup_cb)(void) = nullptr;

/* -----------------------------------------------------------------------
 * Background input/overlay thread
 * ----------------------------------------------------------------------- */
/* -----------------------------------------------------------------------
 * Music tick callback slot — set by MusicPlayerView before audio playback.
 * Called ~60 Hz from the background thread with pause state and button events.
 * ----------------------------------------------------------------------- */
static void (*s_music_tick_cb)(int paused, uint32_t btnsDown, uint32_t btnsHeld) = nullptr;
static void (*s_audio_render_cb)() = nullptr;

/* Build a YUYV word from two luma values with neutral chroma (fallback XFB use) */
static inline uint32_t yuyv(uint8_t y0, uint8_t y1)
{
    return ((uint32_t)y0 << 24) | (0x80u << 16) | ((uint32_t)y1 << 8) | 0x80u;
}

/* -----------------------------------------------------------------------
 * Background input/overlay thread
 * ----------------------------------------------------------------------- */
static PlayerOverlay* s_overlay     = nullptr;
static lwp_t          s_bg_thread   = LWP_THREAD_NULL;
static volatile int   s_bg_stop     = 0;
static uint8_t        s_bg_stack[48 * 1024] __attribute__((aligned(32)));

/* Wii power-off / reset flags — defined in App.cpp */
extern volatile bool g_app_powerOff;
extern volatile bool g_app_reset;

static void* bgThreadFunc(void*)
{
    /* In video mode (-vo gx): MPlayer's vo_gx/flip_page calls WPAD_ScanPads
     * every frame from its own main thread — we must NOT double-scan or we
     * race on the WPAD state.  Just read btns_h (already updated by mplayer).
     *
     * In audio mode (-vo null): flip_page is never called so mplayer never
     * scans WPAD.  bgThread must scan itself so that buttons and the IR
     * cursor stay live.  libogc's WPAD_ScanPads is mutex-protected, so
     * calling it from bgThread while mplayer's input loop occasionally
     * calls it is safe (they serialise). */
    bool audioMode = (s_audio_render_cb != nullptr);

    /* Initial scan so prevHeld reflects the real current state and avoids
     * treating already-held buttons as new presses on the first tick. */
    if (audioMode) WPAD_ScanPads();
    WPADData* wd0 = WPAD_Data(WPAD_CHAN_0);
    u32 prevHeld = wd0 ? wd0->btns_h : 0;
    int prevPaused = g_mplayer_paused;
    while (!s_bg_stop) {
        /* If the user pressed the Wii power button or reset button,
         * stop MPlayer so wii_player_play() returns cleanly and App.cpp
         * can call reportPlaybackStopped / deleteActiveEncoding before
         * the system actually powers off or resets.                    */
        if (g_app_powerOff || g_app_reset) {
            g_player_stop_reason = PLAYER_STOP_EOF;
            async_quit_request   = 1;
        }

        /* Refresh WPAD state in audio mode, XFB spinner phase, or GRRLIB
         * spinner phase (mplayer's main thread is blocked in open_stream,
         * so nobody else scans) */
        if (audioMode || g_wiifin_xfb_spinner_active || g_wiifin_grrlib_render_cb)
            WPAD_ScanPads();
        WPADData* wd = WPAD_Data(WPAD_CHAN_0);
        u32 held = wd ? wd->btns_h : 0;

        /* In video mode, ov_tick() owns WPAD_ScanPads() from the MPlayer
         * main thread (called at top of every ov_tick).  When the player
         * transitions from playing to paused, ov_tick already processed the
         * button press (A) that triggered the pause command.  bgThread's
         * prevHeld still reflects the pre-pause state, so that same A press
         * would appear as a new "down" event here and fire a second
         * pause_toggle — immediately unpausing.  Fix: whenever g_mplayer_paused
         * transitions to 1, resync prevHeld to the current held-state so that
         * buttons already handled by ov_tick are not re-fired here. */
        int curPaused = g_mplayer_paused;
        if (!audioMode && curPaused && !prevPaused)
            prevHeld = held;   /* swallow buttons active at pause entry */
        prevPaused = curPaused;

        u32 down = held & ~prevHeld;
        prevHeld = held;

        /* Read IR pointer — WPAD_IR() computes screen coords from raw sensor
         * data already stored in wd; safe to call from a second thread. */
        ir_t ir;
        WPAD_IR(WPAD_CHAN_0, &ir);
        float irX = ir.valid ? ir.x : -1.0f;
        float irY = ir.valid ? ir.y : -1.0f;

        /* Safety timeout: if the demuxer has established stream duration
         * (g_mplayer_duration > 0) but g_mplayer_time_pos has not moved
         * above 0 for more than ~15 s (900 ticks × ~16 ms), force the
         * loading flag off.  This fires when sh_video->pts stays at 0
         * due to -framedrop + skipframe dropping every decoded frame
         * before a PTS is ever emitted (common at 848×480@24fps with a
         * small cache-min).  Without this guard the overlay spinner shows
         * forever and the video is never revealed even though MPlayer is
         * playing. */
        if (g_wiifin_loading_active) {
            ++s_loading_watchdog;
            /* If time_pos is already > 1 s, the stream has started decoding
             * but the mplayer.c hook failed to clear loading_active.  This
             * happens when the stream starts at a high PTS (mid-stream resume)
             * causing mpgxForceLoadingFrame spam → GX queue overflow → video
             * decode stall → time_pos frozen → 1-s threshold never reached.
             * Clear after a short grace period (10 ticks ≈ 160 ms).
             * Also clear on general timeout (900 ticks ≈ 14 s) regardless of
             * duration — the old code reset to 0 when duration==0 meaning the
             * watchdog never fired for live-transcoded streams. */
            if ((g_mplayer_time_pos > 1.0f && s_loading_watchdog > 10) ||
                s_loading_watchdog > 900) {
                SYS_Report("[DBG] loading_active SET=0 (watchdog fired, wd=%d time=%.1f)\n",
                           s_loading_watchdog, (float)g_mplayer_time_pos);
                g_wiifin_loading_active = 0;
                s_loading_watchdog = 0;
            }
        }

        if (s_overlay)
            s_overlay->tick(g_mplayer_time_pos, g_mplayer_paused, down, held,
                            irX, irY, (bool)ir.valid);

        if (s_music_tick_cb)
            s_music_tick_cb(g_mplayer_paused, down, held);

        /* During pause OR seek stall, MPlayer's flip_page is not called, so
         * mpgxPushFrame is never invoked — the GX overlay (cursor, buttons)
         * freezes.  Drive the display ourselves from bgThread in both cases.
         *
         * Seek stall detection: g_wiifin_loading_active is set by
         * wii_player_seek_abs/rel(); while the demuxer seeks the mplayer
         * main loop blocks completely and g_mplayer_time_pos stops advancing.
         * Once mplayer resumes decoding, time_pos changes each tick →
         * seekStalled goes false and flip_page takes over again, avoiding
         * any double-push race with the decoder's render path. */
        {
            static float s_prev_vpos = -9999.0f;
            float curVpos = g_mplayer_time_pos;
            bool seekStalled = (bool)g_wiifin_loading_active &&
                               (curVpos == s_prev_vpos);
            s_prev_vpos = curVpos;

            if (!audioMode && g_wiifin_gx_overlay_cb &&
                (g_mplayer_paused || seekStalled)) {
                mpgxWaitDrawDone();
                mpgxRunOverlay();
                mpgxPushFrame();
            }
        }

        /* For audio-only playback the render callback calls GRRLIB_Render()
         * which waits for vsync (~16 ms), providing natural 60 Hz pacing.
         * In video mode, use usleep for the same pacing. */
        if (s_audio_render_cb)
            s_audio_render_cb();
        else if (g_wiifin_grrlib_render_cb) {
            /* GRRLIB loading spinner: App.cpp provided a callback that
             * draws ring.png centered on screen via GRRLIB_DrawImg.
             * GRRLIB_Render() inside the callback waits for vsync. */
            g_wiifin_grrlib_render_cb();
        } else {
            static int idle_log = 0;
            if (++idle_log <= 3)
                SYS_Report("[bgThread] idle (no render cb, no audio cb)\n");
            usleep(16000);
        }
    }
    return nullptr;
}

/* -----------------------------------------------------------------------
 * wii_player_play
 * ----------------------------------------------------------------------- */
int wii_player_play(const char* url)
{
    /* Build argv dynamically so we can conditionally append -ss when the
     * stream starts with a RESUME_PAD back-off (track switches / resumes).
     * Without -ss, Jellyfin's live transcoder produces an initial A/V gap
     * of 6+ seconds (video packets pipeline ahead of audio at transcode
     * start), causing -hardframedrop to trigger at 390%+ on the Wii CPU. */
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
                                        * streams (PROBING UP TO 0 → "NO VIDEO!").
                                        * FFmpeg's lavf demuxer parses PAT/PMT
                                        * properly and always finds both A+V. */
    addArg("-lavfdopts"); addArg("format=mpegts:probesize=32768:analyzeduration=1");
                                       /* Skip lavf probing — we know the
                                        * stream is MPEG-TS.  probesize=32K
                                        * keeps header scan small so audio
                                        * doesn't race 16s ahead of video. */
    addArg("-vo"); addArg("gx:colorspace=1");
    addArg("-ao"); addArg("gekko");
    addArg("-cache"); addArg("4096");
    addArg("-cache-min"); addArg("5");   /* 5% of 4096 KB ≈ 205 KB ≈ 1.6s at 1 Mbps */
    addArg("-cache-seek-min"); addArg("5");
    addArg("-autosync"); addArg("10");
    addArg("-mc"); addArg("15");         /* 15 s max A/V correction — handles the
                                        * large initial gap produced by Jellyfin's
                                        * live transcoder on mid-stream resumes */
    addArg("-delay"); addArg("0.3");
    addArg("-lavdopts"); addArg("fast:skiploopfilter=all:skipidct=nonref:skipframe=nonref");
    addArg("-hardframedrop"); /* drop decode units at demuxer level when pts queue
                              * fills — prevents the pts-overflow deadlock that
                              * freezes playback permanently at a given position */
    /* When the Jellyfin URL was built with a 3-second RESUME_PAD back-off
     * (startTimeTicks > 3 s before play), discard that many seconds at the
     * demuxer level so MPlayer starts output at the exact target position.
     * This is a packet-read-and-discard operation (no HTTP range request). */
    char ssBuf[16];
    if (g_wiifin_ss_secs > 0.0f) {
        snprintf(ssBuf, sizeof(ssBuf), "%.1f", (double)g_wiifin_ss_secs);
        addArg("-ss"); addArg(ssBuf);
        SYS_Report("[WiiPlayer] -ss %.1f applied\n", (double)g_wiifin_ss_secs);
    }
    addArg(url);
    argv[argc] = nullptr;

    /* Reset control state — clear MPlayer position so the premature-EOF
     * check (time_pos < 2 && duration == 0) works correctly even when
     * a previous session left a non-zero duration in these globals.    */
    async_quit_request    = 0;
    g_player_stop_reason  = PLAYER_STOP_EOF;
    g_wiifin_stopping     = 0;
    g_mplayer_time_pos    = 0.0f;
    g_mplayer_duration    = 0.0f;
    g_mplayer_paused      = 0;
    g_wiifin_seek_secs    = -1.0f;
    g_wiifin_vol_delta    = 0;
    g_wiifin_gx_btn_consumed  = 0;
    g_wiifin_home_menu_active = 0;
    /* g_wiifin_known_duration is set by App before this call — do not reset it.
     * g_wiifin_loading_active is cleared by the mplayer.c main-loop hook once
     * time_pos has advanced > 1 s from the point where the first frame was
     * decoded; set it here so it is 1 from the very start.
     * Exception: for track-switch restarts (g_wiifin_track_switch == 1) leave
     * loading_active at 0 so the loading overlay is suppressed and the video
     * resumes immediately without the spinner-over-video artefact. */
    g_wiifin_loading_active    = g_wiifin_track_switch ? 0 : 1;
    SYS_Report("[DBG] loading_active SET=%d (track_switch=%d) @ wii_player_play init\n",
               (int)g_wiifin_loading_active, (int)g_wiifin_track_switch);
    g_wiifin_track_switch      = 0;   /* consume — one shot per restart */
    g_wiifin_loading_start_pos = -1.0f;
    s_loading_watchdog         = 0;   /* reset between sessions */
    g_wiifin_xfb_spinner_active = 0;
    g_wiifin_xfb_spinner_ptr    = nullptr;

    SYS_Report("[DBG] wii_player_play: state reset done, overlay=%p\n", s_overlay);

    /* Register texture-overlay callback if an overlay is attached */
    if (s_overlay)
        g_wiifin_overlay_cb = PlayerOverlay::onFrame;
    else
        g_wiifin_overlay_cb = nullptr;

    /* Start background input thread */
    s_bg_stop = 0;
    /* Cache thread in cache2.c uses GEKKO_THREAD_PRIO = 70.
     * This thread MUST be below 70, otherwise it preempts the cache
     * fill thread and triggers "Cache not responding!" on startup. */
    LWP_CreateThread(&s_bg_thread, bgThreadFunc, nullptr,
                     s_bg_stack, sizeof(s_bg_stack), 40);
    SYS_Report("[DBG] wii_player_play: bgThread created\n");

    SYS_STDIO_Report(true);

    /* Redact api_key token from URL before any logging */
    std::string safeUrl(url);
    {
        size_t kp = safeUrl.find("api_key=");
        if (kp != std::string::npos) {
            size_t ve = safeUrl.find('&', kp + 8);
            safeUrl.replace(kp + 8,
                (ve == std::string::npos ? safeUrl.size() : ve) - kp - 8, "***");
        }
    }
    SYS_Report("[WiiPlayer] play: %s\n", safeUrl.c_str());

    {
        FILE *dbg = fopen("sd:/wiiplayer_debug.txt", "a");
        if (!dbg) dbg = fopen("fat:/wiiplayer_debug.txt", "a");
        if (dbg) {
            fprintf(dbg, "[WiiPlayer] url=%s\n", safeUrl.c_str());
            fclose(dbg);
        }
    }

    int rc = setjmp(g_mplayer_jmp);
    if (rc == 0) {
        if (!vmode)
            vmode = VIDEO_GetPreferredMode(NULL);
        screenwidth  = WiiUtils::widescreen ? 848 : 640;
        screenheight = 480;

        /* --- Loading screen ---
         *
         * If App.cpp set up a GRRLIB render callback (g_wiifin_grrlib_render_cb),
         * GRRLIB is still active and bgThread draws ring.png each frame.
         * We do NOT allocate XFBs or touch VIDEO — GRRLIB owns all that.
         * mpgxInit() will call g_wiifin_grrlib_cleanup_cb → GRRLIB_Exit()
         * before taking over GX.
         *
         * If no GRRLIB callback is set (shouldn't happen in normal flow),
         * fall back to old XFB approach. */
        if (!g_wiifin_grrlib_render_cb) {
            SYS_Report("[DBG] wii_player_play: GRRLIB cb NOT set, XFB fallback path\n");
            VIDEO_SetBlack(true);
            VIDEO_Flush();
            VIDEO_WaitVSync();

            VIDEO_Configure(vmode);
            void* pre_xfb0 = MEM_K0_TO_K1(SYS_AllocateFramebuffer(vmode));
            void* pre_xfb1 = MEM_K0_TO_K1(SYS_AllocateFramebuffer(vmode));
            uint32_t blk = yuyv(16, 16);
            int n = (vmode->fbWidth * vmode->xfbHeight * 2) / 4;
            uint32_t* p;
            p = (uint32_t*)pre_xfb0; for (int i = 0; i < n; i++) p[i] = blk;
            p = (uint32_t*)pre_xfb1; for (int i = 0; i < n; i++) p[i] = blk;
            VIDEO_SetNextFramebuffer(pre_xfb0);
            VIDEO_Flush();
            VIDEO_WaitVSync();
            VIDEO_SetNextFramebuffer(pre_xfb1);
            VIDEO_SetBlack(false);
            VIDEO_Flush();
            VIDEO_WaitVSync();
            /* Free the temporary loading framebuffers — mpgxInit() allocates
             * its own XFBs so these are no longer needed.  Each buffer is
             * ~640 KB; leaking both (~1.2 MB) would waste ~5 % of the Wii's
             * usable RAM on every video playback. */
            free(MEM_K1_TO_K0(pre_xfb0));
            free(MEM_K1_TO_K0(pre_xfb1));
        } else {
            SYS_Report("[DBG] wii_player_play: GRRLIB cb set, using GRRLIB spinner\n");
        }

        /* Flush any stale WPAD key events that accumulated in the mp_fifo
         * key FIFO from prior navigation (e.g. the A press that launched
         * playback).  Without this, mp_input_check_interrupt() fires
         * within the first cache prefill cycle and kills the stream. */
        while (mplayer_get_key(0) != -3 /* MP_INPUT_NOTHING */) { /* drain */ }
        SYS_Report("[DBG] wii_player_play: key FIFO drained, calling register_mpegts_demuxer\n");
        register_mpegts_demuxer();
        g_wiifin_gx_active = 1; /* suppress getch2 WPAD scan during WiiFin playback */
        SYS_Report("[DBG] >>> mplayer_main() ENTER\n");
        mplayer_main(argc, const_cast<char**>(argv));
        SYS_Report("[DBG] <<< mplayer_main() RETURNED  reason=%d time=%.1f dur=%.1f loading=%d\n",
                   (int)g_player_stop_reason, (float)g_mplayer_time_pos,
                   (float)g_mplayer_duration, (int)g_wiifin_loading_active);
        g_wiifin_gx_active = 0;
    }

    /* Stop background thread */
    SYS_Report("[DBG] wii_player_play: stopping bgThread\n");
    s_bg_stop = 1;
    if (s_bg_thread != LWP_THREAD_NULL) {
        LWP_JoinThread(s_bg_thread, nullptr);
        s_bg_thread = LWP_THREAD_NULL;
    }
    SYS_Report("[DBG] wii_player_play: bgThread joined\n");

    /* Close any TCP sockets that MPlayer CE left open after exiting via longjmp.
     * MPlayer's stream and cache threads cannot unwind through the setjmp boundary,
     * leaving their IOS socket handles open.  The cache2 thread in particular keeps
     * calling net_read() on the orphaned stream socket in a tight error-retry loop,
     * flooding the IOS network IPC queue.  When WiiFin then calls net_write() on a
     * new socket for the next HTTPS request, IOS returns an error (visible as
     * MBEDTLS_ERR_NET_SEND_FAILED / TLS handshake failed: UNKNOWN ERROR CODE 004C).
     *
     * WiiFin closes its own sockets inside each httpsRequest call, so any socket
     * FD still open at this point is a leaked MPlayer socket.  net_close() returns
     * EBADF for invalid / already-closed FDs without crashing.
     * IOS supports at most 24 concurrent socket handles (FDs 0–23). */
    {
        SYS_Report("[WiiPlayer] closing leaked MPlayer sockets\n");
        for (s32 _fd = 0; _fd < 24; ++_fd) net_close(_fd);
    }
    /* Give IOS time to process the socket closures and let the MPlayer cache
     * thread observe the errors and stop issuing net_read() calls. */
    usleep(200000);

    g_wiifin_gx_active = 0; /* re-enable getch2 WPAD scan after playback */

    /* Clear overlay callbacks so the next GRRLIB owner is not surprised */
    g_wiifin_overlay_cb         = nullptr;
    g_wiifin_gx_overlay_cb      = nullptr;
    SYS_Report("[DBG] loading_active SET=0 @ cleanup (was %d)\n", (int)g_wiifin_loading_active);
    g_wiifin_loading_active     = 0;
    g_wiifin_xfb_spinner_active = 0;
    g_wiifin_xfb_spinner_ptr    = nullptr;
    g_wiifin_grrlib_render_cb   = nullptr;
    g_wiifin_grrlib_cleanup_cb  = nullptr;

    /* Premature-EOF detection: MPlayer exited with EOF but played nothing
     * (g_mplayer_time_pos < 2 s, duration still 0 because demux never
     * completed).  Root cause: a 504 retry inside stream_https_wii.c
     * stalls the stream open by ~60 s; by the time the server responds
     * HTTP 200 the Jellyfin transcoder session has expired and the server
     * delivers only a small proxy-buffered fragment before closing.
     * Promote to PLAYER_STOP_ERROR so App.cpp re-acquires a fresh
     * streaming session and retries automatically. */
    if (g_player_stop_reason == PLAYER_STOP_EOF &&
        g_mplayer_time_pos < 2.0f && g_mplayer_duration == 0.0f)
        g_player_stop_reason = PLAYER_STOP_ERROR;

    SYS_Report("[WiiPlayer] mplayer exited rc=%d reason=%d time=%.1f\n",
               rc, (int)g_player_stop_reason, (float)g_mplayer_time_pos);

    /* MPlayer exits via longjmp — mpviClear() is never called, so GX and
     * VIDEO callbacks set by mpgxInit() are still active:
     *   - GX_SetDrawDoneCallback(drawdone_cb)  (gx_supp.c)
     *   - VIDEO_SetPreRetraceCallback(vblank_cb)
     * With vo_vsync==0 (default), drawdone_cb directly calls
     * VIDEO_SetNextFramebuffer() pointing at MPlayer's XFBs (which still
     * contain the last video frame).  This fires on every GX_DrawDone()
     * — i.e. every GRRLIB_Render() — causing a race: if a VI retrace
     * falls in the ~1 ms window between drawdone_cb's
     * VIDEO_SetNextFramebuffer and GRRLIB_Render's own
     * VIDEO_SetNextFramebuffer, MPlayer's last video frame is briefly
     * displayed.  Clear all stale callbacks now. */
    GX_SetDrawDoneCallback(NULL);
    VIDEO_SetPreRetraceCallback(NULL);
    VIDEO_SetPostRetraceCallback(NULL);

    /* Blank the display RIGHT NOW so the last video frame is never visible
     * during GRRLIB re-init. */
    VIDEO_SetBlack(true);
    VIDEO_Flush();

    SYS_Report("[DBG] wii_player_play returning reason=%d\\n", (int)g_player_stop_reason);

    return (int)g_player_stop_reason;
}

/* -----------------------------------------------------------------------
 * wii_player_stop
 * ----------------------------------------------------------------------- */
void wii_player_stop(void)
{
    async_quit_request = 1;
}

/* -----------------------------------------------------------------------
 * Cursor texture — call after GRRLIB_LoadTexture() and before play().
 * Pass nullptr to revert to the built-in crosshair fallback.
 * ----------------------------------------------------------------------- */
void wii_player_set_cursor_tex(void* data, u16 w, u16 h, u8 fmt)
{
    if (data) {
        GX_InitTexObj(&g_wiifin_cursor_tex_obj, data, w, h, fmt,
                      GX_CLAMP, GX_CLAMP, GX_FALSE);
        g_wiifin_cursor_tex_valid = 1;
    } else {
        g_wiifin_cursor_tex_valid = 0;
    }
}

/* -----------------------------------------------------------------------
 * Overlay attachment — call before wii_player_play()
 * ----------------------------------------------------------------------- */
void wii_player_set_overlay(PlayerOverlay* overlay)
{
    s_overlay = overlay;
}

/* -----------------------------------------------------------------------
 * Control functions (safe to call from background thread)
 * ----------------------------------------------------------------------- */

void wii_player_show_osd(const char* text, int durationMs)
{
    char cmd[256];
    snprintf(cmd, sizeof(cmd), "osd_show_text \"%s\" %d 0", text, durationMs);
    mp_input_queue_cmd(mp_input_parse_cmd(cmd));
}

void wii_player_pause_toggle(void)
{
    /* mp_input_queue_cmd is mutex-protected in MPlayer CE (HAVE_PTHREADS=yes) */
    mp_input_queue_cmd(mp_input_parse_cmd(const_cast<char*>("pause")));
}

void wii_player_seek_abs(float seconds)
{
    if (seconds < 0.0f) seconds = 0.0f;
    /* Re-activate loading screen so the user sees the spinner instead of
     * purple / garbage video frames while MPlayer seeks and rebuffers. */
    g_wiifin_loading_start_pos = -1.0f;
    SYS_Report("[DBG] loading_active SET=1 @ seek_abs(%.1f)\n", seconds);
    g_wiifin_loading_active    = 1;
    g_wiifin_seek_secs = seconds;
}

void wii_player_seek_rel(float delta)
{
    float pos = g_mplayer_time_pos + delta;
    if (pos < 0.0f) pos = 0.0f;
    g_wiifin_loading_start_pos = -1.0f;
    SYS_Report("[DBG] loading_active SET=1 @ seek_rel(%.1f)\n", pos);
    g_wiifin_loading_active    = 1;
    g_wiifin_seek_secs = pos;
}

void wii_player_vol_up(void)
{
    g_wiifin_vol_delta = 1;
}

void wii_player_vol_down(void)
{
    g_wiifin_vol_delta = -1;
}

void wii_player_request_next(void)
{
    g_player_stop_reason = PLAYER_STOP_NEXT;
    async_quit_request   = 1;
}

void wii_player_request_prev(void)
{
    g_player_stop_reason = PLAYER_STOP_PREV;
    async_quit_request   = 1;
}

void wii_player_request_audio_switch(void)
{
    g_player_stop_reason = PLAYER_STOP_AUDIO;
    async_quit_request   = 1;
}

void wii_player_request_sub_switch(void)
{
    g_player_stop_reason = PLAYER_STOP_SUB;
    async_quit_request   = 1;
}

/* -----------------------------------------------------------------------
 * wii_player_set_music_tick
 * ----------------------------------------------------------------------- */
void wii_player_set_music_tick(
    void (*cb)(int paused, uint32_t btnsDown, uint32_t btnsHeld))
{
    s_music_tick_cb = cb;
}

/* -----------------------------------------------------------------------
 * wii_player_play_audio — audio-only variant of wii_player_play().
 *
 * Uses the libavformat (lavf) demuxer which handles mp3/aac/ogg/flac etc.
 * Keeps -vo gx so that the Y-texture overlay callback still fires each
 * frame (MPlayer renders black frames for audio-only streams).
 * The caller must set g_wiifin_overlay_cb BEFORE calling this function;
 * unlike wii_player_play() this function does NOT overwrite it.
 * ----------------------------------------------------------------------- */
int wii_player_play_audio(const char* url)
{
    const char* argv[] = {
        "mplayer",
        "-noconsolecontrols",
        "-v",
        "-fs",
        "-vo", "null",              /* audio-only: no video output; GRRLIB stays active */
        "-ao", "gekko",
        "-cache", "2048",
        "-cache-min", "5",
        "-autosync", "30",
        "-demuxer", "audio",        /* native streaming audio demuxer — lavf can't probe non-seekable HTTP MP3 */
        url,
        nullptr
    };
    const int argc = (int)(sizeof(argv) / sizeof(argv[0])) - 1;

    /* Reset control state — do NOT touch g_wiifin_overlay_cb            */
    async_quit_request   = 0;
    g_player_stop_reason = PLAYER_STOP_EOF;
    g_mplayer_time_pos   = 0.0f;
    g_mplayer_duration   = 0.0f;
    g_mplayer_paused     = 0;
    g_wiifin_seek_secs   = -1.0f;
    g_wiifin_vol_delta   = 0;
    s_overlay            = nullptr; /* no video PlayerOverlay for audio */

    /* Start background input thread */
    s_bg_stop = 0;
    LWP_CreateThread(&s_bg_thread, bgThreadFunc, nullptr,
                     s_bg_stack, sizeof(s_bg_stack), 40);

    SYS_STDIO_Report(true);

    /* Redact api_key token from URL before any logging */
    std::string safeUrl(url);
    {
        size_t kp = safeUrl.find("api_key=");
        if (kp != std::string::npos) {
            size_t ve = safeUrl.find('&', kp + 8);
            safeUrl.replace(kp + 8,
                (ve == std::string::npos ? safeUrl.size() : ve) - kp - 8, "***");
        }
    }
    SYS_Report("[WiiPlayer] play_audio: %s\n", safeUrl.c_str());

    int rc = setjmp(g_mplayer_jmp);
    if (rc == 0) {
        if (!vmode)
            vmode = VIDEO_GetPreferredMode(NULL);
        screenwidth  = WiiUtils::widescreen ? 848 : 640;
        screenheight = 480;
        while (mplayer_get_key(0) != -3) { /* drain stale input */ }
        register_mpegts_demuxer();
        mplayer_main(argc, const_cast<char**>(argv));
    }

    /* Stop background thread */
    s_bg_stop = 1;
    if (s_bg_thread != LWP_THREAD_NULL) {
        LWP_JoinThread(s_bg_thread, nullptr);
        s_bg_thread = LWP_THREAD_NULL;
    }

    /* Clear callbacks */
    g_wiifin_overlay_cb    = nullptr;
    g_wiifin_gx_overlay_cb = nullptr;
    s_music_tick_cb        = nullptr;
    s_audio_render_cb      = nullptr;

    SYS_Report("[WiiPlayer] play_audio exited rc=%d reason=%d\n",
               rc, (int)g_player_stop_reason);

    return (int)g_player_stop_reason;
}

/* -----------------------------------------------------------------------
 * wii_player_set_audio_render_cb
 * ----------------------------------------------------------------------- */
void wii_player_set_audio_render_cb(void (*cb)())
{
    s_audio_render_cb = cb;
}
