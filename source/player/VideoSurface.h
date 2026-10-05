#pragma once

/* -----------------------------------------------------------------------
 * VideoSurface — draws the latest MPlayer frame inside a GRRLIB frame.
 *
 * Call between GRRLIB drawing calls (main thread only):
 *     GRRLIB_FillScreen(...);
 *     VideoSurface::draw();          // video, letterboxed
 *     ... GRRLIB UI on top ...
 *     GRRLIB_Render();
 *     VideoSurface::endFrame();      // GPU is done with the textures
 *
 * The YUV->RGB conversion runs in the TEV (12 stages, from MPlayer CE's
 * gx_supp.c); GRRLIB's single-stage state is restored afterwards.
 * ----------------------------------------------------------------------- */
namespace VideoSurface {
    /* Draws the current frame fitted into the 640x480 screen (taking 16:9
     * TVs into account).  Returns false if no frame is available yet. */
    bool draw();

    /* Must follow every draw() that returned true, after GRRLIB_Render(). */
    void endFrame();

    /* Fit: the whole picture, with black bars.  Fill: zoomed until the
     * screen is full, cropping what overflows (4:3 crop of a wide film on a
     * CRT, or the black frame of a 4:3 show stored as 16:9). */
    enum class Zoom { Fit = 0, Fill = 1 };
    void setZoom(Zoom z);
    Zoom zoom();
}
