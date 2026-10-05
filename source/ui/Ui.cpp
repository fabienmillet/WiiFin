#include "Ui.h"
#include "../core/Text.h"

#include <gccore.h>
#include <math.h>
#include <time.h>
#include <stdio.h>
#include <string.h>
#include <ogc/lwp_watchdog.h>

/* GRRLIB's 2D model-view matrix (defined in GRRLIB_core.c, not in its headers) */
extern Mtx GXmodelView2D;

namespace Ui {

/* ---- Palettes ----------------------------------------------------------- */

static const Palette LIGHT = {
    /* bgTop, bgBottom, stripe */        0xF4F5F6FF, 0xDCDFE3FF, 0x0000000A,
    /* cardTop, cardBottom, cardBorder */0xFFFFFFFF, 0xEEF0F2FF, 0xB4B9BFFF,
    /* text, textDim, textOnAccent */    0x4C4F54FF, 0x8B9097FF, 0xFFFFFFFF,
    /* accent, accentDark, glow */       0x34BEEDFF, 0x0A93C6FF, 0x34BEED90,
    /* shadow */                         0x00000030,
    /* barTop, barBottom, barBorder */   0xF0F1F3FF, 0xCDD1D6FF, 0xA7ADB4FF,
    /* field, fieldBorder */             0xFFFFFFFF, 0xC3C8CEFF,
    /* danger, ok */                     0xE0524DFF, 0x3DAF5AFF,
    /* dim */                            0x000000A0,
};

static const Palette DARK = {
    /* bgTop, bgBottom, stripe */        0x1D2736FF, 0x0D131CFF, 0xFFFFFF07,
    /* cardTop, cardBottom, cardBorder */0x2E3A4CFF, 0x232D3BFF, 0x43526AFF,
    /* text, textDim, textOnAccent */    0xEDF1F6FF, 0x8E9DB0FF, 0xFFFFFFFF,
    /* accent, accentDark, glow */       0x34BEEDFF, 0x5CCFF4FF, 0x34BEED80,
    /* shadow */                         0x00000070,
    /* barTop, barBottom, barBorder */   0x2A3546FF, 0x1A222FFF, 0x3B485CFF,
    /* field, fieldBorder */             0x18202BFF, 0x3B485CFF,
    /* danger, ok */                     0xF0605AFF, 0x4CC46AFF,
    /* dim */                            0x000000B0,
};

static const Palette FLIX = {
    /* bgTop, bgBottom, stripe */        0x303030FF, 0x111111FF, 0x00000000,
    /* cardTop, cardBottom, cardBorder */0x4A4A4AFF, 0x363636FF, 0x5E5E5EFF,
    /* text, textDim, textOnAccent */    0xF2F2F2FF, 0xA8A8A8FF, 0xFFFFFFFF,
    /* accent, accentDark, glow */       0xD81F26FF, 0xFF6B6FFF, 0xFF333370,
    /* shadow */                         0x00000090,
    /* barTop, barBottom, barBorder */   0x3C3C3CFF, 0x222222FF, 0x555555FF,
    /* field, fieldBorder */             0x1C1C1CFF, 0x555555FF,
    /* danger, ok */                     0xFF5A52FF, 0x46C35FFF,
    /* dim */                            0x000000B8,
};

static Theme           s_theme = Theme::Dark;
static HomeLayout      s_layout = HomeLayout::Rows;
void       setHomeLayout(HomeLayout l) { s_layout = l; }
static LibraryStyle s_libStyle = LibraryStyle::ListCover;
void         setLibraryStyle(LibraryStyle s) { s_libStyle = s; }
LibraryStyle libraryStyle()                  { return s_libStyle; }
HomeLayout homeLayout()                { return s_layout; }
static GRRLIB_ttfFont* s_font  = nullptr;

void        setTheme(Theme t)   { s_theme = t; }
Theme       theme()             { return s_theme; }
Theme       nextTheme(Theme t)  { return (Theme)(((int)t + 1) % THEME_COUNT); }
const char* themeName(Theme t)
{
    switch (t) {
    case Theme::Dark: return "Dark";
    case Theme::Flix: return "Flix";
    default:          return "Light";
    }
}
const Palette& pal()
{
    switch (s_theme) {
    case Theme::Dark: return DARK;
    case Theme::Flix: return FLIX;
    default:          return LIGHT;
    }
}
void setFont(GRRLIB_ttfFont* f) { s_font = f; }
GRRLIB_ttfFont* font()          { return s_font; }

/* ---- Colour helpers ------------------------------------------------------ */

u32 mix(u32 a, u32 b, float t)
{
    if (t <= 0.0f) return a;
    if (t >= 1.0f) return b;
    u32 out = 0;
    for (int sh = 0; sh < 32; sh += 8) {
        int ca = (a >> sh) & 0xFF, cb = (b >> sh) & 0xFF;
        out |= (u32)(ca + (cb - ca) * t + 0.5f) << sh;
    }
    return out;
}

u32 alpha(u32 c, float k)
{
    if (k >= 1.0f) return c;
    if (k <= 0.0f) return c & 0xFFFFFF00;
    return (c & 0xFFFFFF00) | (u32)((c & 0xFF) * k + 0.5f);
}

float approach(float cur, float target, float speed)
{
    float d = target - cur;
    if (d > -0.002f && d < 0.002f) return target;
    return cur + d * speed;
}

/* ---- Geometry ------------------------------------------------------------ */

static const int CORNER_SEGS = 7;
static const int MAX_PTS     = 4 * (CORNER_SEGS + 1);

struct Outline {
    int   n;
    float x[MAX_PTS], y[MAX_PTS];     /* perimeter, clockwise from the left of the top-left arc */
    float nx[MAX_PTS], ny[MAX_PTS];   /* outward normals */
};

static void outline(Outline& o, float x, float y, float w, float h, float r)
{
    if (r > w * 0.5f) r = w * 0.5f;
    if (r > h * 0.5f) r = h * 0.5f;
    if (r < 0.0f) r = 0.0f;
    const float cx[4] = { x + r, x + w - r, x + w - r, x + r };
    const float cy[4] = { y + r, y + r,     y + h - r, y + h - r };
    o.n = 0;
    for (int c = 0; c < 4; ++c) {
        float a0 = (float)M_PI * (1.0f + 0.5f * c);
        for (int i = 0; i <= CORNER_SEGS; ++i) {
            float a = a0 + (float)M_PI * 0.5f * i / CORNER_SEGS;
            float ca = cosf(a), sa = sinf(a);
            o.x[o.n]  = cx[c] + r * ca;
            o.y[o.n]  = cy[c] + r * sa;
            o.nx[o.n] = ca;
            o.ny[o.n] = sa;
            ++o.n;
        }
    }
}

static inline void vtx(float x, float y, u32 c)
{
    GX_Position3f32(x, y, 0.0f);
    GX_Color1u32(c);
}

static void beginShapes()
{
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);
    GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);
}

static inline u32 gradAt(float y, float y0, float h, u32 top, u32 bottom)
{
    return top == bottom ? top : mix(top, bottom, h > 0.0f ? (y - y0) / h : 0.0f);
}

/* Band from the outline to the outline pushed out by `width`, colour
 * inner -> outer (used for anti-aliasing feathers, shadows and glows). */
static void band(const Outline& o, float inset, float width, u32 inner, u32 outer,
                 float y0, float h, u32 top, u32 bottom, bool gradInner)
{
    GX_Begin(GX_TRIANGLESTRIP, GX_VTXFMT0, (o.n + 1) * 2);
    for (int k = 0; k <= o.n; ++k) {
        int i = k % o.n;
        float ix = o.x[i] - o.nx[i] * inset, iy = o.y[i] - o.ny[i] * inset;
        u32 ci = gradInner ? gradAt(iy, y0, h, top, bottom) : inner;
        u32 co = gradInner ? (ci & 0xFFFFFF00) : outer;
        vtx(ix, iy, ci);
        vtx(o.x[i] + o.nx[i] * width, o.y[i] + o.ny[i] * width, co);
    }
    GX_End();
}

void roundRect(float x, float y, float w, float h, float r, u32 top, u32 bottom)
{
    if (w <= 0 || h <= 0) return;
    Outline o;
    outline(o, x, y, w, h, r);
    beginShapes();
    GX_Begin(GX_TRIANGLEFAN, GX_VTXFMT0, o.n + 2);
    vtx(x + w * 0.5f, y + h * 0.5f, gradAt(y + h * 0.5f, y, h, top, bottom));
    for (int k = 0; k <= o.n; ++k) {
        int i = k % o.n;
        vtx(o.x[i], o.y[i], gradAt(o.y[i], y, h, top, bottom));
    }
    GX_End();
    band(o, 0.0f, 1.0f, 0, 0, y, h, top, bottom, true);   /* anti-aliased edge */
}

void roundBorder(float x, float y, float w, float h, float r, float t, u32 color)
{
    if (w <= 0 || h <= 0) return;
    Outline o;
    outline(o, x, y, w, h, r);
    beginShapes();
    band(o, t, 0.0f, color, color, 0, 0, 0, 0, false);              /* solid ring  */
    band(o, 0.0f, 1.0f, color, color & 0xFFFFFF00, 0, 0, 0, 0, false);  /* outer AA */
    band(o, t + 1.0f, -t, color & 0xFFFFFF00, color, 0, 0, 0, 0, false); /* inner AA */
}

void shadow(float x, float y, float w, float h, float r, float blur, u32 color)
{
    Outline o;
    outline(o, x, y, w, h, r);
    beginShapes();
    GX_Begin(GX_TRIANGLEFAN, GX_VTXFMT0, o.n + 2);
    vtx(x + w * 0.5f, y + h * 0.5f, color);
    for (int k = 0; k <= o.n; ++k) { int i = k % o.n; vtx(o.x[i], o.y[i], color); }
    GX_End();
    band(o, 0.0f, blur, color, color & 0xFFFFFF00, 0, 0, 0, 0, false);
}

void circle(float cx, float cy, float rad, u32 color)
{
    roundRect(cx - rad, cy - rad, rad * 2, rad * 2, rad, color);
}

void texRound(GRRLIB_texImg* tex, float x, float y, float w, float h, float r, u32 tint)
{
    texRound(tex, x, y, w, h, r, 0.0f, 0.0f, 1.0f, 1.0f, tint);
}

void texCover(GRRLIB_texImg* tex, float x, float y, float w, float h, float r,
              float aspect, u32 tint)
{
    if (!tex || tex->w == 0 || tex->h == 0) return;
    float ta = (float)tex->w / tex->h;
    float u0 = 0, v0 = 0, u1 = 1, v1 = 1;
    if (ta > aspect) { float k = aspect / ta; u0 = (1 - k) * 0.5f; u1 = u0 + k; }
    else             { float k = ta / aspect; v0 = (1 - k) * 0.5f; v1 = v0 + k; }
    texRound(tex, x, y, w, h, r, u0, v0, u1, v1, tint);
}

void texRound(GRRLIB_texImg* tex, float x, float y, float w, float h, float r,
              float u0, float v0, float u1, float v1, u32 tint)
{
    if (!tex || !tex->data || w <= 0 || h <= 0) return;
    Outline o;
    outline(o, x, y, w, h, r);
    GXTexObj obj;
    GX_InitTexObj(&obj, tex->data, tex->w, tex->h, tex->format, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(&obj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f, GX_FALSE, GX_FALSE, GX_ANISO_1);
    GX_LoadTexObj(&obj, GX_TEXMAP0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_MODULATE);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);
    GX_Begin(GX_TRIANGLEFAN, GX_VTXFMT0, o.n + 2);
    const float du = u1 - u0, dv = v1 - v0;
    GX_Position3f32(x + w * 0.5f, y + h * 0.5f, 0.0f); GX_Color1u32(tint);
    GX_TexCoord2f32(u0 + du * 0.5f, v0 + dv * 0.5f);
    for (int k = 0; k <= o.n; ++k) {
        int i = k % o.n;
        GX_Position3f32(o.x[i], o.y[i], 0.0f);
        GX_Color1u32(tint);
        GX_TexCoord2f32(u0 + du * (o.x[i] - x) / w, v0 + dv * (o.y[i] - y) / h);
    }
    GX_End();
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);
    GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);
}

void triangle(float x0, float y0, float x1, float y1, float x2, float y2, u32 c)
{
    beginShapes();
    GX_Begin(GX_TRIANGLES, GX_VTXFMT0, 3);
    vtx(x0, y0, c); vtx(x1, y1, c); vtx(x2, y2, c);
    GX_End();
}

/* ---- Text --------------------------------------------------------------- */

int  textWidth(const char* s, int size) { return (int)Text::width(s_font, s, size); }
void text(float x, float y, const char* s, int size, u32 c) { Text::print((int)x, (int)y, s_font, s, size, c); }
void textCentered(float cx, float y, const char* s, int size, u32 c)
{
    Text::print((int)(cx - textWidth(s, size) * 0.5f), (int)y, s_font, s, size, c);
}
void textRight(float xr, float y, const char* s, int size, u32 c)
{
    Text::print((int)(xr - textWidth(s, size)), (int)y, s_font, s, size, c);
}

/* ---- Safe area ------------------------------------------------------------- */

static int   s_safeL = 0, s_safeT = 0, s_safeR = 0, s_safeB = 0;
static float s_sx = 1.0f, s_sy = 1.0f;
static float s_off = 0.0f;           /* extra drawing width on each side (16:9) */

void initScreen(bool widescreen)
{
    s_off = widescreen ? (640.0f * 4.0f / 3.0f - 640.0f) * 0.5f : 0.0f;   /* 106.67 */
    /* GRRLIB's projection maps 0..640 to the frame; widen it so one unit is
     * square on the TV.  Everything (GRRLIB, Text, Ui, video) uses it. */
    Mtx44 m;
    guOrtho(m, 0.0f, 480.0f, -s_off, 640.0f + s_off, 0.0f, 1000.0f);
    GX_LoadProjectionMtx(m, GX_ORTHOGRAPHIC);
    /* glyphs rasterised at frame resolution (see Text::setFrameMapping) */
    Text::setFrameMapping(-s_off, 640.0f / (640.0f + 2.0f * s_off));
}

float screenLeft()  { return -s_off; }
float screenRight() { return 640.0f + s_off; }

void pointerToScreen(ir_t& ir)
{
    if (!ir.valid || s_off == 0.0f) return;
    ir.x = -s_off + ir.x * (640.0f + 2.0f * s_off) / 640.0f;
}

void setSafeArea(int l, int t, int r, int b)
{
    auto clampi = [](int v, int hi) { return v < 0 ? 0 : (v > hi ? hi : v); };
    s_safeL = clampi(l, SAFE_MAX_X); s_safeR = clampi(r, SAFE_MAX_X);
    s_safeT = clampi(t, SAFE_MAX_Y); s_safeB = clampi(b, SAFE_MAX_Y);
    s_sx = (640.0f - s_safeL - s_safeR) / 640.0f;
    s_sy = (480.0f - s_safeT - s_safeB) / 480.0f;
    /* GRRLIB's orthographic projection covers 640x480; the viewport decides
     * where that lands in the frame.  Everything outside stays black (the
     * frame is cleared on every copy to the TV). */
    GX_SetViewport((f32)s_safeL, (f32)s_safeT, 640.0f * s_sx, 480.0f * s_sy, 0.0f, 1.0f);
}

void safeArea(int& l, int& t, int& r, int& b) { l = s_safeL; t = s_safeT; r = s_safeR; b = s_safeB; }

void clip(float x, float y, float w, float h)
{
    /* drawing units -> frame pixels: aspect widening, then the safe area */
    const float k = 640.0f / (640.0f + 2.0f * s_off);
    GRRLIB_ClipDrawing((int)(s_safeL + (x + s_off) * k * s_sx), (int)(s_safeT + y * s_sy),
                       (int)(w * k * s_sx + 0.5f), (int)(h * s_sy + 0.5f));
}

void clipReset() { GRRLIB_ClipReset(); }

/* ---- Components --------------------------------------------------------- */

void background(bool withBand)
{
    const Palette& p = pal();
    beginShapes();
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    const float L = screenLeft(), R = screenRight();
    vtx(L, 0, p.bgTop); vtx(R, 0, p.bgTop); vtx(R, 480, p.bgBottom); vtx(L, 480, p.bgBottom);
    GX_End();
    /* Fine horizontal pinstripes, like the Wii Menu */
    if (p.stripe & 0xFF) {
        GX_Begin(GX_QUADS, GX_VTXFMT0, 4 * 120);
        for (int y = 0; y < 480; y += 4) {
            vtx(L, y, p.stripe); vtx(R, y, p.stripe); vtx(R, y + 2, p.stripe); vtx(L, y + 2, p.stripe);
        }
        GX_End();
    }
    if (withBand) drawHeaderBand();
}

float headerBand() { return s_theme == Theme::Flix ? 44.0f : 0.0f; }

void drawHeaderBand()
{
    const float h = headerBand();
    if (h <= 0) return;
    /* glossy red title band of the old video channels */
    const float L = screenLeft(), R = screenRight();
    beginShapes();
    GX_Begin(GX_QUADS, GX_VTXFMT0, 12);
    vtx(L, 0, 0xD3242AFF); vtx(R, 0, 0xD3242AFF); vtx(R, h * 0.5f, 0xB8141AFF); vtx(L, h * 0.5f, 0xB8141AFF);
    vtx(L, h * 0.5f, 0xA80F15FF); vtx(R, h * 0.5f, 0xA80F15FF); vtx(R, h, 0x86090DFF); vtx(L, h, 0x86090DFF);
    vtx(L, h, 0x4A0306FF); vtx(R, h, 0x4A0306FF); vtx(R, h + 2, 0x4A030600); vtx(L, h + 2, 0x4A030600);
    GX_End();
    GX_Begin(GX_QUADS, GX_VTXFMT0, 4);
    vtx(L, 0, 0xFFFFFF40); vtx(R, 0, 0xFFFFFF40); vtx(R, 1, 0xFFFFFF40); vtx(L, 1, 0xFFFFFF40);
    GX_End();
}

void card(float x, float y, float w, float h, float r, float focus)
{
    const Palette& p = pal();
    shadow(x + 1, y + 3, w - 2, h - 2, r, 6.0f, p.shadow);
    if (focus > 0.0f) shadow(x - 2, y - 2, w + 4, h + 4, r + 2, 9.0f, alpha(p.glow, focus));
    roundRect(x, y, w, h, r, p.cardTop, p.cardBottom);
    roundBorder(x, y, w, h, r, focus > 0.0f ? 2.0f + focus : 2.0f, mix(p.cardBorder, p.accent, focus));
}

void button(float x, float y, float w, float h, const char* label, int size, float focus)
{
    const Palette& p = pal();
    card(x, y, w, h, h * 0.5f, focus);
    /* glossy top highlight */
    roundRect(x + h * 0.3f, y + 3, w - h * 0.6f, h * 0.36f, h * 0.18f,
              alpha(0xFFFFFFFF, theme() == Theme::Light ? 0.55f : 0.06f),
              alpha(0xFFFFFFFF, 0.0f));
    textCentered(x + w * 0.5f, y + (h - size) * 0.5f - 1, label, size,
                 mix(p.text, p.accentDark, focus));
}

void header(const char* title, const char* subtitle)
{
    const Palette& p = pal();
    text(28, 10 + (headerBand() > 0 ? 0 : 8), title, 24, p.text);
    if (subtitle) textRight(612, headerBand() > 0 ? 16 : 24, subtitle, 15,
                            headerBand() > 0 ? 0xFFFFFFC0 : p.textDim);
    if (headerBand() <= 0) roundRect(28, 52, 584, 2, 1, alpha(p.cardBorder, 0.8f));
}

/* D-pad hint: the same round badge as the buttons, with two arrowheads. */
static void dpadGlyph(float cx, float cy, bool vertical, u32 c)
{
    const Palette& p = pal();
    circle(cx, cy, 9, p.cardBorder);
    circle(cx, cy, 7.5f, p.cardTop);
    const float a = 5.5f, b = 1.2f, hw = 3.4f;     /* tip, base, half width */
    if (vertical) {
        triangle(cx, cy - a, cx + hw, cy - b, cx - hw, cy - b, c);
        triangle(cx, cy + a, cx - hw, cy + b, cx + hw, cy + b, c);
    } else {
        triangle(cx - a, cy, cx - b, cy - hw, cx - b, cy + hw, c);
        triangle(cx + a, cy, cx + b, cy + hw, cx + b, cy - hw, c);
    }
}

/* Width of the glyph drawn for a hint button (0 = label only). */
static ButtonStyle s_buttonStyle = ButtonStyle::WiiRemote;
void        setButtonStyle(ButtonStyle s) { s_buttonStyle = s; }
ButtonStyle buttonStyle()                 { return s_buttonStyle; }

const char* buttonName(const char* b)
{
    if (s_buttonStyle == ButtonStyle::WiiRemote) return b;
    static const struct { const char* wii; const char* classic; const char* gc; } MAP[] = {
        { "1",    "Y",    "Y" },
        { "2",    "X",    "X" },
        { "-",    "-",    "L" },
        { "+",    "+",    "R" },
        { "-/+",  "-/+",  "L/R" },
        { "HOME", "HOME", "START" },
    };
    for (const auto& m : MAP)
        if (strcmp(b, m.wii) == 0)
            return s_buttonStyle == ButtonStyle::Classic ? m.classic : m.gc;
    return b;
}

/* GameCube A and B are green and red; everything else is a grey button */
static void buttonDisc(float cx, float cy, const char* b, u32& textColor)
{
    const Palette& p = pal();
    u32 fill = p.cardTop;
    textColor = p.text;
    if (s_buttonStyle == ButtonStyle::GameCube && (b[0] == 'A' || b[0] == 'B') && !b[1]) {
        fill = b[0] == 'A' ? 0x2FB56AFF : 0xD8463CFF;
        textColor = 0xFFFFFFFF;
    }
    circle(cx, cy, 9, p.cardBorder);
    circle(cx, cy, 7.5f, fill);
}

static float glyphWidth(const char* b)
{
    if (!b[0]) return 0;
    if (strcmp(b, "UD") == 0 || strcmp(b, "LR") == 0) return 18;
    if (strcmp(b, "-/+") == 0 || strcmp(b, "L/R") == 0) return 40;
    if (strlen(b) > 1) return textWidth(b, 10) + 16;   /* HOME ... */
    return 18;
}

/* Minus / plus drawn as bars: a 12 px "-" character is a single thin row of
 * pixels that vanishes on the TV (and when the picture is scaled). */
static void minusGlyph(float cx, float cy, u32 c) { roundRect(cx - 4.5f, cy - 1.25f, 9, 2.5f, 1.25f, c); }
static void plusGlyph(float cx, float cy, u32 c)
{
    roundRect(cx - 4.5f, cy - 1.25f, 9, 2.5f, 1.25f, c);
    roundRect(cx - 1.25f, cy - 4.5f, 2.5f, 9, 1.25f, c);
}

float hint(float x, float y, const Hint& h)
{
    const Palette& p = pal();
    const char* b = buttonName(h.button);
    float cy = y + 9, w = glyphWidth(b);
    u32 tc = p.text;
    if (w == 0) {
        /* plain caption */
    } else if (strcmp(b, "UD") == 0 || strcmp(b, "LR") == 0) {
        dpadGlyph(x + 9, cy, b[0] == 'U', p.text);
    } else if (strcmp(b, "-/+") == 0 || strcmp(b, "L/R") == 0) {
        /* two small buttons side by side */
        for (int i = 0; i < 2; ++i) {
            float bx = x + 9 + i * 22;
            buttonDisc(bx, cy, "", tc);
            if (b[0] == 'L') textCentered(bx, cy - 7, i == 0 ? "L" : "R", 12, tc);
            else if (i == 0) minusGlyph(bx, cy, tc);
            else             plusGlyph(bx, cy, tc);
        }
    } else if (strlen(b) > 1) {
        roundRect(x, cy - 8, w, 16, 8, p.cardTop, p.cardBottom);
        roundBorder(x, cy - 8, w, 16, 8, 1.5f, p.cardBorder);
        textCentered(x + w * 0.5f, cy - 6, b, 10, p.textDim);
    } else {
        buttonDisc(x + 9, cy, b, tc);
        if      (b[0] == '-') minusGlyph(x + 9, cy, tc);
        else if (b[0] == '+') plusGlyph(x + 9, cy, tc);
        else textCentered(x + 9, cy - 7, b, 12, tc);
    }
    if (w > 0) w += 5;
    text(x + w, y + 2, h.label, 14, p.textDim);
    return w + textWidth(h.label, 14) + 14;
}

float hintWidth(const Hint& h)
{
    float g = glyphWidth(buttonName(h.button));
    return (g > 0 ? g + 5 : 0) + textWidth(h.label, 14) + 14;
}

void bottomBar(const Hint* left, int nLeft, const Hint* right, int nRight)
{
    const Palette& p = pal();
    const float top = 424;
    /* Wii-style bar: wide rounded slab with a light top edge */
    const float L = screenLeft() - 20, W = screenWidth() + 40;
    shadow(L, top - 2, W, 90, 34, 8.0f, p.shadow);
    roundRect(L, top, W, 90, 34, p.barTop, p.barBottom);
    roundBorder(L, top, W, 90, 34, 2.0f, p.barBorder);

    /* clock */
    time_t now = time(nullptr);
    struct tm lt;
    localtime_r(&now, &lt);
    char clock[8], date[24];
    strftime(clock, sizeof(clock), "%H:%M", &lt);
    strftime(date, sizeof(date), "%a %d/%m", &lt);
    textCentered(320, top + 7, clock, 26, p.text);
    textCentered(320, top + 36, date, 13, p.textDim);

    /* hints out to the screen edges (wide screens have room there), never
     * under the clock: left hints that would reach it go to the right side */
    const float x0 = screenLeft() + 40 < 30 ? screenLeft() + 40 : 30;
    const float x1 = screenRight() - 40 > 612 ? screenRight() - 40 : 612;
    const float clockL = 320 - 62, clockR = 320 + 62;
    int fit = 0;
    for (float w = x0; fit < nLeft && w + hintWidth(left[fit]) - 14 <= clockL; ++fit)
        w += hintWidth(left[fit]);
    float x = x0;
    for (int i = 0; i < fit; ++i) x += hint(x, top + 15, left[i]);
    float w = 0;
    for (int i = fit; i < nLeft; ++i) w += hintWidth(left[i]);
    for (int i = 0; right && i < nRight; ++i) w += hintWidth(right[i]);
    x = x1 - w;
    if (x < clockR) x = clockR;
    for (int i = fit; i < nLeft; ++i) x += hint(x, top + 15, left[i]);
    for (int i = 0; right && i < nRight; ++i) x += hint(x, top + 15, right[i]);
}

void footer(const Hint* left, int nLeft, const Hint* right, int nRight, const char* center)
{
    const Palette& p = pal();
    const float top = 450;
    const float L = screenLeft() - 20, W = screenWidth() + 40;
    shadow(L, top - 1, W, 60, 16, 6.0f, p.shadow);
    roundRect(L, top, W, 60, 16, p.barTop, p.barBottom);
    roundBorder(L, top, W, 60, 16, 1.5f, p.barBorder);

    float x = 20, xl = 20;
    for (int i = 0; i < nLeft; ++i) x += hint(x, top + 5, left[i]);
    xl = x;
    float xr = 620;
    if (right && nRight > 0) {
        float w = 0;
        for (int i = 0; i < nRight; ++i) w += hintWidth(right[i]);
        x = xr = 626 - w;
        for (int i = 0; i < nRight; ++i) x += hint(x, top + 5, right[i]);
    }
    if (center && *center) {
        /* centred caption, shortened to the room left between the hints */
        float room = 2.0f * (320.0f - xl > xr - 320.0f ? xr - 320.0f : 320.0f - xl) - 16.0f;
        char buf[160], out[164];
        snprintf(buf, sizeof(buf), "%s", center);
        snprintf(out, sizeof(out), "%s", buf);
        size_t len = strlen(buf);
        while (len > 0 && textWidth(out, 14) > room) {
            do { --len; } while (len > 0 && ((unsigned char)buf[len] & 0xC0) == 0x80);
            buf[len] = 0;
            snprintf(out, sizeof(out), "%s...", buf);
        }
        if (len > 0) textCentered(320, top + 7, out, 14, p.text);
    }
}

float tabs(float cx, float y, const char* const* names, int n, int sel, int size)
{
    const Palette& p = pal();
    const float pad = 13, h = size + 14;
    float w[8], total = 0;
    if (n > 8) n = 8;
    for (int i = 0; i < n; ++i) { w[i] = textWidth(names[i], size) + pad * 2; total += w[i]; }
    float x0 = cx - total * 0.5f;
    shadow(x0, y + 1, total, h, h * 0.5f, 4.0f, p.shadow);
    roundRect(x0, y, total, h, h * 0.5f, p.field, mix(p.field, p.cardBottom, 0.5f));
    roundBorder(x0, y, total, h, h * 0.5f, 1.5f, p.fieldBorder);
    float x = x0;
    for (int i = 0; i < n; ++i) {
        if (i == sel) {
            roundRect(x + 2, y + 2, w[i] - 4, h - 4, (h - 4) * 0.5f,
                      mix(p.accent, 0xFFFFFFFF, 0.25f), p.accentDark);
            textCentered(x + w[i] * 0.5f, y + (h - size) * 0.5f - 1, names[i], size, p.textOnAccent);
        } else {
            textCentered(x + w[i] * 0.5f, y + (h - size) * 0.5f - 1, names[i], size, p.textDim);
        }
        x += w[i];
    }
    return x0;
}

/* Round button with a triangle pointing dir (0 up, 1 down, 2 left, 3 right). */
static void roundArrow(float cx, float cy, float r0, int dir, bool enabled, bool hover)
{
    const Palette& p = pal();
    float k   = enabled ? 1.0f : 0.35f;
    bool  hot = hover && enabled;
    float rad = hot ? r0 + 2 : r0;
    shadow(cx - rad, cy - rad + 2, rad * 2, rad * 2, rad, 5.0f, alpha(p.shadow, k));
    if (hot) shadow(cx - rad - 2, cy - rad - 2, rad * 2 + 4, rad * 2 + 4, rad + 2, 8.0f, p.glow);
    roundRect(cx - rad, cy - rad, rad * 2, rad * 2, rad, alpha(p.cardTop, k), alpha(p.cardBottom, k));
    roundBorder(cx - rad, cy - rad, rad * 2, rad * 2, rad, hot ? 2.5f : 1.5f,
                alpha(hot ? p.accent : p.cardBorder, k));
    u32 c = enabled ? p.accent : alpha(p.textDim, 0.5f);
    float a = r0 * 0.47f, w = r0 * 0.37f;     /* triangle half-length / half-width */
    switch (dir) {
    case 0: triangle(cx, cy - a, cx + a, cy + w, cx - a, cy + w, c); break;
    case 1: triangle(cx - a, cy - w, cx + a, cy - w, cx, cy + a, c); break;
    case 2: triangle(cx - a, cy, cx + w, cy - a, cx + w, cy + a, c); break;
    default:triangle(cx + a, cy, cx - w, cy + a, cx - w, cy - a, c); break;
    }
}

void arrowButton(float cx, float cy, bool up, bool enabled, bool hover)
{
    roundArrow(cx, cy, 15, up ? 0 : 1, enabled, hover);
}

void pageArrow(float cx, float cy, bool left, bool enabled, bool hover)
{
    roundArrow(cx, cy, 21, left ? 2 : 3, enabled, hover);
}

void progress(float x, float y, float w, float h, float frac)
{
    const Palette& p = pal();
    if (frac < 0) frac = 0;
    if (frac > 1) frac = 1;
    roundRect(x, y, w, h, h * 0.5f, 0x00000080);
    if (frac > 0) roundRect(x, y, w * frac < h ? h : w * frac, h, h * 0.5f,
                            mix(p.accent, 0xFFFFFFFF, 0.2f), p.accent);
}

void scrollbar(float x, float y, float h, int first, int visible, int total)
{
    const Palette& p = pal();
    if (total <= visible) return;
    float barH = h * visible / total;
    if (barH < 20) barH = 20;
    int maxTop = total - visible;
    float barY = y + (maxTop > 0 ? (h - barH) * first / maxTop : 0);
    roundRect(x, y, 6, h, 3, alpha(p.cardBorder, 0.5f));
    roundRect(x, barY, 6, barH, 3, p.accent, p.accentDark);
}

void key(float x, float y, float w, float h, const char* label,
         bool sel, bool hover, bool special, bool latched)
{
    const Palette& p = pal();
    if (sel) {
        shadow(x - 2, y - 2, w + 4, h + 4, 9, 7.0f, p.glow);
        roundRect(x, y, w, h, 7, mix(p.accent, 0xFFFFFFFF, 0.25f), p.accentDark);
    } else if (latched) {
        roundRect(x, y, w, h, 7, 0xF7B54AFF, 0xE08E1BFF);
    } else {
        u32 top = special ? mix(p.cardBottom, p.cardBorder, 0.35f) : p.cardTop;
        roundRect(x, y, w, h, 7, top, p.cardBottom);
        roundBorder(x, y, w, h, 7, hover ? 2.0f : 1.0f, hover ? p.accent : alpha(p.cardBorder, 0.8f));
    }
    /* short ASCII words (SYM, ABC) get a smaller size than single glyphs */
    int sz = 15;
    if (strlen(label) > 1 && (unsigned char)label[0] < 0x80) sz = w > 60 ? 13 : 11;
    u32 tc = (sel || latched) ? p.textOnAccent : (special ? p.textDim : p.text);
    textCentered(x + w * 0.5f, y + (h - sz) * 0.5f - 1, label, sz, tc);
}

void field(float x, float y, float w, float h, const char* value, int size, bool focused)
{
    const Palette& p = pal();
    if (focused) shadow(x - 2, y - 2, w + 4, h + 4, h * 0.5f + 2, 8.0f, p.glow);
    else         shadow(x, y + 1, w, h, h * 0.5f, 4.0f, p.shadow);
    roundRect(x, y, w, h, h * 0.5f, p.field);
    roundBorder(x, y, w, h, h * 0.5f, focused ? 2.5f : 1.5f, focused ? p.accent : p.fieldBorder);
    if (value) text(x + h * 0.5f, y + (h - size) * 0.5f - 1, value, size, p.text);
}

static Mtx s_savedView;
static int s_offsetDepth = 0;

void pushOffset(float dx, float dy)
{
    if (s_offsetDepth++ == 0) guMtxCopy(GXmodelView2D, s_savedView);
    guMtxTransApply(GXmodelView2D, GXmodelView2D, dx, dy, 0.0f);
    GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);
}

void popOffset()
{
    if (s_offsetDepth > 0 && --s_offsetDepth == 0) {
        guMtxCopy(s_savedView, GXmodelView2D);
        GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);
    }
}

float pulse()
{
    float t = (float)(ticks_to_millisecs(gettime()) % 1600) / 1600.0f;
    return 0.8f + 0.2f * sinf(t * 2.0f * (float)M_PI);
}

void avatar(float cx, float cy, float r, const char* name, float focus)
{
    const Palette& p = pal();
    /* first character (whole UTF-8 sequence), ASCII upper-cased */
    char initial[5] = { '?', 0, 0, 0, 0 };
    if (name && name[0]) {
        unsigned char c = (unsigned char)name[0];
        int len = c < 0x80 ? 1 : c < 0xE0 ? 2 : c < 0xF0 ? 3 : 4;
        for (int i = 0; i < len && name[i]; ++i) initial[i] = name[i];
        initial[len] = 0;
        if (initial[0] >= 'a' && initial[0] <= 'z') initial[0] -= 32;
    }
    if (focus > 0.0f) shadow(cx - r - 2, cy - r - 2, r * 2 + 4, r * 2 + 4, r + 2, 8.0f, alpha(p.glow, focus));
    else              shadow(cx - r, cy - r + 2, r * 2, r * 2, r, 4.0f, p.shadow);
    roundRect(cx - r, cy - r, r * 2, r * 2, r,
              mix(p.accent, 0xFFFFFFFF, 0.25f + 0.15f * focus), mix(p.accent, 0x000000FF, 0.15f));
    roundBorder(cx - r, cy - r, r * 2, r * 2, r, 2.0f, 0xFFFFFFFF);
    int size = (int)(r * 1.1f);
    textCentered(cx, cy - size * 0.55f, initial, size, 0xFFFFFFFF);
}

void spinner(GRRLIB_texImg* ring, float cx, float cy)
{
    if (!ring) return;
    float angle = (float)(ticks_to_millisecs(gettime()) % 1500) * (360.0f / 1500.0f);
    GRRLIB_SetMidHandle(ring, true);
    GRRLIB_DrawImg(cx, cy, ring, angle, 1.0f, 1.0f, theme() == Theme::Light ? 0x34BEEDFF : (theme() == Theme::Flix ? 0xE52A30FF : 0xFFFFFFFF));
    GRRLIB_SetMidHandle(ring, false);
}

} // namespace Ui
