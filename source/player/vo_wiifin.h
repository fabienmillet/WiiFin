#pragma once
/*
 * vo_wiifin.h — frame hand-off between MPlayer's video output and WiiFin.
 *
 * vo_wiifin.c registers itself as MPlayer's "gx" video driver (it replaces
 * vo_gx.o and gx_supp.o from libmplayer.a at link time).  It never touches
 * GX or VIDEO: decoded frames are converted to GX I8 tiled textures on the
 * MPlayer thread and published through a triple buffer.  WiiFin's main loop
 * picks up the latest frame with wiifin_video_acquire(), draws it (see
 * VideoSurface), then calls wiifin_video_release() once the GPU is done.
 */

#include <gctypes.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    void* y;                 /* GX_TF_I8 textures, 32-byte aligned            */
    void* u;
    void* v;
    u16   yw, yh;            /* luma texture size (padded to 8x4 tiles)        */
    u16   uvw, uvh;          /* chroma texture size                            */
    f32   ys, yt;            /* texcoord extent of the visible luma image      */
    f32   uvs, uvt;          /* texcoord extent of the visible chroma image    */
    f32   aspect;            /* display aspect ratio (width / height)          */
    /* Smooth motion: when blend > 0, the next frame (y2/u2/v2, same sizes)
     * is drawn over this one with that opacity. */
    void* y2;
    void* u2;
    void* v2;
    f32   blend;
} wiifin_video_frame;

/* Smooth motion (frame blending on the refresh where the picture changes),
 * on by default; set from the settings. */
extern int g_wiifin_smooth_motion;

/* Main thread: get the most recent frame.  Returns 1 and fills *out when a
 * frame is available; the frame stays valid until wiifin_video_release(). */
int  wiifin_video_acquire(wiifin_video_frame* out);

/* Main thread: call after GRRLIB_Render() (GPU finished reading textures). */
void wiifin_video_release(void);

/* Writes the cadence statistics gathered since the last call to the log. */
void wiifin_video_report(const char* when);

/* Frames MPlayer has output since the stream started (decoded in time:
 * those it dropped behind schedule are not counted). */
unsigned wiifin_video_frames(void);

/* Main thread, player stopped: forget the current frame. */
void wiifin_video_clear(void);

#ifdef __cplusplus
}
#endif
