#include "Trickplay.h"
#include "../core/ExitZone.h"
#include <malloc.h>
#include <setjmp.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <jpeglib.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>

/* ---- Loader thread ------------------------------------------------------ */

void Trickplay::start(JellyfinClient& c, const std::string& url, const JellyfinAuth& a,
                      const std::string& item, const std::string& source)
{
    stop();
    client        = &c;
    serverUrl     = url;
    auth          = a;
    itemId        = item;
    mediaSourceId = source;
    info          = TrickplayInfo();
    quit = false; online = false; roomy = false; urgent = false; infoDone = false;
    wantTile = -1; readyTile = -1; shownTile = -1;
    static u8 stack[64 * 1024] DEAD_AT_EXIT __attribute__((aligned(32)));
    lwp_t t = LWP_THREAD_NULL;
    /* Below MPlayer's threads and the main one: it runs on the time they
     * leave, so the picture never waits for a thumbnail. */
    if (LWP_CreateThread(&t, threadMain, this, stack, sizeof(stack), 40) >= 0)
        thread = t;
}

void Trickplay::stop()
{
    if (thread) {
        quit = true;
        LWP_JoinThread((lwp_t)thread, nullptr);
        thread = 0;
    }
    freeAll(shown);
    freeAll(ready);
    shownTile = -1;
    readyTile = -1;
    itemId.clear();
}

void* Trickplay::threadMain(void* self)
{
    static_cast<Trickplay*>(self)->loop();
    return nullptr;
}

void Trickplay::loop()
{
    int infoTries = 0;
    int loaded = -1;                  /* last tile handed over */
    int failedTile = -1, failures = 0;
    while (!quit) {
        if (!online) { usleep(50 * 1000); continue; }
        if (!infoDone) {
            TrickplayInfo t;
            bool ok = client->getTrickplayInfo(serverUrl, auth, itemId, mediaSourceId, t);
            info = t;
            /* a request cut by a stream restart gets one more try */
            if (ok || ++infoTries >= 2) infoDone = true;
            else usleep(2000 * 1000);
            continue;
        }
        int tile = wantTile;
        if (!info.ok() || tile < 0 || tile == loaded || readyTile >= 0 || !(urgent || roomy) ||
            (tile == failedTile && failures >= 3)) {
            usleep(30 * 1000);
            continue;
        }
        std::string jpeg;
        u64 t0 = ticks_to_millisecs(gettime());
        bool ok = client->getTrickplayTile(serverUrl, auth, itemId, mediaSourceId, info.width, tile, jpeg);
        u64 t1 = ticks_to_millisecs(gettime());
        std::vector<GRRLIB_texImg*> thumbs;
        if (ok && !quit) ok = decodeTile(jpeg, tile, thumbs);
        SYS_Report("[Trickplay] tile %d: %s, %u bytes in %llu ms, %u thumbnails in %llu ms\n",
                   tile, ok ? "ok" : "failed", (unsigned)jpeg.size(), t1 - t0,
                   (unsigned)thumbs.size(), ticks_to_millisecs(gettime()) - t1);
        if (!ok) {
            if (tile != failedTile) { failedTile = tile; failures = 0; }
            ++failures;
            usleep(500 * 1000);
            continue;
        }
        ready.swap(thumbs);
        loaded    = tile;
        readyTile = tile;             /* last: the main thread takes `ready` now */
    }
}

/* ---- Main thread -------------------------------------------------------- */

GRRLIB_texImg* Trickplay::thumbnail(float secs, bool show)
{
    if (!thread || !infoDone || !info.ok()) return nullptr;
    if (readyTile >= 0) {
        freeAll(shown);
        shown.swap(ready);
        shownTile = readyTile;
        readyTile = -1;
    }
    int idx = secs > 0.0f ? (int)(secs * 1000.0f / (float)info.intervalMs) : 0;
    if (idx >= info.count) idx = info.count - 1;
    const int per = info.tileW * info.tileH;
    wantTile = idx / per;
    urgent   = show;
    if (idx / per != shownTile) return nullptr;
    size_t k = (size_t)(idx % per);
    return k < shown.size() ? shown[k] : nullptr;
}

void Trickplay::freeAll(std::vector<GRRLIB_texImg*>& v)
{
    for (GRRLIB_texImg* t : v) { free(t->data); free(t); }
    v.clear();
}

/* ---- Decoding ----------------------------------------------------------- */

namespace {

struct ErrMgr {
    struct jpeg_error_mgr pub;
    jmp_buf               buf;
};
void errExit(j_common_ptr c) { longjmp(((ErrMgr*)c->err)->buf, 1); }
void noOutput(j_common_ptr) {}   /* no fprintf from a worker thread */

/* One thumbnail of the band (RGB, `stride` bytes a row) into an RGB565 GX
 * texture, half the memory of RGBA8.  GX stores it in 4x4 blocks; the
 * blocks past the edge repeat the last row and column. */
GRRLIB_texImg* makeThumb(const u8* band, u32 stride, int x0, int w, int h)
{
    GRRLIB_texImg* t = (GRRLIB_texImg*)calloc(1, sizeof(GRRLIB_texImg));
    if (!t) return nullptr;
    u32 size = GX_GetTexBufferSize(w, h, GX_TF_RGB565, 0, 0);
    t->data = memalign(32, size);
    if (!t->data) { free(t); return nullptr; }
    u16* d = (u16*)t->data;
    for (int by = 0; by < h; by += 4)
        for (int bx = 0; bx < w; bx += 4)
            for (int r = 0; r < 4; ++r) {
                int y = by + r < h ? by + r : h - 1;
                for (int c = 0; c < 4; ++c) {
                    int x = bx + c < w ? bx + c : w - 1;
                    const u8* p = band + y * stride + (x0 + x) * 3;
                    *d++ = (u16)(((p[0] & 0xF8) << 8) | ((p[1] & 0xFC) << 3) | (p[2] >> 3));
                }
            }
    t->w = w;
    t->h = h;
    t->format = GX_TF_RGB565;
    GRRLIB_SetHandle(t, 0, 0);
    GRRLIB_FlushTex(t);
    return t;
}

} // namespace

/* The tile is decoded scaled down by libjpeg (1/2 for 320-wide thumbnails:
 * 160 x 90), one band of thumbnails at a time, and only as far as the last
 * thumbnail of the video: the last tile is mostly black. */
bool Trickplay::decodeTile(const std::string& jpeg, int tile, std::vector<GRRLIB_texImg*>& out)
{
    const int per = info.tileW * info.tileH;
    const int n = info.count - tile * per < per ? info.count - tile * per : per;
    if (n <= 0 || jpeg.size() < 3) return false;

    struct jpeg_decompress_struct cinfo __attribute__((aligned(32)));
    ErrMgr jerr __attribute__((aligned(32)));
    u8* band = nullptr;
    JSAMPROW* rows = nullptr;
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit     = errExit;
    jerr.pub.output_message = noOutput;
    if (setjmp(jerr.buf)) {
        jpeg_destroy_decompress(&cinfo);
        free(band);
        free(rows);
        freeAll(out);
        return false;
    }
    jpeg_create_decompress(&cinfo);
    jpeg_mem_src(&cinfo, (const unsigned char*)jpeg.data(), (unsigned long)jpeg.size());
    jpeg_read_header(&cinfo, TRUE);
    cinfo.out_color_space     = JCS_RGB;
    cinfo.dct_method          = JDCT_IFAST;
    cinfo.do_fancy_upsampling = FALSE;
    cinfo.scale_num   = 1;
    cinfo.scale_denom = 1;
    while (cinfo.scale_denom < 8 && info.width / (int)(cinfo.scale_denom * 2) >= 150)
        cinfo.scale_denom *= 2;
    jpeg_start_decompress(&cinfo);

    const int W = (int)cinfo.output_width, H = (int)cinfo.output_height;
    const int tw = (W / info.tileW) & ~3, th = H / info.tileH;
    const int bands = (n + info.tileW - 1) / info.tileW;
    if (tw < 16 || th < 8 || cinfo.output_components != 3) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return false;
    }
    const u32 stride = (u32)W * 3;
    band = (u8*)malloc(stride * th);
    rows = (JSAMPROW*)malloc(sizeof(JSAMPROW) * th);
    bool ok = band && rows;
    for (int b = 0; ok && b < bands && !quit; ++b) {
        for (int i = 0; i < th; ++i) rows[i] = band + i * stride;
        int done = 0;
        while (done < th && cinfo.output_scanline < (JDIMENSION)H)
            done += (int)jpeg_read_scanlines(&cinfo, rows + done, (JDIMENSION)(th - done));
        for (int c = 0; ok && c < info.tileW && b * info.tileW + c < n; ++c) {
            GRRLIB_texImg* t = makeThumb(band, stride, c * (W / info.tileW), tw, th);
            if (t) out.push_back(t);
            else   ok = false;
        }
    }
    free(band); band = nullptr;
    free(rows); rows = nullptr;
    jpeg_abort_decompress(&cinfo);   /* the rows past the last thumbnail are left */
    jpeg_destroy_decompress(&cinfo);
    if (!ok || quit) { freeAll(out); return false; }
    return true;
}
