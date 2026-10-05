/*
 * MusicPlayerView.cpp — music player HUD + session management for WiiFin.
 *
 * GRRLIB stays active throughout audio playback.  MPlayer CE runs with
 * -vo null (audio-only) and never touches GX, so the bgThread can safely
 * render the animated music HUD via MusicOverlay::renderFrameGRRLIB()
 * at ~60 Hz.  Between tracks the bgThread has been joined; the main
 * thread pushes a static "Loading…" frame before doing network I/O.
 *
 * Controls (Wii Remote horizontal):
 *   A          — toggle play / pause
 *   ← / →      — seek −10 s / +10 s
 *   + (PLUS)   — next track
 *   − (MINUS)  — previous track
 *   HOME / B   — stop playback, return to library
 */

#include "MusicPlayerView.h"
#include "../core/ExitZone.h"
#include "../core/Text.h"
#include "Ui.h"
#include "../input/Input.h"
#include "../core/Utils.h"
#include "../player/WiiPlayer.h"
#include "../core/MusicBGM.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <setjmp.h>
#include <unistd.h>
#include <ogcsys.h>
#include <ogc/lwp.h>           /* LWP_CreateThread / LWP_JoinThread */
#include <ogc/lwp_watchdog.h>  /* gettime(), ticks_to_millisecs()   */
#include <ogc/video.h>         /* VIDEO_WaitVSync                   */
#include <wiiuse/wpad.h>
#include <jpeglib.h>

/* Global power/reset flags (defined in App.cpp) */
extern volatile bool g_app_powerOff;
extern volatile bool g_app_reset;

/* -----------------------------------------------------------------------
 * Deferred reportPlaybackStart — the callback g_stream_opened_cb fires
 * from MPlayer's main thread the instant the HTTP stream is opened.
 * Doing HTTP I/O there corrupts MPlayer's state, so we just set a flag
 * and let the bgThread (MusicOverlay::bgTick) send the report.
 * This way the Jellyfin transcode is already running when the report
 * arrives, so the dashboard correctly shows "Transcoding".
 * ----------------------------------------------------------------------- */
static volatile bool  s_reportNeeded = false;
static volatile bool  s_startReported = false;  /* true once reportPlaybackStart succeeded */
static JellyfinClient* s_reportClient   = nullptr;
static std::string     s_reportServer;
static JellyfinAuth    s_reportAuth;
static std::string     s_reportItemId;
static std::string     s_reportSessionId;
static uint32_t        s_lastProgressTick = 0;  /* for periodic progress reports */

/* -----------------------------------------------------------------------
 * Async reporter — runs HTTP progress/start reports on a dedicated LWP
 * thread so that the bgThread render loop is never blocked by network I/O.
 * ----------------------------------------------------------------------- */
static volatile bool      s_asyncStartPending = false; /* queue a reportPlaybackStart  */
static volatile bool      s_asyncProgPending  = false; /* queue a reportPlaybackProgress */
static volatile long long s_asyncProgTicks    = 0;
static volatile int       s_asyncProgPaused   = 0;
static volatile bool      s_asyncStop         = false;
static lwp_t              s_asyncThread       = LWP_THREAD_NULL;
/* httpsRequest alone uses ~8 KB of stack (mbedTLS contexts + buffers); give
 * enough room for the full call chain: asyncReporterFunc → report* → https. */
static uint8_t            s_asyncStack[32 * 1024] DEAD_AT_EXIT __attribute__((aligned(32)));
/* Hovered transport button: -1=Prev, 0=None, 1=Play/Pause, 2=Next.
 * Written by renderFrameGRRLIB (bgThread), read by bgTick (same thread). */
static volatile int       s_btnHovered = 0;

static void* asyncReporterFunc(void*)
{
    while (!s_asyncStop) {
        if (s_asyncStartPending && s_reportClient) {
            s_reportClient->reportPlaybackStart(s_reportServer, s_reportAuth,
                                                s_reportItemId, s_reportItemId,
                                                s_reportSessionId);
            s_asyncStartPending = false;
            s_startReported     = true;
            s_lastProgressTick  = (uint32_t)ticks_to_millisecs(gettime());
        }
        if (s_asyncProgPending && s_reportClient) {
            long long ticks  = s_asyncProgTicks;
            bool      isPaused = (s_asyncProgPaused != 0);
            s_asyncProgPending = false;   /* clear before HTTP so next can queue */
            s_reportClient->reportPlaybackProgress(
                s_reportServer, s_reportAuth,
                s_reportItemId, s_reportItemId,
                s_reportSessionId, ticks, isPaused);
        }
        usleep(16000); /* ~60 Hz poll — yields CPU between checks */
    }
    return nullptr;
}

/* Gate flag — prevents bgThread from calling GRRLIB while mplayer_main's
 * common init path is still touching GX.  Set to true once g_stream_opened_cb
 * fires (init is complete, stream is playing). */
static volatile bool s_renderEnabled = false;

static void onAudioStreamOpened()
{
    s_reportNeeded  = true;
    s_renderEnabled = true;
    /* mplayer_main's common init calls VIDEO_SetBlack(TRUE) to blank the
     * screen during startup.  With -vo gx the GX driver unblanks it, but
     * with -vo null nobody does.  Unblank here so our GRRLIB frames are
     * actually visible on screen. */
    VIDEO_SetBlack(FALSE);
    VIDEO_Flush();
}

/* -----------------------------------------------------------------------
 * Singleton
 * ----------------------------------------------------------------------- */
MusicOverlay* MusicOverlay::instance  = nullptr;
GRRLIB_ttfFont* MusicOverlay::renderFont = nullptr;
GRRLIB_texImg*  MusicOverlay::renderCursorTex = nullptr;
GRRLIB_texImg*  MusicOverlay::renderArtTex    = nullptr;

/* -----------------------------------------------------------------------
 * loadJPEGTexture — decode a JPEG image buffer into a GRRLIB texture.
 * Returns nullptr on any error (corrupt data, alloc failure, etc.)
 * Thread-safe: only called from the main thread in MusicPlayerView::run().
 * ----------------------------------------------------------------------- */
struct MpvJpegErrMgr {
    struct jpeg_error_mgr pub;
    jmp_buf               buf;
};
static void mpvJpegErrExit(j_common_ptr cinfo) {
    longjmp(((MpvJpegErrMgr*)cinfo->err)->buf, 1);
}
static void mpvJpegNoOp(j_common_ptr) {}

static GRRLIB_texImg* loadJPEGTexture(const u8* data, u32 size)
{
    if (size < 3 || data[0] != 0xFF || data[1] != 0xD8 || data[2] != 0xFF)
        return nullptr;

    struct jpeg_decompress_struct cinfo __attribute__((aligned(32)));
    MpvJpegErrMgr jerr __attribute__((aligned(32)));
    unsigned char* strip = nullptr;
    GRRLIB_texImg* tex   = nullptr;

    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit     = mpvJpegErrExit;
    jerr.pub.output_message = mpvJpegNoOp;

    if (setjmp(jerr.buf)) {
        jpeg_destroy_decompress(&cinfo);
        free(strip);
        if (tex) { free(tex->data); free(tex); }
        return nullptr;
    }

    jpeg_create_decompress(&cinfo);
    cinfo.progress = nullptr;
    jpeg_mem_src(&cinfo, data, size);
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space = JCS_RGB;
    // Speed over exactness: the images are small and already resized by the
    // server, so the integer DCT and plain chroma upsampling are not visible.
    cinfo.dct_method          = JDCT_IFAST;
    cinfo.do_fancy_upsampling = FALSE;
    jpeg_start_decompress(&cinfo);

    u32 w  = cinfo.output_width;
    u32 h  = cinfo.output_height;
    u32 nc = (u32)cinfo.output_components;
    if (w == 0 || h == 0 || w > 2048 || h > 2048 || nc != 3) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }

    tex = (GRRLIB_texImg*)calloc(1, sizeof(GRRLIB_texImg));
    if (!tex) { jpeg_abort_decompress(&cinfo); jpeg_destroy_decompress(&cinfo); return nullptr; }

    u32 bufsize = GX_GetTexBufferSize(w, h, GX_TF_RGBA8, 0, 0);
    tex->data = memalign(32, bufsize);
    if (!tex->data) {
        free(tex); tex = nullptr;
        jpeg_abort_decompress(&cinfo); jpeg_destroy_decompress(&cinfo); return nullptr;
    }

    strip = (unsigned char*)malloc(w * 4 * nc);
    if (!strip) {
        free(tex->data); free(tex); tex = nullptr;
        jpeg_abort_decompress(&cinfo); jpeg_destroy_decompress(&cinfo); return nullptr;
    }

    u8* tileData = (u8*)tex->data;
    for (u32 by = 0; by < h; by += 4) {
        int nrows = (int)(h - by);
        if (nrows > 4) nrows = 4;
        JSAMPROW rp[4];
        for (int i = 0; i < 4; i++)
            rp[i] = strip + (u32)i * w * nc;
        int done = 0;
        while (done < nrows && cinfo.output_scanline < h)
            done += (int)jpeg_read_scanlines(&cinfo, rp + done, (JDIMENSION)(nrows - done));
        for (u32 bx = 0; bx < w; bx += 4) {
            for (u8 r = 0; r < 4; r++) {
                for (u8 c = 0; c < 4; c++) {
                    u32 sx = bx + c;
                    u8 red = (sx < w && r < (u8)nrows) ? strip[((u32)r * w + sx) * nc] : 0;
                    *tileData++ = 0xFF;
                    *tileData++ = red;
                }
            }
            for (u8 r = 0; r < 4; r++) {
                for (u8 c = 0; c < 4; c++) {
                    u32 sx = bx + c;
                    u8 g = 0, b = 0;
                    if (sx < w && r < (u8)nrows) {
                        g = strip[((u32)r * w + sx) * nc + 1];
                        b = strip[((u32)r * w + sx) * nc + 2];
                    }
                    *tileData++ = g;
                    *tileData++ = b;
                }
            }
        }
    }

    free(strip);
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    tex->w      = w;
    tex->h      = h;
    tex->format = GX_TF_RGBA8;
    GRRLIB_SetHandle(tex, 0, 0);
    GRRLIB_FlushTex(tex);
    return tex;
}

/* (g_stream_opened_cb sets s_reportNeeded; bgTick queues to async reporter\n * thread which calls reportPlaybackStart without blocking the render loop.) */

/* -----------------------------------------------------------------------
 * Constructor
 * ----------------------------------------------------------------------- */
MusicOverlay::MusicOverlay(const std::vector<Track>& t, int idx)
    : tracks(t), currentIdx(idx < (int)t.size() ? idx : 0)
{
    for (int i = 0; i < VIZ_BARS; i++) { vizHeight[i] = 0.0f; vizTarget[i] = 0.0f; }
    instance = this;
}

/* -----------------------------------------------------------------------
 * Timing helpers
 * ----------------------------------------------------------------------- */
void MusicOverlay::resetTiming(float startSecs)
{
    startTick      = 0;
    seekBase       = startSecs;
    totalPauseSecs = 0.0f;
    wasPaused      = false;
    pausedAtTick   = 0;
    timingActive   = false;  /* defer: timer starts when g_mplayer_duration>0 */
}

void MusicOverlay::notifySeek(float targetSecs)
{
    startTick      = gettime();
    seekBase       = targetSecs;
    totalPauseSecs = 0.0f;
    timingActive   = true;  /* audio is already running when a seek happens */
    /* preserve pause state */
    if (wasPaused) pausedAtTick = startTick;
}

float MusicOverlay::getPosition() const
{
    /* Lazy-start: don't count time until MPlayer's main loop has begun
     * (g_mplayer_duration > 0), which is the first moment audio is actually
     * being decoded/played — avoids counting the ~3 s demuxer init phase. */
    if (!timingActive) {
        if (g_mplayer_duration <= 0.0f)
            return seekBase;
        /* First call after audio begins — arm the wall-clock now. */
        const_cast<MusicOverlay*>(this)->startTick    = gettime();
        const_cast<MusicOverlay*>(this)->timingActive = true;
    }
    uint64_t now   = gettime();
    float elapsed  = (float)ticks_to_millisecs(now - startTick) / 1000.0f;
    float paused   = totalPauseSecs;
    if (wasPaused && pausedAtTick >= startTick)
        paused += (float)ticks_to_millisecs(now - pausedAtTick) / 1000.0f;
    float pos = seekBase + elapsed - paused;
    if (pos < 0.0f) pos = 0.0f;
    return pos;
}

float MusicOverlay::getDuration() const
{
    /* Jellyfin's runtime first: MPlayer estimates a streamed track's length
     * from the bitrate, and the estimate goes wild after a rebuffer (a
     * 4-minute song showed 22:52). */
    if (!tracks.empty() && currentIdx < (int)tracks.size() && tracks[currentIdx].runtimeTicks > 0)
        return (float)(tracks[currentIdx].runtimeTicks / 10000000LL);
    return (float)g_mplayer_duration;
}

/* -----------------------------------------------------------------------
 * bgTick — called ~60 Hz from the background LWP thread.
 * Handles button input, pause-time tracking, periodic progress reports,
 * and power/reset detection.
 * ----------------------------------------------------------------------- */
void MusicOverlay::bgTick(int paused, uint32_t btnsDown, uint32_t btnsHeld)
{
    MusicOverlay* mo = instance;
    if (!mo) return;

    /* ---- Power / reset detection: stop playback so cleanup runs ------- */
    if (g_app_powerOff || g_app_reset) {
        mo->requestStop = true;
        wii_player_stop();
    }

    /* ---- Deferred playback-start report ---------------------------------
     * Hand off to the async reporter thread to avoid blocking the render loop. */
    if (s_reportNeeded && s_reportClient) {
        s_asyncStartPending = true;
        s_reportNeeded      = false;
    }

    /* ---- Periodic progress report (~every 10 s) -----------------------
     * Queue to async reporter thread; do NOT call HTTP here. */
    if (s_startReported && s_reportClient && !s_asyncProgPending) {
        uint32_t now = (uint32_t)ticks_to_millisecs(gettime());
        if (now - s_lastProgressTick >= 10000) {
            s_lastProgressTick  = now;
            s_asyncProgTicks    = (long long)(mo->getPosition() * 10000000.0f);
            s_asyncProgPaused   = paused;
            s_asyncProgPending  = true;
        }
    }

    /* ---- Pause-time tracking ------------------------------------------ */
    bool nowPaused = (paused != 0);
    if (nowPaused && !mo->wasPaused) {
        mo->pausedAtTick = gettime();
        mo->wasPaused    = true;
    } else if (!nowPaused && mo->wasPaused) {
        uint64_t now = gettime();
        mo->totalPauseSecs += (float)ticks_to_millisecs(now - mo->pausedAtTick) / 1000.0f;
        mo->wasPaused        = false;
        mo->pausedAtTick     = 0;
    }

    /* ---- Auto-advance at end of track ---------------------------------- */
    /* MPlayer uses -demuxer audio for non-seekable HTTP streams and can't
     * determine duration from the stream headers (reports -5.4 "unknown").
     * When the HTTP response ends MPlayer's cache empties and it hangs for
     * up to ~60 s waiting for more data.  Detect EOF proactively via
     * g_mplayer_time_pos vs. the Jellyfin-known duration (runtimeTicks)
     * and stop MPlayer immediately so the next track starts without delay. */
    if (!nowPaused && !mo->requestStop && mo->pendingNextIdx < 0) {
        float dur = mo->getDuration();
        if (dur > 0.0f && g_mplayer_time_pos > 2.0f && g_mplayer_time_pos >= dur - 1.0f) {
            int n = (int)mo->tracks.size();
            if (mo->currentIdx + 1 < n)
                mo->pendingNextIdx = mo->currentIdx + 1;
            else
                mo->requestStop = true;
            wii_player_stop();
        }
    }

    /* ---- Button handling ---------------------------------------------- */
    /* A: action depends on which transport button the IR is hovering over. */
    if (btnsDown & WPAD_BUTTON_A) {
        int hov = s_btnHovered;
        if (hov == -1) {
            /* Prev button */
            if (mo->currentIdx > 0)
                mo->pendingNextIdx = mo->currentIdx - 1;
            else
                mo->pendingNextIdx = mo->currentIdx;
            wii_player_stop();
        } else if (hov == 2) {
            /* Next button */
            int n = (int)mo->tracks.size();
            if (n > 0 && mo->currentIdx + 1 < n) {
                mo->pendingNextIdx = mo->currentIdx + 1;
                wii_player_stop();
            }
        } else {
            /* Play/Pause button (hov==1) or no button hovered */
            wii_player_pause_toggle();
        }
    }

    /* LEFT / RIGHT: seek -10s / +10s */
    if (btnsDown & WPAD_BUTTON_LEFT) {
        float pos = mo->getPosition() - 10.0f;
        if (pos < 0.0f) pos = 0.0f;
        wii_player_seek_abs(pos);
        mo->notifySeek(pos);
    }
    if (btnsDown & WPAD_BUTTON_RIGHT) {
        float dur = mo->getDuration();
        float pos = mo->getPosition() + 10.0f;
        if (dur > 0.0f && pos > dur) pos = dur;
        wii_player_seek_abs(pos);
        mo->notifySeek(pos);
    }

    /* + : next track */
    if (btnsDown & WPAD_BUTTON_PLUS) {
        int n = (int)mo->tracks.size();
        if (n > 0 && mo->currentIdx + 1 < n) {
            mo->pendingNextIdx = mo->currentIdx + 1;
            wii_player_stop();
        }
    }

    /* - : previous track */
    if (btnsDown & WPAD_BUTTON_MINUS) {
        if (mo->currentIdx > 0) {
            mo->pendingNextIdx = mo->currentIdx - 1;
        } else {
            /* restart current track */
            mo->pendingNextIdx = mo->currentIdx;
        }
        wii_player_stop();
    }

    /* HOME or B: stop and return to library */
    if ((btnsDown & WPAD_BUTTON_HOME) || (btnsDown & WPAD_BUTTON_B)) {
        mo->requestStop = true;
        wii_player_stop();
    }

    (void)btnsHeld;
}

/* -----------------------------------------------------------------------
 * updateVisualizerFrame — called each GX frame from onFrame().
 * dt: time since last frame in seconds.
 * When paused, bars gently fall toward 0.
 * When playing, targets are updated using sine waves with per-bar phases.
 * ----------------------------------------------------------------------- */
void MusicOverlay::updateVisualizerFrame(float dt)
{
    vizPhase += dt * 3.5f;   /* global phase advance rate          */
    /* Keep vizPhase bounded — prevents sinf() slow range reduction on long
     * sessions (large argument → many extra FP ops → periodic micro-freeze). */
    if (vizPhase > 1000.0f) vizPhase -= 1000.0f;

    bool playing = !(g_mplayer_paused);

    for (int i = 0; i < VIZ_BARS; i++) {
        if (playing) {
            /* Each bar has a unique frequency and phase offset */
            float freq  = 0.8f + (float)i * 0.18f;
            float phase = (float)i * 0.42f;
            float raw   = sinf(vizPhase * freq + phase) * 0.5f + 0.5f;
            /* Secondary harmonic adds texture */
            raw += sinf(vizPhase * freq * 1.7f + phase * 2.1f) * 0.2f;
            if (raw < 0.0f) raw = 0.0f;
            if (raw > 1.0f) raw = 1.0f;
            /* Bars near centre are generally taller (music "energy" shape) */
            float centre = 1.0f - fabsf((float)i - (float)(VIZ_BARS / 2)) / (float)(VIZ_BARS / 2);
            vizTarget[i] = raw * (0.3f + centre * 0.7f);
        } else {
            /* Paused: slowly drop to 0 */
            vizTarget[i] = 0.0f;
        }

        /* Smooth interpolation toward target */
        float rate = playing ? 8.0f : 3.0f;
        vizHeight[i] += (vizTarget[i] - vizHeight[i]) * rate * dt;
        if (vizHeight[i] < 0.0f) vizHeight[i] = 0.0f;
        if (vizHeight[i] > 1.0f) vizHeight[i] = 1.0f;
    }
}

/* -----------------------------------------------------------------------
 * drawMusicScreen — the whole "Now Playing" screen (Ui kit look).  Used for
 * the static frame shown while MPlayer starts and for the animated HUD.
 * viz may be null (flat bars).  Transport button rectangles are shared with
 * the IR hit test in renderFrameGRRLIB.
 * ----------------------------------------------------------------------- */
static const int MP_BTN_Y   = 304;
static const int MP_BTN_H   = 52;
static const int MP_BTN_W   = 80;
static const int MP_BTN_GAP = 20;
static const int MP_BTN_X0  = (640 - (MP_BTN_W * 3 + MP_BTN_GAP * 2)) / 2;  /* = 180 */

static void drawMusicScreen(const std::vector<MusicOverlay::Track>& tracks, int idx,
                            GRRLIB_texImg* art, float pos, float dur, bool playing,
                            int hov, const float* viz, int nViz)
{
    const Ui::Palette& p = Ui::pal();
    Ui::background();

    /* ---- Header ---- */
    Ui::text(28, 16, "Now Playing", 22, p.text);
    int n = (int)tracks.size();
    if (n > 1) {
        char buf[32];
        snprintf(buf, sizeof(buf), "Track %d / %d", idx + 1, n);
        Ui::textRight(612, 22, buf, 15, p.textDim);
    }
    Ui::roundRect(28, 52, 584, 2, 1, Ui::alpha(p.cardBorder, 0.8f));

    /* ---- Album art ---- */
    const float ART_X = 30, ART_Y = 70, ART_SZ = 176;
    const float aw = ART_SZ * WiiUtils::wsScaleX();
    Ui::shadow(ART_X + 1, ART_Y + 4, aw - 2, ART_SZ - 2, 14, 8.0f, p.shadow);
    if (art && art->w > 0 && art->h > 0) {
        Ui::texCover(art, ART_X, ART_Y, aw, ART_SZ, 14, 1.0f);
    } else {
        Ui::roundRect(ART_X, ART_Y, aw, ART_SZ, 14, p.cardTop, p.cardBottom);
        Ui::textCentered(ART_X + aw * 0.5f, ART_Y + ART_SZ * 0.5f - 40, "\xe2\x99\xab", 72,
                         Ui::alpha(p.textDim, 0.6f));
    }
    Ui::roundBorder(ART_X, ART_Y, aw, ART_SZ, 14, 1.5f, Ui::alpha(p.cardBorder, 0.8f));

    /* ---- Title / artist / album ---- */
    const int RX = (int)(ART_X + aw) + 26, RW = 612 - RX;
    auto fit = [&](const std::string& in, int size) {
        std::string t = in;
        if (Ui::textWidth(t.c_str(), size) <= RW) return t;
        while (!t.empty() && Ui::textWidth((t + "...").c_str(), size) > RW) {
            while (!t.empty() && (t.back() & 0xC0) == 0x80) t.pop_back();
            if (!t.empty()) t.pop_back();
        }
        return t + "...";
    };
    if (idx >= 0 && idx < n) {
        const MusicOverlay::Track& tr = tracks[idx];
        Ui::text(RX, ART_Y + 2, fit(tr.title, 22).c_str(), 22, p.text);
        if (!tr.artist.empty()) Ui::text(RX, ART_Y + 32, fit(tr.artist, 16).c_str(), 16, p.accentDark);
        if (!tr.album.empty())  Ui::text(RX, ART_Y + 54, fit(tr.album, 14).c_str(), 14, p.textDim);
    }

    /* ---- Visualizer (rounded bars on a baseline) ---- */
    {
        const float BASE = ART_Y + ART_SZ, MAXH = 92;
        const float gap = 4, bw = (RW - gap * (nViz - 1)) / (float)nViz;
        for (int i = 0; i < nViz; i++) {
            float v  = viz ? viz[i] : 0.0f;
            float bh = 6 + v * MAXH;
            float bx = RX + i * (bw + gap);
            Ui::roundRect(bx, BASE - bh, bw, bh, bw * 0.5f,
                          Ui::mix(p.accent, 0xFFFFFFFF, 0.35f * v), Ui::alpha(p.accent, 0.55f));
        }
    }

    /* ---- Seek bar + times ---- */
    {
        const float SX = 30, SW = 580, SY = 266;
        if (dur > 0.0f && pos > dur) pos = dur;
        float frac = dur > 0.0f ? pos / dur : 0.0f;
        Ui::roundRect(SX, SY, SW, 8, 4, Ui::alpha(p.cardBorder, 0.6f));
        if (frac > 0.0f)
            Ui::roundRect(SX, SY, SW * frac < 8 ? 8 : SW * frac, 8, 4,
                          Ui::mix(p.accent, 0xFFFFFFFF, 0.2f), p.accent);
        float kx = SX + SW * frac;
        Ui::shadow(kx - 8, SY - 3, 16, 16, 8, 4.0f, p.shadow);
        Ui::circle(kx, SY + 4, 8, p.cardTop);
        Ui::roundBorder(kx - 8, SY - 4, 16, 16, 8, 2.0f, p.accent);

        char buf[16];
        int s = (int)pos;
        snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
        Ui::text(SX, SY + 14, buf, 14, p.text);
        if (dur > 0.0f) {
            s = (int)dur;
            snprintf(buf, sizeof(buf), "%d:%02d", s / 60, s % 60);
            Ui::textRight(SX + SW, SY + 14, buf, 14, p.textDim);
        }
    }

    /* ---- Transport buttons ---- */
    const int vals[3] = { -1, 1, 2 };
    for (int b = 0; b < 3; b++) {
        float bx = MP_BTN_X0 + b * (MP_BTN_W + MP_BTN_GAP);
        bool  on = hov == vals[b];
        Ui::button(bx, MP_BTN_Y, MP_BTN_W, MP_BTN_H, "", 14, on ? Ui::pulse() : 0.0f);
        float cx = bx + MP_BTN_W * 0.5f, cy = MP_BTN_Y + MP_BTN_H * 0.5f;
        u32 c = on ? p.accentDark : p.text;
        if (b == 0) {          /* |<  previous */
            Ui::roundRect(cx - 11, cy - 9, 4, 18, 1.5f, c);
            Ui::triangle(cx - 6, cy, cx + 9, cy - 9, cx + 9, cy + 9, c);
        } else if (b == 2) {   /* >|  next */
            Ui::triangle(cx - 9, cy - 9, cx + 6, cy, cx - 9, cy + 9, c);
            Ui::roundRect(cx + 7, cy - 9, 4, 18, 1.5f, c);
        } else if (playing) {  /* ||  pause */
            Ui::roundRect(cx - 8, cy - 10, 6, 20, 2, c);
            Ui::roundRect(cx + 2, cy - 10, 6, 20, 2, c);
        } else {               /* >   play */
            Ui::triangle(cx - 6, cy - 11, cx + 11, cy, cx - 6, cy + 11, c);
        }
    }

    /* ---- Track dots ---- */
    if (n > 1 && n <= 20) {
        const float DOT = 8, GAP = 6;
        float total = n * DOT + (n - 1) * GAP;
        float dx = 320 - total * 0.5f;
        for (int i = 0; i < n; i++)
            Ui::circle(dx + i * (DOT + GAP) + DOT * 0.5f, 384, i == idx ? 4.5f : 3.5f,
                       i == idx ? p.accent : Ui::alpha(p.cardBorder, 0.8f));
    }

    const Ui::Hint l[] = { { "A", "Play/Pause" }, { "LR", "Seek" } };
    const Ui::Hint r[] = { { "-/+", "Track" }, { "B", "Back" } };
    Ui::footer(l, 2, r, 2);
}

/* -----------------------------------------------------------------------
 * renderFrameGRRLIB — static, called ~60 Hz from bgThread during
 * audio-only playback.  Renders the full music HUD using GRRLIB and
 * calls GRRLIB_Render() (which waits for vsync) at the end.
 *
 * GRRLIB must still be active (GRRLIB_Exit() must NOT have been called).
 * With -vo null, MPlayer's main thread never touches GX, so there is no
 * contention.
 * ----------------------------------------------------------------------- */
void MusicOverlay::renderFrameGRRLIB()
{
    MusicOverlay* mo = instance;
    if (!mo) return;

    /* Do NOT touch GRRLIB/GX until mplayer_main's init phase is done.
     * mplayer_main's common init path touches GX even with -vo null;
     * calling GRRLIB_Render() concurrently corrupts the GX FIFO and
     * produces a black screen on the second track onwards.
     * The main thread renders a static "Now Playing" screen before
     * calling wii_player_play_audio(); the Wii VI holds that frame
     * until we take over here. */
    if (!s_renderEnabled) {
        VIDEO_WaitVSync();   /* pacing (~16 ms) without touching GX */
        return;
    }

    /* Compute dt */
    uint32_t now = (uint32_t)ticks_to_millisecs(gettime());
    float dt = (mo->lastFrameTick == 0) ? 0.016f
             : (float)(now - mo->lastFrameTick) / 1000.0f;
    if (dt > 0.2f) dt = 0.033f;
    mo->lastFrameTick = now;

    mo->updateVisualizerFrame(dt);

    float pos = mo->getPosition();
    const float dur = mo->getDuration();
    /* Cap displayed position at duration — wall-clock timer can overrun
     * during cache-stall at EOF before mplayer detects end-of-stream. */
    if (dur > 0.0f && pos > dur) pos = dur;

    /* Read IR once — shared for hover detection and cursor drawing.
     * Do NOT call WPAD_ScanPads() here (the bgThread owns the pads). */
    ir_t ir;
    Input::readIR(ir);
    int newHov = 0;
    if (ir.valid) {
        int ix = (int)ir.x, iy = (int)ir.y;
        if (iy >= MP_BTN_Y && iy < MP_BTN_Y + MP_BTN_H) {
            for (int b = 0; b < 3; b++) {
                int bx = MP_BTN_X0 + b * (MP_BTN_W + MP_BTN_GAP);
                if (ix >= bx && ix < bx + MP_BTN_W) { newHov = b == 0 ? -1 : (b == 1 ? 1 : 2); break; }
            }
        }
    }
    s_btnHovered = newHov;

    drawMusicScreen(mo->tracks, mo->currentIdx, renderArtTex, pos, dur,
                    !(bool)g_mplayer_paused, newHov, mo->vizHeight, VIZ_BARS);

    /* Cursor drawn on top of everything */
    if (renderCursorTex && ir.valid) {
        orient_t orient;
        WPAD_Orientation(WPAD_CHAN_0, &orient);
        GRRLIB_DrawImg((int)ir.x - 8, (int)ir.y - 4,
                       renderCursorTex, orient.roll, 1.0f, 1.0f, 0xFFFFFFFF);
    }

    VIDEO_SetBlack(FALSE);
    GRRLIB_Render();
}

/* ======================================================================= */
/*  MusicPlayerView                                                          */
/* ======================================================================= */

MusicPlayerView::MusicPlayerView(GRRLIB_ttfFont* f,
                                  JellyfinClient& c,
                                  const JellyfinAuth& a,
                                  const std::string& srv)
    : font(f), client(c), auth(a), serverUrl(srv)
{}

void MusicPlayerView::setCursorTex(GRRLIB_texImg* tex)
{
    MusicOverlay::renderCursorTex = tex;
}

void MusicPlayerView::setTracks(const std::vector<MusicOverlay::Track>& t, int idx)
{
    tracks     = t;
    currentIdx = (idx >= 0 && idx < (int)t.size()) ? idx : 0;
}

/* -----------------------------------------------------------------------
 * run — main session loop.
 *
 * GRRLIB stays active throughout.  The bgThread calls renderFrameGRRLIB()
 * ~60 Hz during playback.  Between tracks the main thread renders a
 * static "Loading…" screen so the user never sees a black gap.
 * ----------------------------------------------------------------------- */
bool MusicPlayerView::run()
{
    if (tracks.empty()) return false;

    int idx = currentIdx;

    /* Pre-fetch URL for the first track before entering the loop. */
    std::string url, sessionId;
    {
        const MusicOverlay::Track& first = tracks[idx];
        if (!client.getAudioStreamUrl(serverUrl, auth, first.id, 0, url, sessionId)) {
            SYS_Report("[MusicPlayerView] getAudioStreamUrl failed (first): %s\n",
                       client.lastError().c_str());
            return false;
        }
    }

    /* Pause background music once before entering the playback loop. */
    MusicBGM::pause();

    /* Make the font available to the bgThread render callback. */
    MusicOverlay::renderFont = font;

    for (;;) {
        if (idx < 0 || idx >= (int)tracks.size()) break;

        const MusicOverlay::Track& tr = tracks[idx];

        /* Arm overlay (handles button input + HUD rendering from bgThread) */
        MusicOverlay overlay(tracks, idx);
        overlay.resetTiming();

        /* ---- Prepare deferred playback-start report -------------------
         * g_stream_opened_cb fires from MPlayer's main thread when the
         * HTTP stream opens.  We just set a flag; bgTick (running on the
         * bgThread) will send the actual HTTP report so Jellyfin sees the
         * transcode session as already active — dashboard shows "Transcoding". */
        s_reportNeeded      = false;
        s_startReported     = false;
        s_lastProgressTick  = 0;
        s_reportClient    = &client;
        s_reportServer    = serverUrl;
        s_reportAuth      = auth;
        s_reportItemId    = tr.id;
        s_reportSessionId = sessionId;
        g_stream_opened_cb  = onAudioStreamOpened;
        s_renderEnabled     = false;

        /* ---- Load album art for this track (main thread, blocking) ---- */
        {
            if (MusicOverlay::renderArtTex) {
                GRRLIB_FreeTexture(MusicOverlay::renderArtTex);
                MusicOverlay::renderArtTex = nullptr;
            }
            std::string imgBytes;
            if (client.getItemImageBytes(serverUrl, auth, tr.id, 210, 210, imgBytes)
                    && !imgBytes.empty()) {
                MusicOverlay::renderArtTex = loadJPEGTexture(
                    (const u8*)imgBytes.data(), (u32)imgBytes.size());
            }
        }

        /* ---- Render static "Now Playing" into both XFBs ---------------
         * The bgThread will NOT touch GRRLIB until g_stream_opened_cb sets
         * s_renderEnabled (after mplayer init completes).  The Wii VI holds
         * this frame in the meantime. */
        for (int _fi = 0; _fi < 2; ++_fi) {
            drawMusicScreen(tracks, idx, MusicOverlay::renderArtTex, 0.0f,
                            (float)(tr.runtimeTicks / 10000000LL), true, 0, nullptr,
                            MusicOverlay::VIZ_BARS);
            GRRLIB_Render();
        }

        /* ---- Set bgThread callbacks: GRRLIB HUD + button input -------- */
        wii_player_set_audio_render_cb(MusicOverlay::renderFrameGRRLIB);
        wii_player_set_music_tick(MusicOverlay::bgTick);

        /* Start async reporter thread before mplayer so it’s ready to handle
         * the deferred reportPlaybackStart without blocking the render loop. */
        s_asyncStop         = false;
        s_asyncStartPending = false;
        s_asyncProgPending  = false;
        /* Priority 30 < bgThread (40) so HTTPS never preempts the render loop.
         * CPU is yielded during vsync waits, giving the reporter enough runtime. */
        LWP_CreateThread(&s_asyncThread, asyncReporterFunc, nullptr,
                        s_asyncStack, sizeof(s_asyncStack), 30);

        /* Run playback (blocks until EOF / user stop).
         * bgThread waits (VIDEO_WaitVSync) during mplayer init, then
         * switches to animated HUD once s_renderEnabled is set.
         * Set g_wiifin_known_duration so MPlayer's main-loop hook can assign
         * g_mplayer_duration from the first iteration — that's the signal
         * getPosition() uses to start counting, avoiding the ~3 s init delay. */
        g_wiifin_known_duration = overlay.getDuration();
        wii_player_play_audio(url.c_str());
        g_wiifin_known_duration = 0.0f;

        /* --- bgThread is now joined; stop async reporter thread. --- */
        s_asyncStop = true;
        if (s_asyncThread != LWP_THREAD_NULL) {
            LWP_JoinThread(s_asyncThread, nullptr);
            s_asyncThread = LWP_THREAD_NULL;
        }
        /* Blank the display while we fill both GRRLIB framebuffers with the
         * transition spinner.  Without this, one of the XFBs may still
         * contain a stale frame from a prior video session and briefly
         * flashes through before the spinner render completes.
         * Blank -> fill -> unblank keeps the transition clean. */
        VIDEO_SetBlack(TRUE);
        VIDEO_Flush();

        /* Render a transition frame (navy bg + ring spinner) matching the
         * post-play spinner used by App.cpp so there is no abrupt colour
         * jump between the music player and the library view. */
        {
            extern unsigned char data_ring_png[];
            GRRLIB_texImg* ringTex = GRRLIB_LoadTexture(data_ring_png);
            for (int _fi = 0; _fi < 2; ++_fi) {
                Ui::background(false);
                Ui::spinner(ringTex, 320, 240);
                GRRLIB_Render();
            }
            GRRLIB_FreeTexture(ringTex);
        }
        /* Both XFBs now contain the spinner — safe to unblank. */
        VIDEO_SetBlack(FALSE);
        VIDEO_Flush();

        /* If the deferred start report was queued but never executed
         * (e.g. track was very short), send it synchronously now. */
        if ((s_asyncStartPending || !s_startReported) && s_reportClient) {
            s_reportClient->reportPlaybackStart(s_reportServer, s_reportAuth,
                                                s_reportItemId, s_reportItemId,
                                                s_reportSessionId);
            s_asyncStartPending = false;
        }

        /* Report stopped and clean up encoding session. */
        {
            long long posTicks = (long long)(overlay.getPosition() * 10000000.0f);
            client.reportPlaybackStopped(serverUrl, auth, tr.id, tr.id, sessionId, posTicks);
            if (!sessionId.empty())
                client.deleteActiveEncoding(serverUrl, auth, sessionId);
        }

        /* Determine next track index */
        bool doStop  = overlay.requestStop;
        int  nextIdx = (overlay.pendingNextIdx >= 0) ? overlay.pendingNextIdx : idx + 1;

        /* Pre-fetch URL for the next track */
        std::string nextUrl, nextSessionId;
        bool hasNext = false;
        if (!doStop && nextIdx >= 0 && nextIdx < (int)tracks.size()) {
            hasNext = client.getAudioStreamUrl(serverUrl, auth,
                                               tracks[nextIdx].id, 0,
                                               nextUrl, nextSessionId);
            if (!hasNext)
                SYS_Report("[MusicPlayerView] getAudioStreamUrl failed (next): %s\n",
                           client.lastError().c_str());
        }

        if (doStop || !hasNext)
            break;

        /* Advance to next track using pre-fetched URL */
        idx       = nextIdx;
        url       = nextUrl;
        sessionId = nextSessionId;
    }

    g_stream_opened_cb     = nullptr;
    s_reportClient         = nullptr;
    MusicOverlay::instance = nullptr;
    wii_player_set_music_tick(nullptr);

    if (MusicOverlay::renderArtTex) {
        GRRLIB_FreeTexture(MusicOverlay::renderArtTex);
        MusicOverlay::renderArtTex = nullptr;
    }

    MusicBGM::resume();

    return false;
}
