#include "VideoSurface.h"
#include "../ui/Ui.h"
#include "vo_wiifin.h"
#include "../core/Utils.h"

#include <grrlib.h>
#include <gccore.h>

/* GRRLIB's 2D model-view matrix (defined in GRRLIB_core.c, not in its headers) */
extern Mtx GXmodelView2D;

/* Y'CbCr -> RGB in 12 TEV stages (MPlayer CE gx_supp.c, "formulation 2"),
 * BT.601 coefficients with 16-235 level conversion.  TEXMAP0 = Y,
 * TEXMAP1 = U (Cb), TEXMAP2 = V (Cr); the vertex colour must be
 * {0, 255, 0, 255}, it feeds constants into stages 0-3 through RASC. */
static void setupYuvTev()
{
    GX_SetNumChans(1);
    GX_SetNumTexGens(2);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetTexCoordGen(GX_TEXCOORD1, GX_TG_MTX2x4, GX_TG_TEX1, GX_IDENTITY);

    GX_SetNumTevStages(12);
    GX_SetTevKColor(GX_KCOLOR0, (GXColor){255,   0,   0, 18});
    GX_SetTevKColor(GX_KCOLOR1, (GXColor){  0,   0, 255, 41});
    GX_SetTevKColor(GX_KCOLOR2, (GXColor){179,  90,   0, 255});   /* BT.601 */
    GX_SetTevKColor(GX_KCOLOR3, (GXColor){  0,  21, 112, 255});

    GX_SetTevKColorSel(GX_TEVSTAGE0, GX_TEV_KCSEL_K1);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR0A0);
    GX_SetTevColorIn(GX_TEVSTAGE0, GX_CC_RASC, GX_CC_KONST, GX_CC_TEXC, GX_CC_ZERO);
    GX_SetTevColorOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_SUBHALF, GX_CS_SCALE_2, GX_ENABLE, GX_TEVREG0);
    GX_SetTevKAlphaSel(GX_TEVSTAGE0, GX_TEV_KASEL_K0_A);
    GX_SetTevAlphaIn(GX_TEVSTAGE0, GX_CA_ZERO, GX_CA_RASA, GX_CA_KONST, GX_CA_ZERO);
    GX_SetTevAlphaOp(GX_TEVSTAGE0, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVREG0);

    GX_SetTevKColorSel(GX_TEVSTAGE1, GX_TEV_KCSEL_K1);
    GX_SetTevOrder(GX_TEVSTAGE1, GX_TEXCOORD1, GX_TEXMAP1, GX_COLOR0A0);
    GX_SetTevColorIn(GX_TEVSTAGE1, GX_CC_KONST, GX_CC_RASC, GX_CC_TEXC, GX_CC_ZERO);
    GX_SetTevColorOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_SUBHALF, GX_CS_SCALE_2, GX_ENABLE, GX_TEVREG1);
    GX_SetTevAlphaIn(GX_TEVSTAGE1, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GX_SetTevAlphaOp(GX_TEVSTAGE1, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);

    GX_SetTevKColorSel(GX_TEVSTAGE2, GX_TEV_KCSEL_K0);
    GX_SetTevOrder(GX_TEVSTAGE2, GX_TEXCOORD1, GX_TEXMAP2, GX_COLOR0A0);
    GX_SetTevColorIn(GX_TEVSTAGE2, GX_CC_RASC, GX_CC_KONST, GX_CC_TEXC, GX_CC_ZERO);
    GX_SetTevColorOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_SUBHALF, GX_CS_SCALE_1, GX_ENABLE, GX_TEVREG2);
    GX_SetTevAlphaIn(GX_TEVSTAGE2, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GX_SetTevAlphaOp(GX_TEVSTAGE2, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);

    GX_SetTevKColorSel(GX_TEVSTAGE3, GX_TEV_KCSEL_K0);
    GX_SetTevOrder(GX_TEVSTAGE3, GX_TEXCOORD1, GX_TEXMAP2, GX_COLOR0A0);
    GX_SetTevColorIn(GX_TEVSTAGE3, GX_CC_KONST, GX_CC_RASC, GX_CC_TEXC, GX_CC_ZERO);
    GX_SetTevColorOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_SUBHALF, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE3, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GX_SetTevAlphaOp(GX_TEVSTAGE3, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);

    GX_SetTevKColorSel(GX_TEVSTAGE4, GX_TEV_KCSEL_K2);
    GX_SetTevOrder(GX_TEVSTAGE4, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
    GX_SetTevColorIn(GX_TEVSTAGE4, GX_CC_ZERO, GX_CC_KONST, GX_CC_CPREV, GX_CC_ZERO);
    GX_SetTevColorOp(GX_TEVSTAGE4, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_2, GX_DISABLE, GX_TEVPREV);
    GX_SetTevKAlphaSel(GX_TEVSTAGE4, GX_TEV_KASEL_1);
    GX_SetTevAlphaIn(GX_TEVSTAGE4, GX_CA_ZERO, GX_CA_KONST, GX_CA_A0, GX_CA_TEXA);
    GX_SetTevAlphaOp(GX_TEVSTAGE4, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);

    GX_SetTevKColorSel(GX_TEVSTAGE5, GX_TEV_KCSEL_K2);
    GX_SetTevOrder(GX_TEVSTAGE5, GX_TEXCOORD0, GX_TEXMAP0, GX_COLORNULL);
    GX_SetTevColorIn(GX_TEVSTAGE5, GX_CC_ZERO, GX_CC_KONST, GX_CC_C2, GX_CC_CPREV);
    GX_SetTevColorOp(GX_TEVSTAGE5, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);
    GX_SetTevKAlphaSel(GX_TEVSTAGE5, GX_TEV_KASEL_K1_A);
    GX_SetTevAlphaIn(GX_TEVSTAGE5, GX_CA_ZERO, GX_CA_KONST, GX_CA_TEXA, GX_CA_APREV);
    GX_SetTevAlphaOp(GX_TEVSTAGE5, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVREG1);

    GX_SetTevKColorSel(GX_TEVSTAGE6, GX_TEV_KCSEL_K2);
    GX_SetTevOrder(GX_TEVSTAGE6, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    GX_SetTevColorIn(GX_TEVSTAGE6, GX_CC_ZERO, GX_CC_KONST, GX_CC_C2, GX_CC_CPREV);
    GX_SetTevColorOp(GX_TEVSTAGE6, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE6, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GX_SetTevAlphaOp(GX_TEVSTAGE6, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);

    GX_SetTevKColorSel(GX_TEVSTAGE7, GX_TEV_KCSEL_1);
    GX_SetTevOrder(GX_TEVSTAGE7, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    GX_SetTevColorIn(GX_TEVSTAGE7, GX_CC_ZERO, GX_CC_ONE, GX_CC_A1, GX_CC_CPREV);
    GX_SetTevColorOp(GX_TEVSTAGE7, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE7, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GX_SetTevAlphaOp(GX_TEVSTAGE7, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);

    GX_SetTevKColorSel(GX_TEVSTAGE8, GX_TEV_KCSEL_K3);
    GX_SetTevOrder(GX_TEVSTAGE8, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    GX_SetTevColorIn(GX_TEVSTAGE8, GX_CC_ZERO, GX_CC_KONST, GX_CC_C1, GX_CC_CPREV);
    GX_SetTevColorOp(GX_TEVSTAGE8, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE8, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GX_SetTevAlphaOp(GX_TEVSTAGE8, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);

    GX_SetTevKColorSel(GX_TEVSTAGE9, GX_TEV_KCSEL_K3);
    GX_SetTevOrder(GX_TEVSTAGE9, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    GX_SetTevColorIn(GX_TEVSTAGE9, GX_CC_ZERO, GX_CC_KONST, GX_CC_C1, GX_CC_CPREV);
    GX_SetTevColorOp(GX_TEVSTAGE9, GX_TEV_SUB, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE9, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GX_SetTevAlphaOp(GX_TEVSTAGE9, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);

    GX_SetTevKColorSel(GX_TEVSTAGE10, GX_TEV_KCSEL_K3);
    GX_SetTevOrder(GX_TEVSTAGE10, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    GX_SetTevColorIn(GX_TEVSTAGE10, GX_CC_ZERO, GX_CC_KONST, GX_CC_C0, GX_CC_CPREV);
    GX_SetTevColorOp(GX_TEVSTAGE10, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_DISABLE, GX_TEVPREV);
    GX_SetTevAlphaIn(GX_TEVSTAGE10, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO);
    GX_SetTevAlphaOp(GX_TEVSTAGE10, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);

    GX_SetTevKColorSel(GX_TEVSTAGE11, GX_TEV_KCSEL_K3);
    GX_SetTevOrder(GX_TEVSTAGE11, GX_TEXCOORDNULL, GX_TEXMAP_NULL, GX_COLORNULL);
    GX_SetTevColorIn(GX_TEVSTAGE11, GX_CC_ZERO, GX_CC_KONST, GX_CC_C0, GX_CC_CPREV);
    GX_SetTevColorOp(GX_TEVSTAGE11, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
    /* output alpha = K3's alpha (opaque, or the blend weight of the second
     * pass in smooth motion); K3's colour part is the BT.601 constant above */
    GX_SetTevKAlphaSel(GX_TEVSTAGE11, GX_TEV_KASEL_K3_A);
    GX_SetTevAlphaIn(GX_TEVSTAGE11, GX_CA_ZERO, GX_CA_ZERO, GX_CA_ZERO, GX_CA_KONST);
    GX_SetTevAlphaOp(GX_TEVSTAGE11, GX_TEV_ADD, GX_TB_ZERO, GX_CS_SCALE_1, GX_ENABLE, GX_TEVPREV);
}

/* Put back the state GRRLIB_Init() set up and its draw calls rely on. */
static void restoreGrrlibState()
{
    GX_SetNumTevStages(1);
    GX_SetNumTexGens(1);
    GX_SetTexCoordGen(GX_TEXCOORD0, GX_TG_MTX2x4, GX_TG_TEX0, GX_IDENTITY);
    GX_SetTevOrder(GX_TEVSTAGE0, GX_TEXCOORD0, GX_TEXMAP0, GX_COLOR0A0);
    GX_SetTevOp(GX_TEVSTAGE0, GX_PASSCLR);

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_NONE);
}

static void initPlaneTex(GXTexObj* obj, void* data, u16 w, u16 h)
{
    GX_InitTexObj(obj, data, w, h, GX_TF_I8, GX_CLAMP, GX_CLAMP, GX_FALSE);
    GX_InitTexObjLOD(obj, GX_LINEAR, GX_LINEAR, 0.0f, 0.0f, 0.0f,
                     GX_FALSE, GX_FALSE, GX_ANISO_1);
}

static VideoSurface::Zoom s_zoom = VideoSurface::Zoom::Fit;
void VideoSurface::setZoom(Zoom z) { s_zoom = z; }
VideoSurface::Zoom VideoSurface::zoom() { return s_zoom; }

bool VideoSurface::draw()
{
    wiifin_video_frame f;
    if (!wiifin_video_acquire(&f)) return false;

    /* Fit the picture into the whole screen.  Drawing units are square on
     * the TV (Ui::initScreen widens them in 16:9), so this is a plain fit.
     * Fill crops the texture instead: the quad never leaves the screen. */
    const float SW = Ui::screenWidth(), SH = 480.0f;
    float w = SW, h = SH;
    float cs = 0.0f, ct = 0.0f;          /* fraction cropped off each side */
    if (s_zoom == Zoom::Fill) {
        if (f.aspect > SW / SH) cs = (1.0f - (SW / SH) / f.aspect) * 0.5f;
        else                    ct = (1.0f - f.aspect / (SW / SH)) * 0.5f;
    } else {
        if (f.aspect > SW / SH) h = SW / f.aspect;
        else                    w = SH * f.aspect;
    }
    const float x0 = Ui::screenLeft() + (SW - w) * 0.5f, y0 = (SH - h) * 0.5f;
    const float x1 = x0 + w,              y1 = y0 + h;
    const float ys0 = f.ys * cs,  ys1 = f.ys * (1.0f - cs),  yt0 = f.yt * ct,  yt1 = f.yt * (1.0f - ct);
    const float us0 = f.uvs * cs, us1 = f.uvs * (1.0f - cs), ut0 = f.uvt * ct, ut1 = f.uvt * (1.0f - ct);
    auto quad = [&]() {
        GX_Begin(GX_QUADS, GX_VTXFMT5, 4);
            GX_Position3f32(x0, y0, 0.0f); GX_Color4u8(0, 255, 0, 255);
            GX_TexCoord2f32(ys0, yt0);     GX_TexCoord2f32(us0, ut0);
            GX_Position3f32(x1, y0, 0.0f); GX_Color4u8(0, 255, 0, 255);
            GX_TexCoord2f32(ys1, yt0);     GX_TexCoord2f32(us1, ut0);
            GX_Position3f32(x1, y1, 0.0f); GX_Color4u8(0, 255, 0, 255);
            GX_TexCoord2f32(ys1, yt1);     GX_TexCoord2f32(us1, ut1);
            GX_Position3f32(x0, y1, 0.0f); GX_Color4u8(0, 255, 0, 255);
            GX_TexCoord2f32(ys0, yt1);     GX_TexCoord2f32(us0, ut1);
        GX_End();
    };

    GXTexObj yTex, uTex, vTex;
    initPlaneTex(&yTex, f.y, f.yw,  f.yh);
    initPlaneTex(&uTex, f.u, f.uvw, f.uvh);
    initPlaneTex(&vTex, f.v, f.uvw, f.uvh);
    GX_InvalidateTexAll();   /* texture memory was rewritten by the CPU */
    GX_LoadTexObj(&yTex, GX_TEXMAP0);
    GX_LoadTexObj(&uTex, GX_TEXMAP1);
    GX_LoadTexObj(&vTex, GX_TEXMAP2);

    setupYuvTev();

    GX_ClearVtxDesc();
    GX_SetVtxDesc(GX_VA_POS,  GX_DIRECT);
    GX_SetVtxDesc(GX_VA_CLR0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX0, GX_DIRECT);
    GX_SetVtxDesc(GX_VA_TEX1, GX_DIRECT);
    /* Own vertex format slot so GRRLIB's GX_VTXFMT0 stays untouched. */
    GX_SetVtxAttrFmt(GX_VTXFMT5, GX_VA_POS,  GX_POS_XYZ,  GX_F32,   0);
    GX_SetVtxAttrFmt(GX_VTXFMT5, GX_VA_CLR0, GX_CLR_RGBA, GX_RGBA8, 0);
    GX_SetVtxAttrFmt(GX_VTXFMT5, GX_VA_TEX0, GX_TEX_ST,   GX_F32,   0);
    GX_SetVtxAttrFmt(GX_VTXFMT5, GX_VA_TEX1, GX_TEX_ST,   GX_F32,   0);
    GX_LoadPosMtxImm(GXmodelView2D, GX_PNMTX0);

    quad();

    /* Smooth motion: the next frame over it, with the blend weight as
     * opacity (alpha comes from K3, see setupYuvTev). */
    if (f.blend > 0.0f && f.y2) {
        GXTexObj y2, u2, v2;
        initPlaneTex(&y2, f.y2, f.yw,  f.yh);
        initPlaneTex(&u2, f.u2, f.uvw, f.uvh);
        initPlaneTex(&v2, f.v2, f.uvw, f.uvh);
        GX_LoadTexObj(&y2, GX_TEXMAP0);
        GX_LoadTexObj(&u2, GX_TEXMAP1);
        GX_LoadTexObj(&v2, GX_TEXMAP2);
        GX_SetTevKColor(GX_KCOLOR3, (GXColor){0, 21, 112, (u8)(f.blend * 255.0f + 0.5f)});
        GX_SetBlendMode(GX_BM_BLEND, GX_BL_SRCALPHA, GX_BL_INVSRCALPHA, GX_LO_CLEAR);
        quad();
        GX_SetTevKColor(GX_KCOLOR3, (GXColor){0, 21, 112, 255});
    }

    restoreGrrlibState();
    return true;
}

void VideoSurface::endFrame()
{
    wiifin_video_release();
}
