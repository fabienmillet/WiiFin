#pragma once
#include <grrlib.h>

/* -----------------------------------------------------------------------
 * Text — cached TrueType text rendering (replaces GRRLIB_PrintfTTF and
 * GRRLIB_WidthTTF, same arguments).
 *
 * GRRLIB rasterises every glyph with FreeType on each call and plots it one
 * GX point per pixel, so a 20-character label costs ~1 ms per frame, and a
 * width query costs as much as drawing.  Here each glyph is rasterised once
 * per (font, size) into a small texture and drawn as a single quad; advances
 * and kerning are cached too.  Strings are decoded as UTF-8.
 * ----------------------------------------------------------------------- */
namespace Text {
    void print(int x, int y, GRRLIB_ttfFont* font, const char* utf8,
               unsigned int size, u32 color);

    u32  width(GRRLIB_ttfFont* font, const char* utf8, unsigned int size);

    /* Free every cached glyph (call before GRRLIB_FreeTTF). */
    void clearCache();

    /* How drawing units map to frame pixels horizontally: frameX =
     * (x - left) * scale.  In 16:9 the drawing space is wider than the
     * 640-pixel frame (scale 0.75); glyphs are then rasterised that much
     * narrower and placed on whole frame pixels, so they stay sharp instead
     * of being shrunk by the GPU.  Clears the cache when it changes. */
    void setFrameMapping(float left, float scale);
}
