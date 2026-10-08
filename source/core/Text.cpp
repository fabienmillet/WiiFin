#include "Text.h"

#include <ft2build.h>
#include FT_FREETYPE_H
#include <malloc.h>
#include <math.h>
#include <string.h>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include <ogc/system.h>   /* SYS_Report */
#include <utility>

/* GRRLIB's 2D model-view matrix (defined in GRRLIB_core.c, not in its headers) */
extern Mtx GXmodelView2D;

namespace {

struct Glyph {
    GXTexObj tex;
    u8*      data    = nullptr;   /* GX_TF_IA8 texels; nullptr for blank glyphs */
    u16      w = 0, h = 0;        /* bitmap size plus a 1-texel clear border    */
    f32      s = 0, t = 0;        /* texcoord extent (bitmap / padded texture)  */
    s16      left = 0, top = 0;   /* bitmap offset from the pen position        */
    float    advance = 0;         /* drawing units                              */
    u32      index   = 0;         /* FreeType glyph index, for kerning          */
};

struct FaceSize {
    std::unordered_map<u32, Glyph> glyphs;    /* by code point            */
    std::unordered_map<u64, float> kerning;   /* by (left << 32 | right), drawing units */
};

std::map<std::pair<void*, unsigned>, FaceSize> s_cache;

/* for the characters a font lacks, tried in order: the built-in Japanese
 * one, then the SD card's (fonts/: Chinese, Korean...) */
std::vector<GRRLIB_ttfFont*> s_fallbacks;

float s_left  = 0.0f;   /* drawing x of frame pixel 0         */
float s_scale = 1.0f;   /* frame pixels per drawing unit (x)  */

FaceSize& faceSize(GRRLIB_ttfFont* font, unsigned size)
{
    return s_cache[std::make_pair(font->face, size)];
}

void setSize(FT_Face face, unsigned size)
{
    /* narrower pixels when the frame is squeezed (16:9), see setFrameMapping */
    FT_UInt w = (FT_UInt)(size * s_scale + 0.5f);
    if (FT_Set_Pixel_Sizes(face, w ? w : 1, size) != 0)
        FT_Set_Pixel_Sizes(face, 0, 12);   /* same fallback as GRRLIB */
}

/* Decode one UTF-8 code point; bytes that don't form a valid sequence are
 * taken as Latin-1. */
u32 nextCodePoint(const unsigned char*& s)
{
    u32 c = *s++;
    if (c < 0x80) return c;
    int extra;
    u32 cp;
    if      ((c & 0xE0) == 0xC0) { extra = 1; cp = c & 0x1F; }
    else if ((c & 0xF0) == 0xE0) { extra = 2; cp = c & 0x0F; }
    else if ((c & 0xF8) == 0xF0) { extra = 3; cp = c & 0x07; }
    else return c;
    for (int i = 0; i < extra; ++i) {
        if ((s[i] & 0xC0) != 0x80) return c;
        cp = (cp << 6) | (s[i] & 0x3F);
    }
    s += extra;
    return cp;
}

/* Rasterise a glyph once into an IA8 texture (intensity 255, alpha =
 * coverage) so the vertex colour tints it like GRRLIB's point plotting.
 * The bitmap sits inside a 1-texel transparent border and is sampled with
 * linear filtering: drawn 1:1 on whole pixels that is exactly the bitmap,
 * and when the picture is scaled (screen area setting) thin strokes and
 * edge rows blend instead of being skipped. */
/* Characters drawn as something else, whatever the font has: every kind
 * of space (no-break, the narrow one of French typography...) as a space;
 * format characters (zero-width spaces, BOM, soft hyphen, direction
 * marks) as nothing (0); and C1 controls, which are Windows-1252 bytes
 * mis-converted (subtitles: ’ … – and the no-break space), as what they
 * were.  cp itself: drawn as it is. */
u32 normalize(u32 cp)
{
    if (cp == 0x00A0 || (cp >= 0x2000 && cp <= 0x200A) || cp == 0x202F ||
        cp == 0x205F || cp == 0x3000 || cp == 0x2028 || cp == 0x2029)
        return ' ';
    if ((cp >= 0x200B && cp <= 0x200F) || (cp >= 0x2060 && cp <= 0x2064) || cp == 0xFEFF ||
        cp == 0x00AD || (cp >= 0x202A && cp <= 0x202E))
        return 0;
    if (cp >= 0x80 && cp <= 0x9F) {
        static const u16 CP1252[32] = {
            0x20AC, 0,      0x201A, 0x0192, 0x201E, 0x2026, 0x2020, 0x2021,
            0x02C6, 0x2030, 0x0160, 0x2039, 0x0152, 0,      0x017D, 0,
            0,      0x2018, 0x2019, 0x201C, 0x201D, 0x2022, 0x2013, 0x2014,
            0x02DC, 0x2122, 0x0161, 0x203A, 0x0153, 0,      0x017E, 0x0178,
        };
        return CP1252[cp - 0x80];
    }
    return cp;
}

/* A character the font lacks, as one it has (typographic quotes and
 * dashes as plain ones, full-width forms as ASCII); cp itself = none
 * (then the fallback font's, or a space) */
u32 substitute(u32 cp)
{
    if (cp >= 0x2018 && cp <= 0x201B) return '\'';
    if (cp >= 0x201C && cp <= 0x201F) return '"';
    if (cp == 0x00AB || cp == 0x00BB) return '"';
    if (cp >= 0x2010 && cp <= 0x2015) return '-';
    if (cp == 0x2026) return '.';
    if (cp == 0x2032) return '\'';
    if (cp == 0x2033) return '"';
    if (cp >= 0xFF01 && cp <= 0xFF5E) return cp - 0xFEE0;   /* full-width ！？Ａ１ */
    return cp;
}

const Glyph& glyph(GRRLIB_ttfFont* font, unsigned size, FaceSize& fs, u32 cp)
{
    auto it = fs.glyphs.find(cp);
    if (it != fs.glyphs.end()) return it->second;

    Glyph g;
    FT_Face face = (FT_Face)font->face;
    setSize(face, size);
    u32 draw = cp >= 0x80 ? normalize(cp) : cp;
    if (draw == 0) return fs.glyphs.emplace(cp, g).first->second;   /* nothing, no advance */
    g.index = FT_Get_Char_Index(face, draw);
    FT_Face from = face;          /* the font the glyph comes from */
    FT_UInt load = g.index;
    if (g.index == 0 && draw >= 0x80) {
        u32 sub = substitute(draw);
        if (sub != draw) load = g.index = FT_Get_Char_Index(face, sub);
        /* none: a fallback font's (Japanese, then the SD card's), else a
         * space rather than the font's box between words */
        for (size_t i = 0; g.index == 0 && from == face && i < s_fallbacks.size(); ++i) {
            if (s_fallbacks[i]->face == font->face) continue;
            FT_Face fb = (FT_Face)s_fallbacks[i]->face;
            FT_UInt fi = FT_Get_Char_Index(fb, draw);
            if (fi) {
                setSize(fb, size); from = fb; load = fi;
                /* tests: which fallback drew something (1 the Japanese one,
                 * 2 the first of the SD card's...), once each */
                static unsigned s_used = 0;
                if (i < 32 && !(s_used & (1u << i))) {
                    s_used |= 1u << i;
                    SYS_Report("[Text] fallback font %u drew U+%04X\n", (unsigned)i + 1, (unsigned)draw);
                }
            }
        }
        if (from == face && g.index == 0) {
            load = g.index = FT_Get_Char_Index(face, ' ');
            /* in the log once (tests: no character of a title or a subtitle
             * may be missing from both fonts) */
            static std::unordered_set<u32> s_missing;
            if (s_missing.size() < 64 && s_missing.insert(draw).second)
                SYS_Report("[Text] no glyph for U+%04X\n", (unsigned)draw);
        }
    }
    if (FT_Load_Glyph(from, load, FT_LOAD_RENDER) == 0) {
        FT_GlyphSlot slot = from->glyph;
        const FT_Bitmap& bm = slot->bitmap;
        g.advance = (slot->advance.x / 64.0f) / s_scale;
        g.left    = (s16)slot->bitmap_left;
        g.top     = (s16)slot->bitmap_top;
        if (bm.width > 0 && bm.rows > 0 && bm.pixel_mode == FT_PIXEL_MODE_GRAY) {
            g.w = (u16)bm.width + 2;
            g.h = (u16)bm.rows + 2;
            u16 tw = (g.w + 3) & ~3, th = (g.h + 3) & ~3;   /* 4x4 texel tiles */
            u32 bytes = (u32)tw * th * 2;
            g.data = (u8*)memalign(32, bytes);
            if (g.data) {
                memset(g.data, 0, bytes);
                for (u32 y = 0; y < th; ++y) {
                    for (u32 x = 0; x < tw; ++x) {
                        u32 tile = (y >> 2) * (tw >> 2) + (x >> 2);
                        u32 off  = (tile * 16 + (y & 3) * 4 + (x & 3)) * 2;
                        bool in  = x >= 1 && y >= 1 && x <= bm.width && y <= bm.rows;
                        g.data[off]     = in ? bm.buffer[(y - 1) * bm.pitch + (x - 1)] : 0;  /* alpha */
                        g.data[off + 1] = 0xFF;                          /* intensity */
                    }
                }
                DCFlushRange(g.data, bytes);
                GX_InitTexObj(&g.tex, g.data, tw, th, GX_TF_IA8, GX_CLAMP, GX_CLAMP, GX_FALSE);
                GX_InitTexObjLOD(&g.tex, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f,
                                 GX_FALSE, GX_FALSE, GX_ANISO_1);
                g.s = (f32)g.w / tw;
                g.t = (f32)g.h / th;
                /* The GPU may still cache a texture freed at this address */
                GX_InvalidateTexAll();
            }
        }
    }
    return fs.glyphs.emplace(cp, g).first->second;
}

float kerning(GRRLIB_ttfFont* font, unsigned size, FaceSize& fs, u32 left, u32 right)
{
    if (!font->kerning || !left || !right) return 0;
    u64 key = ((u64)left << 32) | right;
    auto it = fs.kerning.find(key);
    if (it != fs.kerning.end()) return it->second;
    FT_Face face = (FT_Face)font->face;
    setSize(face, size);
    FT_Vector delta;
    FT_Get_Kerning(face, left, right, FT_KERNING_DEFAULT, &delta);
    float k = (delta.x / 64.0f) / s_scale;
    fs.kerning.emplace(key, k);
    return k;
}

} // namespace

u32 Text::width(GRRLIB_ttfFont* font, const char* utf8, unsigned int size)
{
    if (!font || !utf8) return 0;
    FaceSize& fs = faceSize(font, size);
    const unsigned char* s = (const unsigned char*)utf8;
    float pen = 0;
    u32 prev = 0;
    while (*s) {
        const Glyph& g = glyph(font, size, fs, nextCodePoint(s));
        pen += kerning(font, size, fs, prev, g.index) + g.advance;
        prev = g.index;
    }
    return pen > 0 ? (u32)(pen + 0.5f) : 0;
}

void Text::print(int x, int y, GRRLIB_ttfFont* font, const char* utf8,
                 unsigned int size, u32 color)
{
    if (!font || !utf8 || !*utf8) return;
    FaceSize& fs = faceSize(font, size);

    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);

    const unsigned char* s = (const unsigned char*)utf8;
    float penX = 0;
    const int penY = (int)size;   /* GRRLIB puts the baseline at y + size */
    u32 prev = 0;
    while (*s) {
        const Glyph& g = glyph(font, size, fs, nextCodePoint(s));
        penX += kerning(font, size, fs, prev, g.index);
        if (g.data) {
            /* Place the bitmap on whole frame pixels so each texel lands on
             * one pixel (sharp, no resampling); -1: clear border. */
            f32 framePen = (x + penX - s_left) * s_scale;
            f32 frameX0  = floorf(framePen + 0.5f) + g.left - 1;
            f32 x0 = s_left + frameX0 / s_scale;
            f32 y0 = (f32)(y + penY - g.top - 1);
            f32 x1 = x0 + g.w / s_scale, y1 = y0 + g.h;
            GX_LoadTexObj(const_cast<GXTexObj*>(&g.tex), GX_TEXMAP0);
            GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
                GX_Position3f32(x0, y0, 0.0f); GX_Color1u32(color); GX_TexCoord2f32(0.0f, 0.0f);
                GX_Position3f32(x1, y0, 0.0f); GX_Color1u32(color); GX_TexCoord2f32(g.s,  0.0f);
                GX_Position3f32(x1, y1, 0.0f); GX_Color1u32(color); GX_TexCoord2f32(g.s,  g.t);
                GX_Position3f32(x0, y1, 0.0f); GX_Color1u32(color); GX_TexCoord2f32(0.0f, g.t);
            GX_End();
        }
        penX += g.advance;
        prev = g.index;
    }

    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);
}

/* Every glyph of a size is kept: a film's worth of Japanese subtitles is
 * thousands of them, so past this many that size starts again.  Between
 * frames only: the GPU draws the frame's glyphs from their texels until
 * GRRLIB_Render has waited for it. */
void Text::endFrame()
{
    for (auto& fsEntry : s_cache) {
        FaceSize& fs = fsEntry.second;
        if (fs.glyphs.size() < 1500) continue;
        for (auto& e : fs.glyphs) free(e.second.data);
        fs.glyphs.clear();
        fs.kerning.clear();
    }
}

void Text::setFallback(GRRLIB_ttfFont* font)
{
    s_fallbacks.clear();
    if (font) s_fallbacks.push_back(font);
}

void Text::addFallback(GRRLIB_ttfFont* font)
{
    if (font) s_fallbacks.push_back(font);
}

size_t Text::fitBytes(GRRLIB_ttfFont* font, const char* utf8, unsigned int size, float maxW)
{
    if (!font || !utf8) return 0;
    FaceSize& fs = faceSize(font, size);
    const unsigned char* s = (const unsigned char*)utf8;
    const unsigned char* last = s;
    float pen = 0;
    u32 prev = 0;
    while (*s) {
        const unsigned char* at = s;
        const Glyph& g = glyph(font, size, fs, nextCodePoint(s));
        pen += kerning(font, size, fs, prev, g.index) + g.advance;
        if (pen > maxW && at != (const unsigned char*)utf8) return (size_t)(at - (const unsigned char*)utf8);
        prev = g.index;
        last = s;
    }
    return (size_t)(last - (const unsigned char*)utf8);
}

void Text::clearCache()
{
    for (auto& fsEntry : s_cache)
        for (auto& gEntry : fsEntry.second.glyphs)
            free(gEntry.second.data);
    s_cache.clear();
}

void Text::setFrameMapping(float left, float scale)
{
    if (left == s_left && scale == s_scale) return;
    clearCache();             /* glyphs were rasterised for the old width */
    s_left  = left;
    s_scale = scale > 0.0f ? scale : 1.0f;
}
