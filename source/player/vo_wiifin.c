/*
 * vo_wiifin.c — WiiFin video output driver for MPlayer CE.
 *
 * Defines `video_out_gx` plus the handful of gx_supp.c entry points that the
 * rest of libmplayer.a calls (mplayer.o, getch2-gekko.o, plat_gekko.o), so
 * the linker never pulls the archive's own vo_gx.o / gx_supp.o.  Those took
 * over GX and VIDEO for the whole playback session, forcing WiiFin to shut
 * GRRLIB down.  This driver only converts frames to textures; WiiFin keeps
 * owning the display and draws the video like any other texture.
 *
 * Frame flow (NBUF buffers, each FREE, WRITING, QUEUED or FRONT):
 *   MPlayer thread  draw_image() fills a FREE buffer and notes its timestamp
 *                   flip_page()  queues it, with the wall-clock time it came
 *   main thread     acquire()    once per vsync, takes the newest QUEUED
 *                                frame whose display time has come
 *
 * Why not simply show the latest frame: MPlayer flips each frame at its own
 * clock's time, give or take a few milliseconds.  A 24 fps film on a 60 Hz
 * screen must alternate 2 and 3 vsyncs per frame; when flips land right on
 * a vsync, that jitter turns 2/3 into 1/4 and motion judders back and forth.
 * Display times are therefore derived from the frame timestamps:
 *   due = pts + offset + LATENCY
 * where offset (wall clock minus pts) is smoothed over many frames, then
 * held with hysteresis: it only moves when the measured value has drifted
 * more than HYST_MS away.  A slowly drifting offset would otherwise hover
 * at a vsync boundary and flip frames one refresh early or late again and
 * again; held, the drift costs one clean catch-up now and then.  The writer
 * never touches the FRONT buffer, so the GPU reads it without locking.
 *
 * Planar->tiled copy and the ABI below are derived from MPlayer CE's
 * vo_gx.c / gx_supp.c (GPL-2.0-or-later).
 */

#include <stdint.h>
#include <string.h>
#include <malloc.h>
#include <unistd.h>
#include <gccore.h>
#include <ogc/lwp_watchdog.h>        /* gettime(), ticks_to_microsecs()      */
#include <ogc/machine/processor.h>   /* _CPU_ISR_Disable / _CPU_ISR_Restore */

#include "vo_wiifin.h"

/* ---- MPlayer ABI — must match libmplayer.a (libvo/video_out.h) ---------- */

typedef struct {
    const char* name;
    const char* short_name;
    const char* author;
    const char* comment;
} vo_info_t;

typedef struct {
    const vo_info_t* info;
    int  (*preinit)(const char* arg);
    int  (*config)(uint32_t width, uint32_t height, uint32_t d_width,
                   uint32_t d_height, uint32_t flags, char* title, uint32_t format);
    int  (*control)(uint32_t request, void* data, ...);
    int  (*draw_frame)(uint8_t* src[]);
    int  (*draw_slice)(uint8_t* src[], int stride[], int w, int h, int x, int y);
    void (*draw_osd)(void);
    void (*flip_page)(void);
    void (*check_events)(void);
    void (*uninit)(void);
} vo_functions_t;

/* Leading fields of mp_image_t (libmpcodecs/mp_image.h).  Offsets verified
 * against vo_gx.o: planes @44, stride @60 (so pict_type is @84). */
typedef struct {
    unsigned int  flags;
    unsigned char type;
    int           number;
    unsigned char bpp;
    unsigned int  imgfmt;
    int           width, height;
    int           x, y, w, h;
    unsigned char* planes[4];
    int           stride[4];
    char*         qscale;
    int           qstride;
    int           pict_type;     /* 0 unknown, 1 I, 2 P, 3 B */
} mp_image_t;

#define MP_IMGFLAG_PLANAR    0x100

#define VOCTRL_QUERY_FORMAT  2
#define VOCTRL_FULLSCREEN    5
#define VOCTRL_DRAW_IMAGE    13
#define VOCTRL_GET_PANSCAN   15
#define VOCTRL_SET_PANSCAN   16

#define VO_TRUE     1
#define VO_FALSE    0
#define VO_ERROR   -1
#define VO_NOTIMPL -3

#define VFCAP_CSP_SUPPORTED        0x1
#define VFCAP_CSP_SUPPORTED_BY_HW  0x2
#define VFCAP_HWSCALE_UP           0x10
#define VFCAP_HWSCALE_DOWN         0x20
#define VFCAP_ACCEPT_STRIDE        0x400
#define VOCAP_NOSLICES             0x8000

#define IMGFMT_YV12 0x32315659
#define IMGFMT_I420 0x30323449
#define IMGFMT_IYUV 0x56555949

extern int vo_dwidth, vo_dheight, vo_screenwidth, vo_screenheight, vo_fs;
/* Video timer of the frame being decoded, in 90 kHz units (set by mplayer.c
 * before decoding, so it belongs to the frame reaching draw_image). */
extern int vo_pts;

/* ---- Frame buffers ------------------------------------------------------ */

typedef struct { u8* y; u8* u; u8* v; } frame_buf;

#define NBUF 6
/* BUF_NEXT: queued frame also drawn this refresh, blended over the front */
enum { BUF_FREE, BUF_WRITING, BUF_QUEUED, BUF_FRONT, BUF_NEXT };

int g_wiifin_smooth_motion = 1;

static frame_buf s_buf[NBUF];
static u32 s_buf_ysize, s_buf_uvsize;    /* allocated plane sizes */

static u32 s_img_w, s_img_h;             /* decoded image size   */
static u32 s_chroma_w, s_chroma_h;
static u16 s_yw, s_yh, s_uvw, s_uvh;     /* texture sizes        */
static f32 s_ys, s_yt, s_uvs, s_uvt;
static f32 s_aspect = 16.0f / 9.0f;

static volatile int s_state[NBUF];       /* BUF_*                                 */
static double       s_pts[NBUF];         /* frame timestamp (ms, MPlayer timer)   */
static u64          s_seq[NBUF];         /* queue order                           */
static u64          s_next_seq = 1;
static int          s_write = -1;        /* buffer being filled, -1 none          */
static int          s_front = -1;        /* buffer on screen, -1 none             */
static volatile int s_front_in_use = 0;
static int s_write_dirty = 0;            /* s_buf[s_write] holds an unpublished frame */
/* Every frame goes to the screen from the first one on: each stream starts
 * on a keyframe (Jellyfin copies the video only when playing from 0, and
 * re-encodes otherwise).  Waiting for an I-frame here made things worse:
 * MPlayer drops the first frame or two when it starts late, the keyframe
 * among them, and a copied DivX/Xvid has its next one 10 s later, so the
 * sound played over a black screen for that long.  A frame whose display
 * is dropped is still decoded, so the ones after it are whole. */
static int s_first_shown = 0;            /* "[vo] first picture" logged */
static volatile unsigned s_frames = 0;   /* output since the stream started */

/* wall clock - pts, smoothed (ms); see the header comment */
#define LATENCY_MS   12.0
/* Smooth motion needs the next frame one refresh before it is due */
#define LATENCY_SMOOTH_MS 32.0
#define RESYNC_MS   250.0
#define HYST_MS       6.0
static double s_offset  = 0.0;      /* measured, smoothed           */
/* Timestamp of the last frame handed to the queue.  While paused, MPlayer
 * redraws by sending its current image again; that buffer has already been
 * recycled by the decoder and holds an older reference frame (often the last
 * keyframe), yet it comes with the current, no longer moving timestamp.
 * Queued as "newest" it made the picture jump back.  A frame whose
 * timestamp does not move forward is therefore ignored. */
static double s_last_pts    = 0.0;
static int    s_have_last   = 0;
static double s_held    = 0.0;      /* used for display decisions   */
static int    s_have_offset = 0;

static double now_ms(void) { return ticks_to_microsecs(gettime()) / 1000.0; }

/* Cadence statistics for the log (wiifin_video_report): how long each frame
 * stayed on screen, in refreshes; 24 fps on 60 Hz should be only 2s and 3s. */
static struct {
    u32 queued, shown, skipped, back, resync, blended, refreshes;
    u32 hold[6];                 /* 1, 2, 3, 4, 5, 6+ refreshes */
    u32 curHold;                 /* refreshes of the frame on screen */
    u32 retraceStart;            /* s_retraces at the first acquire */
    double frontPts;
} s_st;

/* Time of the last vertical retrace, taken in the retrace interrupt.  The
 * display decision uses it instead of "now": the main loop reaches acquire()
 * a variable few ms after each vsync, and that jitter alone was enough to
 * push frames due near a vsync boundary one refresh early or late. */
static volatile u64 s_retrace_ticks = 0;
static volatile double s_vsync_ms = 1000.0 / 59.94;   /* refresh period, measured */
static VIRetraceCallback s_prev_retrace = NULL;
static int s_retrace_hooked = 0;

static volatile u32 s_retraces = 0;   /* for the stats: refreshes the TV showed */
static void on_retrace(u32 count)
{
    u64 now = gettime();
    ++s_retraces;
    if (s_retrace_ticks) {
        double d = ticks_to_microsecs(now - s_retrace_ticks) / 1000.0;
        if (d > 15.0 && d < 22.0) s_vsync_ms += (d - s_vsync_ms) * 0.05;   /* 50/60 Hz */
    }
    s_retrace_ticks = now;
    if (s_prev_retrace) s_prev_retrace(count);
}

/* Called on every acquire, not once: MPlayer's log console swaps the
 * post-retrace callback each time MPlayer starts (log_console.o saves and
 * restores it), which unhooked this one after the first session.  The
 * frozen timestamp then made every later frame "not due yet": audio played,
 * the picture never came back until WiiFin was restarted.  Only the callback
 * found at the very first hook is chained; later swaps are simply undone. */
static void hook_retrace(void)
{
    VIRetraceCallback cur = VIDEO_SetPostRetraceCallback(on_retrace);
    if (!s_retrace_hooked) {
        s_retrace_hooked = 1;
        s_prev_retrace = cur == on_retrace ? NULL : cur;
    }
}

static double vsync_ms(void)
{
    u64 t = s_retrace_ticks;
    double now = now_ms();
    if (!t) return now;
    double r = ticks_to_microsecs(t) / 1000.0;
    /* no retrace for 3 refreshes: the hook is gone, use the clock */
    return now - r > 50.0 ? now : r;
}

/* Forget every queued frame (caller holds the ISR lock). */
static void drop_queue_locked(void)
{
    for (int i = 0; i < NBUF; ++i)
        if (s_state[i] == BUF_QUEUED || s_state[i] == BUF_NEXT) s_state[i] = BUF_FREE;
}

static void clear_buf(frame_buf* b)
{
    memset(b->y, 16, s_buf_ysize);       /* BT.601 black */
    memset(b->u, 128, s_buf_uvsize);
    memset(b->v, 128, s_buf_uvsize);
    DCFlushRange(b->y, s_buf_ysize);
    DCFlushRange(b->u, s_buf_uvsize);
    DCFlushRange(b->v, s_buf_uvsize);
}

static void free_bufs(void)
{
    for (int i = 0; i < NBUF; ++i) {
        free(s_buf[i].y); free(s_buf[i].u); free(s_buf[i].v);
        s_buf[i].y = s_buf[i].u = s_buf[i].v = NULL;
    }
    s_buf_ysize = s_buf_uvsize = 0;
}

static int alloc_bufs(u32 ysize, u32 uvsize)
{
    if (s_buf[0].y && ysize == s_buf_ysize && uvsize == s_buf_uvsize)
        return 1;
    free_bufs();
    for (int i = 0; i < NBUF; ++i) {
        s_buf[i].y = memalign(32, ysize);
        s_buf[i].u = memalign(32, uvsize);
        s_buf[i].v = memalign(32, uvsize);
        if (!s_buf[i].y || !s_buf[i].u || !s_buf[i].v) { free_bufs(); return 0; }
    }
    s_buf_ysize  = ysize;
    s_buf_uvsize = uvsize;
    return 1;
}

/* Planar 8-bit -> GX I8 tiles (8x4 texels, 32 bytes per tile).  Rows/columns
 * past the image edge repeat the last one so filtering stays clean. */
static void copy_plane(u8* dst, u16 tw, u16 th, const u8* src, int stride,
                       u32 w, u32 h)
{
    for (u32 ty = 0; ty < th; ty += 4) {
        const u8* rows[4];
        for (int r = 0; r < 4; ++r) {
            u32 sy = ty + r;
            if (sy >= h) sy = h - 1;
            rows[r] = src + sy * stride;
        }
        for (u32 tx = 0; tx < tw; tx += 8) {
            for (int r = 0; r < 4; ++r) {
                if (tx + 8 <= w) {
                    memcpy(dst, rows[r] + tx, 8);
                } else {
                    for (int i = 0; i < 8; ++i) {
                        u32 sx = tx + i;
                        dst[i] = rows[r][sx < w ? sx : w - 1];
                    }
                }
                dst += 8;
            }
        }
    }
}

/* Buffer for the next decoded frame: reuse an unpublished one (dropped by
 * MPlayer before flip), else a free one, else steal the oldest queued. */
static int take_write_buffer(void)
{
    u32 level;
    _CPU_ISR_Disable(level);
    int w = s_write;
    if (w < 0) {
        for (int i = 0; i < NBUF && w < 0; ++i)
            if (s_state[i] == BUF_FREE) w = i;
        if (w < 0) {
            u64 best = ~0ull;
            for (int i = 0; i < NBUF; ++i)
                if (s_state[i] == BUF_QUEUED && s_seq[i] < best) { best = s_seq[i]; w = i; }
            if (w >= 0) ++s_st.skipped;   /* queue full: never shown */
        }
        if (w >= 0) s_state[w] = BUF_WRITING;
        s_write = w;
    }
    _CPU_ISR_Restore(level);
    return w;
}

static void copy_frame(uint8_t* planes[], int stride[])
{
    double pts = vo_pts / 90.0;            /* 90 kHz -> ms */
    /* redraw of an already shown instant (see s_last_pts); a big step back
     * is a real discontinuity (90 kHz counter wrap) and goes through */
    if (s_have_last && pts <= s_last_pts && s_last_pts - pts < 10000.0) return;
    int w = take_write_buffer();
    if (w < 0) return;
    frame_buf* b = &s_buf[w];
    if (!b->y || !planes[0] || !planes[1] || !planes[2]) return;
    s_pts[w] = pts;
    copy_plane(b->y, s_yw,  s_yh,  planes[0], stride[0], s_img_w,    s_img_h);
    copy_plane(b->u, s_uvw, s_uvh, planes[1], stride[1], s_chroma_w, s_chroma_h);
    copy_plane(b->v, s_uvw, s_uvh, planes[2], stride[2], s_chroma_w, s_chroma_h);
    DCFlushRange(b->y, s_buf_ysize);
    DCFlushRange(b->u, s_buf_uvsize);
    DCFlushRange(b->v, s_buf_uvsize);
    s_write_dirty = 1;
}

/* ---- vo_functions_t ----------------------------------------------------- */

static const vo_info_t info = {
    "WiiFin GX texture output",
    "gx",
    "WiiFin",
    ""
};

static int preinit(const char* arg)
{
    (void)arg;
    return VO_FALSE;
}

static int config(uint32_t width, uint32_t height, uint32_t d_width,
                  uint32_t d_height, uint32_t flags, char* title, uint32_t format)
{
    (void)title; (void)format;
    u32 level;

    /* Stop showing frames before the buffers may be reallocated, and wait
     * for the main thread to finish drawing the one it holds. */
    _CPU_ISR_Disable(level);
    for (int i = 0; i < NBUF; ++i) s_state[i] = BUF_FREE;
    s_front = -1;
    s_write = -1;
    s_have_offset = 0;
    s_have_last   = 0;
    _CPU_ISR_Restore(level);
    s_write_dirty   = 0;
    s_first_shown   = 0;
    s_frames        = 0;
    for (int i = 0; s_front_in_use && i < 100; ++i) usleep(2000);

    s_img_w    = width;
    s_img_h    = height;
    s_chroma_w = width  >> 1;            /* 4:2:0 only, see query_format */
    s_chroma_h = height >> 1;

    s_yw  = (u16)(((width + 7) & ~7) > 1024 ? 1024 : ((width + 7) & ~7));
    s_yh  = (u16)(((height + 3) & ~3) > 1024 ? 1024 : ((height + 3) & ~3));
    s_uvw = (u16)((s_chroma_w + 7) & ~7);
    s_uvh = (u16)((s_chroma_h + 3) & ~3);

    s_ys  = (f32)width        / (f32)s_yw;
    s_yt  = (f32)height       / (f32)s_yh;
    s_uvs = (f32)s_chroma_w   / (f32)s_uvw;
    s_uvt = (f32)s_chroma_h   / (f32)s_uvh;

    s_aspect = (d_width && d_height) ? (f32)d_width / (f32)d_height
                                     : (f32)width / (f32)height;

    if (!alloc_bufs((u32)s_yw * s_yh, (u32)s_uvw * s_uvh))
        return VO_ERROR;
    for (int i = 0; i < NBUF; ++i) clear_buf(&s_buf[i]);

    vo_fs           = flags & 1;
    vo_dwidth       = d_width;
    vo_dheight      = d_height;
    vo_screenwidth  = 640;
    vo_screenheight = 480;
    return VO_FALSE;
}

static int query_format(uint32_t format)
{
    if (format == IMGFMT_YV12 || format == IMGFMT_I420 || format == IMGFMT_IYUV)
        return VFCAP_CSP_SUPPORTED | VFCAP_CSP_SUPPORTED_BY_HW |
               VFCAP_HWSCALE_UP | VFCAP_HWSCALE_DOWN |
               VFCAP_ACCEPT_STRIDE | VOCAP_NOSLICES;
    return VO_FALSE;
}

static int control(uint32_t request, void* data, ...)
{
    switch (request) {
    case VOCTRL_QUERY_FORMAT:
        return query_format(*(uint32_t*)data);
    case VOCTRL_DRAW_IMAGE: {
        mp_image_t* mpi = (mp_image_t*)data;
        if (mpi->flags & MP_IMGFLAG_PLANAR)
            copy_frame(mpi->planes, mpi->stride);
        return VO_TRUE;
    }
    case VOCTRL_FULLSCREEN:
    case VOCTRL_GET_PANSCAN:
    case VOCTRL_SET_PANSCAN:
        return VO_TRUE;
    default:
        return VO_NOTIMPL;
    }
}

static int draw_frame(uint8_t* src[])
{
    (void)src;
    return VO_ERROR;
}

static int draw_slice(uint8_t* src[], int stride[], int w, int h, int x, int y)
{
    (void)w; (void)h; (void)x; (void)y;
    copy_frame(src, stride);
    return VO_FALSE;
}

static void draw_osd(void)
{
    /* MPlayer's OSD is not used: WiiFin draws its own player UI. */
}

static void flip_page(void)
{
    if (!s_write_dirty || s_write < 0) return;
    s_write_dirty = 0;
    double t = now_ms();
    u32 level;
    _CPU_ISR_Disable(level);
    int w = s_write;
    /* track wall clock - pts; a jump (seek, pause, restart) resyncs */
    double sample = t - s_pts[w];
    if (!s_have_offset || sample - s_offset > RESYNC_MS || s_offset - sample > RESYNC_MS) {
        s_offset = s_held = sample;
        if (s_have_offset) ++s_st.resync;
        s_have_offset = 1;
        drop_queue_locked();
    } else {
        s_offset += (sample - s_offset) / 32.0;
        if (s_offset - s_held > HYST_MS || s_held - s_offset > HYST_MS) s_held = s_offset;
    }
    s_seq[w]   = s_next_seq++;
    s_state[w] = BUF_QUEUED;
    ++s_st.queued;
    s_write    = -1;
    s_last_pts  = s_pts[w];
    s_have_last = 1;
    _CPU_ISR_Restore(level);
    ++s_frames;
    if (!s_first_shown) {
        s_first_shown = 1;
        SYS_Report("[vo] first picture at %.2f s\n", s_last_pts / 1000.0);
    }
}

static void check_events(void) {}

static void uninit(void)
{
    /* Keep the buffers: the last frame stays on screen (dimmed, under the
     * loading spinner) while WiiFin restarts the stream. */
}

const vo_functions_t video_out_gx = {
    &info, preinit, config, control, draw_frame, draw_slice,
    draw_osd, flip_page, check_events, uninit
};

/* ---- gx_supp.c entry points still referenced by libmplayer.a ------------
 * mplayer.o calls these to keep the screen alive while loading or paused;
 * WiiFin's main loop redraws every frame, so they have nothing to do. */
void mpgxWaitDrawDone(void)      {}
void mpgxRunOverlay(void)        {}
void mpgxPushFrame(void)         {}
void mpgxForceLoadingFrame(void) {}
void mpgxUpdateSquare(void)      {}
void mpviClear(void)             {}

/* ---- WiiFin side -------------------------------------------------------- */

int wiifin_video_acquire(wiifin_video_frame* out)
{
    hook_retrace();
    const double t   = vsync_ms();
    const double V   = s_vsync_ms;
    const int smooth = g_wiifin_smooth_motion;
    const double lat = smooth ? LATENCY_SMOOTH_MS : LATENCY_MS;
    u32 level;
    _CPU_ISR_Disable(level);
    /* last refresh's blended frame is just queued again */
    for (int i = 0; i < NBUF; ++i)
        if (s_state[i] == BUF_NEXT) s_state[i] = BUF_QUEUED;
    /* newest queued frame that is due; older ones are dropped */
    int pick = -1;
    u64 pickSeq = 0;
    for (int i = 0; i < NBUF; ++i) {
        if (s_state[i] != BUF_QUEUED) continue;
        if (s_pts[i] + s_held + lat > t) continue;
        if (pick < 0 || s_seq[i] > pickSeq) { pick = i; pickSeq = s_seq[i]; }
    }
    if (!s_st.refreshes) s_st.retraceStart = s_retraces;
    ++s_st.refreshes;
    ++s_st.curHold;
    if (pick >= 0) {
        for (int i = 0; i < NBUF; ++i)
            if (s_state[i] == BUF_QUEUED && s_seq[i] < pickSeq) { s_state[i] = BUF_FREE; ++s_st.skipped; }
        if (s_front >= 0) {
            u32 h = s_st.curHold - 1;            /* this refresh shows the new one */
            if (h >= 1) ++s_st.hold[h > 6 ? 5 : h - 1];
            if (s_pts[pick] < s_st.frontPts) ++s_st.back;
        }
        s_st.curHold  = 1;
        s_st.frontPts = s_pts[pick];
        ++s_st.shown;
        if (s_front >= 0) s_state[s_front] = BUF_FREE;   /* GPU done: released last frame */
        s_front = pick;
        s_state[pick] = BUF_FRONT;
    }

    /* Smooth motion: the refresh shown next covers [t, t + V] on the
     * frames' clock.  If the following frame becomes due inside it, show
     * both, weighted by how long each one covers (mpv's "oversample"):
     * 24 fps on 60 Hz then moves evenly instead of the 2-3-2-3 judder,
     * and refreshes without a frame change stay sharp. */
    int next = -1;
    float blend = 0.0f;
    if (smooth && s_front >= 0) {
        u64 frontSeq = s_seq[s_front], best = ~0ull;
        for (int i = 0; i < NBUF; ++i)
            if (s_state[i] == BUF_QUEUED && s_seq[i] > frontSeq && s_seq[i] < best) {
                best = s_seq[i]; next = i;
            }
        if (next >= 0) {
            double due = s_pts[next] + s_held + lat;
            double w = (t + V - due) / V;
            if (w > 0.08 && w < 0.92) {
                blend = (float)w;
                ++s_st.blended;
                s_state[next] = BUF_NEXT;   /* the writer must not reuse it */
            } else {
                next = -1;
            }
        }
    }

    int ok = s_front >= 0 && s_buf[s_front].y;
    if (ok) {
        s_front_in_use = 1;
        out->y = s_buf[s_front].y;
        out->u = s_buf[s_front].u;
        out->v = s_buf[s_front].v;
        out->yw = s_yw;   out->yh = s_yh;
        out->uvw = s_uvw; out->uvh = s_uvh;
        out->ys = s_ys;   out->yt = s_yt;
        out->uvs = s_uvs; out->uvt = s_uvt;
        out->aspect = s_aspect;
        out->blend = next >= 0 ? blend : 0.0f;
        out->y2 = next >= 0 ? s_buf[next].y : NULL;
        out->u2 = next >= 0 ? s_buf[next].u : NULL;
        out->v2 = next >= 0 ? s_buf[next].v : NULL;
    }
    _CPU_ISR_Restore(level);
    return ok;
}

unsigned wiifin_video_frames(void) { return s_frames; }

void wiifin_video_release(void)
{
    s_front_in_use = 0;
}

void wiifin_video_clear(void)
{
    u32 level;
    _CPU_ISR_Disable(level);
    drop_queue_locked();
    if (s_front >= 0) { s_state[s_front] = BUF_FREE; s_front = -1; }
    s_have_offset = 0;
    s_have_last   = 0;
    _CPU_ISR_Restore(level);
}

void wiifin_video_report(const char* when)
{
    u32 level;
    _CPU_ISR_Disable(level);
    __typeof__(s_st) st = s_st;
    memset(&s_st, 0, sizeof(s_st));
    u32 retraces = s_retraces - st.retraceStart;
    _CPU_ISR_Restore(level);
    if (!st.queued) return;
    SYS_Report("[cadence] %s: %u frames, shown %u, skipped %u, backwards %u, resyncs %u, "
               "refresh %.2f ms, holds 1:%u 2:%u 3:%u 4:%u 5:%u 6+:%u, blended %u, "
               "drawn %u of %u TV refreshes\n",
               when, st.queued, st.shown, st.skipped, st.back, st.resync, s_vsync_ms,
               st.hold[0], st.hold[1], st.hold[2], st.hold[3], st.hold[4], st.hold[5],
               st.blended, st.refreshes, retraces);
}
