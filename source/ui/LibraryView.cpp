#include "LibraryView.h"
#include "../input/Input.h"

extern unsigned char data_icon_user_png[];
extern unsigned int  data_icon_user_png_len;
#include "../jellyfin/JellyfinClient.h"
#include "../core/SoundFX.h"
#include <ogcsys.h>
#include <cmath>
#include "../core/Utils.h"
#include <stdio.h>
#include <string.h>
#include <malloc.h>
#include <setjmp.h>
#include <jpeglib.h>
#include <ogc/lwp_watchdog.h> // gettime(), ticks_to_millisecs()
#include <ogc/lwp.h>          // LWP_CreateThread / LWP_JoinThread

// widescreen flag definition (declared in Utils.h)
namespace WiiUtils { bool widescreen = false; }

// libjpeg error manager with setjmp so corrupt/bad JPEG data never reaches
// the default error_exit which calls exit() and causes an invalid write crash.
struct JpegErrMgr {
    struct jpeg_error_mgr pub;
    jmp_buf               buf;
};
static void jpegErrExit(j_common_ptr cinfo) {
    longjmp(((JpegErrMgr*)cinfo->err)->buf, 1);
}
// Suppress all libjpeg diagnostic output to prevent the default
// output_message → fprintf(stderr,...) path, which triggers setvbuf → malloc
// from the worker thread and corrupts the heap allocator state.
static void jpegNoOp(j_common_ptr) {}

static GRRLIB_texImg* loadJPEGTexture(const u8* data, u32 size) {
    // Reject immediately if data doesn't start with the JPEG SOI marker.
    if (size < 3 || data[0] != 0xFF || data[1] != 0xD8 || data[2] != 0xFF)
        return nullptr;

    struct jpeg_decompress_struct cinfo __attribute__((aligned(32)));
    JpegErrMgr jerr __attribute__((aligned(32)));
    // Strip buffer: 4 scanlines at a time — avoids a large w*h*3 intermediate
    // allocation and the associated heap pressure / fragmentation.
    unsigned char* strip = nullptr;
    GRRLIB_texImg* tex   = nullptr;

    // err MUST be set before jpeg_create_decompress
    cinfo.err = jpeg_std_error(&jerr.pub);
    jerr.pub.error_exit    = jpegErrExit;
    jerr.pub.output_message = jpegNoOp; // suppress fprintf → setvbuf → malloc

    // Any libjpeg error longjmps here; clean up and return nullptr
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
    // Always request RGB output regardless of source color space.
    cinfo.out_color_space = JCS_RGB;
    jpeg_start_decompress(&cinfo);

    u32 w  = cinfo.output_width;
    u32 h  = cinfo.output_height;
    u32 nc = (u32)cinfo.output_components; // 3 for JCS_RGB
    if (w == 0 || h == 0 || w > 2048 || h > 2048 || nc != 3) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }

    // Allocate the GX texture buffer first (32-byte aligned, correct tile size)
    tex = (GRRLIB_texImg*)calloc(1, sizeof(GRRLIB_texImg));
    if (!tex) {
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }
    u32 bufsize = GX_GetTexBufferSize(w, h, GX_TF_RGBA8, 0, 0);
    tex->data = memalign(32, bufsize);
    if (!tex->data) {
        free(tex); tex = nullptr;
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }

    // Decode in 4-scanline strips and write directly to GX RGBA8 tile layout.
    // Peak extra allocation: w*4*3 bytes (≤2880 B for 240-wide posters) vs the
    // old w*h*3 approach (up to 244 KB) that caused heap fragmentation crashes.
    strip = (unsigned char*)malloc(w * 4 * nc);
    if (!strip) {
        free(tex->data); free(tex); tex = nullptr;
        jpeg_abort_decompress(&cinfo);
        jpeg_destroy_decompress(&cinfo);
        return nullptr;
    }

    u8* tileData = (u8*)tex->data;
    for (u32 by = 0; by < h; by += 4) {
        int nrows = (int)(h - by);
        if (nrows > 4) nrows = 4;

        // Point each row-pointer into the strip buffer
        JSAMPROW rp[4];
        for (int i = 0; i < 4; i++)
            rp[i] = strip + (u32)i * w * nc;

        // Read nrows scanlines (libjpeg may deliver them one at a time)
        int done = 0;
        while (done < nrows && cinfo.output_scanline < h)
            done += (int)jpeg_read_scanlines(&cinfo, rp + done, (JDIMENSION)(nrows - done));

        // Convert strip to GX RGBA8 tile format (in-place, tile-by-tile)
        for (u32 bx = 0; bx < w; bx += 4) {
            // AR sub-block (alpha + red for all 16 texels in this 4×4 tile)
            for (u8 r = 0; r < 4; r++) {
                for (u8 c = 0; c < 4; c++) {
                    u32 sx = bx + c;
                    u8  red = (sx < w && r < (u8)nrows)
                              ? strip[((u32)r * w + sx) * nc] : 0;
                    *tileData++ = 0xFF; // alpha
                    *tileData++ = red;
                }
            }
            // GB sub-block (green + blue for same 16 texels)
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

    free(strip); strip = nullptr;
    jpeg_finish_decompress(&cinfo);
    jpeg_destroy_decompress(&cinfo);

    tex->w      = w;
    tex->h      = h;
    tex->format = GX_TF_RGBA8;
    GRRLIB_SetHandle(tex, 0, 0);
    GRRLIB_FlushTex(tex);
    return tex;
}

// ---------------------------------------------------------------
LibraryView::LibraryView(GRRLIB_ttfFont* f, GRRLIB_ttfFont* jf, GRRLIB_texImg* cursor,
                          GRRLIB_texImg* ring,
                          JellyfinClient& c,
                          const JellyfinAuth& a, const std::string& url)
    : font(f), jpFont(jf), cursorTex(cursor), ringTex(ring), client(c), auth(a), serverUrl(url) {
    userIconTex = GRRLIB_LoadTexture(data_icon_user_png);
}

// ---------------------------------------------------------------------------
// Utility: filter a UTF-8 string to codepoints DejaVu Sans covers, then
// truncate at maxCp codepoints. Use for any list label rendered with `font`.
// ---------------------------------------------------------------------------
static std::string filterDejaVu(const std::string& s, int maxCp) {
    std::string out;
    const unsigned char* p = (const unsigned char*)s.c_str();
    int count = 0;
    while (*p && count < maxCp) {
        uint32_t cp; int seqLen;
        if      (*p < 0x80) { cp = *p;          seqLen = 1; }
        else if (*p < 0xE0) { cp = *p & 0x1F;   seqLen = 2; }
        else if (*p < 0xF0) { cp = *p & 0x0F;   seqLen = 3; }
        else                { cp = *p & 0x07;    seqLen = 4; }
        bool valid = true;
        for (int i = 1; i < seqLen; i++) {
            if ((p[i] & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (!valid) { p++; continue; }
        bool ok = cp < 0x0500 || (cp >= 0x2000 && cp <= 0x26FF);
        if (ok) {
            for (int i = 0; i < seqLen; i++) out += (char)p[i];
            ++count;
        }
        p += seqLen;
    }
    if (*p) out += "...";
    return out;
}

// ---------------------------------------------------------------------------
// Background fetch: worker thread does network I/O, main thread spins loader.
// ---------------------------------------------------------------------------
static u8               s_fetchStack[64 * 1024];
static volatile bool    s_fetchDone;
struct FetchCtx { std::function<void()> fn; };
static FetchCtx         s_fetchCtx;

static void* fetchWorker(void*) {
    s_fetchCtx.fn();
    s_fetchDone = true;
    return nullptr;
}

void LibraryView::runWithLoading(std::function<void()> fn) {
    s_fetchCtx.fn = std::move(fn);
    s_fetchDone   = false;
    lwp_t thread;
    // Priority 64 is BELOW the main thread default (~80).
    // The worker therefore only runs during VIDEO_WaitVSync() inside GRRLIB_Render(),
    // when the main thread is intentionally idle. This prevents the worker from
    // preempting the main thread mid-drawcall and causing frame drops.
    LWP_CreateThread(&thread, fetchWorker, nullptr,
                     s_fetchStack, sizeof(s_fetchStack), 64);
    SoundFX::play(SoundFX::FX::Loading);
    while (!s_fetchDone) drawLoadingFrame();
    SoundFX::stopLoading();
    LWP_JoinThread(thread, nullptr);
    s_fetchCtx.fn = nullptr; // release lambda captures
}

// ---------------------------------------------------------------
void LibraryView::drawGradientBG() {
    const int r1=0x1a, g1=0x1a, b1=0x2e;
    const int r2=0x16, g2=0x21, b2=0x3e;
    const int bands=16, bh=480/bands;
    for (int i=0; i<bands; i++) {
        float t = i / (float)(bands - 1);
        u32 col = (((int)(r1+(r2-r1)*t)) << 24)
                | (((int)(g1+(g2-g1)*t)) << 16)
                | (((int)(b1+(b2-b1)*t)) << 8) | 0xFF;
        GRRLIB_Rectangle(0, i*bh, 640, bh, col, 1);
    }
}

void LibraryView::drawCenteredText(int x, int y, int w,
                                    const char* text, int sz, u32 col) {
    int tw = (int)GRRLIB_WidthTTF(font, text, sz);
    int tx = x + (w - tw) / 2;
    if (tx < x) tx = x;
    GRRLIB_PrintfTTF(tx, y, font, text, sz, col);
}

void LibraryView::drawCursor(ir_t& ir) {
    if (ir.valid && cursorTex) {
        orient_t orient;
        WPAD_Orientation(WPAD_CHAN_0, &orient);
        GRRLIB_DrawImg((int)ir.x - 20, (int)ir.y - 4,
                       cursorTex, orient.roll, 1.0f, 1.0f, 0xFFFFFFFF);
    }
}

u32 LibraryView::colorForType(const std::string& type, bool sel) {
    if (type == "movies")    return sel ? 0x2266CCFF : 0x1A4A8AFF;
    if (type == "tvshows")   return sel ? 0xCC4433FF : 0x7A2A1AFF;
    if (type == "music")     return sel ? 0x33AA55FF : 0x1A5A2AFF;
    if (type == "books")     return sel ? 0xAA7733FF : 0x5A3A0AFF;
    if (type == "playlists") return sel ? 0x6655BBFF : 0x443377FF;
    return sel ? 0x446688FF : 0x2A3A4AFF;
}

const char* LibraryView::labelForType(const std::string& type) {
    if (type == "movies")      return "MOVIES";
    if (type == "tvshows")     return "SERIES";
    if (type == "music")       return "MUSIC";
    if (type == "books")       return "BOOKS";
    if (type == "playlists")   return "PLAYLISTS";
    if (type == "boxsets")     return "COLLECTIONS";
    if (type == "livetv")      return "LIVE TV";
    if (type == "homevideos")  return "HOME VIDEOS";
    if (type == "photos")      return "PHOTOS";
    if (type == "musicvideos") return "MUSIC VIDEOS";
    if (type == "trailers")    return "TRAILERS";
    if (type == "mixed")       return "MIXED";
    // Fallback: show the raw type in upper case (max 12 chars) so it's still informative
    if (!type.empty()) {
        static char buf[16];
        int i = 0;
        for (char c : type) {
            if (i >= 12) break;
            buf[i++] = (c >= 'a' && c <= 'z') ? (c - 32) : c;
        }
        buf[i] = '\0';
        return buf;
    }
    return "MEDIA";
}

void LibraryView::drawLoadingFrame() {
    // Time-based angle: one full rotation per 1.5 seconds, independent of vsync/network speed.
    float angle = (float)(ticks_to_millisecs(gettime()) % 1500) * (360.0f / 1500.0f);
    drawGradientBG();
    if (ringTex) {
        GRRLIB_SetMidHandle(ringTex, true);
        GRRLIB_DrawImg(320, 240, ringTex, angle, 1.0f, 1.0f, 0xFFFFFFFF);
        GRRLIB_SetMidHandle(ringTex, false);
    }
    SoundFX::tickLoading();
    // Flush to screen and block on vsync. This serves two purposes:
    // 1. The spinner actually appears and animates on screen.
    // 2. The ~16 ms vsync wait yields the CPU to the worker thread
    //    (which runs at lower scheduling priority than the main thread).
    GRRLIB_Render();
}

void LibraryView::clampScroll() {
    int n = (int)items.size();
    if (itemSel < 0) itemSel = 0;
    if (n > 0 && itemSel >= n) itemSel = n - 1;
    if (itemSel < viewTop) viewTop = itemSel;
    if (itemSel >= viewTop + ITEMS_VISIBLE) viewTop = itemSel - ITEMS_VISIBLE + 1;
    if (viewTop < 0) viewTop = 0;
}

void LibraryView::freePosters() {
    for (int i = 0; i < POSTER_VISIBLE; i++) {
        if (posterTextures[i]) {
            GRRLIB_FreeTexture(posterTextures[i]);
            posterTextures[i] = nullptr;
        }
    }
}

void LibraryView::freeDetail() {
    if (detailTex) { GRRLIB_FreeTexture(detailTex); detailTex = nullptr; }
    detail = JellyfinItemDetail();
    detailLines.clear();
    detailAudioSel = 0;
    detailSubSel   = -1;
    detailFocusRow = 0;
    detailIsEpisode = false;
}

// ---------------------------------------------------------------------------

void LibraryView::loadPosters() {
    freePosters();
    int n = (int)items.size();
    if (n > POSTER_VISIBLE) n = POSTER_VISIBLE;
    // Fetch and decode all posters inside one background pass so the spinner
    // keeps animating through both network I/O and JPEG decode.
    // loadJPEGTexture only does CPU/heap work; GRRLIB_FlushTex is just
    // DCFlushRange (no GX), so it is safe to call from the worker thread.
    runWithLoading([&]() {
        for (int i = 0; i < n; i++) {
            std::string imgBytes;
            client.getItemImageBytes(serverUrl, auth, items[i].id, POSTER_W, POSTER_H, imgBytes);
            if (!imgBytes.empty())
                posterTextures[i] = loadJPEGTexture(
                    (const u8*)imgBytes.data(), (u32)imgBytes.size());
        }
    });

    // Pre-compute truncated display labels
    {
        float ws   = WiiUtils::wsScaleX();
        int   visW = (int)(POSTER_W * ws + 0.5f);
        for (int i = 0; i < n; i++) {
            std::string name = items[i].name;
            if (GRRLIB_WidthTTF(font, name.c_str(), 12) > (u32)visW) {
                auto popCodePoint = [](std::string &s) {
                    while (!s.empty() && (s.back() & 0xC0) == 0x80) s.pop_back();
                    if (!s.empty()) s.pop_back();
                };
                while (!name.empty() &&
                       GRRLIB_WidthTTF(font, (name + "...").c_str(), 12) > (u32)visW)
                    popCodePoint(name);
                name += "...";
            }
            posterLabels[i] = name;
        }
    }

    posterSel = 0;
    state = globFavMode ? State::GlobalFavoritesReady : State::PostersReady;
}

// ---------------------------------------------------------------
void LibraryView::loadDetail() {
    if (detailItemId.empty()) { state = State::PostersReady; return; }
    freeDetail();

    detailIsEpisode     = (detailReturnState == State::EpisodesReady) || detailIsEpisodeHint;
    detailIsEpisodeHint = false;

    // Fetch metadata and decode thumbnail inside one background pass so the
    // spinner animates throughout (no freeze between network and JPEG decode).
    bool ok = false; std::string fetchErr;
    bool isEp = detailIsEpisode;
    runWithLoading([&]() {
        ok = client.getItemDetail(serverUrl, auth, detailItemId, detail);
        if (!ok) { fetchErr = client.lastError(); return; }
        std::string imgBytes;
        if (isEp)
            client.getItemImageBytes(serverUrl, auth, detailItemId, 304, 171, imgBytes);
        else
            client.getItemImageBytes(serverUrl, auth, detailItemId, 240, 340, imgBytes);
        if (!imgBytes.empty())
            detailTex = loadJPEGTexture((const u8*)imgBytes.data(), (u32)imgBytes.size());
    });
    if (!ok) { errMsg = fetchErr; state = State::Error; return; }

    state = State::DetailReady;

    // Word-wrap overview on main thread
    const int MAX_CHARS = 60, MAX_LINES = 5;
    std::string ov = detail.overview;
    int nlines = 0;
    while (!ov.empty() && nlines < MAX_LINES) {
        size_t fit = ov.size() < (size_t)MAX_CHARS ? ov.size() : (size_t)MAX_CHARS;
        if (fit < ov.size()) {
            size_t sp = ov.rfind(' ', fit);
            if (sp != std::string::npos && sp > 0) fit = sp;
        }
        std::string line = ov.substr(0, fit);
        if (nlines == MAX_LINES - 1 && fit < ov.size())
            line += "...";
        detailLines.push_back(line);
        nlines++;
        ov = (fit < ov.size()) ? ov.substr(fit + (ov[fit] == ' ' ? 1 : 0)) : "";
    }
}

void LibraryView::clampSeasonScroll() {
    int n = (int)seasons.size();
    if (seasonSel < 0) seasonSel = 0;
    if (n > 0 && seasonSel >= n) seasonSel = n - 1;
    if (seasonSel < seasonTop) seasonTop = seasonSel;
    if (seasonSel >= seasonTop + ITEMS_VISIBLE) seasonTop = seasonSel - ITEMS_VISIBLE + 1;
    if (seasonTop < 0) seasonTop = 0;
}

void LibraryView::clampEpisodeScroll() {
    int n = (int)episodes.size();
    if (episodeSel < 0) episodeSel = 0;
    if (n > 0 && episodeSel >= n) episodeSel = n - 1;
    if (episodeSel < episodeTop) episodeTop = episodeSel;
    if (episodeSel >= episodeTop + ITEMS_VISIBLE) episodeTop = episodeSel - ITEMS_VISIBLE + 1;
    if (episodeTop < 0) episodeTop = 0;
}

// ---------------------------------------------------------------
void LibraryView::loadLibraries() {
    bool ok = false; std::string err;
    runWithLoading([&]() {
        ok = client.getLibraries(serverUrl, auth, libraries);
        if (!ok) err = client.lastError();
    });
    if (!ok)             { errMsg = err;                state = State::Error; return; }
    if (libraries.empty()) { errMsg = "No library found"; state = State::Error; return; }
    loadContinueWatching();
    loadNextUp();
    buildActDisplayStrings();
    state = State::LibsReady;
}

void LibraryView::freeCWTextures() {
    for (int i = 0; i < 3; i++) {
        if (cwTextures[i]) { GRRLIB_FreeTexture(cwTextures[i]); cwTextures[i] = nullptr; }
    }
}

void LibraryView::freeNextUpTextures() {
    for (int i = 0; i < 3; i++) {
        if (nextUpTextures[i]) { GRRLIB_FreeTexture(nextUpTextures[i]); nextUpTextures[i] = nullptr; }
    }
}

// Free all textures and bulk heap data before handing control to wii_player_play().
// MPlayer CE needs ~9 MB of contiguous heap (8 MB read-ahead cache + codec buffers).
// Keeping poster / detail / activity textures alive while the player starts causes
// Balloc() to fail inside _dtoa_r (called by MPlayer's diagnostic printf), producing
// an "Invalid read from 0x00000010" DSI crash. Releasing them here frees ~1–2 MB.
void LibraryView::releaseForPlayback() {
    if (userIconTex) { GRRLIB_FreeTexture(userIconTex); userIconTex = nullptr; }
    freePosters();
    freeCWTextures();
    freeNextUpTextures();
    freeMovieSuggestions();
    freeTVSuggestions();
    freeTVUpcoming();
    freeMusicSuggestions();
    if (detailTex) { GRRLIB_FreeTexture(detailTex); detailTex = nullptr; }
    detail = JellyfinItemDetail();
    detailLines.clear();
    /* Keep items, seasons, episodes, continueItems, nextUpItems:
     * they are lightweight metadata (a few KB total) and preserving them
     * lets reinitAfterPlayback() skip the network refetch, going straight
     * to PostersLoad instead of LibsReady → ItemsInit → ItemsLoad. */
}

void LibraryView::reinitAfterPlayback(GRRLIB_ttfFont* f, GRRLIB_ttfFont* jf,
                                       GRRLIB_texImg* cursor, GRRLIB_texImg* ring) {
    font      = f;
    jpFont    = jf;
    cursorTex = cursor;
    ringTex   = ring;

    // Reload the user icon (freed by releaseForPlayback → freePosters path or destructor)
    if (!userIconTex)
        userIconTex = GRRLIB_LoadTexture(data_icon_user_png);

    // Reset pending play fields
    pendingPlayUrl.clear();
    pendingPlayItemId.clear();
    pendingPlayMediaSourceId.clear();
    pendingPlaySessionId.clear();
    pendingPlayStartTimeTicks = 0;
    pendingPlayRuntimeTicks   = 0;
    pendingPlayIsMusic        = false;
    pendingMusicTracks.clear();
    pendingMusicTrackIdx      = 0;
    pendingPlayEpisodes.clear();
    pendingPlayEpisodeIdx     = 0;
    pendingPlaySeriesId.clear();
    pendingPlayAudioStreams.clear();
    pendingPlaySubStreams.clear();
    pendingPlayAudioIdx = 0;
    pendingPlaySubIdx   = -1;

    // Restore to the deepest safe state — items[] (and sometimes
    // seasons/episodes) are still populated so skip the network fetch.
    // Go straight to the Ready state so no loading spinner is shown.
    // Poster textures were freed by releaseForPlayback(); the render
    // path draws dark placeholder tiles for null textures, so the grid
    // is immediately usable — posters will reload on the next page change.
    spinAngle = 0.0f;
    if (!items.empty() && posterMode) {
        // Poster grid — show immediately with placeholder tiles (no spinner)
        posterSel = 0;
        state = State::PostersReady;
    } else if (!items.empty()) {
        // Text list — no images needed, show immediately
        state = State::ItemsReady;
        viewTop = 0;
        itemSel = 0;
    } else {
        // No items cached — fall back to library grid
        state    = State::LibsReady;
        libSel   = 0;
        homePage = 0;
        actRow   = 0;
    }

    // Activity textures (continue watching / next up) were freed by
    // releaseForPlayback().  Re-download them now so the Activity tab
    // has images again.  The metadata vectors are still populated, so
    // we only need to refetch the backdrop bytes and rebuild textures.
    reloadActivityTextures();
}

void LibraryView::loadContinueWatching() {
    freeCWTextures();
    continueItems.clear();
    runWithLoading([&]() {
        client.getContinueWatching(serverUrl, auth, continueItems);
        if ((int)continueItems.size() > 3)
            continueItems.resize(3);
        for (int i = 0; i < (int)continueItems.size(); i++) {
            std::string imgBytes;
            client.getItemBackdropBytes(serverUrl, auth, continueItems[i], 190, 107, imgBytes);
            if (!imgBytes.empty())
                cwTextures[i] = loadJPEGTexture((const u8*)imgBytes.data(), (u32)imgBytes.size());
        }
    });
}

void LibraryView::loadNextUp() {
    freeNextUpTextures();
    nextUpItems.clear();
    runWithLoading([&]() {
        client.getNextUp(serverUrl, auth, nextUpItems);
        if ((int)nextUpItems.size() > 3)
            nextUpItems.resize(3);
        for (int i = 0; i < (int)nextUpItems.size(); i++) {
            std::string imgBytes;
            client.getItemBackdropBytes(serverUrl, auth, nextUpItems[i], 190, 107, imgBytes);
            if (!imgBytes.empty())
                nextUpTextures[i] = loadJPEGTexture((const u8*)imgBytes.data(), (u32)imgBytes.size());
        }
    });
}

void LibraryView::reloadActivityTextures() {
    // Re-fetch backdrop images for continue-watching and next-up items whose
    // metadata is still cached but whose textures were freed for playback.
    runWithLoading([&]() {
        for (int i = 0; i < (int)continueItems.size() && i < 3; i++) {
            if (!cwTextures[i]) {
                std::string imgBytes;
                client.getItemBackdropBytes(serverUrl, auth, continueItems[i], 190, 107, imgBytes);
                if (!imgBytes.empty())
                    cwTextures[i] = loadJPEGTexture((const u8*)imgBytes.data(), (u32)imgBytes.size());
            }
        }
        for (int i = 0; i < (int)nextUpItems.size() && i < 3; i++) {
            if (!nextUpTextures[i]) {
                std::string imgBytes;
                client.getItemBackdropBytes(serverUrl, auth, nextUpItems[i], 190, 107, imgBytes);
                if (!imgBytes.empty())
                    nextUpTextures[i] = loadJPEGTexture((const u8*)imgBytes.data(), (u32)imgBytes.size());
            }
        }
    });
}

// Pre-compute truncated display strings for activity cards so the render loop
// never calls GRRLIB_WidthTTF (a costly FreeType operation) per frame.
void LibraryView::buildActDisplayStrings() {
    float ws   = WiiUtils::wsScaleX();
    int   visW = (int)(190 * ws + 0.5f); // ACT_CARD_W * wsScaleX

    auto build = [&](const std::vector<JellyfinItem>& items,
                     std::string* mains, std::string* subs) {
        for (int i = 0; i < (int)items.size(); i++) {
            const JellyfinItem& item = items[i];
            mains[i].clear(); subs[i].clear();
            if (item.type == "Episode") {
                const std::string& sn = item.seriesName.empty() ? item.name : item.seriesName;
                mains[i] = filterDejaVu(sn, 22);
                if (item.episodeNumber > 0) {
                    char tmp[80];
                    snprintf(tmp, sizeof(tmp), "S%dE%02d - %s",
                             item.seasonNumber, item.episodeNumber,
                             filterDejaVu(item.name, 15).c_str());
                    subs[i] = tmp;
                }
            } else {
                mains[i] = filterDejaVu(item.name, 22);
                if (item.year > 0) {
                    char tmp[8]; snprintf(tmp, sizeof(tmp), "%d", item.year);
                    subs[i] = tmp;
                }
            }
            // Trim mainTitle to fit cell width
            std::string& mt = mains[i];
            while (!mt.empty() && (int)GRRLIB_WidthTTF(font, mt.c_str(), 13) > visW) {
                while (!mt.empty() && (mt.back() & 0xC0) == 0x80) mt.pop_back();
                if (!mt.empty()) mt.pop_back();
            }
            // Trim subTitle to fit cell width
            std::string& st = subs[i];
            while (!st.empty() && (int)GRRLIB_WidthTTF(font, st.c_str(), 11) > visW) {
                while (!st.empty() && (st.back() & 0xC0) == 0x80) st.pop_back();
                if (!st.empty()) st.pop_back();
            }
        }
    };
    build(continueItems, cwDisplayMain, cwDisplaySub);
    build(nextUpItems,   nuDisplayMain, nuDisplaySub);
}

void LibraryView::loadMovieCollections() {
    items.clear();
    int limit      = POSTERS_PER_PAGE;
    int startIndex = itemPage * limit;
    bool ok = false; std::string err;
    runWithLoading([&]() {
        ok = client.getMovieCollections(serverUrl, auth, movieLibId,
                                        startIndex, limit, items, itemTotal);
        if (!ok) err = client.lastError();
    });
    if (!ok) { errMsg = err.empty() ? "Collections unavailable" : err; state = State::Error; }
    else     { globFavMode = false; itemSel = 0; viewTop = 0; posterSel = 0; state = State::PostersLoad; }
}

void LibraryView::loadMovieFavorites() {
    items.clear();
    int limit      = POSTERS_PER_PAGE;
    int startIndex = itemPage * limit;
    bool ok = false; std::string err;
    runWithLoading([&]() {
        ok = client.getFavoriteMovies(serverUrl, auth, movieLibId,
                                      startIndex, limit, items, itemTotal);
        if (!ok) err = client.lastError();
    });
    if (!ok) { errMsg = err.empty() ? "Favorites unavailable" : err; state = State::Error; }
    else     { globFavMode = false; itemSel = 0; viewTop = 0; posterSel = 0; state = State::PostersLoad; }
}

void LibraryView::loadGlobalFavorites() {
    items.clear();
    int limit      = POSTERS_PER_PAGE;
    int startIndex = itemPage * limit;
    bool ok = false; std::string err;
    runWithLoading([&]() {
        ok = client.getGlobalFavorites(serverUrl, auth, startIndex, limit, items, itemTotal);
        if (!ok) err = client.lastError();
    });
    if (!ok) { errMsg = err.empty() ? "Favorites unavailable" : err; state = State::Error; }
    else     { globFavMode = true; itemSel = 0; viewTop = 0; posterSel = 0; state = State::PostersLoad; }
}

void LibraryView::freeMovieSuggestions() {
    for (int i = 0; i < 4; i++) {
        if (movieContTex[i]) { GRRLIB_FreeTexture(movieContTex[i]); movieContTex[i] = nullptr; }
    }
    for (int i = 0; i < 8; i++) {
        if (movieRecentTex[i]) { GRRLIB_FreeTexture(movieRecentTex[i]); movieRecentTex[i] = nullptr; }
    }
    movieContItems.clear();
    movieRecentItems.clear();
}

void LibraryView::loadMovieSuggestions() {
    freeMovieSuggestions();
    runWithLoading([&]() {
        client.getMovieContinueWatching(serverUrl, auth, movieContItems);
        if ((int)movieContItems.size() > 4) movieContItems.resize(4);
        for (int i = 0; i < (int)movieContItems.size(); i++) {
            std::string imgBytes;
            client.getItemImageBytes(serverUrl, auth, movieContItems[i].id,
                                     POSTER_W, POSTER_H, imgBytes);
            if (!imgBytes.empty())
                movieContTex[i] = loadJPEGTexture((const u8*)imgBytes.data(),
                                                    (u32)imgBytes.size());
        }
        client.getMoviesLatest(serverUrl, auth, movieLibId, 8, movieRecentItems);
        for (int i = 0; i < (int)movieRecentItems.size(); i++) {
            std::string imgBytes;
            client.getItemImageBytes(serverUrl, auth, movieRecentItems[i].id,
                                     POSTER_W, POSTER_H, imgBytes);
            if (!imgBytes.empty())
                movieRecentTex[i] = loadJPEGTexture((const u8*)imgBytes.data(),
                                                      (u32)imgBytes.size());
        }
    });
    movieSuggestRow     = 0;
    movieSuggestContSel = 0;
    movieSuggestRecSel  = 0;
    movieSuggestContOff = 0;
    movieSuggestRecOff  = 0;
    state = State::MovieSuggestionsReady;
}

void LibraryView::loadItems() {
    items.clear();
    int limit      = posterMode ? POSTERS_PER_PAGE : ITEMS_PER_PAGE;
    int startIndex = itemPage * limit;
    bool ok = false; std::string err;
    runWithLoading([&]() {
        if (currentLibType == "music" && inItemsDrilldown) {
            // Artist drilldown: use AlbumArtistIds query (works for IDs from Search/Hints too)
            ok = client.getAlbumsByArtist(serverUrl, auth, currentLibId, startIndex, limit, items, itemTotal);
        } else {
            ok = client.getItems(serverUrl, auth, currentLibId, startIndex, limit, items, itemTotal);
        }
        if (!ok) err = client.lastError();
    });
    if (!ok) { errMsg = err; state = State::Error; }
    else     { globFavMode = false; itemSel = 0; viewTop = 0; state = posterMode ? State::PostersLoad : State::ItemsReady; }
}

void LibraryView::loadSeasons() {
    seasons.clear();
    bool ok = false; std::string err;
    runWithLoading([&]() {
        ok = client.getSeasons(serverUrl, auth, currentSeriesId, seasons);
        if (!ok) err = client.lastError();
    });
    if (!ok) { errMsg = err; state = State::Error; }
    else     { seasonSel = 0; seasonTop = 0; state = State::SeasonsReady; }
}

void LibraryView::freeTVSuggestions() {
    for (int i = 0; i < 4; i++) {
        if (tvContTex[i]) { GRRLIB_FreeTexture(tvContTex[i]); tvContTex[i] = nullptr; }
    }
    for (int i = 0; i < 8; i++) {
        if (tvRecentTex[i]) { GRRLIB_FreeTexture(tvRecentTex[i]); tvRecentTex[i] = nullptr; }
    }
    tvContItems.clear();
    tvRecentItems.clear();
}

void LibraryView::loadTVSuggestions() {
    freeTVSuggestions();
    runWithLoading([&]() {
        client.getTVContinueWatching(serverUrl, auth, tvContItems);
        if ((int)tvContItems.size() > 4) tvContItems.resize(4);
        for (int i = 0; i < (int)tvContItems.size(); i++) {
            std::string imgBytes;
            client.getItemBackdropBytes(serverUrl, auth, tvContItems[i],
                                        POSTER_W, POSTER_H, imgBytes);
            if (!imgBytes.empty())
                tvContTex[i] = loadJPEGTexture((const u8*)imgBytes.data(),
                                               (u32)imgBytes.size());
        }
        client.getTVSeriesLatest(serverUrl, auth, tvLibId, 8, tvRecentItems);
        for (int i = 0; i < (int)tvRecentItems.size(); i++) {
            std::string imgBytes;
            client.getItemImageBytes(serverUrl, auth, tvRecentItems[i].id,
                                     POSTER_W, POSTER_H, imgBytes);
            if (!imgBytes.empty())
                tvRecentTex[i] = loadJPEGTexture((const u8*)imgBytes.data(),
                                                  (u32)imgBytes.size());
        }
    });
    tvSuggestRow     = 0;
    tvSuggestContSel = 0;
    tvSuggestRecSel  = 0;
    tvSuggestContOff = 0;
    tvSuggestRecOff  = 0;
    state = State::TVSuggestionsReady;
}

void LibraryView::freeTVUpcoming() {
    for (int i = 0; i < 8; i++) {
        if (tvUpcomingTex[i]) { GRRLIB_FreeTexture(tvUpcomingTex[i]); tvUpcomingTex[i] = nullptr; }
    }
    tvUpcomingItems.clear();
}

void LibraryView::loadTVUpcoming() {
    freeTVUpcoming();
    runWithLoading([&]() {
        client.getTVUpcoming(serverUrl, auth, 8, tvUpcomingItems);
        for (int i = 0; i < (int)tvUpcomingItems.size(); i++) {
            std::string imgBytes;
            client.getItemBackdropBytes(serverUrl, auth, tvUpcomingItems[i],
                                        POSTER_W, POSTER_H, imgBytes);
            if (!imgBytes.empty())
                tvUpcomingTex[i] = loadJPEGTexture((const u8*)imgBytes.data(),
                                                    (u32)imgBytes.size());
        }
    });
    tvUpcomingSel = 0;
    tvUpcomingOff = 0;
    state = State::TVUpcomingReady;
}

void LibraryView::freeMusicSuggestions() {
    for (int i = 0; i < 8; i++) {
        if (musicRecentTex[i]) { GRRLIB_FreeTexture(musicRecentTex[i]); musicRecentTex[i] = nullptr; }
    }
    musicRecentItems.clear();
}

void LibraryView::loadMusicSuggestions() {
    freeMusicSuggestions();
    runWithLoading([&]() {
        client.getMusicLatest(serverUrl, auth, musicLibId, 8, musicRecentItems);
        for (int i = 0; i < (int)musicRecentItems.size(); i++) {
            std::string imgBytes;
            client.getItemImageBytes(serverUrl, auth, musicRecentItems[i].id,
                                     POSTER_W, POSTER_H, imgBytes);
            if (!imgBytes.empty())
                musicRecentTex[i] = loadJPEGTexture((const u8*)imgBytes.data(),
                                                     (u32)imgBytes.size());
        }
    });
    musicSuggestSel = 0;
    musicSuggestOff = 0;
    state = State::MusicSuggestionsReady;
}

void LibraryView::loadPlaylistsTab() {
    items.clear();
    bool ok = false; std::string err;
    int total = 0;
    runWithLoading([&]() {
        ok = client.getPlaylists(serverUrl, auth, 0, 50, items, total);
        if (!ok) err = client.lastError();
    });
    if (!ok) { errMsg = err; state = State::Error; }
    else     { itemSel = 0; viewTop = 0; itemTotal = total; state = State::ItemsReady; }
}

void LibraryView::loadEpisodes() {
    episodes.clear();
    bool ok = false; std::string err;
    runWithLoading([&]() {
        ok = client.getEpisodes(serverUrl, auth, currentSeriesId, currentSeasonId, episodes);
        if (!ok) err = client.lastError();
    });
    if (!ok) { errMsg = err; state = State::Error; }
    else     { episodeSel = 0; episodeTop = 0; state = State::EpisodesReady; }
}

void LibraryView::loadMusicTracks() {
    musicTracks.clear();
    bool ok = false; std::string err;
    runWithLoading([&]() {
        if (musicIsPlaylist)
            ok = client.getPlaylistTracks(serverUrl, auth, musicAlbumId, musicTracks);
        else
            ok = client.getAlbumTracks(serverUrl, auth, musicAlbumId, musicTracks);
        if (!ok) err = client.lastError();
        // Pick album artist from first track if not already known
        if (ok && musicAlbumArtist.empty() && !musicTracks.empty())
            musicAlbumArtist = musicTracks[0].artist;
    });
    if (!ok) { errMsg = err; state = State::Error; }
    else     { musicTrackSel = 0; musicTrackTop = 0; state = State::MusicTracksReady; }
}

void LibraryView::clampMusicTrackScroll() {
    int n = (int)musicTracks.size();
    if (musicTrackSel < 0) musicTrackSel = 0;
    if (musicTrackSel >= n) musicTrackSel = n > 0 ? n - 1 : 0;
    if (musicTrackSel < musicTrackTop) musicTrackTop = musicTrackSel;
    if (musicTrackSel >= musicTrackTop + MUSIC_TRACKS_VISIBLE)
        musicTrackTop = musicTrackSel - MUSIC_TRACKS_VISIBLE + 1;
}

// ---------------------------------------------------------------
// update() — returns true when the user exits to the main menu
// ---------------------------------------------------------------
bool LibraryView::update(ir_t& ir) {
    bool aPressed = Input::isAJustPressed();

    // Detect IR cursor movement: if the pointer moves significantly, switch to IR mode.
    // This prevents IR hover from overriding d-pad navigation on the same frame.
    if (ir.valid) {
        bool moved = (fabsf(ir.x - irLastX) > 3.0f || fabsf(ir.y - irLastY) > 3.0f);
        irLastX = ir.x;
        irLastY = ir.y;
        if (moved) irMode = true;
    } else {
        irLastX = -1.0f;
        irLastY = -1.0f;
    }

    switch (state) {
        case State::LibsInit:
            state = State::LibsLoad;
            return false;

        case State::LibsLoad:
            loadLibraries();
            return false;

        case State::LibsReady: {
            int n = (int)libraries.size();
            if (n == 0) return true;

            // User icon click (top right of header) → return to profile picker
            if (aPressed && irMode && ir.valid &&
                fabsf(ir.x - 614.0f) < 24.0f && fabsf(ir.y - 26.0f) < 24.0f) {
                SoundFX::play(SoundFX::FX::Back);
                return true;
            }

            if (homePage == 0) {
                // ---- Libraries page ----
                if (Input::isLPressed()) { itemPage = 0; state = State::GlobalFavoritesLoad; irMode = false; return false; } // [-] prev: wrap to Favourites
                if (Input::isRPressed()) { homePage = 1; irMode = false; return false; }                                    // [+] next: Activity

                // [1] button (mapped to WPAD_BUTTON_1) → open search
                if (WPAD_ButtonsDown(0) & WPAD_BUTTON_1) {
                    searchQuery.clear();
                    searchResults.clear();
                    searchSel       = 0;
                    searchTop       = 0;
                    srchKbRow       = 0;
                    srchKbCol       = 0;
                    srchKbPage      = 0;
                    srchKbShift     = false;
                    searchReturnState = State::LibsReady;
                    state = State::SearchInput;
                    irMode = false;
                    return false;
                }

                // D-pad navigation in library grid
                if (Input::isLeftPressed())  { if (libSel > 0)   libSel--; irMode = false; }
                if (Input::isRightPressed()) { if (libSel < n-1) libSel++; irMode = false; }
                if (Input::isUpPressed())    { int ns = libSel - TILE_COLS; if (ns >= 0) libSel = ns; irMode = false; }
                if (Input::isDownPressed())  { int ns = libSel + TILE_COLS; if (ns < n)  libSel = ns; irMode = false; }

                // IR hover
                bool irHoveredLib = false;
                if (irMode && ir.valid && !Input::isUpPressed() && !Input::isDownPressed()
                             && !Input::isLeftPressed() && !Input::isRightPressed()) {
                    for (int i = 0; i < n; i++) {
                        int col = i % TILE_COLS;
                        int row = i / TILE_COLS;
                        int tx  = GRID_X + col * (TILE_W + TILE_GAP);
                        int ty  = GRID_Y + row * (TILE_H + TILE_GAP);
                        if (ir.x >= tx && ir.x <= tx + TILE_W &&
                            ir.y >= ty && ir.y <= ty + TILE_H) {
                            libSel = i;
                            irHoveredLib = true;
                            irMode = true;
                            break;
                        }
                    }
                }

                if (aPressed) {
                    currentLibId     = libraries[libSel].id;
                    currentLibName   = libraries[libSel].name;
                    currentLibType   = libraries[libSel].collectionType;
                    posterMode       = (currentLibType == "movies" || currentLibType == "tvshows" || currentLibType == "boxsets");
                    inItemsDrilldown = false;
                    if (currentLibType == "movies") {
                        movieTab          = 0;
                        movieLibId        = currentLibId;
                        inBoxSetDrilldown = false;
                    }
                    if (currentLibType == "boxsets") {
                        inBoxSetDrilldown = false;
                    }
                    if (currentLibType == "tvshows") {
                        tvTab   = 0;
                        tvLibId = currentLibId;
                    }
                    if (currentLibType == "music") {
                        musicTab        = 0;
                        musicLibId      = currentLibId;
                        musicIsPlaylist = false;
                    }
                    itemPage = 0;
                    state = State::ItemsInit;
                }
            } else {
                // ---- Activity page ----
                if (Input::isBackPressed()) { homePage = 0; irMode = false; return false; }
                if (Input::isLPressed()) { homePage = 0; irMode = false; return false; }                                       // [-] prev: Libraries
                if (Input::isRPressed()) { itemPage = 0; state = State::GlobalFavoritesLoad; irMode = false; return false; }   // [+] next: Favourites

                int nc = (int)continueItems.size();
                int nu = (int)nextUpItems.size();

                // Clamp actRow to available rows
                if (actRow == 0 && nc == 0 && nu > 0) actRow = 1;
                if (actRow == 1 && nu == 0)            actRow = 0;

                // Clamp column selections
                if (nc > 0 && continueSel >= nc) continueSel = nc - 1;
                if (nu > 0 && nextUpSel   >= nu) nextUpSel   = nu - 1;

                int& sel = (actRow == 0) ? continueSel : nextUpSel;
                int  sz  = (actRow == 0) ? nc : nu;

                if (Input::isUpPressed()   && actRow == 1)           { actRow = 0; irMode = false; }
                if (Input::isDownPressed() && actRow == 0 && nu > 0) { actRow = 1; irMode = false; }
                if (Input::isLeftPressed()  && sel > 0)    { sel--; irMode = false; }
                if (Input::isRightPressed() && sel < sz-1) { sel++; irMode = false; }

                const int ACT_X0       = 20;
                const int ACT_CARD_W   = 190;
                const int ACT_CARD_GAP = 15;
                const int ACT_ROW0_Y   = 72;
                const int ACT_ROW1_Y   = 229;
                const int ACT_CARD_H   = 107;

                // IR hover
                bool irHoveredAct = false;
                if (irMode && ir.valid && !Input::isUpPressed() && !Input::isDownPressed()
                             && !Input::isLeftPressed() && !Input::isRightPressed()) {
                    for (int i = 0; i < nc; i++) {
                        int cx = ACT_X0 + i * (ACT_CARD_W + ACT_CARD_GAP);
                        if (ir.x >= cx && ir.x < cx + ACT_CARD_W &&
                            ir.y >= ACT_ROW0_Y && ir.y < ACT_ROW0_Y + ACT_CARD_H) {
                            continueSel = i; actRow = 0; irMode = true; irHoveredAct = true;
                        }
                    }
                    for (int i = 0; i < nu; i++) {
                        int cx = ACT_X0 + i * (ACT_CARD_W + ACT_CARD_GAP);
                        if (ir.x >= cx && ir.x < cx + ACT_CARD_W &&
                            ir.y >= ACT_ROW1_Y && ir.y < ACT_ROW1_Y + ACT_CARD_H) {
                            nextUpSel = i; actRow = 1; irMode = true; irHoveredAct = true;
                        }
                    }
                }

                if (aPressed) {
                    if (actRow == 0 && nc > 0 && continueSel < nc) {
                        detailItemId        = continueItems[continueSel].id;
                        detailReturnState   = State::LibsReady;
                        detailIsEpisodeHint = (continueItems[continueSel].type == "Episode");
                        state = State::DetailLoad;
                    } else if (actRow == 1 && nu > 0 && nextUpSel < nu) {
                        detailItemId        = nextUpItems[nextUpSel].id;
                        detailReturnState   = State::LibsReady;
                        detailIsEpisodeHint = (nextUpItems[nextUpSel].type == "Episode");
                        state = State::DetailLoad;
                    }
                }
            }
            return false;
        }

        case State::ItemsInit:
            state = State::ItemsLoad;
            return false;

        case State::ItemsLoad:
            loadItems();
            return false;

        case State::PostersLoad:
            loadPosters();
            return false;

        case State::ItemsReady: {
            int n = (int)items.size();

            if (Input::isBackPressed()) {
                if (inItemsDrilldown) {
                    inItemsDrilldown = false;
                    if (drilldownFromSearch) {
                        drilldownFromSearch = false;
                        state = State::SearchReady;
                    } else {
                        currentLibId   = parentLibId;
                        currentLibName = parentLibName;
                        itemPage       = parentItemPage;
                        state          = State::ItemsInit;
                    }
                } else {
                    state = State::LibsReady;
                }
                return false;
            }

            if (Input::isUpPressed())   { itemSel--; irMode = false; clampScroll(); }
            if (Input::isDownPressed()) { itemSel++; irMode = false; clampScroll(); }
            // Music library: -/+ switches tabs instead of paginating
            if (currentLibType == "music" && !inItemsDrilldown) {
                if (Input::isLPressed()) {
                    musicTab = (musicTab + 2) % 3;
                    itemPage = 0;
                    if      (musicTab == 0) state = State::ItemsInit;
                    else if (musicTab == 1) state = State::MusicSuggestionsLoad;
                    else                   state = State::PlaylistsLoad;
                    return false;
                }
                if (Input::isRPressed()) {
                    musicTab = (musicTab + 1) % 3;
                    itemPage = 0;
                    if      (musicTab == 0) state = State::ItemsInit;
                    else if (musicTab == 1) state = State::MusicSuggestionsLoad;
                    else                   state = State::PlaylistsLoad;
                    return false;
                }
            } else {
                if (Input::isLPressed() && itemPage > 0) {
                    itemPage--;
                    state = State::ItemsInit;
                }
                if (Input::isRPressed()) {
                    int totalPages = (itemTotal + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
                    if (itemPage + 1 < totalPages) {
                        itemPage++;
                        state = State::ItemsInit;
                    }
                }
            }

            // IR hover (skip when d-pad was used this frame)
            bool irHoveredItem = false;
            if (irMode && ir.valid && !Input::isUpPressed() && !Input::isDownPressed()
                         && !Input::isLeftPressed() && !Input::isRightPressed()) {
                for (int i = 0; i < ITEMS_VISIBLE; i++) {
                    int idx = viewTop + i;
                    if (idx >= n) break;
                    int ry = LIST_Y + i * ROW_H;
                    if (ir.x >= LIST_X && ir.x <= LIST_X + LIST_W &&
                        ir.y >= ry && ir.y < ry + ROW_H) {
                        itemSel = idx;
                        irMode = true;
                        irHoveredItem = true;
                        clampScroll();
                    }
                }
            }

            // A: select item
            if (aPressed && n > 0 && itemSel < n) {
                const JellyfinItem& sel = items[itemSel];
                SYS_Report("[LibraryView] A pressed: itemSel=%d n=%d type='%s' libType='%s' ir.valid=%d ir.y=%.0f\n",
                           itemSel, n, sel.type.c_str(), currentLibType.c_str(),
                           (int)ir.valid, (float)ir.y);
                if (currentLibType == "music") {
                    if (sel.type == "MusicArtist") {
                        // Drill into artist's albums — reuse item list with artist as parent
                        parentLibId      = currentLibId;
                        parentLibName    = currentLibName;
                        parentItemPage   = itemPage;
                        inItemsDrilldown = true;
                        currentLibId     = sel.id;
                        currentLibName   = sel.name;
                        itemPage         = 0;
                        state            = State::ItemsInit;
                    } else if (sel.type == "MusicAlbum") {
                        musicAlbumId     = sel.id;
                        musicAlbumName   = sel.name;
                        musicAlbumArtist.clear();
                        musicTracks.clear();
                        musicTrackSel    = 0;
                        musicTrackTop    = 0;
                        musicIsPlaylist  = false;
                        state = State::MusicTracksLoad;
                    } else if (sel.type == "Playlist") {
                        musicAlbumId     = sel.id;
                        musicAlbumName   = sel.name;
                        musicAlbumArtist.clear();
                        musicTracks.clear();
                        musicTrackSel    = 0;
                        musicTrackTop    = 0;
                        musicIsPlaylist  = true;
                        state = State::MusicTracksLoad;
                    } else if (sel.type == "Audio") {
                        // Single track selected (flat library browse)
                        MusicOverlay::Track t;
                        t.id    = sel.id;
                        t.title = sel.name;
                        t.runtimeTicks = sel.runtimeTicks;
                        pendingMusicTracks.clear();
                        pendingMusicTracks.push_back(t);
                        pendingMusicTrackIdx = 0;
                        pendingPlayIsMusic   = true;
                        return true;
                    } else {
                        // Unknown type (Folder, AlbumArtist, etc.) — drill in generically
                        SYS_Report("[LibraryView] music: unhandled type '%s', drilling in\n",
                                   sel.type.c_str());
                        parentLibId      = currentLibId;
                        parentLibName    = currentLibName;
                        parentItemPage   = itemPage;
                        inItemsDrilldown = true;
                        currentLibId     = sel.id;
                        currentLibName   = sel.name;
                        itemPage         = 0;
                        state            = State::ItemsInit;
                    }
                } else if (currentLibType == "playlists" && sel.type == "Playlist") {
                    musicAlbumId     = sel.id;
                    musicAlbumName   = sel.name;
                    musicAlbumArtist.clear();
                    musicTracks.clear();
                    musicTrackSel    = 0;
                    musicTrackTop    = 0;
                    musicIsPlaylist  = true;
                    state = State::MusicTracksLoad;
                } else {
                    // Non-music: open detail
                    if (!sel.id.empty()) {
                        detailItemId = sel.id;
                        detailReturnState = State::ItemsReady;
                        detailIsEpisodeHint = (sel.type == "Episode");
                        state = State::DetailLoad;
                    }
                }
            }
            return false;
        }

        case State::Error:
            if (Input::isBackPressed() || (aPressed && (!irMode || ir.valid))) return true;
            return false;

        case State::PostersReady: {
            int n = (int)items.size();
            if (Input::isBackPressed()) {
                freePosters();
                if (inBoxSetDrilldown) {
                    inBoxSetDrilldown = false;
                    if (drilldownFromSearch) {
                        drilldownFromSearch = false;
                        state = State::SearchReady;
                    } else if (currentLibType == "movies") {
                        // Go back to Collections tab with the original library
                        currentLibId   = movieLibId;
                        currentLibName = libraries[libSel].name;
                        movieTab       = 1;
                        itemPage       = 0;
                        state          = State::CollectionsLoad;
                    } else {
                        // Boxsets library: go back to the boxsets poster grid
                        currentLibId   = libraries[libSel].id;
                        currentLibName = libraries[libSel].name;
                        itemPage       = 0;
                        state          = State::ItemsInit;
                    }
                } else {
                    state = State::LibsReady;
                }
                return false;
            }
            // Movies tab switching with -/+ (replaces page-nav for movies)
            if (currentLibType == "movies" && !inBoxSetDrilldown) {
                if (Input::isLPressed()) {
                    freePosters();
                    movieTab = (movieTab + 3) % 4;
                    itemPage = 0;
                    if      (movieTab == 0) { currentLibId = movieLibId; state = State::ItemsInit; }
                    else if (movieTab == 1) state = State::CollectionsLoad;
                    else if (movieTab == 2) state = State::FavoritesLoad;
                    else                   state = State::MovieSuggestionsLoad;
                    return false;
                }
                if (Input::isRPressed()) {
                    freePosters();
                    movieTab = (movieTab + 1) % 4;
                    itemPage = 0;
                    if      (movieTab == 0) { currentLibId = movieLibId; state = State::ItemsInit; }
                    else if (movieTab == 1) state = State::CollectionsLoad;
                    else if (movieTab == 2) state = State::FavoritesLoad;
                    else                   state = State::MovieSuggestionsLoad;
                    return false;
                }
            }
            // TV shows tab switching with -/+
            if (currentLibType == "tvshows") {
                if (Input::isLPressed()) {
                    freePosters();
                    tvTab = (tvTab + 2) % 3;
                    itemPage = 0;
                    if      (tvTab == 0) { currentLibId = tvLibId; state = State::ItemsInit; }
                    else if (tvTab == 1) state = State::TVSuggestionsLoad;
                    else                 state = State::TVUpcomingLoad;
                    return false;
                }
                if (Input::isRPressed()) {
                    freePosters();
                    tvTab = (tvTab + 1) % 3;
                    itemPage = 0;
                    if      (tvTab == 0) { currentLibId = tvLibId; state = State::ItemsInit; }
                    else if (tvTab == 1) state = State::TVSuggestionsLoad;
                    else                 state = State::TVUpcomingLoad;
                    return false;
                }
            }
            if (Input::isLeftPressed()  && posterSel % POSTER_COLS > 0)               { posterSel--; irMode = false; }
            if (Input::isRightPressed() && posterSel % POSTER_COLS < POSTER_COLS-1
                                        && posterSel + 1 < n)                          { posterSel++; irMode = false; }
            // Up: move row, or go to previous page from top row
            if (Input::isUpPressed()) {
                if (posterSel >= POSTER_COLS) {
                    posterSel -= POSTER_COLS;
                } else if (itemPage > 0) {
                    freePosters(); itemPage--; state = State::ItemsInit;
                }
                irMode = false;
            }
            // Down: move row, or go to next page from bottom row
            if (Input::isDownPressed()) {
                if (posterSel + POSTER_COLS < n) {
                    posterSel += POSTER_COLS;
                } else {
                    int totalPages = (itemTotal + POSTERS_PER_PAGE - 1) / POSTERS_PER_PAGE;
                    if (itemPage + 1 < totalPages) { freePosters(); itemPage++; state = State::ItemsInit; }
                }
                irMode = false;
            }

            if (Input::isLPressed() && itemPage > 0) {
                freePosters();
                itemPage--;
                state = State::ItemsInit;
            }
            if (Input::isRPressed()) {
                int totalPages = (itemTotal + POSTERS_PER_PAGE - 1) / POSTERS_PER_PAGE;
                if (itemPage + 1 < totalPages) {
                    freePosters();
                    itemPage++;
                    state = State::ItemsInit;
                }
            }
            // Arrow button IR click
            bool arrowClicked = false;
            if (aPressed && ir.valid) {
                int totalPages = (itemTotal + POSTERS_PER_PAGE - 1) / POSTERS_PER_PAGE;
                int ax = (int)ir.x, ay = (int)ir.y;
                if (ax >= ARROW_CX - ARROW_HIT_R && ax < ARROW_CX + ARROW_HIT_R) {
                    if (ay >= ARROW_UP_CY - ARROW_HIT_R && ay < ARROW_UP_CY + ARROW_HIT_R
                            && itemPage > 0) {
                        freePosters(); itemPage--; state = State::ItemsInit;
                        arrowClicked = true;
                    }
                    if (ay >= ARROW_DN_CY - ARROW_HIT_R && ay < ARROW_DN_CY + ARROW_HIT_R
                            && itemPage + 1 < totalPages) {
                        freePosters(); itemPage++; state = State::ItemsInit;
                        arrowClicked = true;
                    }
                }
            }
            bool irHoveredPoster = false;
            if (irMode && ir.valid && !Input::isUpPressed() && !Input::isDownPressed()
                         && !Input::isLeftPressed() && !Input::isRightPressed()) {
                for (int i = 0; i < n && i < POSTER_VISIBLE; i++) {
                    int col = i % POSTER_COLS;
                    int row = i / POSTER_COLS;
                    int px  = POSTER_X0 + col * POSTER_STRIDE_X;
                    int py  = POSTER_Y0 + row * POSTER_STRIDE_Y;
                    if (ir.x >= px && ir.x < px + POSTER_W &&
                        ir.y >= py && ir.y < py + POSTER_H) {
                        posterSel = i;
                        irHoveredPoster = true;
                        irMode = true;
                    }
                }
            }
            // A on a poster: open detail view (skip if an arrow was just clicked)
            if (!arrowClicked && aPressed && posterSel < n) {
                if (!items[posterSel].id.empty()) {
                    if (items[posterSel].type == "Series") {
                        currentSeriesId    = items[posterSel].id;
                        currentSeriesName  = items[posterSel].name;
                        seasonsCallerState = State::PostersReady;
                        seasons.clear(); seasonSel = 0; seasonTop = 0;
                        state = State::SeasonsLoad;
                    } else if (items[posterSel].type == "BoxSet") {
                        // Drill into the collection (show its movies)
                        inBoxSetDrilldown = true;
                        currentLibId   = items[posterSel].id;
                        currentLibName = items[posterSel].name;
                        itemPage = 0;
                        freePosters();
                        state = State::ItemsInit;
                    } else {
                        detailItemId = items[posterSel].id;
                        detailReturnState = State::PostersReady;
                        state = State::DetailLoad;
                    }
                }
            }
            return false;
        }

        case State::SeasonsLoad:
            loadSeasons();
            return false;

        case State::SeasonsReady: {
            int n = (int)seasons.size();
            if (Input::isBackPressed()) {
                state = seasonsCallerState;
                return false;
            }
            if (Input::isUpPressed())   { if (seasonSel > 0)   { seasonSel--; clampSeasonScroll(); } irMode = false; }
            if (Input::isDownPressed()) { if (seasonSel < n-1) { seasonSel++; clampSeasonScroll(); } irMode = false; }
            bool irHoveredSeason = false;
            if (irMode && ir.valid && !Input::isUpPressed() && !Input::isDownPressed()) {
                for (int i = 0; i < ITEMS_VISIBLE; i++) {
                    int idx = seasonTop + i;
                    if (idx >= n) break;
                    int ry = LIST_Y + i * ROW_H;
                    if (ir.x >= LIST_X && ir.x <= LIST_X + LIST_W &&
                        ir.y >= ry && ir.y < ry + ROW_H) {
                        seasonSel = idx;
                        irHoveredSeason = true;
                        irMode = true;
                    }
                }
            }
            if (aPressed && n > 0 && seasonSel < n) {
                currentSeasonId   = seasons[seasonSel].id;
                currentSeasonName = seasons[seasonSel].name;
                episodes.clear(); episodeSel = 0; episodeTop = 0;
                state = State::EpisodesLoad;
            }
            return false;
        }

        case State::EpisodesLoad:
            loadEpisodes();
            return false;

        case State::EpisodesReady: {
            int n = (int)episodes.size();
            if (Input::isBackPressed()) {
                state = State::SeasonsReady;
                return false;
            }
            if (Input::isUpPressed())   { if (episodeSel > 0)   { episodeSel--; clampEpisodeScroll(); } irMode = false; }
            if (Input::isDownPressed()) { if (episodeSel < n-1) { episodeSel++; clampEpisodeScroll(); } irMode = false; }
            bool irHoveredEpisode = false;
            if (irMode && ir.valid && !Input::isUpPressed() && !Input::isDownPressed()) {
                for (int i = 0; i < ITEMS_VISIBLE; i++) {
                    int idx = episodeTop + i;
                    if (idx >= n) break;
                    int ry = LIST_Y + i * ROW_H;
                    if (ir.x >= LIST_X && ir.x <= LIST_X + LIST_W &&
                        ir.y >= ry && ir.y < ry + ROW_H) {
                        episodeSel = idx;
                        irHoveredEpisode = true;
                        irMode = true;
                    }
                }
            }
            if (aPressed && n > 0 && episodeSel < n) {
                detailItemId = episodes[episodeSel].id;
                detailReturnState = State::EpisodesReady;
                state = State::DetailLoad;
            }
            return false;
        }

        case State::DetailLoad:
            loadDetail();
            return false;

        case State::MusicTracksLoad:
            loadMusicTracks();
            return false;

        case State::CollectionsLoad:
            loadMovieCollections();
            return false;

        case State::FavoritesLoad:
            loadMovieFavorites();
            return false;

        case State::MovieSuggestionsLoad:
            loadMovieSuggestions();
            return false;

        case State::MovieSuggestionsReady: {
            if (Input::isBackPressed()) {
                freeMovieSuggestions();
                state = State::LibsReady;
                return false;
            }
            // Tab switch with -/+
            if (Input::isLPressed()) {
                freeMovieSuggestions();
                movieTab = (movieTab + 3) % 4;
                itemPage = 0;
                if      (movieTab == 0) { currentLibId = movieLibId; state = State::ItemsInit; }
                else if (movieTab == 1) state = State::CollectionsLoad;
                else if (movieTab == 2) state = State::FavoritesLoad;
                return false;
            }
            if (Input::isRPressed()) {
                freeMovieSuggestions();
                movieTab = (movieTab + 1) % 4;
                itemPage = 0;
                if      (movieTab == 0) { currentLibId = movieLibId; state = State::ItemsInit; }
                else if (movieTab == 1) state = State::CollectionsLoad;
                else if (movieTab == 2) state = State::FavoritesLoad;
                // movieTab == 3 → reload suggestions (already freed above)
                else state = State::MovieSuggestionsLoad;
                return false;
            }
            {
                int nc = (int)movieContItems.size();
                int nr = (int)movieRecentItems.size();

                if (Input::isUpPressed()   && movieSuggestRow == 1) { movieSuggestRow = 0; irMode = false; }
                if (Input::isDownPressed() && movieSuggestRow == 0 && nr > 0) { movieSuggestRow = 1; irMode = false; }

                int& colSel = (movieSuggestRow == 0) ? movieSuggestContSel : movieSuggestRecSel;
                int& colOff = (movieSuggestRow == 0) ? movieSuggestContOff : movieSuggestRecOff;
                int  sz     = (movieSuggestRow == 0) ? nc : nr;

                if (Input::isLeftPressed() && colSel > 0) {
                    colSel--; irMode = false;
                    if (colSel < colOff) colOff = colSel;
                }
                if (Input::isRightPressed() && colSel < sz - 1) {
                    colSel++; irMode = false;
                    if (colSel >= colOff + SUGG_VISIBLE) colOff = colSel - SUGG_VISIBLE + 1;
                }

                // IR hover: only switches the active ROW when the cursor *moves into*
                // a row zone — prevents a stationary pointer from overriding d-pad
                // row navigation every frame.
                const int SG_X0_H    = 15, SG_CW_H = POSTER_W, SG_CH_H = 160;
                const int SG_GAP_H   = 20;
                const int SG_ROW0_YH = 65, SG_ROW1_YH = 270;
                const int SG_ROW_W   = SUGG_VISIBLE * (SG_CW_H + SG_GAP_H) - SG_GAP_H;
                {
                    static int lastIrRow = -1;
                    int hoverRow = -1;
                    if (irMode && ir.valid && !Input::isUpPressed() && !Input::isDownPressed()
                                 && !Input::isLeftPressed() && !Input::isRightPressed()) {
                        if (ir.x >= SG_X0_H && ir.x < SG_X0_H + SG_ROW_W) {
                            if (ir.y >= SG_ROW0_YH && ir.y < SG_ROW0_YH + SG_CH_H) hoverRow = 0;
                            else if (ir.y >= SG_ROW1_YH && ir.y < SG_ROW1_YH + SG_CH_H) hoverRow = 1;
                        }
                    }
                    if (hoverRow != lastIrRow) {
                        if (hoverRow >= 0) { movieSuggestRow = hoverRow; irMode = true; }
                        lastIrRow = hoverRow;
                    }
                }

                if (aPressed) {
                    auto& selItems = (movieSuggestRow == 0) ? movieContItems : movieRecentItems;
                    int   sel      = (movieSuggestRow == 0) ? movieSuggestContSel : movieSuggestRecSel;
                    if (!selItems.empty() && sel < (int)selItems.size()) {
                        detailItemId        = selItems[sel].id;
                        detailReturnState   = State::MovieSuggestionsReady;
                        detailIsEpisodeHint = false;
                        state = State::DetailLoad;
                    }
                }
            }
            return false;
        }

        case State::TVSuggestionsLoad:
            loadTVSuggestions();
            return false;

        case State::TVUpcomingLoad:
            loadTVUpcoming();
            return false;

        case State::TVSuggestionsReady: {
            if (Input::isBackPressed()) {
                freeTVSuggestions();
                state = State::LibsReady;
                return false;
            }
            // Tab switch with -/+
            if (Input::isLPressed()) {
                freeTVSuggestions();
                tvTab = (tvTab + 2) % 3;
                itemPage = 0;
                if      (tvTab == 0) { currentLibId = tvLibId; state = State::ItemsInit; }
                else                  state = State::TVUpcomingLoad;
                return false;
            }
            if (Input::isRPressed()) {
                freeTVSuggestions();
                tvTab = (tvTab + 1) % 3;
                itemPage = 0;
                if      (tvTab == 0) { currentLibId = tvLibId; state = State::ItemsInit; }
                else if (tvTab == 1) state = State::TVSuggestionsLoad;
                else                  state = State::TVUpcomingLoad;
                return false;
            }
            {
                int nc = (int)tvContItems.size();
                int nr = (int)tvRecentItems.size();

                if (Input::isUpPressed()   && tvSuggestRow == 1) { tvSuggestRow = 0; irMode = false; }
                if (Input::isDownPressed() && tvSuggestRow == 0 && nr > 0) { tvSuggestRow = 1; irMode = false; }

                int& colSel = (tvSuggestRow == 0) ? tvSuggestContSel : tvSuggestRecSel;
                int& colOff = (tvSuggestRow == 0) ? tvSuggestContOff : tvSuggestRecOff;
                int  sz     = (tvSuggestRow == 0) ? nc : nr;

                if (Input::isLeftPressed() && colSel > 0) {
                    colSel--; irMode = false;
                    if (colSel < colOff) colOff = colSel;
                }
                if (Input::isRightPressed() && colSel < sz - 1) {
                    colSel++; irMode = false;
                    if (colSel >= colOff + SUGG_VISIBLE) colOff = colSel - SUGG_VISIBLE + 1;
                }

                // IR row hover
                const int SG_X0_H    = 15, SG_CW_H = POSTER_W, SG_CH_H = 160;
                const int SG_GAP_H   = 20;
                const int SG_ROW0_YH = 65, SG_ROW1_YH = 270;
                const int SG_ROW_W   = SUGG_VISIBLE * (SG_CW_H + SG_GAP_H) - SG_GAP_H;
                {
                    static int lastIrRowTV = -1;
                    int hoverRow = -1;
                    if (irMode && ir.valid && !Input::isUpPressed() && !Input::isDownPressed()
                                 && !Input::isLeftPressed() && !Input::isRightPressed()) {
                        if (ir.x >= SG_X0_H && ir.x < SG_X0_H + SG_ROW_W) {
                            if (ir.y >= SG_ROW0_YH && ir.y < SG_ROW0_YH + SG_CH_H) hoverRow = 0;
                            else if (ir.y >= SG_ROW1_YH && ir.y < SG_ROW1_YH + SG_CH_H) hoverRow = 1;
                        }
                    }
                    if (hoverRow != lastIrRowTV) {
                        if (hoverRow >= 0) { tvSuggestRow = hoverRow; irMode = true; }
                        lastIrRowTV = hoverRow;
                    }
                }

                if (aPressed) {
                    if (tvSuggestRow == 0) {
                        // Continue watching: episode → detail view
                        if (!tvContItems.empty() && tvSuggestContSel < nc) {
                            detailItemId        = tvContItems[tvSuggestContSel].id;
                            detailReturnState   = State::TVSuggestionsReady;
                            detailIsEpisodeHint = true;
                            state = State::DetailLoad;
                        }
                    } else {
                        // Recently added series → season list
                        if (!tvRecentItems.empty() && tvSuggestRecSel < nr) {
                            currentSeriesId    = tvRecentItems[tvSuggestRecSel].id;
                            currentSeriesName  = tvRecentItems[tvSuggestRecSel].name;
                            seasonsCallerState = State::TVSuggestionsReady;
                            seasons.clear(); seasonSel = 0; seasonTop = 0;
                            state = State::SeasonsLoad;
                        }
                    }
                }
            }
            return false;
        }

        case State::TVUpcomingReady: {
            if (Input::isBackPressed()) {
                freeTVUpcoming();
                state = State::LibsReady;
                return false;
            }
            // Tab switch with -/+
            if (Input::isLPressed()) {
                freeTVUpcoming();
                tvTab = (tvTab + 2) % 3;
                itemPage = 0;
                if      (tvTab == 0) { currentLibId = tvLibId; state = State::ItemsInit; }
                else if (tvTab == 1) state = State::TVSuggestionsLoad;
                else                  state = State::TVUpcomingLoad;
                return false;
            }
            if (Input::isRPressed()) {
                freeTVUpcoming();
                tvTab = (tvTab + 1) % 3;
                itemPage = 0;
                if      (tvTab == 0) { currentLibId = tvLibId; state = State::ItemsInit; }
                else if (tvTab == 1) state = State::TVSuggestionsLoad;
                // tvTab == 2 → reload upcoming
                else                  state = State::TVUpcomingLoad;
                return false;
            }
            {
                int nu = (int)tvUpcomingItems.size();
                if (Input::isLeftPressed() && tvUpcomingSel > 0) {
                    tvUpcomingSel--; irMode = false;
                    if (tvUpcomingSel < tvUpcomingOff) tvUpcomingOff = tvUpcomingSel;
                }
                if (Input::isRightPressed() && tvUpcomingSel < nu - 1) {
                    tvUpcomingSel++; irMode = false;
                    if (tvUpcomingSel >= tvUpcomingOff + SUGG_VISIBLE)
                        tvUpcomingOff = tvUpcomingSel - SUGG_VISIBLE + 1;
                }
                if (aPressed && nu > 0 && tvUpcomingSel < nu) {
                    detailItemId        = tvUpcomingItems[tvUpcomingSel].id;
                    detailReturnState   = State::TVUpcomingReady;
                    detailIsEpisodeHint = (tvUpcomingItems[tvUpcomingSel].type == "Episode");
                    state = State::DetailLoad;
                }
            }
            return false;
        }

        case State::MusicSuggestionsLoad:
            loadMusicSuggestions();
            return false;

        case State::PlaylistsLoad:
            loadPlaylistsTab();
            return false;

        case State::MusicSuggestionsReady: {
            if (Input::isBackPressed()) {
                freeMusicSuggestions();
                state = State::LibsReady;
                return false;
            }
            // Tab switch with -/+
            if (Input::isLPressed()) {
                freeMusicSuggestions();
                musicTab = (musicTab + 2) % 3;
                itemPage = 0;
                if      (musicTab == 0) { currentLibId = musicLibId; state = State::ItemsInit; }
                else                   state = State::PlaylistsLoad;
                return false;
            }
            if (Input::isRPressed()) {
                freeMusicSuggestions();
                musicTab = (musicTab + 1) % 3;
                itemPage = 0;
                if      (musicTab == 0) { currentLibId = musicLibId; state = State::ItemsInit; }
                else if (musicTab == 1) state = State::MusicSuggestionsLoad;
                else                   state = State::PlaylistsLoad;
                return false;
            }
            {
                int nr = (int)musicRecentItems.size();
                int cols = 4;
                int rows = (nr + cols - 1) / cols;
                int curRow = musicSuggestSel / cols;
                int curCol = musicSuggestSel % cols;

                if (Input::isLeftPressed()  && curCol > 0) {
                    musicSuggestSel--; irMode = false;
                }
                if (Input::isRightPressed() && curCol < cols - 1 && musicSuggestSel + 1 < nr) {
                    musicSuggestSel++; irMode = false;
                }
                if (Input::isUpPressed()   && curRow > 0) {
                    musicSuggestSel -= cols; irMode = false;
                }
                if (Input::isDownPressed() && curRow < rows - 1
                                          && musicSuggestSel + cols < nr) {
                    musicSuggestSel += cols; irMode = false;
                }
                if (musicSuggestSel < 0) musicSuggestSel = 0;
                if (musicSuggestSel >= nr && nr > 0) musicSuggestSel = nr - 1;

                if (aPressed && nr > 0 && musicSuggestSel < nr) {
                    const JellyfinItem& item = musicRecentItems[musicSuggestSel];
                    musicAlbumId     = item.id;
                    musicAlbumName   = item.name;
                    musicAlbumArtist.clear();
                    musicTracks.clear();
                    musicTrackSel    = 0;
                    musicTrackTop    = 0;
                    musicIsPlaylist  = false;
                    state = State::MusicTracksLoad;
                }
            }
            return false;
        }

        case State::MusicTracksReady: {
            int n = (int)musicTracks.size();
            if (Input::isBackPressed()) {
                if (musicTab == 1)
                    state = State::MusicSuggestionsReady;
                else
                    state = State::ItemsReady;
                return false;
            }
            if (Input::isUpPressed())   {
                if (musicTrackSel > 0) { musicTrackSel--; clampMusicTrackScroll(); }
                irMode = false;
            }
            if (Input::isDownPressed()) {
                if (musicTrackSel < n - 1) { musicTrackSel++; clampMusicTrackScroll(); }
                irMode = false;
            }
            // IR hover
            bool irHoveredTrack = false;
            if (irMode && ir.valid && !Input::isUpPressed() && !Input::isDownPressed()) {
                for (int i = 0; i < MUSIC_TRACKS_VISIBLE; i++) {
                    int idx = musicTrackTop + i;
                    if (idx >= n) break;
                    int ry = LIST_Y + i * ROW_H;
                    if (ir.x >= LIST_X && ir.x <= LIST_X + LIST_W &&
                        ir.y >= ry && ir.y < ry + ROW_H) {
                        musicTrackSel = idx;
                        clampMusicTrackScroll();
                        irMode = true;
                        irHoveredTrack = true;
                    }
                }
            }
            // A: play this track (with full album context for prev/next)
            if (aPressed && n > 0 && musicTrackSel < n) {
                pendingMusicTracks.clear();
                for (const auto& at : musicTracks) {
                    MusicOverlay::Track t;
                    t.id           = at.id;
                    t.title        = at.name;
                    t.artist       = at.artist.empty() ? musicAlbumArtist : at.artist;
                    t.album        = musicAlbumName;
                    t.runtimeTicks = at.runtimeTicks;
                    pendingMusicTracks.push_back(t);
                }
                pendingMusicTrackIdx = musicTrackSel;
                pendingPlayIsMusic   = true;
                return true;
            }
            return false;
        }

        case State::DetailReady: {
            // Play: A without IR (d-pad), or A with IR hovering the poster
            const int DPW = detailIsEpisode ? 210 : 200;
            const int DPH = detailIsEpisode ? 118 : 285;
            float dws  = WiiUtils::wsScaleX();
            int   dvisW = (int)(DPW * dws + 0.5f);
            if (aPressed) {
                if (detail.playbackPositionTicks > 0) {
                    // There is a saved position — ask the user Continue / Start Over
                    resumeSel = 0;
                    state = State::ResumePrompt;
                    return false;
                }
                // No saved position: play from the beginning immediately
                int audioIdx = (!detail.audioStreams.empty())
                    ? detail.audioStreams[detailAudioSel].index : 0;
                int subIdx = (detailSubSel >= 0 && !detail.subtitleStreams.empty())
                    ? detail.subtitleStreams[detailSubSel].index : -1;

                std::string url;
                std::string playSessionId;
                long long startTicks = 0LL;
                // Show the spinner immediately in both framebuffers so the
                // film/series detail page is hidden during the network call.
                drawLoadingFrame();
                drawLoadingFrame();
                if (!client.getTranscodingUrl(serverUrl, auth,
                                              detailItemId, detailItemId,
                                              audioIdx, subIdx, startTicks, url, playSessionId)) {
                    SYS_Report("[LibraryView] getTranscodingUrl failed: %s — using fallback\n",
                               client.lastError().c_str());
                    // Fallback: build URL directly (may result in direct play on server)
                    char fallback[1024];
                    if (subIdx >= 0) {
                        snprintf(fallback, sizeof(fallback),
                            "%s/Videos/%s/stream?Static=false&MediaSourceId=%s"
                            "&VideoCodec=h264&AudioCodec=aac&Container=ts"
                            "&Profile=baseline&Level=30"
                            "&MaxWidth=480&MaxHeight=272"
                            "&VideoBitrate=1500000&AudioBitrate=128000"
                            "&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false"
                            "&AudioStreamIndex=%d&SubtitleStreamIndex=%d&api_key=%s",
                            serverUrl.c_str(), detailItemId.c_str(), detailItemId.c_str(),
                            audioIdx, subIdx, auth.accessToken.c_str());
                    } else {
                        snprintf(fallback, sizeof(fallback),
                            "%s/Videos/%s/stream?Static=false&MediaSourceId=%s"
                            "&VideoCodec=h264&AudioCodec=aac&Container=ts"
                            "&Profile=baseline&Level=30"
                            "&MaxWidth=480&MaxHeight=272"
                            "&VideoBitrate=1500000&AudioBitrate=128000"
                            "&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false"
                            "&AudioStreamIndex=%d&api_key=%s",
                            serverUrl.c_str(), detailItemId.c_str(), detailItemId.c_str(),
                            audioIdx, auth.accessToken.c_str());
                    }
                    url = fallback;
                }
                pendingPlayUrl = url;
                pendingPlayItemId = detailItemId;
                pendingPlayMediaSourceId = detailItemId;
                pendingPlaySessionId = playSessionId;
                pendingPlayStartTimeTicks = startTicks;
                pendingPlayRuntimeTicks  = detail.runtimeTicks;
                // Audio / subtitle stream lists for in-player track switching
                pendingPlayAudioStreams = detail.audioStreams;
                pendingPlaySubStreams   = detail.subtitleStreams;
                pendingPlayAudioIdx = (detailAudioSel < (int)detail.audioStreams.size())
                                      ? detail.audioStreams[detailAudioSel].index : 0;
                pendingPlaySubIdx   = (detailSubSel >= 0 && detailSubSel < (int)detail.subtitleStreams.size())
                                      ? detail.subtitleStreams[detailSubSel].index : -1;
                // Propagate episode list context for the player overlay
                if (detailIsEpisode && !episodes.empty()) {
                    pendingPlayEpisodes   = episodes;
                    pendingPlaySeriesId   = currentSeriesId;
                    pendingPlayEpisodeIdx = 0;
                    for (int i = 0; i < (int)episodes.size(); ++i) {
                        if (episodes[i].id == detailItemId) {
                            pendingPlayEpisodeIdx = i;
                            break;
                        }
                    }
                } else {
                    pendingPlayEpisodes.clear();
                    pendingPlayEpisodeIdx = 0;
                    pendingPlaySeriesId.clear();
                }
                return true;
            }
            if (Input::isBackPressed()) {
                freeDetail();
                state = detailReturnState;
            } else if (Input::isUpPressed()) {
                detailFocusRow = 0;
            } else if (Input::isDownPressed()) {
                detailFocusRow = 1;
            } else if (Input::isLeftPressed()) {
                if (detailFocusRow == 0 && !detail.audioStreams.empty()) {
                    if (--detailAudioSel < 0) detailAudioSel = (int)detail.audioStreams.size() - 1;
                } else if (detailFocusRow == 1 && !detail.subtitleStreams.empty()) {
                    if (--detailSubSel < -1) detailSubSel = (int)detail.subtitleStreams.size() - 1;
                }
            } else if (Input::isRightPressed()) {
                if (detailFocusRow == 0 && !detail.audioStreams.empty()) {
                    if (++detailAudioSel >= (int)detail.audioStreams.size()) detailAudioSel = 0;
                } else if (detailFocusRow == 1) {
                    if (++detailSubSel >= (int)detail.subtitleStreams.size()) detailSubSel = -1;
                }
            }
            return false;
        }

        case State::ResumePrompt: {
            // IR hover: set resumeSel to whichever button the cursor is over
            if (ir.valid) {
                const int DW2 = 340, DH2 = 120;
                const int DX2 = (640 - DW2) / 2;
                const int DY2 = (480 - DH2) / 2;
                const int BW2 = 130, BH2 = 30, BGAP2 = 16;
                const int bTotalW2 = BW2 * 2 + BGAP2;
                const int bStartX2 = DX2 + (DW2 - bTotalW2) / 2;
                const int bY2      = DY2 + DH2 - BH2 - 14;
                for (int i = 0; i < 2; ++i) {
                    int bx = bStartX2 + i * (BW2 + BGAP2);
                    if (ir.x >= bx && ir.x < bx + BW2 &&
                        ir.y >= bY2 && ir.y < bY2 + BH2) {
                        resumeSel = i;
                        break;
                    }
                }
            }
            // Left/Right toggles between Continue (0) and Start Over (1)
            if (Input::isLeftPressed() || Input::isRightPressed())
                resumeSel ^= 1;
            if (Input::isBackPressed()) {
                state = State::DetailReady;
                return false;
            }
            if (aPressed) {
                int audioIdx = (!detail.audioStreams.empty())
                    ? detail.audioStreams[detailAudioSel].index : 0;
                int subIdx = (detailSubSel >= 0 && !detail.subtitleStreams.empty())
                    ? detail.subtitleStreams[detailSubSel].index : -1;

                long long startTicks = (resumeSel == 0) ? detail.playbackPositionTicks : 0LL;
                std::string url;
                std::string playSessionId;
                // Show the spinner immediately in both framebuffers so the
                // film/series detail page is hidden during the network call.
                drawLoadingFrame();
                drawLoadingFrame();
                if (!client.getTranscodingUrl(serverUrl, auth,
                                              detailItemId, detailItemId,
                                              audioIdx, subIdx, startTicks, url, playSessionId)) {
                    SYS_Report("[LibraryView] getTranscodingUrl failed: %s — using fallback\n",
                               client.lastError().c_str());
                    char fallback[1024];
                    if (subIdx >= 0) {
                        snprintf(fallback, sizeof(fallback),
                            "%s/Videos/%s/stream?Static=false&MediaSourceId=%s"
                            "&VideoCodec=h264&AudioCodec=aac&Container=ts"
                            "&Profile=baseline&Level=30"
                            "&MaxWidth=480&MaxHeight=272"
                            "&VideoBitrate=1500000&AudioBitrate=128000"
                            "&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false"
                            "&AudioStreamIndex=%d&SubtitleStreamIndex=%d&api_key=%s",
                            serverUrl.c_str(), detailItemId.c_str(), detailItemId.c_str(),
                            audioIdx, subIdx, auth.accessToken.c_str());
                    } else {
                        snprintf(fallback, sizeof(fallback),
                            "%s/Videos/%s/stream?Static=false&MediaSourceId=%s"
                            "&VideoCodec=h264&AudioCodec=aac&Container=ts"
                            "&Profile=baseline&Level=30"
                            "&MaxWidth=480&MaxHeight=272"
                            "&VideoBitrate=1500000&AudioBitrate=128000"
                            "&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false"
                            "&AudioStreamIndex=%d&api_key=%s",
                            serverUrl.c_str(), detailItemId.c_str(), detailItemId.c_str(),
                            audioIdx, auth.accessToken.c_str());
                    }
                    url = fallback;
                }
                pendingPlayUrl            = url;
                pendingPlayItemId         = detailItemId;
                pendingPlayMediaSourceId  = detailItemId;
                pendingPlaySessionId      = playSessionId;
                pendingPlayStartTimeTicks = startTicks;
                pendingPlayRuntimeTicks   = detail.runtimeTicks;
                pendingPlayAudioStreams    = detail.audioStreams;
                pendingPlaySubStreams      = detail.subtitleStreams;
                pendingPlayAudioIdx = (detailAudioSel < (int)detail.audioStreams.size())
                                      ? detail.audioStreams[detailAudioSel].index : 0;
                pendingPlaySubIdx   = (detailSubSel >= 0 && detailSubSel < (int)detail.subtitleStreams.size())
                                      ? detail.subtitleStreams[detailSubSel].index : -1;
                if (detailIsEpisode && !episodes.empty()) {
                    pendingPlayEpisodes   = episodes;
                    pendingPlaySeriesId   = currentSeriesId;
                    pendingPlayEpisodeIdx = 0;
                    for (int i = 0; i < (int)episodes.size(); ++i) {
                        if (episodes[i].id == detailItemId) {
                            pendingPlayEpisodeIdx = i;
                            break;
                        }
                    }
                } else {
                    pendingPlayEpisodes.clear();
                    pendingPlayEpisodeIdx = 0;
                    pendingPlaySeriesId.clear();
                }
                return true;
            }
            return false;
        }

        case State::GlobalFavoritesLoad:
            loadGlobalFavorites();
            return false;

        case State::GlobalFavoritesReady: {
            int n = (int)items.size();
            // Back → return to Libraries tab
            if (Input::isBackPressed()) {
                freePosters();
                globFavMode = false;
                state = State::LibsReady;
                return false;
            }
            // D-pad navigation in poster grid
            if (Input::isLeftPressed()  && posterSel % POSTER_COLS > 0)               { posterSel--; irMode = false; }
            if (Input::isRightPressed() && posterSel % POSTER_COLS < POSTER_COLS - 1
                                        && posterSel + 1 < n)                          { posterSel++; irMode = false; }
            if (Input::isUpPressed()) {
                if (posterSel >= POSTER_COLS) {
                    posterSel -= POSTER_COLS;
                } else if (itemPage > 0) {
                    freePosters(); itemPage--; state = State::GlobalFavoritesLoad;
                }
                irMode = false;
            }
            if (Input::isDownPressed()) {
                if (posterSel + POSTER_COLS < n) {
                    posterSel += POSTER_COLS;
                } else {
                    int totalPages = (itemTotal + POSTERS_PER_PAGE - 1) / POSTERS_PER_PAGE;
                    if (itemPage + 1 < totalPages) { freePosters(); itemPage++; state = State::GlobalFavoritesLoad; }
                }
                irMode = false;
            }
            // [-] prev tab = Activity, [+] next tab = Libraries (wrap)
            if (Input::isLPressed()) {
                freePosters(); globFavMode = false; homePage = 1; state = State::LibsReady;
                return false;
            }
            if (Input::isRPressed()) {
                freePosters(); globFavMode = false; homePage = 0; state = State::LibsReady;
                return false;
            }
            // Arrow button IR click (page nav)
            bool arrowClicked = false;
            if (aPressed && ir.valid) {
                int totalPages = (itemTotal + POSTERS_PER_PAGE - 1) / POSTERS_PER_PAGE;
                int ax = (int)ir.x, ay = (int)ir.y;
                const int GF_AR_UP = ARROW_UP_CY + 12;
                const int GF_AR_DN = ARROW_DN_CY + 12;
                if (ax >= ARROW_CX - ARROW_HIT_R && ax < ARROW_CX + ARROW_HIT_R) {
                    if (ay >= GF_AR_UP - ARROW_HIT_R && ay < GF_AR_UP + ARROW_HIT_R && itemPage > 0) {
                        freePosters(); itemPage--; state = State::GlobalFavoritesLoad;
                        arrowClicked = true;
                    }
                    if (ay >= GF_AR_DN - ARROW_HIT_R && ay < GF_AR_DN + ARROW_HIT_R && itemPage + 1 < totalPages) {
                        freePosters(); itemPage++; state = State::GlobalFavoritesLoad;
                        arrowClicked = true;
                    }
                }
            }
            // IR hover
            if (irMode && ir.valid && !Input::isUpPressed() && !Input::isDownPressed()
                         && !Input::isLeftPressed() && !Input::isRightPressed()) {
                for (int i = 0; i < n && i < POSTER_VISIBLE; i++) {
                    int col = i % POSTER_COLS;
                    int row = i / POSTER_COLS;
                    int px  = POSTER_X0 + col * POSTER_STRIDE_X;
                    int py  = (POSTER_Y0 + 12) + row * POSTER_STRIDE_Y;
                    if (ir.x >= px && ir.x < px + POSTER_W &&
                        ir.y >= py && ir.y < py + POSTER_H) {
                        posterSel = i;
                        irMode = true;
                    }
                }
            }
            // A on a poster
            if (!arrowClicked && aPressed && posterSel < n) {
                if (!items[posterSel].id.empty()) {
                    if (items[posterSel].type == "Series") {
                        currentSeriesId    = items[posterSel].id;
                        currentSeriesName  = items[posterSel].name;
                        seasonsCallerState = State::GlobalFavoritesReady;
                        seasons.clear(); seasonSel = 0; seasonTop = 0;
                        state = State::SeasonsLoad;
                    } else {
                        detailItemId      = items[posterSel].id;
                        detailReturnState = State::GlobalFavoritesReady;
                        state = State::DetailLoad;
                    }
                }
            }
            return false;
        }

        case State::SearchInput: {
            // B → back to wherever we came from
            if (Input::isBackPressed()) {
                state = searchReturnState;
                return false;
            }
            handleSearchVKB(ir);
            // + → execute search
            if (Input::isRPressed() && !searchQuery.empty()) {
                state = State::SearchLoad;
            }
            return false;
        }

        case State::SearchLoad:
            performSearch();
            return false;

        case State::SearchReady: {
            int n = (int)searchResults.size();
            // B → back to search input
            if (Input::isBackPressed()) {
                state = State::SearchInput;
                return false;
            }
            // [1] → new search
            if (WPAD_ButtonsDown(0) & WPAD_BUTTON_1) {
                searchQuery.clear();
                searchResults.clear();
                searchSel = 0; searchTop = 0;
                srchKbRow = 0; srchKbCol = 0;
                srchKbPage = 0; srchKbShift = false;
                state = State::SearchInput;
                irMode = false;
                return false;
            }
            if (n == 0) return false;

            if (Input::isUpPressed())   { searchSel--; irMode = false; clampSearchScroll(); }
            if (Input::isDownPressed()) { searchSel++; irMode = false; clampSearchScroll(); }

            // IR hover
            if (irMode && ir.valid && !Input::isUpPressed() && !Input::isDownPressed()) {
                for (int i = 0; i < SEARCH_VISIBLE; i++) {
                    int idx = searchTop + i;
                    if (idx >= n) break;
                    int ry = LIST_Y + i * ROW_H;
                    if (ir.x >= LIST_X && ir.x <= LIST_X + LIST_W &&
                        ir.y >= ry && ir.y < ry + ROW_H) {
                        searchSel = idx;
                        irMode = true;
                        clampSearchScroll();
                    }
                }
            }

            if (aPressed && n > 0 && searchSel < n) {
                const JellyfinItem& sel = searchResults[searchSel];
                if (sel.type == "Series") {
                    currentSeriesId   = sel.id;
                    currentSeriesName = sel.name;
                    seasonsCallerState = State::SearchReady;
                    seasons.clear(); seasonSel = 0; seasonTop = 0;
                    state = State::SeasonsLoad;
                } else if (sel.type == "MusicAlbum") {
                    musicAlbumId     = sel.id;
                    musicAlbumName   = sel.name;
                    musicAlbumArtist.clear();
                    musicTracks.clear();
                    musicTrackSel = 0;
                    musicTrackTop = 0;
                    musicIsPlaylist = false;
                    state = State::MusicTracksLoad;
                } else if (sel.type == "Playlist") {
                    musicAlbumId     = sel.id;
                    musicAlbumName   = sel.name;
                    musicAlbumArtist.clear();
                    musicTracks.clear();
                    musicTrackSel = 0;
                    musicTrackTop = 0;
                    musicIsPlaylist = true;
                    state = State::MusicTracksLoad;
                } else if (sel.type == "MusicArtist") {
                    parentLibId      = currentLibId;
                    parentLibName    = currentLibName;
                    parentItemPage   = itemPage;
                    inItemsDrilldown    = true;
                    drilldownFromSearch = true;
                    currentLibId     = sel.id;
                    currentLibName   = sel.name;
                    currentLibType   = "music";
                    posterMode       = false;
                    itemPage         = 0;
                    state            = State::ItemsInit;
                } else if (sel.type == "BoxSet") {
                    inBoxSetDrilldown   = true;
                    drilldownFromSearch = true;
                    currentLibId      = sel.id;
                    currentLibName    = sel.name;
                    posterMode        = true;
                    itemPage          = 0;
                    freePosters();
                    state = State::ItemsInit;
                } else if (sel.type == "Audio") {
                    MusicOverlay::Track t;
                    t.id    = sel.id;
                    t.title = sel.name;
                    t.runtimeTicks = sel.runtimeTicks;
                    pendingMusicTracks.clear();
                    pendingMusicTracks.push_back(t);
                    pendingMusicTrackIdx = 0;
                    pendingPlayIsMusic   = true;
                    return true;
                } else {
                    detailItemId        = sel.id;
                    detailReturnState   = State::SearchReady;
                    detailIsEpisodeHint = (sel.type == "Episode");
                    state = State::DetailLoad;
                }
            }
            return false;
        }
    }
    return false;
}

// ---------------------------------------------------------------
// render()
// ---------------------------------------------------------------
void LibraryView::render(ir_t& ir) {
    drawGradientBG();

    // Loading screen (shown one frame before blocking load)
    if (state == State::LibsInit || state == State::LibsLoad ||
        state == State::ItemsInit || state == State::ItemsLoad ||
        state == State::PostersLoad || state == State::DetailLoad ||
        state == State::SeasonsLoad || state == State::EpisodesLoad ||
        state == State::MusicTracksLoad ||
        state == State::CollectionsLoad || state == State::FavoritesLoad ||
        state == State::MovieSuggestionsLoad ||
        state == State::TVSuggestionsLoad || state == State::TVUpcomingLoad ||
        state == State::MusicSuggestionsLoad || state == State::PlaylistsLoad ||
        state == State::GlobalFavoritesLoad || state == State::SearchLoad) {
        float angle = (float)(ticks_to_millisecs(gettime()) % 1500) * (360.0f / 1500.0f);

        if (ringTex) {
            GRRLIB_SetMidHandle(ringTex, true);
            GRRLIB_DrawImg(320, 240, ringTex, angle, 1.0f, 1.0f, 0xFFFFFFFF);
            GRRLIB_SetMidHandle(ringTex, false);
        } else {
            drawCenteredText(0, 200, 640, "Loading...", 22, 0xCCCCCCFF);
        }
        drawCursor(ir);
        return;
    }

    // Error screen
    if (state == State::Error) {
        GRRLIB_PrintfTTF(40, 200, font, "Error:", 20, 0xFF5555FF);
        GRRLIB_PrintfTTF(40, 228, font, errMsg.c_str(), 16, 0xEEEEEEFF);
        GRRLIB_PrintfTTF(40, 440, font, "[A] / [B]: Back", 16, 0x889AABFF);
        drawCursor(ir);
        return;
    }

    // ---- Library grid / Activity ----
    if (state == State::LibsReady) {
        // Header: server name
        std::string srvLabel = auth.serverName;
        if (srvLabel.empty()) {
            srvLabel = serverUrl;
            size_t ss = srvLabel.find("://");
            if (ss != std::string::npos) srvLabel = srvLabel.substr(ss + 3);
            if (!srvLabel.empty() && srvLabel.back() == '/') srvLabel.pop_back();
        }
        // Header background bar
        GRRLIB_Rectangle(0, 0, 640, 52, 0x0E1826FF, 1);
        // Server name – small, left-aligned, muted
        GRRLIB_PrintfTTF(20, 6, font, srvLabel.c_str(), 13, 0x5577AAFF);

        // Page tabs – title-case, centered
        const char* t0 = "Libraries";
        const char* t1 = "Activity";
        const char* t2 = "Favorites";
        int tw0 = (int)GRRLIB_WidthTTF(font, t0, 16);
        int tw1 = (int)GRRLIB_WidthTTF(font, t1, 16);
        int tw2 = (int)GRRLIB_WidthTTF(font, t2, 16);
        const int TAB_GAP = 30;
        int totalTabW = tw0 + TAB_GAP + tw1 + TAB_GAP + tw2;
        int tabX0 = 320 - totalTabW / 2;
        int tabX1 = tabX0 + tw0 + TAB_GAP;
        int tabX2 = tabX1 + tw1 + TAB_GAP;
        GRRLIB_PrintfTTF(tabX0, 20, font, t0, 16, homePage == 0 ? 0xFFFFFFFF : 0x5B7A9AFF);
        GRRLIB_PrintfTTF(tabX1, 20, font, t1, 16, homePage == 1 ? 0xFFFFFFFF : 0x5B7A9AFF);
        GRRLIB_PrintfTTF(tabX2, 20, font, t2, 16, 0x5B7A9AFF);
        if (homePage == 0) GRRLIB_Rectangle(tabX0, 42, tw0, 3, 0x4499FFFF, 1);
        else               GRRLIB_Rectangle(tabX1, 42, tw1, 3, 0x4499FFFF, 1);
        GRRLIB_Rectangle(0, 51, 640, 1, 0x1C2D3CFF, 1);

        // User icon – top right of header
        if (userIconTex) {
            bool iconHover = ir.valid && fabsf(ir.x - 614.0f) < 24.0f && fabsf(ir.y - 26.0f) < 24.0f;
            float iconScale = iconHover ? 0.60f : 0.50f;
            GRRLIB_SetMidHandle(userIconTex, true);
            GRRLIB_DrawImg(614, 26, userIconTex, 0, iconScale, iconScale, 0xFFFFFFFF);
            GRRLIB_SetMidHandle(userIconTex, false);
        }

        if (homePage == 0) {
            // ---- Libraries grid ----
            int n = (int)libraries.size();
            for (int i = 0; i < n; i++) {
                int col = i % TILE_COLS;
                int row = i / TILE_COLS;
                int tx  = GRID_X + col * (TILE_W + TILE_GAP);
                int ty  = GRID_Y + row * (TILE_H + TILE_GAP);
                bool sel   = (i == libSel);
                bool hover = ir.valid &&
                             ir.x >= tx && ir.x <= tx + TILE_W &&
                             ir.y >= ty && ir.y <= ty + TILE_H;
                u32 bg = colorForType(libraries[i].collectionType, sel || hover);
                GRRLIB_Rectangle(tx, ty, TILE_W, TILE_H, bg, 1);
                if (sel || hover) {
                    GRRLIB_Rectangle(tx - 2,      ty - 2,      TILE_W + 4, 2,          0xFFFFFFFF, 1);
                    GRRLIB_Rectangle(tx - 2,      ty + TILE_H, TILE_W + 4, 2,          0xFFFFFFFF, 1);
                    GRRLIB_Rectangle(tx - 2,      ty - 2,      2,          TILE_H + 4, 0xFFFFFFFF, 1);
                    GRRLIB_Rectangle(tx + TILE_W, ty - 2,      2,          TILE_H + 4, 0xFFFFFFFF, 1);
                }
                drawCenteredText(tx, ty + 12, TILE_W, labelForType(libraries[i].collectionType), 12, 0xAABBCCFF);
                drawCenteredText(tx, ty + 38, TILE_W, libraries[i].name.c_str(), 18, 0xFFFFFFFF);
            }
            GRRLIB_Rectangle(0, 453, 640, 1, 0x334466FF, 1);
            // Tab cycle: Libraries →[+]→ Activity →[+]→ Favourites →[+]→ Libraries
            {
                struct { const char* label; int cx; } hints[] = {
                    { "[A] Open",    80  },
                    { "[1] Search", 240  },
                    { "[-/+] Tab",  400  },
                };
                for (auto& hi : hints) {
                    int w = (int)GRRLIB_WidthTTF(font, hi.label, 15);
                    GRRLIB_PrintfTTF(hi.cx - w / 2, 458, font, hi.label, 15, 0x889AABFF);
                }
            }

        } else {
            // ---- Activity page ----
            const int ACT_X0       = 20;
            const int ACT_CARD_W   = 190;
            const int ACT_CARD_H   = 107;
            const int ACT_CARD_GAP = 15;
            const int ACT_ROW0_Y   = 72;
            const int ACT_ROW1_Y   = 229;
            int nc = (int)continueItems.size();
            int nu = (int)nextUpItems.size();

            // Draw one activity card (thumbnail + border + progress bar + title)
            auto drawActCard = [&](int i, int cardY, const JellyfinItem& item,
                                   GRRLIB_texImg* tex, bool selCard, bool showPct,
                                   const std::string& mainTitle, const std::string& subTitle) {
                int   cx   = ACT_X0 + i * (ACT_CARD_W + ACT_CARD_GAP);
                float ws   = WiiUtils::wsScaleX();
                int   visW = (int)(ACT_CARD_W * ws + 0.5f);
                bool  hov  = ir.valid && ir.x >= cx && ir.x < cx + ACT_CARD_W
                                      && ir.y >= cardY && ir.y < cardY + ACT_CARD_H;

                GRRLIB_Rectangle(cx, cardY, visW, ACT_CARD_H, 0x0D1520FF, 1);
                if (tex && tex->w > 0) {
                    float sx = (float)ACT_CARD_W / tex->w;
                    float sy = (float)ACT_CARD_H / tex->h;
                    float s  = sx > sy ? sx : sy;
                    int   ox = (int)((visW  - tex->w * s * ws) * 0.5f);
                    int   oy = (int)((ACT_CARD_H - tex->h * s) * 0.5f);
                    GRRLIB_ClipDrawing(cx, cardY, (u32)visW, ACT_CARD_H);
                    GRRLIB_DrawImg(cx + ox, cardY + oy, tex, 0, s * ws, s, 0xFFFFFFFF);
                    GRRLIB_ClipReset();
                    GRRLIB_Rectangle(cx, cardY, visW, ACT_CARD_H,
                                     (selCard || hov) ? 0x00000055 : 0x00000088, 1);
                } else {
                    GRRLIB_Rectangle(cx, cardY, visW, ACT_CARD_H,
                                     (selCard || hov) ? 0x1E3A5FCC : 0x0D1520CC, 1);
                }
                if (selCard || hov) {
                    GRRLIB_Rectangle(cx - 2, cardY - 2,          visW + 4, 2,              0x4499FFFF, 1);
                    GRRLIB_Rectangle(cx - 2, cardY + ACT_CARD_H, visW + 4, 2,              0x4499FFFF, 1);
                    GRRLIB_Rectangle(cx - 2, cardY - 2,          2,        ACT_CARD_H + 4, 0x4499FFFF, 1);
                    GRRLIB_Rectangle(cx + visW, cardY - 2,       2,        ACT_CARD_H + 4, 0x4499FFFF, 1);
                }
                if (showPct && item.playbackPositionTicks > 0 && item.runtimeTicks > 0) {
                    int pct   = (int)(item.playbackPositionTicks * 100LL / item.runtimeTicks);
                    if (pct > 100) pct = 100;
                    int fillW = visW * pct / 100;
                    GRRLIB_Rectangle(cx, cardY + ACT_CARD_H - 4, visW,  4, 0x0A1420FF, 1);
                    if (fillW > 0)
                        GRRLIB_Rectangle(cx, cardY + ACT_CARD_H - 4, fillW, 4, 0x44AAFFFF, 1);
                }
                // Title + subtitle below card (strings pre-computed in buildActDisplayStrings)
                GRRLIB_PrintfTTF(cx, cardY + ACT_CARD_H + 2,  font, mainTitle.c_str(), 13, 0xEEEEEEFF);
                if (!subTitle.empty())
                    GRRLIB_PrintfTTF(cx, cardY + ACT_CARD_H + 16, font, subTitle.c_str(), 11, 0x889AABFF);
            };

            // Continue Watching row
            GRRLIB_PrintfTTF(ACT_X0, 58, font, "IN PROGRESS", 12,
                             nc > 0 ? 0x6688AAFF : 0x445566FF);
            if (nc > 0) {
                for (int i = 0; i < nc; i++)
                    drawActCard(i, ACT_ROW0_Y, continueItems[i], cwTextures[i],
                                i == continueSel && actRow == 0, true,
                                cwDisplayMain[i], cwDisplaySub[i]);
            } else {
                GRRLIB_PrintfTTF(ACT_X0 + 20, ACT_ROW0_Y + 40, font, "Nothing in progress", 14, 0x556677FF);
            }

            // Next Up row
            GRRLIB_PrintfTTF(ACT_X0, 215, font, "NEXT UP", 12,
                             nu > 0 ? 0x6688AAFF : 0x445566FF);
            if (nu > 0) {
                for (int i = 0; i < nu; i++)
                    drawActCard(i, ACT_ROW1_Y, nextUpItems[i], nextUpTextures[i],
                                i == nextUpSel && actRow == 1, false,
                                nuDisplayMain[i], nuDisplaySub[i]);
            } else {
                GRRLIB_PrintfTTF(ACT_X0 + 20, ACT_ROW1_Y + 40, font, "No next episode", 14, 0x556677FF);
            }

            GRRLIB_Rectangle(0, 453, 640, 1, 0x334466FF, 1);
            {
                struct { const char* label; int cx; } hints[] = {
                    { "[A] Open",      80  },
                    { "[Up/Down] Row", 240  },
                    { "[-/+] Tab",    400  },
                    { "[B] Back",      560  },
                };
                for (auto& hi : hints) {
                    int w = (int)GRRLIB_WidthTTF(font, hi.label, 15);
                    GRRLIB_PrintfTTF(hi.cx - w / 2, 458, font, hi.label, 15, 0x889AABFF);
                }
            }
        }
    }

    // ---- Items list ----
    if (state == State::ItemsReady) {
        // Header — music gets a tab bar; everything else gets a breadcrumb
        if (currentLibType == "music" && !inItemsDrilldown) {
            const char* mTabNames[3] = { "Albums", "Suggestions", "Playlists" };
            const int TAB_GAP = 20;
            int totalW = 0;
            for (int t = 0; t < 3; t++)
                totalW += (int)GRRLIB_WidthTTF(font, mTabNames[t], 14) + (t < 2 ? TAB_GAP : 0);
            int tabX = (640 - totalW) / 2;
            for (int t = 0; t < 3; t++) {
                int tw = (int)GRRLIB_WidthTTF(font, mTabNames[t], 14);
                u32 col = (t == musicTab) ? 0xFFFFFFFF : 0x5B7A9AFF;
                GRRLIB_PrintfTTF(tabX, 18, font, mTabNames[t], 14, col);
                if (t == musicTab)
                    GRRLIB_Rectangle(tabX, 42, tw, 2, 0x4499FFFF, 1);
                tabX += tw + TAB_GAP;
            }
            GRRLIB_Rectangle(0, 46, 640, 1, 0x334466FF, 1);
        } else {
            // Breadcrumb header
            char hdr[128];
            snprintf(hdr, sizeof(hdr), "< %s", currentLibName.c_str());
            GRRLIB_PrintfTTF(20, 14, font, hdr, 20, 0xFFFFFFFF);

            // Item range and total (right-aligned)
            int startIdx = itemPage * ITEMS_PER_PAGE;
            int endIdx   = startIdx + (int)items.size();
            char countStr[48];
            snprintf(countStr, sizeof(countStr), "%d-%d / %d", startIdx + 1, endIdx, itemTotal);
            int cw = (int)GRRLIB_WidthTTF(font, countStr, 15);
            GRRLIB_PrintfTTF(620 - cw, 18, font, countStr, 15, 0x889AABFF);

            GRRLIB_Rectangle(20, 46, 600, 1, 0x334466FF, 1);
        }

        int n = (int)items.size();
        for (int i = 0; i < ITEMS_VISIBLE; i++) {
            int idx = viewTop + i;
            if (idx >= n) break;

            bool sel   = (idx == itemSel);
            bool hover = ir.valid &&
                         ir.y >= LIST_Y + i * ROW_H &&
                         ir.y <  LIST_Y + (i + 1) * ROW_H &&
                         ir.x >= LIST_X && ir.x <= LIST_X + LIST_W;

            int ry = LIST_Y + i * ROW_H;
            u32 bg;
            if      (sel || hover) bg = 0x1E3A5FCC;
            else if (i % 2 == 0)   bg = 0x0D1520AA;
            else                   bg = 0x111827AA;

            GRRLIB_Rectangle(LIST_X, ry, LIST_W, ROW_H - 2, bg, 1);

            // Selection bar on the left
            if (sel) GRRLIB_Rectangle(LIST_X, ry, 3, ROW_H - 2, 0x4499FFFF, 1);

            // Title — truncate if too long
            std::string name = items[idx].name;
            if ((int)name.size() > 50) name = name.substr(0, 47) + "...";
            GRRLIB_PrintfTTF(LIST_X + 10, ry + 8, font, name.c_str(), 20, 0xEEEEEEFF);

            // Year (right side)
            if (items[idx].year > 0) {
                char yearStr[12];
                snprintf(yearStr, sizeof(yearStr), "%d", items[idx].year);
                int yw = (int)GRRLIB_WidthTTF(font, yearStr, 16);
                GRRLIB_PrintfTTF(LIST_X + LIST_W - yw - 8, ry + 10, font, yearStr, 16, 0x889AABFF);
            }
        }

        // Scrollbar (right edge)
        if (n > ITEMS_VISIBLE) {
            const int SB_X = 614, SB_Y = LIST_Y, SB_H = ITEMS_VISIBLE * ROW_H;
            int barH = SB_H * ITEMS_VISIBLE / n;
            if (barH < 16) barH = 16;
            int maxTop = n - ITEMS_VISIBLE;
            int barY   = SB_Y + (maxTop > 0 ? (SB_H - barH) * viewTop / maxTop : 0);
            GRRLIB_Rectangle(SB_X, SB_Y, 5, SB_H, 0x2A3A4AFF, 1);
            GRRLIB_Rectangle(SB_X, barY, 5, barH, 0x4499FFFF, 1);
        }

        // Footer
        GRRLIB_Rectangle(0, 453, 640, 1, 0x334466FF, 1);
        GRRLIB_PrintfTTF(20, 458, font, "[B] Back", 15, 0x889AABFF);

        if (currentLibType == "music" && !inItemsDrilldown) {
            GRRLIB_PrintfTTF(340, 458, font, "[-/+] Tab", 15, 0x889AABFF);
        } else {
            int totalPages = (itemTotal + ITEMS_PER_PAGE - 1) / ITEMS_PER_PAGE;
            if (totalPages > 1) {
                char pageStr[32];
                snprintf(pageStr, sizeof(pageStr), "[-/+] Page %d/%d", itemPage + 1, totalPages);
                int pw = (int)GRRLIB_WidthTTF(font, pageStr, 15);
                GRRLIB_PrintfTTF(320 - pw / 2, 458, font, pageStr, 15, 0x889AABFF);
            }
        }
    }

    // ---- Season list ----
    if (state == State::SeasonsReady) {
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "< %s", currentSeriesName.c_str());
        GRRLIB_PrintfTTF(20, 14, font, hdr, 20, 0xFFFFFFFF);
        GRRLIB_Rectangle(20, 46, 600, 1, 0x334466FF, 1);

        int n = (int)seasons.size();
        for (int i = 0; i < ITEMS_VISIBLE; i++) {
            int idx = seasonTop + i;
            if (idx >= n) break;
            bool sel   = (idx == seasonSel);
            bool hover = ir.valid &&
                         ir.y >= LIST_Y + i * ROW_H &&
                         ir.y <  LIST_Y + (i + 1) * ROW_H &&
                         ir.x >= LIST_X && ir.x <= LIST_X + LIST_W;
            int ry = LIST_Y + i * ROW_H;
            u32 bg;
            if      (sel || hover) bg = 0x1E3A5FCC;
            else if (i % 2 == 0)   bg = 0x0D1520AA;
            else                   bg = 0x111827AA;
            GRRLIB_Rectangle(LIST_X, ry, LIST_W, ROW_H - 2, bg, 1);
            if (sel) GRRLIB_Rectangle(LIST_X, ry, 3, ROW_H - 2, 0x4499FFFF, 1);
            GRRLIB_PrintfTTF(LIST_X + 10, ry + 8, font, filterDejaVu(seasons[idx].name, 45).c_str(), 20, 0xEEEEEEFF);
        }
        if (n > ITEMS_VISIBLE) {
            const int SB_X = 614, SB_Y = LIST_Y, SB_H = ITEMS_VISIBLE * ROW_H;
            int barH = SB_H * ITEMS_VISIBLE / n; if (barH < 16) barH = 16;
            int maxTop = n - ITEMS_VISIBLE;
            int barY   = SB_Y + (maxTop > 0 ? (SB_H - barH) * seasonTop / maxTop : 0);
            GRRLIB_Rectangle(SB_X, SB_Y, 5, SB_H, 0x2A3A4AFF, 1);
            GRRLIB_Rectangle(SB_X, barY, 5, barH, 0x4499FFFF, 1);
        }
        GRRLIB_Rectangle(0, 453, 640, 1, 0x334466FF, 1);
        GRRLIB_PrintfTTF(20, 458, font, "[A] Select   [B] Back", 15, 0x889AABFF);
    }

    // ---- Episode list ----
    if (state == State::EpisodesReady) {
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "< %s  /  %s",
                 currentSeriesName.c_str(), currentSeasonName.c_str());
        GRRLIB_PrintfTTF(20, 14, font, hdr, 18, 0xFFFFFFFF);
        GRRLIB_Rectangle(20, 44, 600, 1, 0x334466FF, 1);

        int n = (int)episodes.size();
        for (int i = 0; i < ITEMS_VISIBLE; i++) {
            int idx = episodeTop + i;
            if (idx >= n) break;
            bool sel   = (idx == episodeSel);
            bool hover = ir.valid &&
                         ir.y >= LIST_Y + i * ROW_H &&
                         ir.y <  LIST_Y + (i + 1) * ROW_H &&
                         ir.x >= LIST_X && ir.x <= LIST_X + LIST_W;
            int ry = LIST_Y + i * ROW_H;
            u32 bg;
            if      (sel || hover) bg = 0x1E3A5FCC;
            else if (i % 2 == 0)   bg = 0x0D1520AA;
            else                   bg = 0x111827AA;
            GRRLIB_Rectangle(LIST_X, ry, LIST_W, ROW_H - 2, bg, 1);
            if (sel) GRRLIB_Rectangle(LIST_X, ry, 3, ROW_H - 2, 0x4499FFFF, 1);
            // Episode label: "E01 - Name" (filtered to DejaVu-safe chars)
            char epLabel[256];
            if (episodes[idx].indexNumber > 0)
                snprintf(epLabel, sizeof(epLabel), "E%02d - %s",
                         episodes[idx].indexNumber, episodes[idx].name.c_str());
            else
                snprintf(epLabel, sizeof(epLabel), "%s", episodes[idx].name.c_str());
            std::string labelStr = filterDejaVu(epLabel, 45);
            GRRLIB_PrintfTTF(LIST_X + 10, ry + 8, font, labelStr.c_str(), 20, 0xEEEEEEFF);
        }
        if (n > ITEMS_VISIBLE) {
            const int SB_X = 614, SB_Y = LIST_Y, SB_H = ITEMS_VISIBLE * ROW_H;
            int barH = SB_H * ITEMS_VISIBLE / n; if (barH < 16) barH = 16;
            int maxTop = n - ITEMS_VISIBLE;
            int barY   = SB_Y + (maxTop > 0 ? (SB_H - barH) * episodeTop / maxTop : 0);
            GRRLIB_Rectangle(SB_X, SB_Y, 5, SB_H, 0x2A3A4AFF, 1);
            GRRLIB_Rectangle(SB_X, barY, 5, barH, 0x4499FFFF, 1);
        }
        GRRLIB_Rectangle(0, 453, 640, 1, 0x334466FF, 1);
        GRRLIB_PrintfTTF(20, 458, font, "[A] Details   [B] Back", 15, 0x889AABFF);
    }

    // ---- Music track list ----
    if (state == State::MusicTracksReady) {
        // Header: album name + artist
        char hdr[128];
        if (!musicAlbumArtist.empty())
            snprintf(hdr, sizeof(hdr), "%s  —  %s", musicAlbumName.c_str(), musicAlbumArtist.c_str());
        else
            snprintf(hdr, sizeof(hdr), "%s", musicAlbumName.c_str());
        std::string hdrStr = filterDejaVu(hdr, 55);
        GRRLIB_PrintfTTF(20, 14, font, hdrStr.c_str(), 18, 0xFFFFFFFF);
        // Green accent bar under header
        GRRLIB_Rectangle(20, 44, 600, 2, 0x33AA55FF, 1);

        int n = (int)musicTracks.size();
        for (int i = 0; i < MUSIC_TRACKS_VISIBLE; i++) {
            int idx = musicTrackTop + i;
            if (idx >= n) break;
            bool sel   = (idx == musicTrackSel);
            bool hover = ir.valid &&
                         ir.y >= LIST_Y + i * ROW_H &&
                         ir.y <  LIST_Y + (i + 1) * ROW_H &&
                         ir.x >= LIST_X && ir.x <= LIST_X + LIST_W;
            int ry = LIST_Y + i * ROW_H;
            u32 bg;
            if      (sel || hover) bg = 0x1A5A2ACC;   /* deep green highlight */
            else if (i % 2 == 0)   bg = 0x0D1520AA;
            else                   bg = 0x111827AA;
            GRRLIB_Rectangle(LIST_X, ry, LIST_W, ROW_H - 2, bg, 1);
            if (sel) GRRLIB_Rectangle(LIST_X, ry, 3, ROW_H - 2, 0x33AA55FF, 1);

            // Track number + title
            char trackLabel[256];
            const JellyfinAudioItem& at = musicTracks[idx];
            if (at.trackNumber > 0)
                snprintf(trackLabel, sizeof(trackLabel), "%2d.  %s", at.trackNumber, at.name.c_str());
            else
                snprintf(trackLabel, sizeof(trackLabel), "%s", at.name.c_str());
            std::string labelStr = filterDejaVu(trackLabel, 45);
            GRRLIB_PrintfTTF(LIST_X + 10, ry + 6, font, labelStr.c_str(), 20, 0xEEEEEEFF);

            // Duration on the right
            if (at.runtimeTicks > 0) {
                int secs = (int)(at.runtimeTicks / 10000000LL);
                char dur[12];
                snprintf(dur, sizeof(dur), "%d:%02d", secs / 60, secs % 60);
                int dw = (int)GRRLIB_WidthTTF(font, dur, 16);
                GRRLIB_PrintfTTF(LIST_X + LIST_W - dw - 4, ry + 8, font, dur, 16, 0x889AABFF);
            }
        }
        // Scrollbar
        if (n > MUSIC_TRACKS_VISIBLE) {
            const int SB_X = 614, SB_Y = LIST_Y, SB_H = MUSIC_TRACKS_VISIBLE * ROW_H;
            int barH = SB_H * MUSIC_TRACKS_VISIBLE / n; if (barH < 16) barH = 16;
            int maxTop = n - MUSIC_TRACKS_VISIBLE;
            int barY   = SB_Y + (maxTop > 0 ? (SB_H - barH) * musicTrackTop / maxTop : 0);
            GRRLIB_Rectangle(SB_X, SB_Y, 5, SB_H, 0x2A3A4AFF, 1);
            GRRLIB_Rectangle(SB_X, barY, 5, barH, 0x33AA55FF, 1);
        }
        GRRLIB_Rectangle(0, 453, 640, 1, 0x334466FF, 1);
        GRRLIB_PrintfTTF(20, 458, font, "[A] Play   [B] Back", 15, 0x889AABFF);
    }

    if (state == State::PostersReady) {
        // Header: breadcrumb left, count right
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "< %s", currentLibName.c_str());
        GRRLIB_PrintfTTF(20, 14, font, hdr, inBoxSetDrilldown ? 16 : 20, 0xFFFFFFFF);

        // For movies with tabs: draw tab bar in centre;
        // for tvshows with tabs: draw tv tab bar; otherwise title of hovered item
        if (currentLibType == "movies" && !inBoxSetDrilldown) {
            const char* tabNames[4] = { "Movies", "Collections", "Favorites", "Suggestions" };
            const int TAB_GAP = 20;
            int totalW = 0;
            for (int t = 0; t < 4; t++)
                totalW += (int)GRRLIB_WidthTTF(font, tabNames[t], 14) + (t < 3 ? TAB_GAP : 0);
            int tabX = (640 - totalW) / 2;
            for (int t = 0; t < 4; t++) {
                int tw = (int)GRRLIB_WidthTTF(font, tabNames[t], 14);
                u32 col = (t == movieTab) ? 0xFFFFFFFF : 0x5B7A9AFF;
                GRRLIB_PrintfTTF(tabX, 18, font, tabNames[t], 14, col);
                if (t == movieTab)
                    GRRLIB_Rectangle(tabX, 42, tw, 2, 0x4499FFFF, 1);
                tabX += tw + TAB_GAP;
            }
        } else if (currentLibType == "tvshows") {
            const char* tabNames[3] = { "Series", "Suggestions", "Coming Up" };
            const int TAB_GAP = 20;
            int totalW = 0;
            for (int t = 0; t < 3; t++)
                totalW += (int)GRRLIB_WidthTTF(font, tabNames[t], 14) + (t < 2 ? TAB_GAP : 0);
            int tabX = (640 - totalW) / 2;
            for (int t = 0; t < 3; t++) {
                int tw = (int)GRRLIB_WidthTTF(font, tabNames[t], 14);
                u32 col = (t == tvTab) ? 0xFFFFFFFF : 0x5B7A9AFF;
                GRRLIB_PrintfTTF(tabX, 18, font, tabNames[t], 14, col);
                if (t == tvTab)
                    GRRLIB_Rectangle(tabX, 42, tw, 2, 0x4499FFFF, 1);
                tabX += tw + TAB_GAP;
            }
        } else {
            // Non-movies: centred hovered-item title
            if (posterSel >= 0 && posterSel < (int)items.size()) {
                const int HDR_X = 140, HDR_W = 360;
                std::string t = items[posterSel].name;
                while ((int)GRRLIB_WidthTTF(font, t.c_str(), 16) > HDR_W && !t.empty()) {
                    while (!t.empty() && (t.back() & 0xC0) == 0x80) t.pop_back();
                    if (!t.empty()) t.pop_back();
                }
                int tw = (int)GRRLIB_WidthTTF(font, t.c_str(), 16);
                GRRLIB_PrintfTTF(HDR_X + (HDR_W - tw) / 2, 16, font, t.c_str(), 16, 0xE0E8FFFF);
            }
        }

        int startIdx = itemPage * POSTERS_PER_PAGE;
        int endIdx   = startIdx + (int)items.size();
        char countStr[48];
        snprintf(countStr, sizeof(countStr), "%d-%d / %d", startIdx + 1, endIdx, itemTotal);
        int cw = (int)GRRLIB_WidthTTF(font, countStr, 15);
        GRRLIB_PrintfTTF(620 - cw, 18, font, countStr, 15, 0x889AABFF);

        GRRLIB_Rectangle(20, 46, 600, 1, 0x334466FF, 1);

        int n = (int)items.size();
        for (int i = 0; i < n && i < POSTER_VISIBLE; i++) {
            int col = i % POSTER_COLS;
            int row = i / POSTER_COLS;
            int px  = POSTER_X0 + col * POSTER_STRIDE_X;
            int py  = POSTER_Y0 + row * POSTER_STRIDE_Y;
            bool sel = (i == posterSel);

            // Visual width after widescreen pre-squish (0.75 on 16:9, 1.0 on 4:3)
            float ws   = WiiUtils::wsScaleX();
            int   visW = (int)(POSTER_W * ws + 0.5f);

            // Selection highlight border
            if (sel)
                GRRLIB_Rectangle(px - 3, py - 3, visW + 6, POSTER_H + 6, 0x4499FFFF, 1);

            // Poster background (dark fallback for missing posters)
            GRRLIB_Rectangle(px, py, visW, POSTER_H, 0x1E2A3AFF, 1);

            // Poster image: fill mode — scale to cover the full slot, clip excess.
            if (posterTextures[i] && posterTextures[i]->w > 0 && posterTextures[i]->h > 0) {
                float sx = (float)POSTER_W / posterTextures[i]->w;
                float sy = (float)POSTER_H / posterTextures[i]->h;
                float s  = sx > sy ? sx : sy;  // FILL: larger scale covers slot
                float drawnW = posterTextures[i]->w * s * ws;
                float drawnH = posterTextures[i]->h * s;
                int ox = (int)((visW - drawnW) * 0.5f);
                int oy = (int)((POSTER_H - drawnH) * 0.5f);
                GRRLIB_ClipDrawing(px, py, (u32)visW, POSTER_H);
                GRRLIB_DrawImg(px + ox, py + oy, posterTextures[i], 0, s * ws, s, 0xFFFFFFFF);
                GRRLIB_ClipReset();
            }
            // Progress bar at bottom of poster slot
            if (i < (int)items.size() &&
                items[i].playbackPositionTicks > 0 && items[i].runtimeTicks > 0) {
                int pct   = (int)(items[i].playbackPositionTicks * 100LL / items[i].runtimeTicks);
                if (pct > 100) pct = 100;
                int barY  = py + POSTER_H - 4;
                int fillW = visW * pct / 100;
                GRRLIB_Rectangle(px, barY, visW, 4, 0x0A1420FF, 1);
                if (fillW > 0)
                    GRRLIB_Rectangle(px, barY, fillW, 4, 0x44AAFFFF, 1);
            }
        }

        // Page navigation arrows (right of poster grid)
        {
            int totalPages = (itemTotal + POSTERS_PER_PAGE - 1) / POSTERS_PER_PAGE;
            bool canPrev = itemPage > 0;
            bool canNext = itemPage + 1 < totalPages;
            bool hoverUp = ir.valid &&
                (int)ir.x >= ARROW_CX - ARROW_HIT_R && (int)ir.x < ARROW_CX + ARROW_HIT_R &&
                (int)ir.y >= ARROW_UP_CY - ARROW_HIT_R && (int)ir.y < ARROW_UP_CY + ARROW_HIT_R;
            bool hoverDn = ir.valid &&
                (int)ir.x >= ARROW_CX - ARROW_HIT_R && (int)ir.x < ARROW_CX + ARROW_HIT_R &&
                (int)ir.y >= ARROW_DN_CY - ARROW_HIT_R && (int)ir.y < ARROW_DN_CY + ARROW_HIT_R;

            u32 colUp = !canPrev ? 0x33445566 : (hoverUp ? 0xAADDFFFF : 0x6688AAFF);
            u32 colDn = !canNext ? 0x33445566 : (hoverDn ? 0xAADDFFFF : 0x6688AAFF);

            guVector triUp[3] = {
                {(float)ARROW_CX,       (float)(ARROW_UP_CY - 13), 0},
                {(float)(ARROW_CX - 14),(float)(ARROW_UP_CY + 11), 0},
                {(float)(ARROW_CX + 14),(float)(ARROW_UP_CY + 11), 0},
            };
            u32 cUp[3] = {colUp, colUp, colUp};
            GRRLIB_NGoneFilled(triUp, cUp, 3);

            guVector triDn[3] = {
                {(float)ARROW_CX,       (float)(ARROW_DN_CY + 13), 0},
                {(float)(ARROW_CX - 14),(float)(ARROW_DN_CY - 11), 0},
                {(float)(ARROW_CX + 14),(float)(ARROW_DN_CY - 11), 0},
            };
            u32 cDn[3] = {colDn, colDn, colDn};
            GRRLIB_NGoneFilled(triDn, cDn, 3);
        }

        // Footer
        GRRLIB_Rectangle(0, 453, 640, 1, 0x334466FF, 1);
        GRRLIB_PrintfTTF(20, 458, font, "[B] Back", 15, 0x889AABFF);
        // Show hovered/selected item title centred in the footer
        if (posterSel >= 0 && posterSel < (int)items.size() && !items[posterSel].name.empty()) {
            const int FTR_X = 110, FTR_W = 400;
            std::string t = items[posterSel].name;
            while ((int)GRRLIB_WidthTTF(font, t.c_str(), 15) > FTR_W && !t.empty()) {
                while (!t.empty() && (t.back() & 0xC0) == 0x80) t.pop_back();
                if (!t.empty()) t.pop_back();
            }
            if (!t.empty()) {
                int tw = (int)GRRLIB_WidthTTF(font, t.c_str(), 15);
                GRRLIB_PrintfTTF(FTR_X + (FTR_W - tw) / 2, 458, font, t.c_str(), 15, 0xDDEEFFFF);
            }
        }
        {
            int totalPages = (itemTotal + POSTERS_PER_PAGE - 1) / POSTERS_PER_PAGE;
            if (totalPages > 1) {
                char pageStr[24];
                snprintf(pageStr, sizeof(pageStr), "Page %d / %d", itemPage + 1, totalPages);
                int pw = (int)GRRLIB_WidthTTF(font, pageStr, 15);
                GRRLIB_PrintfTTF(620 - pw, 458, font, pageStr, 15, 0x889AABFF);
            }
        }
    }

    // ---- Global Favourites poster grid ----
    if (state == State::GlobalFavoritesReady) {
        // Header bar
        GRRLIB_Rectangle(0, 0, 640, 52, 0x0E1826FF, 1);
        GRRLIB_PrintfTTF(20, 6, font, auth.serverName.c_str(), 13, 0x5577AAFF);

        // 3-tab header (Libraries | Activity | Favorites) — Favorites selected
        {
            const char* t0 = "Libraries";
            const char* t1 = "Activity";
            const char* t2 = "Favorites";
            int tw0 = (int)GRRLIB_WidthTTF(font, t0, 16);
            int tw1 = (int)GRRLIB_WidthTTF(font, t1, 16);
            int tw2 = (int)GRRLIB_WidthTTF(font, t2, 16);
            const int TAB_GAP = 30;
            int totalTabW = tw0 + TAB_GAP + tw1 + TAB_GAP + tw2;
            int tabX0 = 320 - totalTabW / 2;
            int tabX1 = tabX0 + tw0 + TAB_GAP;
            int tabX2 = tabX1 + tw1 + TAB_GAP;
            GRRLIB_PrintfTTF(tabX0, 20, font, t0, 16, 0x5B7A9AFF);
            GRRLIB_PrintfTTF(tabX1, 20, font, t1, 16, 0x5B7A9AFF);
            GRRLIB_PrintfTTF(tabX2, 20, font, t2, 16, 0xFFFFFFFF);
            GRRLIB_Rectangle(tabX2, 42, tw2, 3, 0x4499FFFF, 1);
        }
        GRRLIB_Rectangle(0, 51, 640, 1, 0x1C2D3CFF, 1);

        // Count top-right
        {
            int startIdx = itemPage * POSTERS_PER_PAGE;
            int endIdx   = startIdx + (int)items.size();
            if (itemTotal > 0) {
                char countStr[48];
                snprintf(countStr, sizeof(countStr), "%d-%d / %d", startIdx + 1, endIdx, itemTotal);
                int cw = (int)GRRLIB_WidthTTF(font, countStr, 15);
                GRRLIB_PrintfTTF(620 - cw, 18, font, countStr, 15, 0x889AABFF);
            } else {
                GRRLIB_PrintfTTF(520, 18, font, "No Favourites", 15, 0x889AABFF);
            }
        }

        int n = (int)items.size();
        const int GF_Y0 = POSTER_Y0 + 12; // header is 52px tall vs 46px for library sub-pages
        for (int i = 0; i < n && i < POSTER_VISIBLE; i++) {
            int col = i % POSTER_COLS;
            int row = i / POSTER_COLS;
            int px  = POSTER_X0 + col * POSTER_STRIDE_X;
            int py  = GF_Y0 + row * POSTER_STRIDE_Y;
            bool sel   = (i == posterSel);
            float ws   = WiiUtils::wsScaleX();
            int   visW = (int)(POSTER_W * ws + 0.5f);

            if (sel)
                GRRLIB_Rectangle(px - 3, py - 3, visW + 6, POSTER_H + 6, 0x4499FFFF, 1);
            GRRLIB_Rectangle(px, py, visW, POSTER_H, 0x1E2A3AFF, 1);

            if (posterTextures[i] && posterTextures[i]->w > 0 && posterTextures[i]->h > 0) {
                float sx = (float)POSTER_W / posterTextures[i]->w;
                float sy = (float)POSTER_H / posterTextures[i]->h;
                float s  = sx > sy ? sx : sy;
                float drawnW = posterTextures[i]->w * s * ws;
                float drawnH = posterTextures[i]->h * s;
                int ox = (int)((visW - drawnW) * 0.5f);
                int oy = (int)((POSTER_H - drawnH) * 0.5f);
                GRRLIB_ClipDrawing(px, py, (u32)visW, POSTER_H);
                GRRLIB_DrawImg(px + ox, py + oy, posterTextures[i], 0, s * ws, s, 0xFFFFFFFF);
                GRRLIB_ClipReset();
            }

            // Type badge (Movie / Series / Album)
            if (i < (int)items.size()) {
                const char* badge = labelForType(items[i].type);
                GRRLIB_PrintfTTF(px + 2, py + POSTER_H - 14, font, badge, 10, 0xAABBCCCC);
            }
        }

        // Page navigation arrows
        {
            int totalPages = (itemTotal + POSTERS_PER_PAGE - 1) / POSTERS_PER_PAGE;
            bool canPrev = itemPage > 0;
            bool canNext = itemPage + 1 < totalPages;
            bool hoverUp = ir.valid &&
                (int)ir.x >= ARROW_CX - ARROW_HIT_R && (int)ir.x < ARROW_CX + ARROW_HIT_R &&
                (int)ir.y >= ARROW_UP_CY - ARROW_HIT_R && (int)ir.y < ARROW_UP_CY + ARROW_HIT_R;
            bool hoverDn = ir.valid &&
                (int)ir.x >= ARROW_CX - ARROW_HIT_R && (int)ir.x < ARROW_CX + ARROW_HIT_R &&
                (int)ir.y >= ARROW_DN_CY - ARROW_HIT_R && (int)ir.y < ARROW_DN_CY + ARROW_HIT_R;
            u32 colUp = !canPrev ? 0x33445566 : (hoverUp ? 0xAADDFFFF : 0x6688AAFF);
            u32 colDn = !canNext ? 0x33445566 : (hoverDn ? 0xAADDFFFF : 0x6688AAFF);
            const int GF_ARROW_UP = ARROW_UP_CY + 12;
            const int GF_ARROW_DN = ARROW_DN_CY + 12;
            guVector triUp[3] = {
                {(float)ARROW_CX,        (float)(GF_ARROW_UP - 13), 0},
                {(float)(ARROW_CX - 14), (float)(GF_ARROW_UP + 11), 0},
                {(float)(ARROW_CX + 14), (float)(GF_ARROW_UP + 11), 0},
            };
            u32 cUp[3] = {colUp, colUp, colUp};
            GRRLIB_NGoneFilled(triUp, cUp, 3);
            guVector triDn[3] = {
                {(float)ARROW_CX,        (float)(GF_ARROW_DN + 13), 0},
                {(float)(ARROW_CX - 14), (float)(GF_ARROW_DN - 11), 0},
                {(float)(ARROW_CX + 14), (float)(GF_ARROW_DN - 11), 0},
            };
            u32 cDn[3] = {colDn, colDn, colDn};
            GRRLIB_NGoneFilled(triDn, cDn, 3);
        }

        // Footer
        GRRLIB_Rectangle(0, 453, 640, 1, 0x334466FF, 1);
        {
            struct { const char* label; int cx; } hints[] = {
                { "[A] Detail",  160  },
                { "[-/+] Tab", 320  },
                { "[B] Back",   480  },
            };
            for (auto& hi : hints) {
                int w = (int)GRRLIB_WidthTTF(font, hi.label, 15);
                GRRLIB_PrintfTTF(hi.cx - w / 2, 458, font, hi.label, 15, 0x889AABFF);
            }
        }
    }

    // ---- Movie Suggestions (continue watching + recently added) ----
    if (state == State::MovieSuggestionsReady) {
        // Header with tab bar
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "< %s", currentLibName.c_str());
        GRRLIB_PrintfTTF(20, 14, font, hdr, 20, 0xFFFFFFFF);

        const char* tabNames[4] = { "Movies", "Collections", "Favorites", "Suggestions" };
        {
            const int TAB_GAP = 20;
            int totalW = 0;
            for (int t = 0; t < 4; t++)
                totalW += (int)GRRLIB_WidthTTF(font, tabNames[t], 14) + (t < 3 ? TAB_GAP : 0);
            int tabX = (640 - totalW) / 2;
            for (int t = 0; t < 4; t++) {
                int tw = (int)GRRLIB_WidthTTF(font, tabNames[t], 14);
                u32 col = (t == movieTab) ? 0xFFFFFFFF : 0x5B7A9AFF;
                GRRLIB_PrintfTTF(tabX, 18, font, tabNames[t], 14, col);
                if (t == movieTab)
                    GRRLIB_Rectangle(tabX, 42, tw, 2, 0x4499FFFF, 1);
                tabX += tw + TAB_GAP;
            }
        }
        GRRLIB_Rectangle(0, 46, 640, 1, 0x334466FF, 1);

        const int SG_X0    = 15;
        const int SG_CW    = POSTER_W;  // 130
        const int SG_CH    = 160;       // card height: row top + 160 + ~16 title = 176 per row
        const int SG_GAP   = 20;        // 4 × 130 + 3 × 20 = 580 fits in 640
        const int SG_ROW0_Y = 65;       // row 0: 65..225,  titles at 227
        const int SG_ROW1_Y = 270;      // row 1: 270..430, titles at 432; footer at 453

        float ws = WiiUtils::wsScaleX();

        // Helper: draw one card at exact screen column position cx
        auto drawSugCard = [&](int cx, int cardY,
                                const JellyfinItem& item,
                                GRRLIB_texImg* tex,
                                bool selCard) {
            int   visW = (int)(SG_CW * ws + 0.5f);
            bool  hov  = ir.valid && ir.x >= cx && ir.x < cx + SG_CW
                                  && ir.y >= cardY && ir.y < cardY + SG_CH;

            GRRLIB_Rectangle(cx, cardY, visW, SG_CH, 0x1E2A3AFF, 1);
            if (tex && tex->w > 0) {
                float sx = (float)SG_CW / tex->w;
                float sy = (float)SG_CH / tex->h;
                float s  = sx > sy ? sx : sy;
                int   ox = (int)((visW - tex->w * s * ws) * 0.5f);
                int   oy = (int)((SG_CH - tex->h * s) * 0.5f);
                GRRLIB_ClipDrawing(cx, cardY, (u32)visW, SG_CH);
                GRRLIB_DrawImg(cx + ox, cardY + oy, tex, 0, s * ws, s, 0xFFFFFFFF);
                GRRLIB_ClipReset();
            }
            if (selCard || hov) {
                GRRLIB_Rectangle(cx - 3, cardY - 3, visW + 6, 3,        0x4499FFFF, 1);
                GRRLIB_Rectangle(cx - 3, cardY + SG_CH, visW + 6, 3,    0x4499FFFF, 1);
                GRRLIB_Rectangle(cx - 3, cardY - 3, 3, SG_CH + 6,       0x4499FFFF, 1);
                GRRLIB_Rectangle(cx + visW, cardY - 3, 3, SG_CH + 6,    0x4499FFFF, 1);
            }
            // Progress bar
            if (item.playbackPositionTicks > 0 && item.runtimeTicks > 0) {
                int pct   = (int)(item.playbackPositionTicks * 100LL / item.runtimeTicks);
                if (pct > 100) pct = 100;
                int fillW = visW * pct / 100;
                GRRLIB_Rectangle(cx, cardY + SG_CH - 4, visW, 4, 0x0A1420FF, 1);
                if (fillW > 0)
                    GRRLIB_Rectangle(cx, cardY + SG_CH - 4, fillW, 4, 0x44AAFFFF, 1);
            }
            // Title below card (truncated to card width)
            std::string title = filterDejaVu(item.name, 20);
            while (!title.empty() && (int)GRRLIB_WidthTTF(font, title.c_str(), 12) > visW) {
                while (!title.empty() && (title.back() & 0xC0) == 0x80) title.pop_back();
                if (!title.empty()) title.pop_back();
            }
            GRRLIB_PrintfTTF(cx, cardY + SG_CH + 2, font, title.c_str(), 12, 0xDDDDDDFF);
        };

        int nc = (int)movieContItems.size();
        int nr = (int)movieRecentItems.size();

        // Helper: draw a scroll arrow indicator at the ends of a row
        auto drawRowArrow = [&](bool left, int rowY, bool active) {
            int   ax  = left ? (SG_X0 - 12) : (SG_X0 + SUGG_VISIBLE * (SG_CW + SG_GAP) - SG_GAP + 2);
            int   ay  = rowY + SG_CH / 2;
            u32   col = active ? 0xAADDFFFF : 0x2A3A4AFF;
            if (left) {
                guVector tri[3] = {{(float)(ax - 8),(float)ay,0},{(float)(ax+6),(float)(ay-8),0},{(float)(ax+6),(float)(ay+8),0}};
                u32 c[3] = {col,col,col}; GRRLIB_NGoneFilled(tri,c,3);
            } else {
                guVector tri[3] = {{(float)(ax+8),(float)ay,0},{(float)(ax-6),(float)(ay-8),0},{(float)(ax-6),(float)(ay+8),0}};
                u32 c[3] = {col,col,col}; GRRLIB_NGoneFilled(tri,c,3);
            }
        };

        // Row 0 — Continue Watching
        GRRLIB_PrintfTTF(SG_X0, 52, font, "IN PROGRESS", 11,
                         nc > 0 ? 0x6688AAFF : 0x445566FF);
        if (nc > 0) {
            for (int si = 0; si < SUGG_VISIBLE; si++) {
                int i = movieSuggestContOff + si;
                if (i >= nc) break;
                int cx = SG_X0 + si * (SG_CW + SG_GAP);
                drawSugCard(cx, SG_ROW0_Y, movieContItems[i], movieContTex[i],
                            i == movieSuggestContSel && movieSuggestRow == 0);
            }
            drawRowArrow(true,  SG_ROW0_Y, movieSuggestContOff > 0);
            drawRowArrow(false, SG_ROW0_Y, movieSuggestContOff + SUGG_VISIBLE < nc);
        } else {
            GRRLIB_PrintfTTF(SG_X0 + 20, SG_ROW0_Y + 60, font, "Aucun film en cours", 14, 0x556677FF);
        }

        // Row 1 — Recently Added
        GRRLIB_PrintfTTF(SG_X0, 252, font, "RECENTLY ADDED", 11,
                         nr > 0 ? 0x6688AAFF : 0x445566FF);
        if (nr > 0) {
            for (int si = 0; si < SUGG_VISIBLE; si++) {
                int i = movieSuggestRecOff + si;
                if (i >= nr) break;
                int cx = SG_X0 + si * (SG_CW + SG_GAP);
                drawSugCard(cx, SG_ROW1_Y, movieRecentItems[i], movieRecentTex[i],
                            i == movieSuggestRecSel && movieSuggestRow == 1);
            }
            drawRowArrow(true,  SG_ROW1_Y, movieSuggestRecOff > 0);
            drawRowArrow(false, SG_ROW1_Y, movieSuggestRecOff + SUGG_VISIBLE < nr);
        } else {
            GRRLIB_PrintfTTF(SG_X0 + 20, SG_ROW1_Y + 60, font, "No recent movies", 14, 0x556677FF);
        }

        GRRLIB_Rectangle(0, 453, 640, 1, 0x334466FF, 1);
        GRRLIB_PrintfTTF(20, 458, font, "[A] Detail  [B] Back", 15, 0x889AABFF);
        GRRLIB_PrintfTTF(340, 458, font, "[-/+] Tab", 15, 0x889AABFF);
    }

    // ---- TV Suggestions (continue watching episodes + recently added series) ----
    if (state == State::TVSuggestionsReady) {
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "< %s", currentLibName.c_str());
        GRRLIB_PrintfTTF(20, 14, font, hdr, 20, 0xFFFFFFFF);

        const char* tabNames[3] = { "Series", "Suggestions", "Coming Up" };
        const int TAB_GAP = 20;
        int totalW = 0;
        for (int t = 0; t < 3; t++)
            totalW += (int)GRRLIB_WidthTTF(font, tabNames[t], 14) + (t < 2 ? TAB_GAP : 0);
        int tabX = (640 - totalW) / 2;
        for (int t = 0; t < 3; t++) {
            int tw = (int)GRRLIB_WidthTTF(font, tabNames[t], 14);
            u32 col = (t == tvTab) ? 0xFFFFFFFF : 0x5B7A9AFF;
            GRRLIB_PrintfTTF(tabX, 18, font, tabNames[t], 14, col);
            if (t == tvTab)
                GRRLIB_Rectangle(tabX, 42, tw, 2, 0x4499FFFF, 1);
            tabX += tw + TAB_GAP;
        }
        GRRLIB_Rectangle(0, 46, 640, 1, 0x334466FF, 1);

        const int SG_X0     = 15;
        const int SG_CW     = POSTER_W;
        const int SG_CH     = 160;
        const int SG_GAP    = 20;
        const int SG_ROW0_Y = 65;
        const int SG_ROW1_Y = 270;

        float ws = WiiUtils::wsScaleX();

        auto drawTVCard = [&](int cx, int cardY,
                               const JellyfinItem& item,
                               GRRLIB_texImg* tex,
                               bool selCard) {
            int  visW = (int)(SG_CW * ws + 0.5f);
            bool hov  = ir.valid && ir.x >= cx && ir.x < cx + SG_CW
                                 && ir.y >= cardY && ir.y < cardY + SG_CH;
            GRRLIB_Rectangle(cx, cardY, visW, SG_CH, 0x1E2A3AFF, 1);
            if (tex && tex->w > 0) {
                float sx = (float)SG_CW / tex->w;
                float sy = (float)SG_CH / tex->h;
                float s  = sx > sy ? sx : sy;
                int   ox = (int)((visW - tex->w * s * ws) * 0.5f);
                int   oy = (int)((SG_CH - tex->h * s) * 0.5f);
                GRRLIB_ClipDrawing(cx, cardY, (u32)visW, SG_CH);
                GRRLIB_DrawImg(cx + ox, cardY + oy, tex, 0, s * ws, s, 0xFFFFFFFF);
                GRRLIB_ClipReset();
            }
            if (selCard || hov) {
                GRRLIB_Rectangle(cx - 3, cardY - 3, visW + 6, 3,     0x4499FFFF, 1);
                GRRLIB_Rectangle(cx - 3, cardY + SG_CH, visW + 6, 3, 0x4499FFFF, 1);
                GRRLIB_Rectangle(cx - 3, cardY - 3, 3, SG_CH + 6,    0x4499FFFF, 1);
                GRRLIB_Rectangle(cx + visW, cardY - 3, 3, SG_CH + 6, 0x4499FFFF, 1);
            }
            if (item.playbackPositionTicks > 0 && item.runtimeTicks > 0) {
                int pct   = (int)(item.playbackPositionTicks * 100LL / item.runtimeTicks);
                if (pct > 100) pct = 100;
                int fillW = visW * pct / 100;
                GRRLIB_Rectangle(cx, cardY + SG_CH - 4, visW, 4, 0x0A1420FF, 1);
                if (fillW > 0)
                    GRRLIB_Rectangle(cx, cardY + SG_CH - 4, fillW, 4, 0x44AAFFFF, 1);
            }
            std::string title = filterDejaVu(
                item.type == "Episode" && !item.seriesName.empty() ? item.seriesName : item.name, 20);
            while (!title.empty() && (int)GRRLIB_WidthTTF(font, title.c_str(), 12) > visW) {
                while (!title.empty() && (title.back() & 0xC0) == 0x80) title.pop_back();
                if (!title.empty()) title.pop_back();
            }
            GRRLIB_PrintfTTF(cx, cardY + SG_CH + 2, font, title.c_str(), 12, 0xDDDDDDFF);
        };

        auto drawTVArrow = [&](bool left, int rowY, bool active) {
            int ax  = left ? (SG_X0 - 12) : (SG_X0 + SUGG_VISIBLE * (SG_CW + SG_GAP) - SG_GAP + 2);
            int ay  = rowY + SG_CH / 2;
            u32 col = active ? 0xAADDFFFF : 0x2A3A4AFF;
            if (left) {
                guVector tri[3] = {{(float)(ax-8),(float)ay,0},{(float)(ax+6),(float)(ay-8),0},{(float)(ax+6),(float)(ay+8),0}};
                u32 c[3] = {col,col,col}; GRRLIB_NGoneFilled(tri,c,3);
            } else {
                guVector tri[3] = {{(float)(ax+8),(float)ay,0},{(float)(ax-6),(float)(ay-8),0},{(float)(ax-6),(float)(ay+8),0}};
                u32 c[3] = {col,col,col}; GRRLIB_NGoneFilled(tri,c,3);
            }
        };

        int nc = (int)tvContItems.size();
        int nr = (int)tvRecentItems.size();

        // Row 0 — Continue Watching (episodes)
        GRRLIB_PrintfTTF(SG_X0, 52, font, "IN PROGRESS", 11,
                         nc > 0 ? 0x6688AAFF : 0x445566FF);
        if (nc > 0) {
            for (int si = 0; si < SUGG_VISIBLE; si++) {
                int i = tvSuggestContOff + si;
                if (i >= nc) break;
                int cx = SG_X0 + si * (SG_CW + SG_GAP);
                drawTVCard(cx, SG_ROW0_Y, tvContItems[i], tvContTex[i],
                           i == tvSuggestContSel && tvSuggestRow == 0);
            }
            drawTVArrow(true,  SG_ROW0_Y, tvSuggestContOff > 0);
            drawTVArrow(false, SG_ROW0_Y, tvSuggestContOff + SUGG_VISIBLE < nc);
        } else {
            GRRLIB_PrintfTTF(SG_X0 + 20, SG_ROW0_Y + 60, font, "No episode in progress", 14, 0x556677FF);
        }

        // Row 1 — Recently Added (series)
        GRRLIB_PrintfTTF(SG_X0, 252, font, "RECENT SERIES", 11,
                         nr > 0 ? 0x6688AAFF : 0x445566FF);
        if (nr > 0) {
            for (int si = 0; si < SUGG_VISIBLE; si++) {
                int i = tvSuggestRecOff + si;
                if (i >= nr) break;
                int cx = SG_X0 + si * (SG_CW + SG_GAP);
                drawTVCard(cx, SG_ROW1_Y, tvRecentItems[i], tvRecentTex[i],
                           i == tvSuggestRecSel && tvSuggestRow == 1);
            }
            drawTVArrow(true,  SG_ROW1_Y, tvSuggestRecOff > 0);
            drawTVArrow(false, SG_ROW1_Y, tvSuggestRecOff + SUGG_VISIBLE < nr);
        } else {
            GRRLIB_PrintfTTF(SG_X0 + 20, SG_ROW1_Y + 60, font, "No recent series", 14, 0x556677FF);
        }

        GRRLIB_Rectangle(0, 453, 640, 1, 0x334466FF, 1);
        GRRLIB_PrintfTTF(20, 458, font, "[A] Select  [B] Back", 15, 0x889AABFF);
        GRRLIB_PrintfTTF(380, 458, font, "[-/+] Tab", 15, 0x889AABFF);
    }

    // ---- TV Upcoming (unaired episodes) ----
    if (state == State::TVUpcomingReady) {
        char hdr[128];
        snprintf(hdr, sizeof(hdr), "< %s", currentLibName.c_str());
        GRRLIB_PrintfTTF(20, 14, font, hdr, 20, 0xFFFFFFFF);

        const char* tabNames[3] = { "Series", "Suggestions", "Coming Up" };
        const int TAB_GAP = 20;
        int totalW = 0;
        for (int t = 0; t < 3; t++)
            totalW += (int)GRRLIB_WidthTTF(font, tabNames[t], 14) + (t < 2 ? TAB_GAP : 0);
        int tabX = (640 - totalW) / 2;
        for (int t = 0; t < 3; t++) {
            int tw = (int)GRRLIB_WidthTTF(font, tabNames[t], 14);
            u32 col = (t == tvTab) ? 0xFFFFFFFF : 0x5B7A9AFF;
            GRRLIB_PrintfTTF(tabX, 18, font, tabNames[t], 14, col);
            if (t == tvTab)
                GRRLIB_Rectangle(tabX, 42, tw, 2, 0x4499FFFF, 1);
            tabX += tw + TAB_GAP;
        }
        GRRLIB_Rectangle(0, 46, 640, 1, 0x334466FF, 1);

        int nu = (int)tvUpcomingItems.size();

        if (nu == 0) {
            // Empty state
            int mw = (int)GRRLIB_WidthTTF(font, "No content.", 20);
            GRRLIB_PrintfTTF(320 - mw / 2, 185, font, "No content.", 20, 0xCCCCCCFF);
            const char* hint = "Make sure metadata download is enabled.";
            int hw = (int)GRRLIB_WidthTTF(font, hint, 13);
            GRRLIB_PrintfTTF(320 - hw / 2, 215, font, hint, 13, 0x889AABFF);
        } else {
            float ws = WiiUtils::wsScaleX();
            const int SG_X0  = 15;
            const int SG_CW  = POSTER_W;
            const int SG_CH  = 160;
            const int SG_GAP = 20;
            const int SG_Y   = 80;

            GRRLIB_PrintfTTF(SG_X0, 55, font, "COMING UP", 11, 0x6688AAFF);

            for (int si = 0; si < SUGG_VISIBLE; si++) {
                int i = tvUpcomingOff + si;
                if (i >= nu) break;
                int cx   = SG_X0 + si * (SG_CW + SG_GAP);
                int visW = (int)(SG_CW * ws + 0.5f);
                bool sel = (i == tvUpcomingSel);
                bool hov = ir.valid && ir.x >= cx && ir.x < cx + SG_CW
                                    && ir.y >= SG_Y && ir.y < SG_Y + SG_CH;

                GRRLIB_Rectangle(cx, SG_Y, visW, SG_CH, 0x1E2A3AFF, 1);
                if (tvUpcomingTex[i] && tvUpcomingTex[i]->w > 0) {
                    float sx = (float)SG_CW / tvUpcomingTex[i]->w;
                    float sy = (float)SG_CH / tvUpcomingTex[i]->h;
                    float s  = sx > sy ? sx : sy;
                    int   ox = (int)((visW - tvUpcomingTex[i]->w * s * ws) * 0.5f);
                    int   oy = (int)((SG_CH - tvUpcomingTex[i]->h * s) * 0.5f);
                    GRRLIB_ClipDrawing(cx, SG_Y, (u32)visW, SG_CH);
                    GRRLIB_DrawImg(cx + ox, SG_Y + oy, tvUpcomingTex[i], 0, s * ws, s, 0xFFFFFFFF);
                    GRRLIB_ClipReset();
                }
                if (sel || hov) {
                    GRRLIB_Rectangle(cx - 3, SG_Y - 3, visW + 6, 3,          0x4499FFFF, 1);
                    GRRLIB_Rectangle(cx - 3, SG_Y + SG_CH, visW + 6, 3,      0x4499FFFF, 1);
                    GRRLIB_Rectangle(cx - 3, SG_Y - 3, 3, SG_CH + 6,         0x4499FFFF, 1);
                    GRRLIB_Rectangle(cx + visW, SG_Y - 3, 3, SG_CH + 6,      0x4499FFFF, 1);
                }
                // Series name below card
                const JellyfinItem& itm = tvUpcomingItems[i];
                std::string title = filterDejaVu(
                    !itm.seriesName.empty() ? itm.seriesName : itm.name, 20);
                while (!title.empty() && (int)GRRLIB_WidthTTF(font, title.c_str(), 12) > visW) {
                    while (!title.empty() && (title.back() & 0xC0) == 0x80) title.pop_back();
                    if (!title.empty()) title.pop_back();
                }
                GRRLIB_PrintfTTF(cx, SG_Y + SG_CH + 2, font, title.c_str(), 12, 0xDDDDDDFF);
            }
            // Scroll arrows
            {
                int ax0 = SG_X0 - 12;
                int ax1 = SG_X0 + SUGG_VISIBLE * (SG_CW + SG_GAP) - SG_GAP + 2;
                int ay  = SG_Y + SG_CH / 2;
                auto drawArrow = [&](int ax, bool left, bool active) {
                    u32 col = active ? 0xAADDFFFF : 0x2A3A4AFF;
                    if (left) {
                        guVector tri[3] = {{(float)(ax-8),(float)ay,0},{(float)(ax+6),(float)(ay-8),0},{(float)(ax+6),(float)(ay+8),0}};
                        u32 c[3] = {col,col,col}; GRRLIB_NGoneFilled(tri,c,3);
                    } else {
                        guVector tri[3] = {{(float)(ax+8),(float)ay,0},{(float)(ax-6),(float)(ay-8),0},{(float)(ax-6),(float)(ay+8),0}};
                        u32 c[3] = {col,col,col}; GRRLIB_NGoneFilled(tri,c,3);
                    }
                };
                drawArrow(ax0, true,  tvUpcomingOff > 0);
                drawArrow(ax1, false, tvUpcomingOff + SUGG_VISIBLE < nu);
            }
        }

        GRRLIB_Rectangle(0, 453, 640, 1, 0x334466FF, 1);
        GRRLIB_PrintfTTF(20, 458, font, "[A] Detail  [B] Back", 15, 0x889AABFF);
        GRRLIB_PrintfTTF(340, 458, font, "[-/+] Tab", 15, 0x889AABFF);
    }

    // ---- Music Suggestions (recently added albums) ----
    if (state == State::MusicSuggestionsReady) {
        // Tab bar
        const char* mTabNames[3] = { "Albums", "Suggestions", "Playlists" };
        const int TAB_GAP = 20;
        int totalW = 0;
        for (int t = 0; t < 3; t++)
            totalW += (int)GRRLIB_WidthTTF(font, mTabNames[t], 14) + (t < 2 ? TAB_GAP : 0);
        int tabX = (640 - totalW) / 2;
        for (int t = 0; t < 3; t++) {
            int tw = (int)GRRLIB_WidthTTF(font, mTabNames[t], 14);
            u32 col = (t == musicTab) ? 0xFFFFFFFF : 0x5B7A9AFF;
            GRRLIB_PrintfTTF(tabX, 18, font, mTabNames[t], 14, col);
            if (t == musicTab)
                GRRLIB_Rectangle(tabX, 42, tw, 2, 0x4499FFFF, 1);
            tabX += tw + TAB_GAP;
        }
        GRRLIB_Rectangle(0, 46, 640, 1, 0x334466FF, 1);

        // 2-row × 4-col grid of album art cards
        // Available height: 47..452 = 405px. Two rows + titles need ~310px, centered.
        const int COLS    = 4;
        const int AC_CW   = 120;  // square album card
        const int AC_CH   = 120;
        const int AC_GAP  = 18;
        const int AC_LABEL= 16;   // label height below card
        const int AC_RSTRIDE = AC_CH + AC_LABEL + 18; // row stride
        const int GRID_W  = COLS * AC_CW + (COLS - 1) * AC_GAP;
        const int GRID_X  = (640 - GRID_W) / 2;
        const int HDR_Y   = 56;   // section header
        const int ROW0_Y  = 74;   // first row starts after header + gap
        const int ROW1_Y  = ROW0_Y + AC_RSTRIDE;

        float ws = WiiUtils::wsScaleX();
        int nr = (int)musicRecentItems.size();

        GRRLIB_PrintfTTF(GRID_X, HDR_Y, font, "RECENTLY ADDED", 11,
                         nr > 0 ? 0x6688AAFF : 0x445566FF);

        auto drawAlbumCard = [&](int col, int cardY, int i) {
            int cx = GRID_X + col * (AC_CW + AC_GAP);
            int visW = (int)(AC_CW * ws + 0.5f);
            bool sel  = (i == musicSuggestSel);
            bool hov  = ir.valid && ir.x >= cx && ir.x < cx + AC_CW
                                 && ir.y >= cardY && ir.y < cardY + AC_CH;

            GRRLIB_texImg* tex = musicRecentTex[i];
            // Background
            GRRLIB_Rectangle(cx, cardY, visW, AC_CH, 0x1A2535FF, 1);
            if (tex && tex->w > 0) {
                float sx = (float)AC_CW / tex->w;
                float sy = (float)AC_CH / tex->h;
                float s  = sx > sy ? sx : sy;
                int   ox = (int)((visW - tex->w * s * ws) * 0.5f);
                int   oy = (int)((AC_CH - tex->h * s) * 0.5f);
                GRRLIB_ClipDrawing(cx, cardY, (u32)visW, AC_CH);
                GRRLIB_DrawImg(cx + ox, cardY + oy, tex, 0, s * ws, s, 0xFFFFFFFF);
                GRRLIB_ClipReset();
            }
            // Selection border
            if (sel || hov) {
                GRRLIB_Rectangle(cx - 3, cardY - 3, visW + 6, 3,           0x4499FFFF, 1);
                GRRLIB_Rectangle(cx - 3, cardY + AC_CH, visW + 6, 3,       0x4499FFFF, 1);
                GRRLIB_Rectangle(cx - 3, cardY - 3, 3, AC_CH + 6,          0x4499FFFF, 1);
                GRRLIB_Rectangle(cx + visW, cardY - 3, 3, AC_CH + 6,       0x4499FFFF, 1);
            }
            // Title label
            std::string title = filterDejaVu(musicRecentItems[i].name, 18);
            while (!title.empty()
                   && (int)GRRLIB_WidthTTF(font, title.c_str(), 12) > visW) {
                while (!title.empty() && (title.back() & 0xC0) == 0x80) title.pop_back();
                if (!title.empty()) title.pop_back();
            }
            GRRLIB_PrintfTTF(cx, cardY + AC_CH + 3, font, title.c_str(), 12,
                             (sel || hov) ? 0xFFFFFFFF : 0x889AABFF);
        };

        if (nr == 0) {
            int mw = (int)GRRLIB_WidthTTF(font, "No recent albums", 16);
            GRRLIB_PrintfTTF(320 - mw / 2, 200, font, "No recent albums", 16, 0x556677FF);
        } else {
            for (int col = 0; col < COLS; col++) {
                if (col < nr)
                    drawAlbumCard(col, ROW0_Y, col);
                if (col + COLS < nr)
                    drawAlbumCard(col, ROW1_Y, col + COLS);
            }
        }

        GRRLIB_Rectangle(0, 453, 640, 1, 0x334466FF, 1);
        GRRLIB_PrintfTTF(20, 458, font, "[A] Open  [B] Back", 15, 0x889AABFF);
        GRRLIB_PrintfTTF(360, 458, font, "[-/+] Tab", 15, 0x889AABFF);
    }

    // ---- Item detail ----
    if (state == State::DetailReady || state == State::ResumePrompt)
        drawDetailView(ir);

    // ---- Resume prompt overlay ----
    if (state == State::ResumePrompt) {
        // Semi-transparent backdrop
        GRRLIB_Rectangle(0, 0, 640, 480, 0x00000099, 1);

        // Dialog box
        const int DW = 340, DH = 120;
        const int DX = (640 - DW) / 2;
        const int DY = (480 - DH) / 2;
        GRRLIB_Rectangle(DX, DY, DW, DH, 0x1A2535FF, 1);
        GRRLIB_Rectangle(DX,      DY,      DW, 2,  0x4499FFFF, 1);
        GRRLIB_Rectangle(DX,      DY+DH-2, DW, 2,  0x4499FFFF, 1);
        GRRLIB_Rectangle(DX,      DY,      2, DH,  0x4499FFFF, 1);
        GRRLIB_Rectangle(DX+DW-2, DY,      2, DH,  0x4499FFFF, 1);

        // Title
        const char* dlgTitle = "Resume playback?";
        int tw = (int)GRRLIB_WidthTTF(font, dlgTitle, 16);
        GRRLIB_PrintfTTF(DX + (DW - tw) / 2, DY + 14, font, dlgTitle, 16, 0xFFFFFFFF);

        // Resume position hint
        {
            int secs = (int)(detail.playbackPositionTicks / 10000000LL);
            int rh = secs / 3600, rm = (secs % 3600) / 60, rs = secs % 60;
            char hint[48];
            if (rh > 0) snprintf(hint, sizeof(hint), "(at %d:%02d:%02d)", rh, rm, rs);
            else         snprintf(hint, sizeof(hint), "(at %d:%02d)", rm, rs);
            int hw = (int)GRRLIB_WidthTTF(font, hint, 13);
            GRRLIB_PrintfTTF(DX + (DW - hw) / 2, DY + 36, font, hint, 13, 0x55CCFFFF);
        }

        // Two buttons: Continue | From Start
        const char* btnLabels[2] = { "Continue", "From Start" };
        const int BW = 130, BH = 30, BGAP = 16;
        int bTotalW = BW * 2 + BGAP;
        int bStartX = DX + (DW - bTotalW) / 2;
        int bY      = DY + DH - BH - 14;

        for (int i = 0; i < 2; ++i) {
            int bx = bStartX + i * (BW + BGAP);
            bool sel = (resumeSel == i);
            u32 bgCol  = sel ? 0x4499FFFF : 0x2A3B4DFF;
            u32 txtCol = sel ? 0x001133FF : 0xAABBCCFF;
            GRRLIB_Rectangle(bx, bY, BW, BH, bgCol, 1);
            if (sel) {
                GRRLIB_Rectangle(bx,      bY,      BW, 2,  0xFFFFFFCC, 1);
                GRRLIB_Rectangle(bx,      bY+BH-2, BW, 2,  0xFFFFFFCC, 1);
                GRRLIB_Rectangle(bx,      bY,      2, BH,  0xFFFFFFCC, 1);
                GRRLIB_Rectangle(bx+BW-2, bY,      2, BH,  0xFFFFFFCC, 1);
            }
            int lw = (int)GRRLIB_WidthTTF(font, btnLabels[i], 15);
            GRRLIB_PrintfTTF(bx + (BW - lw) / 2, bY + 7, font, btnLabels[i], 15, txtCol);
        }

        // Hint bar
        GRRLIB_PrintfTTF(DX + 8, DY + DH + 6, font,
                         "\xe2\x86\x90\xe2\x86\x92 Select  [A] Confirm  [B] Cancel",
                         12, 0x889AABFF);
    }

    // ---- Search ----
    if (state == State::SearchInput)
        renderSearchInput(ir);
    if (state == State::SearchReady)
        renderSearchResults(ir);

    drawCursor(ir);
}

// ---------------------------------------------------------------
// drawDetailView()
// ---------------------------------------------------------------
void LibraryView::drawDetailView(ir_t& ir) {
    const int POSTER_X  = 20;
    const int POSTER_Y  = 30;
    const int INFO_X    = 240;
    const int INFO_W    = 390;

    // Returns true if s contains any codepoint >= U+3000 (CJK/Japanese range)
    auto hasJapanese = [](const std::string& s) -> bool {
        const unsigned char* p = (const unsigned char*)s.c_str();
        while (*p) {
            uint32_t cp;
            if      (*p < 0x80)  { cp = *p++; }
            else if (*p < 0xE0)  { cp = (*p++ & 0x1F) << 6;  cp |= (*p++ & 0x3F); }
            else if (*p < 0xF0)  { cp = (*p++ & 0x0F) << 12; cp |= (*p++ & 0x3F) << 6; cp |= (*p++ & 0x3F); }
            else                 { cp = (*p++ & 0x07) << 18; cp |= (*p++ & 0x3F) << 12; cp |= (*p++ & 0x3F) << 6; cp |= (*p++ & 0x3F); }
            if (cp >= 0x3000) return true;
        }
        return false;
    };

    // Episode thumbnails are 16:9; movie/show posters are portrait
    const int POSTER_W2 = detailIsEpisode ? 210 : 200;
    const int POSTER_H2 = detailIsEpisode ? 118 : 285;

    float ws   = WiiUtils::wsScaleX();
    int   visW = (int)(POSTER_W2 * ws + 0.5f);

    // ---- Thumbnail / Poster ----
    GRRLIB_Rectangle(POSTER_X, POSTER_Y, visW, POSTER_H2, 0x1E2A3AFF, 1);
    // 2-pixel coloured border to visually distinguish episode vs movie
    u32 borderCol = detailIsEpisode ? 0x3399CCFF : 0x334466FF;
    GRRLIB_Rectangle(POSTER_X - 2,       POSTER_Y - 2, visW + 4,  2,         borderCol, 1);
    GRRLIB_Rectangle(POSTER_X - 2,       POSTER_Y + POSTER_H2, visW + 4, 2,  borderCol, 1);
    GRRLIB_Rectangle(POSTER_X - 2,       POSTER_Y - 2, 2, POSTER_H2 + 4,     borderCol, 1);
    GRRLIB_Rectangle(POSTER_X + visW,    POSTER_Y - 2, 2, POSTER_H2 + 4,     borderCol, 1);
    if (detailTex && detailTex->w > 0) {
        float sx = (float)POSTER_W2 / detailTex->w;
        float sy = (float)POSTER_H2 / detailTex->h;
        float s  = sx > sy ? sx : sy; // FILL: crop edges rather than leave gray bars
        float dw = detailTex->w * s * ws, dh = detailTex->h * s;
        int ox = (int)((visW - dw) * 0.5f);
        int oy = (int)((POSTER_H2 - dh) * 0.5f);
        GRRLIB_ClipDrawing(POSTER_X, POSTER_Y, (u32)visW, POSTER_H2);
        GRRLIB_DrawImg(POSTER_X + ox, POSTER_Y + oy, detailTex, 0, s * ws, s, 0xFFFFFFFF);
        GRRLIB_ClipReset();
    }

    // ---- Play button overlay (shown when cursor hovers the poster) ----
    bool posterHover = ir.valid
        && ir.x >= POSTER_X && ir.x < POSTER_X + visW
        && ir.y >= POSTER_Y && ir.y < POSTER_Y + POSTER_H2;
    if (posterHover) {
        GRRLIB_Rectangle(POSTER_X, POSTER_Y, visW, POSTER_H2, 0x00000099, 1);
        int cx = POSTER_X + visW / 2;
        int cy = POSTER_Y + POSTER_H2 / 2;
        int bw = 64, bh = 34;
        GRRLIB_Rectangle(cx - bw/2,     cy - bh/2,     bw,     bh,     0x000000AA, 1);
        GRRLIB_Rectangle(cx - bw/2 - 1, cy - bh/2 - 1, bw + 2, 2,     0xFFFFFFCC, 1);
        GRRLIB_Rectangle(cx - bw/2 - 1, cy + bh/2 - 1, bw + 2, 2,     0xFFFFFFCC, 1);
        GRRLIB_Rectangle(cx - bw/2 - 1, cy - bh/2 - 1, 2,     bh + 2, 0xFFFFFFCC, 1);
        GRRLIB_Rectangle(cx + bw/2 - 1, cy - bh/2 - 1, 2,     bh + 2, 0xFFFFFFCC, 1);
        int tw = GRRLIB_WidthTTF(font, "\xe2\x96\xb6 Play", 16);
        GRRLIB_PrintfTTF(cx - tw/2, cy - 10, font, "\xe2\x96\xb6 Play", 16, 0xFFFFFFFF);
    }

    // ---- Title ----
    int y = POSTER_Y;
    {
        bool jp = hasJapanese(detail.name);
        std::string title = jp ? detail.name : filterDejaVu(detail.name, 40);
        GRRLIB_PrintfTTF(INFO_X, y, jp ? jpFont : font, title.c_str(), 22, 0xFFFFFFFF);
    }
    y += 28;

    // ---- Year  Runtime  Rating ----
    {
        char meta[64] = "";
        if (detail.year)
            snprintf(meta, sizeof(meta), "%d", detail.year);
        if (detail.runtimeTicks > 0) {
            int secs = (int)(detail.runtimeTicks / 10000000LL);
            int h = secs / 3600, m = (secs % 3600) / 60;
            char rt[20];
            if (h > 0) snprintf(rt, sizeof(rt), "  %dh %02dmin", h, m);
            else       snprintf(rt, sizeof(rt), "  %dmin", m);
            strncat(meta, rt, sizeof(meta) - strlen(meta) - 1);
        }
        if (!detail.officialRating.empty()) {
            strncat(meta, "  ", sizeof(meta) - strlen(meta) - 1);
            strncat(meta, detail.officialRating.c_str(), sizeof(meta) - strlen(meta) - 1);
        }
        if (meta[0]) { GRRLIB_PrintfTTF(INFO_X, y, font, meta, 15, 0xAABBCCFF); y += 20; }
    }

    // ---- Resume hint (shown when playback position is saved) ----
    if (detail.playbackPositionTicks > 0 && detail.runtimeTicks > 0) {
        int secs = (int)(detail.playbackPositionTicks / 10000000LL);
        int h    = secs / 3600, m = (secs % 3600) / 60;
        int pct  = (int)(detail.playbackPositionTicks * 100LL / detail.runtimeTicks);
        char buf[48];
        if (h > 0) snprintf(buf, sizeof(buf), "> Resume at %dh%02d (%d%%)", h, m, pct);
        else        snprintf(buf, sizeof(buf), "> Resume at %dmin (%d%%)", m, pct);
        GRRLIB_PrintfTTF(INFO_X, y, font, buf, 14, 0x55CCFFFF);
        y += 20;
    }

    // ---- Genres ----
    if (!detail.genres.empty()) {
        std::string g;
        for (size_t i = 0; i < detail.genres.size(); i++) {
            if (i) g += ", ";
            g += detail.genres[i];
        }
        GRRLIB_PrintfTTF(INFO_X, y, font, g.c_str(), 14, 0x889AABFF);
        y += 18;
    }
    y += 6;

    // ---- Overview (pre-computed lines, no per-frame WidthTTF) ----
    if (!detailLines.empty()) {
        for (const auto& line : detailLines) {
            GRRLIB_PrintfTTF(INFO_X, y, font, line.c_str(), 13, 0xCCCCCCFF);
            y += 17;
        }
        y += 6;
    }

    // ---- Cast & crew (max 6) ----
    if (!detail.people.empty()) {
        GRRLIB_PrintfTTF(INFO_X, y, font, "Cast & crew", 14, 0x6688AAFF);
        y += 18;
        int shown = 0;
        for (const auto& p : detail.people) {
            if (shown >= 6) break;
            if (p.name.empty() && p.character.empty()) continue;
            int rx = INFO_X + 8;
            if (!p.name.empty() && hasJapanese(p.name)) {
                // Japanese VA name: render with jpFont, then Latin suffix with font
                GRRLIB_PrintfTTF(rx, y, jpFont, p.name.c_str(), 13, 0xBBCCDDFF);
                rx += GRRLIB_WidthTTF(jpFont, p.name.c_str(), 13);
                std::string suffix;
                if (p.role == "Director")          suffix = " (director)";
                else if (!p.character.empty())     suffix = " - " + p.character;
                if (!suffix.empty())
                    GRRLIB_PrintfTTF(rx, y, font, suffix.c_str(), 13, 0xBBCCDDFF);
            } else {
                // All-Latin line
                std::string line = !p.name.empty() ? p.name : p.character;
                if (!p.name.empty()) {
                    if (p.role == "Director")          line += " (director)";
                    else if (!p.character.empty())     line += " - " + p.character;
                }
                GRRLIB_PrintfTTF(rx, y, font, line.c_str(), 13, 0xBBCCDDFF);
            }
            y += 16;
            shown++;
        }
        y += 4;
    }

    // ---- Audio / Subtitle stream selectors ----
    // Drawn at a fixed bottom-anchor position so they're always visible.
    const int STREAM_Y0 = 390;
    const int STREAM_ROW_H = 24;
    const bool hasAudio = !detail.audioStreams.empty();
    const bool hasSub   = !detail.subtitleStreams.empty();
    if (hasAudio || hasSub) {
        // Separator line
        GRRLIB_Rectangle(INFO_X, STREAM_Y0 - 8, INFO_W, 1, 0x334466FF, 1);

        // Validate UTF-8 and truncate at a safe codepoint boundary so GRRLIB
        // never receives a broken multi-byte sequence.
        auto safeTitle = [](const char* s, int maxCodepoints) -> std::string {
            if (!s) return "-";
            std::string out;
            const unsigned char* p = (const unsigned char*)s;
            int count = 0;
            while (*p && count < maxCodepoints) {
                int seqLen;
                if      (*p < 0x80) seqLen = 1;
                else if (*p < 0xE0) seqLen = 2;
                else if (*p < 0xF0) seqLen = 3;
                else                seqLen = 4;
                bool valid = true;
                for (int i = 1; i < seqLen; i++) {
                    if ((p[i] & 0xC0) != 0x80) { valid = false; break; }
                }
                if (!valid) { p++; continue; }
                for (int i = 0; i < seqLen; i++) out += (char)p[i];
                p += seqLen;
                ++count;
            }
            if (*p) out += "...";
            if (out.empty()) out = "?";
            return out;
        };

        for (int row = 0; row < 2; row++) {
            if (row == 0 && !hasAudio) continue;
            if (row == 1 && !hasSub)   continue;

            int ry = STREAM_Y0 + row * STREAM_ROW_H;
            bool focused = (detailFocusRow == row);

            // Highlight rect for focused row
            if (focused)
                GRRLIB_Rectangle(INFO_X - 2, ry - 2, INFO_W, STREAM_ROW_H - 2, 0x1E3A5AFF, 1);

            u32 labelCol = focused ? 0xFFFFFFFF : 0x889AABFF;
            u32 valueCol = focused ? 0xFFEE88FF : 0xCCCCCCFF;

            if (row == 0) {
                // Audio
                GRRLIB_PrintfTTF(INFO_X + 2, ry, font, "Audio", 13, labelCol);
                const char* rawTitle = detailAudioSel < (int)detail.audioStreams.size()
                    ? detail.audioStreams[detailAudioSel].displayTitle.c_str() : "-";
                std::string t = safeTitle(rawTitle, 36);
                char buf[64];
                snprintf(buf, sizeof(buf), "< %s >", t.c_str());
                GRRLIB_ttfFont* tf = hasJapanese(t) ? jpFont : font;
                GRRLIB_PrintfTTF(INFO_X + 60, ry, tf, buf, 13, valueCol);
            } else {
                // Subtitle
                GRRLIB_PrintfTTF(INFO_X + 2, ry, font, "Subtitles", 13, labelCol);
                const char* rawTitle = (detailSubSel == -1)
                    ? "Off"
                    : (detailSubSel < (int)detail.subtitleStreams.size()
                        ? detail.subtitleStreams[detailSubSel].displayTitle.c_str() : "-");
                std::string t = safeTitle(rawTitle, 36);
                char buf[64];
                snprintf(buf, sizeof(buf), "< %s >", t.c_str());
                GRRLIB_ttfFont* tf = hasJapanese(t) ? jpFont : font;
                GRRLIB_PrintfTTF(INFO_X + 60, ry, tf, buf, 13, valueCol);
            }
        }
    }

    // ---- Footer ----
    GRRLIB_Rectangle(0, 453, 640, 1, 0x334466FF, 1);
    std::string footerStr = "[A] Play   [B] Back";
    if (hasAudio || hasSub)
        footerStr += "   [Up/Down] Focus   [</>/] Change";
    GRRLIB_PrintfTTF(20, 458, font, footerStr.c_str(), 13, 0x889AABFF);
}

// ---------------------------------------------------------------
// Search: VKB layout (same as ConnectView)
// ---------------------------------------------------------------
static const char* s_srchKbRows[7] = {
    // Page 0: letters
    "1234567890-.",
    "qwertyuiop:/",
    "asdfghjkl@_ ",
    "\x01zxcvbnm.,\x02\x7f",   // \x01=Shift  \x02=SYM  \x7f=Backspace
    // Page 1: symbols
    "!?@#$%^&*()-",
    "_+=|\\[]{};:\"",
    "'<>./`~\x02\x7f",         // \x02=ABC
};
static const int SRCH_KB_COLS_MAX = 12;
static const int SRCH_KB_X        = 80;
static const int SRCH_KB_Y        = 140;
static const int SRCH_KB_CELLW    = 38;
static const int SRCH_KB_CELLH    = 36;

// ---------------------------------------------------------------
void LibraryView::clampSearchScroll() {
    int n = (int)searchResults.size();
    if (searchSel < 0) searchSel = 0;
    if (n > 0 && searchSel >= n) searchSel = n - 1;
    if (searchSel < searchTop) searchTop = searchSel;
    if (searchSel >= searchTop + SEARCH_VISIBLE) searchTop = searchSel - SEARCH_VISIBLE + 1;
    if (searchTop < 0) searchTop = 0;
}

// ---------------------------------------------------------------
void LibraryView::performSearch() {
    searchResults.clear();
    bool ok = false; std::string err;
    runWithLoading([&]() {
        ok = client.searchItems(serverUrl, auth, searchQuery, 50, searchResults);
        if (!ok) err = client.lastError();
    });
    if (!ok) { errMsg = err; state = State::Error; return; }
    searchSel = 0;
    searchTop = 0;
    state = State::SearchReady;
}

// ---------------------------------------------------------------
void LibraryView::handleSearchVKB(ir_t& ir) {
    int pageStart = srchKbPage * 4;
    int pageRows  = (srchKbPage == 0) ? 4 : 3;
    int rowLen    = strlen(s_srchKbRows[pageStart + srchKbRow]);

    if (Input::isUpPressed()) {
        srchKbRow = (srchKbRow - 1 + pageRows) % pageRows;
        int nl = strlen(s_srchKbRows[pageStart + srchKbRow]);
        if (srchKbCol >= nl) srchKbCol = nl - 1;
    }
    if (Input::isDownPressed()) {
        srchKbRow = (srchKbRow + 1) % pageRows;
        int nl = strlen(s_srchKbRows[pageStart + srchKbRow]);
        if (srchKbCol >= nl) srchKbCol = nl - 1;
    }
    if (Input::isLeftPressed())  srchKbCol = (srchKbCol - 1 + rowLen) % rowLen;
    if (Input::isRightPressed()) srchKbCol = (srchKbCol + 1) % rowLen;

    if (Input::isLPressed() && srchKbPage == 0) srchKbShift = !srchKbShift;

    if (Input::isAJustPressed()) {
        char key = 0;
        if (ir.valid) {
            for (int r = 0; r < pageRows && !key; r++) {
                int len = strlen(s_srchKbRows[pageStart + r]);
                for (int c = 0; c < len && !key; c++) {
                    int cx = SRCH_KB_X + c * SRCH_KB_CELLW;
                    int cy = SRCH_KB_Y + r * SRCH_KB_CELLH;
                    if (ir.x >= cx && ir.x <= cx + SRCH_KB_CELLW - 2 &&
                        ir.y >= cy && ir.y <= cy + SRCH_KB_CELLH - 2) {
                        key = s_srchKbRows[pageStart + r][c];
                        srchKbRow = r; srchKbCol = c;
                    }
                }
            }
        }
        if (!key) key = s_srchKbRows[pageStart + srchKbRow][srchKbCol];

        if (key == '\x01') {
            srchKbShift = !srchKbShift;
            SoundFX::play(SoundFX::FX::PressKey);
        } else if (key == '\x02') {
            srchKbPage = 1 - srchKbPage;
            srchKbRow = 0; srchKbCol = 0; srchKbShift = false;
            SoundFX::play(SoundFX::FX::PressKey);
        } else if (key == '\x7f') {
            if (!searchQuery.empty()) searchQuery.pop_back();
            SoundFX::play(SoundFX::FX::Backspace);
        } else if (key == ' ') {
            searchQuery += ' ';
            SoundFX::play(SoundFX::FX::PressKey);
        } else {
            if (srchKbShift && key >= 'a' && key <= 'z')
                searchQuery += (char)(key - 32);
            else
                searchQuery += key;
            srchKbShift = false;
            SoundFX::play(SoundFX::FX::PressKey);
        }
    }
}

// ---------------------------------------------------------------
void LibraryView::renderSearchInput(ir_t& ir) {
    int pageStart = srchKbPage * 4;
    int pageRows  = (srchKbPage == 0) ? 4 : 3;

    // Header
    GRRLIB_Rectangle(0, 0, 640, 52, 0x0E1826FF, 1);
    drawCenteredText(0, 16, 640, "Search", 20, 0xFFFFFFFF);
    GRRLIB_Rectangle(0, 51, 640, 1, 0x1C2D3CFF, 1);

    // Search field
    const int FX = 80, FY = 75, FW = 480;
    GRRLIB_Rectangle(FX - 4, FY - 4, FW + 8, 36, 0x1E3A5FFF, 1);
    GRRLIB_Rectangle(FX - 4, FY - 4, FW + 8, 36, 0x4499FFFF, 0); // outline
    {
        std::string display = searchQuery + "_";
        if (GRRLIB_WidthTTF(font, display.c_str(), 18) > (u32)FW) {
            // Trim from the front so the cursor is always visible
            while (display.size() > 1 &&
                   GRRLIB_WidthTTF(font, display.c_str(), 18) > (u32)FW) {
                display.erase(display.begin());
            }
        }
        GRRLIB_PrintfTTF(FX, FY, font, display.c_str(), 18, 0xFFFFFFFF);
    }

    // Keyboard background
    GRRLIB_Rectangle(SRCH_KB_X - 8, SRCH_KB_Y - 8,
                     SRCH_KB_COLS_MAX * SRCH_KB_CELLW + 16,
                     pageRows * SRCH_KB_CELLH + 16, 0x00000099, 1);

    for (int r = 0; r < pageRows; r++) {
        int ri  = pageStart + r;
        int len = strlen(s_srchKbRows[ri]);
        for (int c = 0; c < len; c++) {
            int cx = SRCH_KB_X + c * SRCH_KB_CELLW;
            int cy = SRCH_KB_Y + r * SRCH_KB_CELLH;
            bool sel     = (r == srchKbRow && c == srchKbCol);
            bool irHover = ir.valid &&
                           ir.x >= cx && ir.x <= cx + SRCH_KB_CELLW - 2 &&
                           ir.y >= cy && ir.y <= cy + SRCH_KB_CELLH - 2;

            char k = s_srchKbRows[ri][c];
            char label[5] = {0};
            if      (k == '\x7f') { label[0]='<'; label[1]='-'; }
            else if (k == ' ')    { label[0]='_'; }
            else if (k == '\x01') { label[0]='^'; label[1]='S'; label[2]='H'; }
            else if (k == '\x02') {
                if (srchKbPage == 0) { label[0]='S'; label[1]='Y'; label[2]='M'; }
                else                 { label[0]='A'; label[1]='B'; label[2]='C'; }
            }
            else if (srchKbShift && k >= 'a' && k <= 'z') { label[0] = (char)(k - 32); }
            else { label[0] = k; }

            u32 bgCol = (k == '\x01' && srchKbShift) ? 0xDD8800CC
                      : (k == '\x02')                ? 0x226633CC
                      : sel                          ? 0x4499FFDD
                      : irHover                      ? 0x6699BBDD
                      :                                0x1E2D44CC;
            GRRLIB_Rectangle(cx + 1, cy + 1, SRCH_KB_CELLW - 2, SRCH_KB_CELLH - 2, bgCol, 1);

            u32 tc = (k == '\x01' && srchKbShift) ? 0xFFDD44FF
                   : (k == '\x02')                ? 0x88FFAAFF
                   : sel                          ? 0xFFFFFFFF
                   :                                0xBBCCDDFF;
            int tw = (int)(strlen(label) * 10);
            GRRLIB_PrintfTTF(cx + (SRCH_KB_CELLW - tw) / 2, cy + 10,
                             font, label, 14, tc);
        }
    }

    // Hints at bottom
    int hintY = SRCH_KB_Y + pageRows * SRCH_KB_CELLH + 10;
    if (srchKbPage == 0 && srchKbShift) {
        GRRLIB_PrintfTTF(SRCH_KB_X, hintY, font,
            "[CAPS]  -: toggle  |  [+] Search  |  B: back", 13, 0xFFDD44FF);
    } else {
        GRRLIB_PrintfTTF(SRCH_KB_X, hintY, font,
            "-: CAPS  |  [+] Search  |  B: back", 13, 0x778899FF);
    }
}

// ---------------------------------------------------------------
void LibraryView::renderSearchResults(ir_t& ir) {
    int n = (int)searchResults.size();

    // Header
    GRRLIB_Rectangle(0, 0, 640, 52, 0x0E1826FF, 1);
    char hdr[128];
    snprintf(hdr, sizeof(hdr), "Results: %s", searchQuery.c_str());
    GRRLIB_PrintfTTF(20, 16, font, hdr, 18, 0xFFFFFFFF);
    GRRLIB_Rectangle(0, 51, 640, 1, 0x1C2D3CFF, 1);

    if (n == 0) {
        drawCenteredText(0, 200, 640, "No results.", 20, 0x889AABFF);
    } else {
        for (int i = 0; i < SEARCH_VISIBLE; i++) {
            int idx = searchTop + i;
            if (idx >= n) break;
            int   ry  = LIST_Y + i * ROW_H;
            bool  sel = (idx == searchSel);
            bool  hov = ir.valid &&
                        ir.x >= LIST_X && ir.x <= LIST_X + LIST_W &&
                        ir.y >= ry && ir.y < ry + ROW_H;

            if (sel || hov)
                GRRLIB_Rectangle(LIST_X - 4, ry, LIST_W + 8, ROW_H - 2, 0x1E3A5FFF, 1);

            // Type badge
            const std::string& type = searchResults[idx].type;
            const char* badge = "?";
            u32 bCol = 0x446688FF;
            if      (type == "Movie")       { badge = "MOVIE";   bCol = 0x2266CCFF; }
            else if (type == "Series")      { badge = "SERIES";  bCol = 0xCC4433FF; }
            else if (type == "Episode")     { badge = "EP";     bCol = 0xAA3322FF; }
            else if (type == "MusicAlbum")  { badge = "ALBUM";  bCol = 0x33AA55FF; }
            else if (type == "Audio")       { badge = "TRACK";  bCol = 0x33AA55FF; }
            else if (type == "MusicArtist") { badge = "ARTIST"; bCol = 0x228844FF; }
            else if (type == "BoxSet")      { badge = "COLLEC"; bCol = 0x8844CCFF; }
            else if (type == "Playlist")    { badge = "LIST";   bCol = 0x228899FF; }
            GRRLIB_Rectangle(LIST_X - 4, ry, 48, ROW_H - 4, bCol, 1);
            int bw = (int)GRRLIB_WidthTTF(font, badge, 11);
            GRRLIB_PrintfTTF(LIST_X - 4 + (48 - bw) / 2, ry + 7, font, badge, 11, 0xFFFFFFFF);

            // Title
            u32 textCol = (sel || hov) ? 0xFFFFFFFF : 0xCCDDEEFF;
            std::string label = filterDejaVu(searchResults[idx].name, 38);
            if (type == "Episode" && !searchResults[idx].seriesName.empty()) {
                // Show "Series S01E02 - Episode title"
                char ep[64];
                snprintf(ep, sizeof(ep), "S%02dE%02d - ",
                         searchResults[idx].seasonNumber,
                         searchResults[idx].episodeNumber);
                label = filterDejaVu(searchResults[idx].seriesName, 20) + " " + ep + filterDejaVu(searchResults[idx].name, 14);
            } else if (searchResults[idx].year > 0) {
                char yb[16];
                snprintf(yb, sizeof(yb), " (%d)", searchResults[idx].year);
                label += yb;
            }
            GRRLIB_PrintfTTF(LIST_X + 50, ry + 8, font, label.c_str(), 16, textCol);
        }

        // Scroll indicator
        if (n > SEARCH_VISIBLE) {
            char sc[16];
            snprintf(sc, sizeof(sc), "%d / %d", searchSel + 1, n);
            int sw = (int)GRRLIB_WidthTTF(font, sc, 14);
            GRRLIB_PrintfTTF(620 - sw, 20, font, sc, 14, 0x556677FF);
        }
    }

    GRRLIB_Rectangle(0, 453, 640, 1, 0x334466FF, 1);
    GRRLIB_PrintfTTF(20, 458, font, "[A] Open", 15, 0x889AABFF);
    {
        const char* nh = "[1] Nouvelle recherche";
        int nw = (int)GRRLIB_WidthTTF(font, nh, 15);
        GRRLIB_PrintfTTF(320 - nw / 2, 458, font, nh, 15, 0x889AABFF);
    }
    GRRLIB_PrintfTTF(490, 458, font, "[B] Back", 15, 0x889AABFF);
}
