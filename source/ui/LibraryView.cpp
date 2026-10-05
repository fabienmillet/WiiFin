#include "LibraryView.h"
#include "../core/ExitZone.h"
#include "../core/Text.h"
#include "Ui.h"
#include "../input/Input.h"

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

GRRLIB_texImg* loadJPEGTexture(const u8* data, u32 size) {
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
    // Speed over exactness: the images are small and already resized by the
    // server, so the integer DCT and plain chroma upsampling are not visible.
    cinfo.dct_method          = JDCT_IFAST;
    cinfo.do_fancy_upsampling = FALSE;
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
    : font(f), jpFont(jf), cursorTex(cursor), ringTex(ring), client(c), auth(a), serverUrl(url),
      browse(c, url, a), catalog(c, url, a, BrowseHome::Mode::Catalog), feed(c, url, a) {
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
static u8               s_fetchStack[64 * 1024] DEAD_AT_EXIT;
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
    Ui::background(false);
    Ui::spinner(ringTex, 320, 240);
    SoundFX::tickLoading();
    // Flush to screen and block on vsync. This serves two purposes:
    // 1. The spinner actually appears and animates on screen.
    // 2. The ~16 ms vsync wait yields the CPU to the worker thread
    //    (which runs at lower scheduling priority than the main thread).
    GRRLIB_Render();
}

void LibraryView::clampScroll() {           // text list (ItemsReady)
    int n = feed.total();
    if (itemSel < 0) itemSel = 0;
    if (n > 0 && itemSel >= n) itemSel = n - 1;
    if (itemSel < viewTop) viewTop = itemSel;
    if (itemSel >= viewTop + LIST_ROWS) viewTop = itemSel - LIST_ROWS + 1;
    if (viewTop > n - LIST_ROWS) viewTop = n - LIST_ROWS;
    if (viewTop < 0) viewTop = 0;
}

int LibraryView::listWidth() const {
    return Ui::libraryStyle() == Ui::LibraryStyle::ListCover ? 384 : LIST_W;
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
            if (Text::width(font, name.c_str(), 12) > (u32)visW) {
                auto popCodePoint = [](std::string &s) {
                    while (!s.empty() && (s.back() & 0xC0) == 0x80) s.pop_back();
                    if (!s.empty()) s.pop_back();
                };
                while (!name.empty() &&
                       Text::width(font, (name + "...").c_str(), 12) > (u32)visW)
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

void LibraryView::clearPendingPlay() {
    pendingPlayUrl.clear();
    pendingPlayTitle.clear();
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
}

void LibraryView::onPlaybackFinished(const std::string& lastItemId) {
    clearPendingPlay();
    browse.invalidate();   // resume positions / next up changed
    catalog.invalidate();
    if (state == State::DetailReady || state == State::ResumePrompt) {
        if (!lastItemId.empty()) detailItemId = lastItemId;
        state = State::DetailLoad;
    }
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
            while (!mt.empty() && (int)Text::width(font, mt.c_str(), 13) > visW) {
                while (!mt.empty() && (mt.back() & 0xC0) == 0x80) mt.pop_back();
                if (!mt.empty()) mt.pop_back();
            }
            // Trim subTitle to fit cell width
            std::string& st = subs[i];
            while (!st.empty() && (int)Text::width(font, st.c_str(), 11) > visW) {
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
    if (!posterMode) {
        // Text list: the whole library, fetched as it scrolls (ListFeed)
        std::string filter = (currentLibType == "music" && inItemsDrilldown)
            ? "AlbumArtistIds=" + currentLibId + "&IncludeItemTypes=MusicAlbum&Recursive=true"
            : "ParentId=" + currentLibId;
        bool ok = false; std::string err;
        runWithLoading([&]() { ok = feed.open(filter, err); });
        if (!ok) { errMsg = err; state = State::Error; return; }
        itemTotal = feed.total();
        globFavMode = false;
        itemSel = 0; viewTop = 0;
        if (listRestoreSel >= 0) { itemSel = listRestoreSel; listRestoreSel = -1; clampScroll(); }
        state = State::ItemsReady;
        return;
    }
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
bool LibraryView::flixHome() const {
    return Ui::homeLayout() == Ui::HomeLayout::Rows;
}

bool LibraryView::update(ir_t& ir) {
    bool done = updateState(ir);
    if (state == State::Error && errMsg != loggedErr) {
        SYS_Report("[Library] error screen: %s\n", errMsg.c_str());
        loggedErr = errMsg;
    }
    // The carousel's poster loader shares the HTTP connection: stop it as
    // soon as anything else may need the network.
    bool rowsShown = !done && state == State::LibsReady && flixHome();
    if (!rowsShown || browsePage)  browse.stopLoader();
    if (!rowsShown || !browsePage) catalog.stopLoader();
    if (done || state != State::ItemsReady) feed.stopWorker();   // same connection
    return done;
}

// Open an item picked on a mixed list (carousel home): series go to their
// seasons, albums and playlists to their tracks, the rest to the detail page.
void LibraryView::openItem(const JellyfinItem& it, State returnState) {
    if (it.type == "Series") {
        currentSeriesId    = it.id;
        currentSeriesName  = it.name;
        seasonsCallerState = returnState;
        seasons.clear(); seasonSel = 0; seasonTop = 0;
        state = State::SeasonsLoad;
    } else if (it.type == "Season" && !it.seriesId.empty()) {
        // a new season shows up as its own item: go straight to its episodes
        currentSeriesId   = it.seriesId;
        currentSeriesName = it.seriesName;
        currentSeasonId   = it.id;
        currentSeasonName = it.name;
        episodesFromHome  = (returnState == State::LibsReady);
        state = State::EpisodesLoad;
    } else if (it.type == "MusicAlbum" || it.type == "Playlist") {
        musicAlbumId     = it.id;
        musicAlbumName   = it.name;
        musicAlbumArtist.clear();
        musicTracks.clear();
        musicTrackSel   = 0;
        musicTrackTop   = 0;
        musicIsPlaylist = (it.type == "Playlist");
        tracksFromHome  = (returnState == State::LibsReady);
        state = State::MusicTracksLoad;
    } else {
        detailItemId        = it.id;
        detailReturnState   = returnState;
        detailIsEpisodeHint = (it.type == "Episode");
        state = State::DetailLoad;
    }
}

// Open library `index` (tile grid or the "Libraries" row of the browse page).
void LibraryView::openLibrary(int index) {
    currentLibId     = libraries[index].id;
    currentLibName   = libraries[index].name;
    currentLibType   = libraries[index].collectionType;
    posterMode       = !Ui::listMode() &&
                       (currentLibType == "movies" || currentLibType == "tvshows" || currentLibType == "boxsets");
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

bool LibraryView::updateState(ir_t& ir) {
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

            if (flixHome()) {
                BrowseHome& page = browsePage ? catalog : browse;
                if (!page.built())
                    runWithLoading([&]() { page.build(libraries); });
                page.startLoader();
                switch (page.update(ir, irMode)) {
                case BrowseHome::Action::Open:
                    if (const JellyfinItem* it = page.selectedItem()) {
                        JellyfinItem copy = *it;
                        if (copy.type == "CollectionFolder") {
                            for (int i = 0; i < n; i++)
                                if (libraries[i].id == copy.id) { libSel = i; openLibrary(i); break; }
                        } else {
                            openItem(copy, State::LibsReady);
                        }
                    }
                    break;
                case BrowseHome::Action::Search:
                    searchQuery.clear();
                    searchResults.clear();
                    searchSel = 0; searchTop = 0;
                    searchKb.reset();
                    searchReturnState = State::LibsReady;
                    state = State::SearchInput;
                    irMode = false;
                    break;
                case BrowseHome::Action::Browse:      // "browse" <-> "home"
                    browsePage = !browsePage;
                    irMode = false;
                    break;
                case BrowseHome::Action::Back:        // B on the browse page
                    browsePage = false;
                    irMode = false;
                    break;
                case BrowseHome::Action::Profiles:
                    SoundFX::play(SoundFX::FX::Back);
                    return true;
                default: break;
                }
                return false;
            }

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
                    searchKb.reset();
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

                if (aPressed) openLibrary(libSel);
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
            // Text list of a whole library (ListFeed): no pages, chunks are
            // fetched in the background as it scrolls.
            int n = feed.total();
            feed.startWorker();
            const int LW = listWidth();

            if (Input::isBackPressed()) {
                if (inItemsDrilldown) {
                    inItemsDrilldown = false;
                    if (drilldownFromSearch) {
                        drilldownFromSearch = false;
                        state = State::SearchReady;
                    } else {
                        currentLibId   = parentLibId;
                        currentLibName = parentLibName;
                        listRestoreSel = parentItemPage;   // selection in the parent list
                        state          = State::ItemsInit;
                    }
                } else {
                    state = State::LibsReady;
                }
                return false;
            }

            if (Input::isUpPressed())   { itemSel--; irMode = false; clampScroll(); }
            if (Input::isDownPressed()) { itemSel++; irMode = false; clampScroll(); }
            // Left/Right: previous/next letter
            if (Input::isLeftPressed())  { feed.requestJump(-1, itemSel); irMode = false; }
            if (Input::isRightPressed()) { feed.requestJump(+1, itemSel); irMode = false; }
            {
                int idx;
                if (feed.takeJump(idx) && idx >= 0) {
                    itemSel = idx;
                    viewTop = idx;            /* the letter starts at the top */
                    clampScroll();
                    JellyfinItem it;
                    letterFlash   = feed.get(idx, it) ? ListFeed::letterOf(it) : 0;
                    letterFlashMs = ticks_to_millisecs(gettime());
                }
            }

            // Music library: -/+ switches tabs; elsewhere -/+ scroll a screen
            if (currentLibType == "music" && !inItemsDrilldown) {
                if (Input::isLPressed() || Input::isRPressed()) {
                    musicTab = Input::isLPressed() ? (musicTab + 2) % 3 : (musicTab + 1) % 3;
                    itemPage = 0;
                    if      (musicTab == 0) state = State::ItemsInit;
                    else if (musicTab == 1) state = State::MusicSuggestionsLoad;
                    else                   state = State::PlaylistsLoad;
                    return false;
                }
            } else {
                if (Input::isLPressed()) { itemSel -= LIST_ROWS; viewTop -= LIST_ROWS; irMode = false; clampScroll(); }
                if (Input::isRPressed()) { itemSel += LIST_ROWS; viewTop += LIST_ROWS; irMode = false; clampScroll(); }
            }

            // IR hover (skip when d-pad was used this frame)
            if (irMode && ir.valid && !Input::isUpPressed() && !Input::isDownPressed()
                         && !Input::isLeftPressed() && !Input::isRightPressed()) {
                for (int i = 0; i < LIST_ROWS; i++) {
                    int idx = viewTop + i;
                    if (idx >= n) break;
                    int ry = LIST_Y + i * LIST_ROW_H;
                    if (ir.x >= LIST_X && ir.x <= LIST_X + LW &&
                        ir.y >= ry && ir.y < ry + LIST_ROW_H) {
                        itemSel = idx;
                        irMode = true;
                    }
                }
            }

            // what to fetch: the rows on screen, a little ahead
            feed.want(viewTop, viewTop + LIST_ROWS + 10);
            JellyfinItem sel;
            bool haveSel = n > 0 && feed.get(itemSel, sel);
            if (haveSel && Ui::libraryStyle() == Ui::LibraryStyle::ListCover)
                feed.requestCover(sel.id);

            // A: select item (rows still loading are ignored)
            if (aPressed && haveSel) {
                if (ir.valid) {   // with the pointer, only a row under it counts
                    int row = (int)((ir.y - LIST_Y) / LIST_ROW_H);
                    if (ir.x < LIST_X || ir.x > LIST_X + LW || row < 0 || row >= LIST_ROWS ||
                        viewTop + row != itemSel)
                        return false;
                }
                if (currentLibType == "music") {
                    if (sel.type == "MusicArtist") {
                        // Drill into artist's albums — reuse item list with artist as parent
                        parentLibId      = currentLibId;
                        parentLibName    = currentLibName;
                        parentItemPage   = itemSel;
                        inItemsDrilldown = true;
                        currentLibId     = sel.id;
                        currentLibName   = sel.name;
                        itemPage         = 0;
                        state            = State::ItemsInit;
                    } else if (sel.type == "MusicAlbum" || sel.type == "Playlist") {
                        musicAlbumId     = sel.id;
                        musicAlbumName   = sel.name;
                        musicAlbumArtist.clear();
                        musicTracks.clear();
                        musicTrackSel    = 0;
                        musicTrackTop    = 0;
                        musicIsPlaylist  = (sel.type == "Playlist");
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
                        parentLibId      = currentLibId;
                        parentLibName    = currentLibName;
                        parentItemPage   = itemSel;
                        inItemsDrilldown = true;
                        currentLibId     = sel.id;
                        currentLibName   = sel.name;
                        itemPage         = 0;
                        state            = State::ItemsInit;
                    }
                } else if (sel.type == "Playlist") {
                    musicAlbumId     = sel.id;
                    musicAlbumName   = sel.name;
                    musicAlbumArtist.clear();
                    musicTracks.clear();
                    musicTrackSel    = 0;
                    musicTrackTop    = 0;
                    musicIsPlaylist  = true;
                    state = State::MusicTracksLoad;
                } else if (sel.type == "Series") {
                    currentSeriesId    = sel.id;
                    currentSeriesName  = sel.name;
                    seasonsCallerState = State::ItemsReady;
                    seasons.clear(); seasonSel = 0; seasonTop = 0;
                    state = State::SeasonsLoad;
                } else if (sel.type == "BoxSet" || sel.type == "Folder" || sel.type == "CollectionFolder") {
                    // a collection or folder: its own list, B comes back here
                    parentLibId      = currentLibId;
                    parentLibName    = currentLibName;
                    parentItemPage   = itemSel;
                    inItemsDrilldown = true;
                    currentLibId     = sel.id;
                    currentLibName   = sel.name;
                    itemPage         = 0;
                    state            = State::ItemsInit;
                } else if (!sel.id.empty()) {
                    detailItemId        = sel.id;
                    detailReturnState   = State::ItemsReady;
                    detailIsEpisodeHint = (sel.type == "Episode");
                    state = State::DetailLoad;
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
                if (episodesFromHome) { episodesFromHome = false; state = State::LibsReady; }
                else                  state = State::SeasonsReady;
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
                if (tracksFromHome) {
                    tracksFromHome = false;
                    state = State::LibsReady;
                } else if (musicTab == 1)
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
                            "&VideoCodec=mpeg4&AudioCodec=mp3&Container=ts"
                            "&MaxWidth=640&MaxHeight=480"
                            "&VideoBitrate=%d&AudioBitrate=128000"
                            "&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false"
                            "&AudioStreamIndex=%d&SubtitleStreamIndex=%d&ApiKey=%s",
                            serverUrl.c_str(), detailItemId.c_str(), detailItemId.c_str(),
                            client.videoBitrate(), audioIdx, subIdx, auth.accessToken.c_str());
                    } else {
                        snprintf(fallback, sizeof(fallback),
                            "%s/Videos/%s/stream?Static=false&MediaSourceId=%s"
                            "&VideoCodec=mpeg4&AudioCodec=mp3&Container=ts"
                            "&MaxWidth=640&MaxHeight=480"
                            "&VideoBitrate=%d&AudioBitrate=128000"
                            "&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false"
                            "&AudioStreamIndex=%d&ApiKey=%s",
                            serverUrl.c_str(), detailItemId.c_str(), detailItemId.c_str(),
                            client.videoBitrate(), audioIdx, auth.accessToken.c_str());
                    }
                    url = fallback;
                }
                pendingPlayUrl = url;
                pendingPlayTitle = detail.name;
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
                            "&VideoCodec=mpeg4&AudioCodec=mp3&Container=ts"
                            "&MaxWidth=640&MaxHeight=480"
                            "&VideoBitrate=%d&AudioBitrate=128000"
                            "&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false"
                            "&AudioStreamIndex=%d&SubtitleStreamIndex=%d&ApiKey=%s",
                            serverUrl.c_str(), detailItemId.c_str(), detailItemId.c_str(),
                            client.videoBitrate(), audioIdx, subIdx, auth.accessToken.c_str());
                    } else {
                        snprintf(fallback, sizeof(fallback),
                            "%s/Videos/%s/stream?Static=false&MediaSourceId=%s"
                            "&VideoCodec=mpeg4&AudioCodec=mp3&Container=ts"
                            "&MaxWidth=640&MaxHeight=480"
                            "&VideoBitrate=%d&AudioBitrate=128000"
                            "&AllowVideoStreamCopy=false&AllowAudioStreamCopy=false"
                            "&AudioStreamIndex=%d&ApiKey=%s",
                            serverUrl.c_str(), detailItemId.c_str(), detailItemId.c_str(),
                            client.videoBitrate(), audioIdx, auth.accessToken.c_str());
                    }
                    url = fallback;
                }
                pendingPlayUrl            = url;
                pendingPlayTitle          = detail.name;
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
            searchKb.setOrigin((640 - searchKb.width()) * 0.5f, 132);
            searchKb.setEnterLabel("Search");
            switch (searchKb.update(ir, searchQuery, 64)) {
            case Keyboard::Result::Enter:            // Search key or [+]
                if (!searchQuery.empty()) state = State::SearchLoad;
                break;
            case Keyboard::Result::Cancel:           // B with an empty query
                state = searchReturnState;
                break;
            default: break;
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
                searchKb.reset();
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
                } else if (sel.type == "BoxSet" && Ui::listMode()) {
                    // text list of the collection; B comes back to the results
                    inItemsDrilldown    = true;
                    drilldownFromSearch = true;
                    currentLibId      = sel.id;
                    currentLibName    = sel.name;
                    currentLibType    = "boxsets";
                    posterMode        = false;
                    itemPage          = 0;
                    state = State::ItemsInit;
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
// Drawing helpers shared by the screens below (Ui kit look)
// ---------------------------------------------------------------
static float focusOf(bool sel, bool hover) {
    return sel ? Ui::pulse() : (hover ? 0.55f : 0.0f);
}

// Shorten s (UTF-8) with "..." until it fits maxW pixels.
static std::string fitText(GRRLIB_ttfFont* f, std::string s, int size, int maxW) {
    if ((int)Text::width(f, s.c_str(), size) <= maxW) return s;
    while (!s.empty() && (int)Text::width(f, (s + "...").c_str(), size) > maxW) {
        while (!s.empty() && (s.back() & 0xC0) == 0x80) s.pop_back();
        if (!s.empty()) s.pop_back();
    }
    return s + "...";
}

template <typename T>
static float progressOf(const T& it) {
    if (it.playbackPositionTicks <= 0 || it.runtimeTicks <= 0) return -1.0f;
    return (float)it.playbackPositionTicks / (float)it.runtimeTicks;
}

// Picture tile: shadow, focus halo, cover-cropped image (or a placeholder
// caption), border and optional progress bar.  w is the logical width; the
// drawn width follows the widescreen pre-squish like every texture.
static void drawThumb(GRRLIB_texImg* tex, float x, float y, float w, float h,
                      float focus, float prog = -1.0f, const char* caption = nullptr,
                      bool grow = true) {
    const Ui::Palette& p = Ui::pal();
    const float r = 10;
    float g  = (grow && focus > 0.0f) ? 4.0f : 0.0f;   // pops out a little when focused
    float vw = w * WiiUtils::wsScaleX() + g;
    float aspect = (w + g) / (h + g);
    x -= g * 0.5f; y -= g * 0.5f; h += g;

    Ui::shadow(x + 1, y + 3, vw - 2, h - 2, r, 6.0f, p.shadow);
    if (focus > 0.0f) Ui::shadow(x - 2, y - 2, vw + 4, h + 4, r + 2, 10.0f, Ui::alpha(p.glow, focus));
    if (tex && tex->w > 0 && tex->h > 0) {
        Ui::texCover(tex, x, y, vw, h, r, aspect);
    } else {
        Ui::roundRect(x, y, vw, h, r, p.cardTop, p.cardBottom);
        if (caption && *caption) {
            std::string c = fitText(Ui::font(), caption, 13, (int)vw - 12);
            Ui::textCentered(x + vw * 0.5f, y + h * 0.5f - 8, c.c_str(), 13, p.textDim);
        }
    }
    Ui::roundBorder(x, y, vw, h, r, 1.5f + focus * 1.5f,
                    Ui::mix(Ui::alpha(p.cardBorder, 0.8f), p.accent, focus));
    if (prog >= 0.0f) Ui::progress(x + 8, y + h - 12, vw - 16, 5, prog);
}

// Row card for the vertical lists.
static void drawRow(int x, int y, int w, int h, float focus) {
    Ui::card(x, y + 2, w, h - 6, 10, focus);
}


static void drawPageArrows(ir_t& ir, int upCy, int dnCy, bool canPrev, bool canNext,
                           int cx, int hitR) {
    auto over = [&](int cy) {
        return ir.valid && (int)ir.x >= cx - hitR && (int)ir.x < cx + hitR &&
               (int)ir.y >= cy - hitR && (int)ir.y < cy + hitR;
    };
    Ui::arrowButton(cx, upCy, true,  canPrev, over(upCy));
    Ui::arrowButton(cx, dnCy, false, canNext, over(dnCy));
}

// Small chevron at the ends of a horizontally scrolling row.
static void drawRowChevron(float ax, float ay, bool left, bool active) {
    const Ui::Palette& p = Ui::pal();
    u32 c = active ? p.accent : Ui::alpha(p.textDim, 0.3f);
    if (left) Ui::triangle(ax - 7, ay, ax + 5, ay - 9, ax + 5, ay + 9, c);
    else      Ui::triangle(ax + 7, ay, ax - 5, ay + 9, ax - 5, ay - 9, c);
}

static void sectionLabel(int x, int y, const char* s, bool active) {
    const Ui::Palette& p = Ui::pal();
    Ui::roundRect(x, y + 1, 3, 12, 1.5f, active ? p.accent : Ui::alpha(p.textDim, 0.5f));
    Ui::text(x + 9, y, s, 12, active ? p.text : p.textDim);
}

static void headerLine(int y) {
    if (Ui::headerBand() > 0) return;   // the band already separates
    Ui::roundRect(20, y, 600, 2, 1, Ui::alpha(Ui::pal().cardBorder, 0.7f));
}

// Secondary header text: dim on plain backgrounds, translucent white on a band.
static u32 headerDim() {
    return Ui::headerBand() > 0 ? 0xFFFFFFC0 : Ui::pal().textDim;
}

// Back chevron + title at the top left, shortened to maxW.
static void drawBreadcrumb(const std::string& title, int size, int maxW) {
    const Ui::Palette& p = Ui::pal();
    float cy = 14 + size * 0.55f;
    Ui::triangle(20, cy, 28, cy - 7, 28, cy + 7, Ui::headerBand() > 0 ? 0xFFFFFFFF : p.accent);
    std::string t = fitText(Ui::font(), filterDejaVu(title, 60), size, maxW - 14);
    Ui::text(34, 14, t.c_str(), size, p.text);
}

static void drawCount(int page, int perPage, int shown, int total) {
    char countStr[48];
    snprintf(countStr, sizeof(countStr), "%d-%d / %d",
             page * perPage + 1, page * perPage + shown, total);
    Ui::textRight(620, 18, countStr, 15, headerDim());
}

static const char* const kMovieTabs[4] = { "Movies", "Collections", "Favorites", "Suggestions" };
static const char* const kTvTabs[3]    = { "Series", "Suggestions", "Coming Up" };
static const char* const kMusicTabs[3] = { "Albums", "Suggestions", "Playlists" };
static const char* const kHomeTabs[3]  = { "Libraries", "Activity", "Favorites" };

// Tabbed library header: tabs centred, breadcrumb in the room on the left.
static void drawLibHeader(const std::string& libName, const char* const* tabNames,
                          int nTabs, int sel) {
    float x0 = Ui::tabs(320, 10, tabNames, nTabs, sel, 13);
    drawBreadcrumb(libName, 16, (int)x0 - 34 - 10);
    headerLine(46);
}

// ---------------------------------------------------------------
// render()
// ---------------------------------------------------------------
void LibraryView::render(ir_t& ir) {
    const Ui::Palette& p = Ui::pal();

    const bool loading = (state == State::LibsInit || state == State::LibsLoad ||
        state == State::ItemsInit || state == State::ItemsLoad ||
        state == State::PostersLoad || state == State::DetailLoad ||
        state == State::SeasonsLoad || state == State::EpisodesLoad ||
        state == State::MusicTracksLoad ||
        state == State::CollectionsLoad || state == State::FavoritesLoad ||
        state == State::MovieSuggestionsLoad ||
        state == State::TVSuggestionsLoad || state == State::TVUpcomingLoad ||
        state == State::MusicSuggestionsLoad || state == State::PlaylistsLoad ||
        state == State::GlobalFavoritesLoad || state == State::SearchLoad);
    Ui::background(!loading);

    // Loading screen (shown one frame before blocking load)
    if (loading) {
        if (ringTex) Ui::spinner(ringTex, 320, 240);
        else         Ui::textCentered(320, 200, "Loading...", 22, p.textDim);
        drawCursor(ir);
        return;
    }

    // Error screen
    if (state == State::Error) {
        const int EX = 100, EY = 140, EW = 440;
        // word-wrap the message into at most 5 lines
        std::vector<std::string> lines;
        {
            std::string cur, word;
            std::string msg = filterDejaVu(errMsg, 400) + " ";
            for (char ch : msg) {
                if (ch != ' ' && ch != '\n') { word += ch; continue; }
                std::string cand = cur.empty() ? word : cur + " " + word;
                if (!cur.empty() && (int)Text::width(font, cand.c_str(), 15) > EW - 40) {
                    lines.push_back(cur);
                    cur = word;
                } else {
                    cur = cand;
                }
                word.clear();
                if (ch == '\n' && !cur.empty()) { lines.push_back(cur); cur.clear(); }
            }
            if (!cur.empty()) lines.push_back(cur);
            if (lines.size() > 5) lines.resize(5);
        }
        int EH = 70 + (int)lines.size() * 20;
        Ui::card(EX, EY, EW, EH, 18, 0.0f);
        Ui::circle(EX + 34, EY + 32, 13, p.danger);
        Ui::textCentered(EX + 34, EY + 22, "!", 18, 0xFFFFFFFF);
        Ui::text(EX + 58, EY + 20, "Error", 20, p.danger);
        for (size_t i = 0; i < lines.size(); ++i)
            Ui::text(EX + 20, EY + 58 + i * 20, lines[i].c_str(), 15, p.text);
        const Ui::Hint h[] = { { "A", "Back" } };
        Ui::footer(h, 1);
        drawCursor(ir);
        return;
    }

    // ---- Carousel home (Flix theme) ----
    if (state == State::LibsReady && flixHome() && (browsePage ? catalog : browse).built()) {
        (browsePage ? catalog : browse).render(ir);
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
        float tabX0 = Ui::tabs(320, 10, kHomeTabs, 3, homePage == 0 ? 0 : 1, 16);
        srvLabel = fitText(font, filterDejaVu(srvLabel, 40), 14, (int)tabX0 - 30);
        Ui::text(20, 18, srvLabel.c_str(), 14, headerDim());
        headerLine(51);

        // Profile avatar – top right of header (click: back to the profile picker)
        {
            bool iconHover = ir.valid && fabsf(ir.x - 614.0f) < 24.0f && fabsf(ir.y - 26.0f) < 24.0f;
            Ui::avatar(614, Ui::headerBand() > 0 ? 22 : 26, iconHover ? 17.0f : 15.0f,
                       userName.c_str(), iconHover ? 1.0f : 0.0f);
        }

        if (homePage == 0) {
            // ---- Libraries grid (channel-style tiles) ----
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
                float f = focusOf(sel, hover);
                Ui::card(tx, ty, TILE_W, TILE_H, 14, f);

                // coloured type pill + library name
                const char* type = labelForType(libraries[i].collectionType);
                int pw = Ui::textWidth(type, 10) + 18;
                Ui::roundRect(tx + (TILE_W - pw) / 2, ty + 14, pw, 17, 8.5f,
                              colorForType(libraries[i].collectionType, true),
                              colorForType(libraries[i].collectionType, false));
                Ui::textCentered(tx + TILE_W / 2, ty + 16, type, 10, 0xFFFFFFFF);
                std::string name = fitText(font, filterDejaVu(libraries[i].name, 40), 18, TILE_W - 20);
                Ui::textCentered(tx + TILE_W / 2, ty + 44, name.c_str(), 18,
                                 Ui::mix(p.text, p.accentDark, f));
            }
            const Ui::Hint l[] = { { "A", "Open" }, { "1", "Search" } };
            const Ui::Hint r[] = { { "-/+", "Tab" } };
            Ui::footer(l, 2, r, 1);

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

            // Draw one activity card (thumbnail + progress bar + title)
            auto drawActCard = [&](int i, int cardY, const JellyfinItem& item,
                                   GRRLIB_texImg* tex, bool selCard, bool showPct,
                                   const std::string& mainTitle, const std::string& subTitle) {
                int  cx  = ACT_X0 + i * (ACT_CARD_W + ACT_CARD_GAP);
                bool hov = ir.valid && ir.x >= cx && ir.x < cx + ACT_CARD_W
                                    && ir.y >= cardY && ir.y < cardY + ACT_CARD_H;
                drawThumb(tex, cx, cardY, ACT_CARD_W, ACT_CARD_H, focusOf(selCard, hov),
                          showPct ? progressOf(item) : -1.0f, mainTitle.c_str());
                // Title + subtitle below card (strings pre-computed in buildActDisplayStrings)
                Ui::text(cx + 2, cardY + ACT_CARD_H + 4, mainTitle.c_str(), 13,
                         selCard ? p.text : Ui::mix(p.text, p.textDim, 0.3f));
                if (!subTitle.empty())
                    Ui::text(cx + 2, cardY + ACT_CARD_H + 19, subTitle.c_str(), 11, p.textDim);
            };

            // Continue Watching row
            sectionLabel(ACT_X0, 56, "IN PROGRESS", nc > 0);
            if (nc > 0) {
                for (int i = 0; i < nc; i++)
                    drawActCard(i, ACT_ROW0_Y, continueItems[i], cwTextures[i],
                                i == continueSel && actRow == 0, true,
                                cwDisplayMain[i], cwDisplaySub[i]);
            } else {
                Ui::text(ACT_X0 + 20, ACT_ROW0_Y + 40, "Rien en cours", 14, p.textDim);
            }

            // Next Up row
            sectionLabel(ACT_X0, 213, "NEXT UP", nu > 0);
            if (nu > 0) {
                for (int i = 0; i < nu; i++)
                    drawActCard(i, ACT_ROW1_Y, nextUpItems[i], nextUpTextures[i],
                                i == nextUpSel && actRow == 1, false,
                                nuDisplayMain[i], nuDisplaySub[i]);
            } else {
                Ui::text(ACT_X0 + 20, ACT_ROW1_Y + 40, "No next episode", 14, p.textDim);
            }

            const Ui::Hint l[] = { { "A", "Open" }, { "UD", "Row" } };
            const Ui::Hint r[] = { { "-/+", "Tab" }, { "B", "Back" } };
            Ui::footer(l, 2, r, 2);
        }
    }

    // ---- Items list (whole library as text) ----
    if (state == State::ItemsReady) {
        const int n  = feed.total();
        const int LW = listWidth();
        const bool coverPanel = Ui::libraryStyle() == Ui::LibraryStyle::ListCover;

        // Header — music gets a tab bar; everything else gets a breadcrumb
        JellyfinItem selItem;
        bool haveSel = n > 0 && feed.get(itemSel, selItem);
        if (currentLibType == "music" && !inItemsDrilldown) {
            Ui::tabs(320, 10, kMusicTabs, 3, musicTab, 14);
            headerLine(46);
        } else {
            drawBreadcrumb(currentLibName, 20, 420);
            char pos[48];
            if (n > 0 && haveSel)
                snprintf(pos, sizeof(pos), "%c   \xc2\xb7   %d / %d", ListFeed::letterOf(selItem), itemSel + 1, n);
            else
                snprintf(pos, sizeof(pos), "%d / %d", n > 0 ? itemSel + 1 : 0, n);
            Ui::textRight(620, 18, pos, 15, headerDim());
            headerLine(46);
        }

        if (n == 0) Ui::textCentered(320, 200, "This library is empty", 16, p.textDim);

        for (int i = 0; i < LIST_ROWS; i++) {
            int idx = viewTop + i;
            if (idx >= n) break;
            int ry = LIST_Y + i * LIST_ROW_H;
            bool sel   = (idx == itemSel);
            bool hover = ir.valid && ir.y >= ry && ir.y < ry + LIST_ROW_H &&
                         ir.x >= LIST_X && ir.x <= LIST_X + LW;
            float f = focusOf(sel, hover);
            Ui::card(LIST_X, ry + 2, LW, LIST_ROW_H - 4, 9, f);

            JellyfinItem it;
            if (!feed.get(idx, it)) {
                // still loading: a soft placeholder bar
                Ui::roundRect(LIST_X + 14, ry + 13, LW * 0.45f, 10, 5, Ui::alpha(p.textDim, 0.25f));
                continue;
            }
            int rightW = 0;
            if (it.year > 0) {
                char y[12];
                snprintf(y, sizeof(y), "%d", it.year);
                rightW = Ui::textWidth(y, 14) + 12;
                Ui::textRight(LIST_X + LW - 12, ry + 10, y, 14, p.textDim);
            }
            int textX = LIST_X + 14;
            // type badge for mixed lists (series among films, folders...)
            const char* badge = it.type == "Series" ? "SERIES" : it.type == "BoxSet" ? "COLLECTION"
                              : (it.type == "Folder" || it.type == "CollectionFolder") ? "FOLDER" : nullptr;
            if (badge && currentLibType != "tvshows") {
                int bw = Ui::textWidth(badge, 9) + 12;
                Ui::roundRect(textX, ry + 11, bw, 14, 7, Ui::alpha(p.accent, 0.85f));
                Ui::textCentered(textX + bw * 0.5f, ry + 12, badge, 9, p.textOnAccent);
                textX += bw + 8;
            }
            std::string name = fitText(font, filterDejaVu(it.name, 80), 16,
                                       LIST_X + LW - 14 - rightW - textX);
            Ui::text(textX, ry + 9, name.c_str(), 16, Ui::mix(p.text, p.accentDark, f));
            if (it.playbackPositionTicks > 0 && it.runtimeTicks > 0)
                Ui::progress(textX, ry + LIST_ROW_H - 9, 60, 3,
                             (float)it.playbackPositionTicks / it.runtimeTicks);
        }
        Ui::scrollbar(LIST_X + LW + 8, LIST_Y + 2, LIST_ROWS * LIST_ROW_H - 4, viewTop, LIST_ROWS, n);

        // Cover of the selected title (List + Cover)
        if (coverPanel && haveSel) {
            const float PX = LIST_X + LW + 26, PW = 600 - PX, PH = PW * 1.5f, PY = LIST_Y + 2;
            GRRLIB_texImg* cover = feed.cover(selItem.id);
            drawThumb(cover, PX, PY, PW, PH, 0.0f, progressOf(selItem),
                      cover ? nullptr : selItem.name.c_str(), false);
            float ty = PY + PH + 10;
            std::string t = fitText(font, filterDejaVu(selItem.name, 60), 15, (int)PW);
            Ui::text(PX, ty, t.c_str(), 15, p.text);
            char meta[64] = "";
            int mins = (int)(selItem.runtimeTicks / 600000000LL);
            if (selItem.type == "Series" && selItem.childCount > 0)
                snprintf(meta, sizeof(meta), "%d season%s", selItem.childCount, selItem.childCount > 1 ? "s" : "");
            else if (mins > 0)
                snprintf(meta, sizeof(meta), "%dh %02dmin", mins / 60, mins % 60);
            if (selItem.year > 0) {
                size_t l = strlen(meta);
                snprintf(meta + l, sizeof(meta) - l, "%s%d", l ? "  \xc2\xb7  " : "", selItem.year);
            }
            if (meta[0]) Ui::text(PX, ty + 20, meta, 13, p.textDim);
        }

        // Letter jump feedback: a big letter for a moment
        {
            unsigned long long now = ticks_to_millisecs(gettime());
            if (letterFlash && now - letterFlashMs < 700) {
                float k = 1.0f - (float)(now - letterFlashMs) / 700.0f;
                float cx = LIST_X + LW * 0.5f, cy = LIST_Y + LIST_ROWS * LIST_ROW_H * 0.5f;
                Ui::roundRect(cx - 44, cy - 44, 88, 88, 22, Ui::alpha(p.accent, 0.9f * k), Ui::alpha(p.accentDark, 0.9f * k));
                char l[2] = { letterFlash, 0 };
                Ui::textCentered(cx, cy - 30, l, 52, Ui::alpha(0xFFFFFFFF, k));
            }
            if (feed.jumpPending())
                Ui::spinner(ringTex, LIST_X + LW * 0.5f, LIST_Y + LIST_ROWS * LIST_ROW_H * 0.5f);
        }

        // Footer
        const Ui::Hint l[] = { { "A", "Open" }, { "B", "Back" } };
        if (currentLibType == "music" && !inItemsDrilldown) {
            const Ui::Hint r[] = { { "LR", "A-Z" }, { "-/+", "Tab" } };
            Ui::footer(l, 2, r, 2);
        } else {
            const Ui::Hint r[] = { { "LR", "A-Z" }, { "-/+", "Page" } };
            Ui::footer(l, 2, r, 2);
        }
    }

    // ---- Season list ----
    if (state == State::SeasonsReady) {
        drawBreadcrumb(currentSeriesName, 20, 580);
        headerLine(46);

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
            float f = focusOf(sel, hover);
            drawRow(LIST_X, ry, LIST_W, ROW_H, f);
            std::string name = fitText(font, filterDejaVu(seasons[idx].name, 45), 18, LIST_W - 32);
            Ui::text(LIST_X + 16, ry + 11, name.c_str(), 18, Ui::mix(p.text, p.accentDark, f));
        }
        Ui::scrollbar(614, LIST_Y + 2, ITEMS_VISIBLE * ROW_H - 6, seasonTop, ITEMS_VISIBLE, n);
        const Ui::Hint l[] = { { "A", "Select" }, { "B", "Back" } };
        Ui::footer(l, 2);
    }

    // ---- Episode list ----
    if (state == State::EpisodesReady) {
        std::string hdr = currentSeriesName + "  /  " + currentSeasonName;
        drawBreadcrumb(hdr, 18, 580);
        headerLine(46);

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
            float f = focusOf(sel, hover);
            drawRow(LIST_X, ry, LIST_W, ROW_H, f);

            int tx = LIST_X + 16;
            // Episode number badge
            if (episodes[idx].indexNumber > 0) {
                char num[8];
                snprintf(num, sizeof(num), "E%02d", episodes[idx].indexNumber);
                int bw = Ui::textWidth(num, 12) + 14;
                Ui::roundRect(tx, ry + 12, bw, 20, 10, Ui::mix(p.accent, 0xFFFFFFFF, 0.2f), p.accentDark);
                Ui::textCentered(tx + bw / 2, ry + 15, num, 12, p.textOnAccent);
                tx += bw + 10;
            }
            std::string labelStr = fitText(font, filterDejaVu(episodes[idx].name, 60), 18,
                                           LIST_X + LIST_W - 16 - tx);
            Ui::text(tx, ry + 11, labelStr.c_str(), 18, Ui::mix(p.text, p.accentDark, f));
        }
        Ui::scrollbar(614, LIST_Y + 2, ITEMS_VISIBLE * ROW_H - 6, episodeTop, ITEMS_VISIBLE, n);
        const Ui::Hint l[] = { { "A", "Details" }, { "B", "Back" } };
        Ui::footer(l, 2);
    }

    // ---- Music track list ----
    if (state == State::MusicTracksReady) {
        // Header: album name + artist
        std::string hdr = musicAlbumName;
        if (!musicAlbumArtist.empty()) hdr += "  \xe2\x80\x94  " + musicAlbumArtist;
        drawBreadcrumb(hdr, 18, 580);
        headerLine(46);

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
            float f = focusOf(sel, hover);
            drawRow(LIST_X, ry, LIST_W, ROW_H, f);
            const JellyfinAudioItem& at = musicTracks[idx];

            // Track number in a circle
            int tx = LIST_X + 14;
            if (at.trackNumber > 0) {
                char num[8];
                snprintf(num, sizeof(num), "%d", at.trackNumber);
                Ui::circle(tx + 11, ry + 22, 11, sel ? p.accent : Ui::alpha(p.cardBorder, 0.6f));
                Ui::textCentered(tx + 11, ry + 15, num, 12, sel ? p.textOnAccent : p.text);
                tx += 32;
            }
            // Duration on the right
            int dw = 0;
            if (at.runtimeTicks > 0) {
                int secs = (int)(at.runtimeTicks / 10000000LL);
                char dur[12];
                snprintf(dur, sizeof(dur), "%d:%02d", secs / 60, secs % 60);
                dw = Ui::textWidth(dur, 15) + 12;
                Ui::textRight(LIST_X + LIST_W - 14, ry + 13, dur, 15, p.textDim);
            }
            std::string labelStr = fitText(font, filterDejaVu(at.name, 60), 18,
                                           LIST_X + LIST_W - 16 - dw - tx);
            Ui::text(tx, ry + 11, labelStr.c_str(), 18, Ui::mix(p.text, p.accentDark, f));
        }
        Ui::scrollbar(614, LIST_Y + 2, MUSIC_TRACKS_VISIBLE * ROW_H - 6, musicTrackTop,
                      MUSIC_TRACKS_VISIBLE, n);
        const Ui::Hint l[] = { { "A", "Play" }, { "B", "Back" } };
        Ui::footer(l, 2);
    }

    if (state == State::PostersReady) {
        // Header: tabs (movies / tv) or the selected title, breadcrumb left, count right
        if (currentLibType == "movies" && !inBoxSetDrilldown) {
            drawLibHeader(currentLibName, kMovieTabs, 4, movieTab);
        } else if (currentLibType == "tvshows") {
            drawLibHeader(currentLibName, kTvTabs, 3, tvTab);
        } else {
            drawBreadcrumb(currentLibName, inBoxSetDrilldown ? 16 : 20, 120);
            headerLine(46);
            if (posterSel >= 0 && posterSel < (int)items.size()) {
                const int HDR_X = 140, HDR_W = 360;
                std::string t = fitText(font, items[posterSel].name, 16, HDR_W);
                Ui::textCentered(HDR_X + HDR_W / 2, 16, t.c_str(), 16, p.text);
            }
        }
        drawCount(itemPage, POSTERS_PER_PAGE, (int)items.size(), itemTotal);

        int n = (int)items.size();
        // draw the focused poster last so its halo overlaps its neighbours
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < n && i < POSTER_VISIBLE; i++) {
                bool sel = (i == posterSel);
                if (sel != (pass == 1)) continue;
                int px = POSTER_X0 + (i % POSTER_COLS) * POSTER_STRIDE_X;
                int py = POSTER_Y0 + (i / POSTER_COLS) * POSTER_STRIDE_Y;
                drawThumb(posterTextures[i], px, py, POSTER_W, POSTER_H,
                          sel ? Ui::pulse() : 0.0f, progressOf(items[i]), items[i].name.c_str());
            }
        }

        int totalPages = (itemTotal + POSTERS_PER_PAGE - 1) / POSTERS_PER_PAGE;
        drawPageArrows(ir, ARROW_UP_CY, ARROW_DN_CY, itemPage > 0, itemPage + 1 < totalPages,
                       ARROW_CX, ARROW_HIT_R);

        // Footer: selected title centred, page on the right
        const Ui::Hint l[] = { { "B", "Back" } };
        char pageStr[24];
        snprintf(pageStr, sizeof(pageStr), "Page %d / %d", itemPage + 1, totalPages);
        const Ui::Hint r[] = { { "", pageStr } };
        const char* center = (posterSel >= 0 && posterSel < n) ? items[posterSel].name.c_str() : nullptr;
        Ui::footer(l, 1, r, totalPages > 1 ? 1 : 0, center);
    }

    // ---- Global Favourites poster grid ----
    if (state == State::GlobalFavoritesReady) {
        float tabX0 = Ui::tabs(320, 10, kHomeTabs, 3, 2, 16);
        std::string srv = fitText(font, filterDejaVu(auth.serverName, 40), 14, (int)tabX0 - 30);
        Ui::text(20, 18, srv.c_str(), 14, headerDim());
        headerLine(51);

        // Count top-right
        if (itemTotal > 0) drawCount(itemPage, POSTERS_PER_PAGE, (int)items.size(), itemTotal);
        else               Ui::textRight(620, 18, "Pas de favoris", 15, headerDim());

        int n = (int)items.size();
        const int GF_Y0 = POSTER_Y0 + 12; // header is 52px tall vs 46px for library sub-pages
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < n && i < POSTER_VISIBLE; i++) {
                bool sel = (i == posterSel);
                if (sel != (pass == 1)) continue;
                int px = POSTER_X0 + (i % POSTER_COLS) * POSTER_STRIDE_X;
                int py = GF_Y0 + (i / POSTER_COLS) * POSTER_STRIDE_Y;
                drawThumb(posterTextures[i], px, py, POSTER_W, POSTER_H,
                          sel ? Ui::pulse() : 0.0f, -1.0f, items[i].name.c_str());
                // Type badge (Movie / Series / Album)
                const char* badge = labelForType(items[i].type);
                int bw = Ui::textWidth(badge, 10) + 12;
                Ui::roundRect(px + 6, py + POSTER_H - 22, bw, 16, 8, 0x000000A0);
                Ui::text(px + 12, py + POSTER_H - 20, badge, 10, 0xFFFFFFFF);
            }
        }

        int totalPages = (itemTotal + POSTERS_PER_PAGE - 1) / POSTERS_PER_PAGE;
        drawPageArrows(ir, ARROW_UP_CY + 12, ARROW_DN_CY + 12, itemPage > 0,
                       itemPage + 1 < totalPages, ARROW_CX, ARROW_HIT_R);

        const Ui::Hint l[] = { { "A", "Detail" } };
        const Ui::Hint r[] = { { "-/+", "Tab" }, { "B", "Back" } };
        const char* center = (posterSel >= 0 && posterSel < n) ? items[posterSel].name.c_str() : nullptr;
        Ui::footer(l, 1, r, 2, center);
    }

    // ---- Suggestion rows (movies / tv / upcoming) ----
    const int SG_X0  = 15;
    const int SG_CW  = POSTER_W;  // 130
    const int SG_CH  = 160;
    const int SG_GAP = 20;        // 4 × 130 + 3 × 20 = 580 fits in 640
    // One horizontally scrolling row of poster cards with its end chevrons.
    auto drawSugRow = [&](int rowY, const std::vector<JellyfinItem>& list,
                          GRRLIB_texImg* const* texs, int off, int selIdx, bool rowActive,
                          bool preferSeries, bool showProgress) {
        int nItems = (int)list.size();
        for (int pass = 0; pass < 2; pass++) {
            for (int si = 0; si < SUGG_VISIBLE; si++) {
                int i = off + si;
                if (i >= nItems) break;
                bool sel = rowActive && i == selIdx;
                if (sel != (pass == 1)) continue;
                int  cx  = SG_X0 + si * (SG_CW + SG_GAP);
                bool hov = ir.valid && ir.x >= cx && ir.x < cx + SG_CW
                                    && ir.y >= rowY && ir.y < rowY + SG_CH;
                const JellyfinItem& item = list[i];
                const std::string& nm = (preferSeries && !item.seriesName.empty()) ? item.seriesName : item.name;
                drawThumb(texs[i], cx, rowY, SG_CW, SG_CH, focusOf(sel, hov),
                          showProgress ? progressOf(item) : -1.0f, nm.c_str());
                int visW = (int)(SG_CW * WiiUtils::wsScaleX() + 0.5f);
                std::string title = fitText(font, filterDejaVu(nm, 30), 12, visW);
                Ui::text(cx + 1, rowY + SG_CH + 4, title.c_str(), 12, sel ? p.text : p.textDim);
            }
        }
        int ay = rowY + SG_CH / 2;
        drawRowChevron(SG_X0 - 9, ay, true, off > 0);
        drawRowChevron(SG_X0 + SUGG_VISIBLE * (SG_CW + SG_GAP) - SG_GAP + 6, ay, false,
                       off + SUGG_VISIBLE < nItems);
    };

    // ---- Movie Suggestions (continue watching + recently added) ----
    if (state == State::MovieSuggestionsReady) {
        drawLibHeader(currentLibName, kMovieTabs, 4, movieTab);
        const int SG_ROW0_Y = 65, SG_ROW1_Y = 270;
        int nc = (int)movieContItems.size();
        int nr = (int)movieRecentItems.size();

        sectionLabel(SG_X0, 50, "IN PROGRESS", nc > 0);
        if (nc > 0)
            drawSugRow(SG_ROW0_Y, movieContItems, movieContTex, movieSuggestContOff,
                       movieSuggestContSel, movieSuggestRow == 0, false, true);
        else
            Ui::text(SG_X0 + 20, SG_ROW0_Y + 60, "Aucun film en cours", 14, p.textDim);

        sectionLabel(SG_X0, 252, "RECENTLY ADDED", nr > 0);
        if (nr > 0)
            drawSugRow(SG_ROW1_Y, movieRecentItems, movieRecentTex, movieSuggestRecOff,
                       movieSuggestRecSel, movieSuggestRow == 1, false, true);
        else
            Ui::text(SG_X0 + 20, SG_ROW1_Y + 60, "No recent movies", 14, p.textDim);

        const Ui::Hint l[] = { { "A", "Detail" }, { "B", "Back" } };
        const Ui::Hint r[] = { { "-/+", "Tab" } };
        Ui::footer(l, 2, r, 1);
    }

    // ---- TV Suggestions (continue watching episodes + recently added series) ----
    if (state == State::TVSuggestionsReady) {
        drawLibHeader(currentLibName, kTvTabs, 3, tvTab);
        const int SG_ROW0_Y = 65, SG_ROW1_Y = 270;
        int nc = (int)tvContItems.size();
        int nr = (int)tvRecentItems.size();

        sectionLabel(SG_X0, 50, "IN PROGRESS", nc > 0);
        if (nc > 0)
            drawSugRow(SG_ROW0_Y, tvContItems, tvContTex, tvSuggestContOff,
                       tvSuggestContSel, tvSuggestRow == 0, true, true);
        else
            Ui::text(SG_X0 + 20, SG_ROW0_Y + 60, "No episode in progress", 14, p.textDim);

        sectionLabel(SG_X0, 252, "RECENT SERIES", nr > 0);
        if (nr > 0)
            drawSugRow(SG_ROW1_Y, tvRecentItems, tvRecentTex, tvSuggestRecOff,
                       tvSuggestRecSel, tvSuggestRow == 1, true, true);
        else
            Ui::text(SG_X0 + 20, SG_ROW1_Y + 60, "No recent series", 14, p.textDim);

        const Ui::Hint l[] = { { "A", "Select" }, { "B", "Back" } };
        const Ui::Hint r[] = { { "-/+", "Tab" } };
        Ui::footer(l, 2, r, 1);
    }

    // ---- TV Upcoming (unaired episodes) ----
    if (state == State::TVUpcomingReady) {
        drawLibHeader(currentLibName, kTvTabs, 3, tvTab);
        int nu = (int)tvUpcomingItems.size();
        if (nu == 0) {
            Ui::card(170, 160, 300, 90, 18, 0.0f);
            Ui::textCentered(320, 182, "No content.", 20, p.text);
            Ui::textCentered(320, 214, "Make sure metadata download is enabled.", 13, p.textDim);
        } else {
            sectionLabel(SG_X0, 55, "COMING UP", true);
            drawSugRow(80, tvUpcomingItems, tvUpcomingTex, tvUpcomingOff,
                       tvUpcomingSel, true, true, false);
        }
        const Ui::Hint l[] = { { "A", "Detail" }, { "B", "Back" } };
        const Ui::Hint r[] = { { "-/+", "Tab" } };
        Ui::footer(l, 2, r, 1);
    }

    // ---- Music Suggestions (recently added albums) ----
    if (state == State::MusicSuggestionsReady) {
        Ui::tabs(320, 10, kMusicTabs, 3, musicTab, 14);
        headerLine(46);

        // 2-row × 4-col grid of album art cards
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

        int nr = (int)musicRecentItems.size();
        sectionLabel(GRID_X, HDR_Y, "RECENTLY ADDED", nr > 0);

        auto drawAlbumCard = [&](int col, int cardY, int i) {
            int  cx  = GRID_X + col * (AC_CW + AC_GAP);
            int  visW = (int)(AC_CW * WiiUtils::wsScaleX() + 0.5f);
            bool sel = (i == musicSuggestSel);
            bool hov = ir.valid && ir.x >= cx && ir.x < cx + AC_CW
                                && ir.y >= cardY && ir.y < cardY + AC_CH;
            drawThumb(musicRecentTex[i], cx, cardY, AC_CW, AC_CH, focusOf(sel, hov), -1.0f,
                      musicRecentItems[i].name.c_str());
            std::string title = fitText(font, filterDejaVu(musicRecentItems[i].name, 30), 12, visW);
            Ui::text(cx + 1, cardY + AC_CH + 5, title.c_str(), 12, (sel || hov) ? p.text : p.textDim);
        };

        if (nr == 0) {
            Ui::textCentered(320, 200, "No recent albums", 16, p.textDim);
        } else {
            for (int pass = 0; pass < 2; pass++) {
                for (int i = 0; i < nr && i < COLS * 2; i++) {
                    if ((i == musicSuggestSel) != (pass == 1)) continue;
                    drawAlbumCard(i % COLS, i < COLS ? ROW0_Y : ROW1_Y, i);
                }
            }
        }
        const Ui::Hint l[] = { { "A", "Open" }, { "B", "Back" } };
        const Ui::Hint r[] = { { "-/+", "Tab" } };
        Ui::footer(l, 2, r, 1);
    }

    // ---- Item detail ----
    if (state == State::DetailReady || state == State::ResumePrompt)
        drawDetailView(ir);

    // ---- Resume prompt overlay ----
    if (state == State::ResumePrompt) {
        GRRLIB_Rectangle(Ui::screenLeft(), 0, Ui::screenWidth(), 480, p.dim, 1);

        // Dialog box (DW/DH/button geometry shared with update())
        const int DW = 340, DH = 120;
        const int DX = (640 - DW) / 2;
        const int DY = (480 - DH) / 2;
        Ui::card(DX, DY, DW, DH + 34, 18, 0.0f);

        Ui::textCentered(320, DY + 14, "Resume playback?", 17, p.text);
        {
            int secs = (int)(detail.playbackPositionTicks / 10000000LL);
            int rh = secs / 3600, rm = (secs % 3600) / 60, rs = secs % 60;
            char hint[48];
            if (rh > 0) snprintf(hint, sizeof(hint), "at %d:%02d:%02d", rh, rm, rs);
            else        snprintf(hint, sizeof(hint), "at %d:%02d", rm, rs);
            Ui::textCentered(320, DY + 38, hint, 13, p.accentDark);
        }

        // Two buttons: Continue | From Start
        const char* btnLabels[2] = { "Continue", "From Start" };
        const int BW = 130, BH = 30, BGAP = 16;
        int bTotalW = BW * 2 + BGAP;
        int bStartX = DX + (DW - bTotalW) / 2;
        int bY      = DY + DH - BH - 14;
        for (int i = 0; i < 2; ++i)
            Ui::button(bStartX + i * (BW + BGAP), bY, BW, BH, btnLabels[i], 15,
                       resumeSel == i ? Ui::pulse() : 0.0f);

        // Hints inside the dialog
        const Ui::Hint h[] = { { "LR", "Select" }, { "A", "Confirm" }, { "B", "Cancel" } };
        float hw = 0;
        for (const auto& hi : h) hw += Ui::hintWidth(hi);
        float hx = 320 - (hw - 14) * 0.5f;
        Ui::roundRect(DX + 16, DY + DH - 2, DW - 32, 1.5f, 0.75f, Ui::alpha(p.cardBorder, 0.6f));
        for (const auto& hi : h) hx += Ui::hint(hx, DY + DH + 6, hi);
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
    const Ui::Palette& p = Ui::pal();
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
    // both end before the info card (INFO_X - 14 = 226): 20 + 196 / 20 + 200
    const int POSTER_W2 = detailIsEpisode ? 196 : 200;
    const int POSTER_H2 = detailIsEpisode ? 110 : 285;

    float ws   = WiiUtils::wsScaleX();
    int   visW = (int)(POSTER_W2 * ws + 0.5f);

    // ---- Info panel ----
    Ui::card(INFO_X - 14, POSTER_Y - 12, 640 - (INFO_X - 14) - 6, 438 - (POSTER_Y - 12), 16, 0.0f);

    // ---- Thumbnail / Poster ----
    bool posterHover = ir.valid
        && ir.x >= POSTER_X && ir.x < POSTER_X + visW
        && ir.y >= POSTER_Y && ir.y < POSTER_Y + POSTER_H2;
    drawThumb(detailTex, POSTER_X, POSTER_Y, POSTER_W2, POSTER_H2,
              posterHover ? 0.8f : 0.0f, progressOf(detail), detail.name.c_str(), false);

    // ---- Play button overlay (shown when cursor hovers the poster) ----
    if (posterHover) {
        Ui::roundRect(POSTER_X, POSTER_Y, visW, POSTER_H2, 10, 0x00000070);
        float cx = POSTER_X + visW * 0.5f;
        float cy = POSTER_Y + POSTER_H2 * 0.5f - 8;
        Ui::shadow(cx - 26, cy - 24, 52, 52, 26, 6.0f, 0x00000060);
        Ui::circle(cx, cy, 26, p.accent);
        Ui::roundBorder(cx - 26, cy - 26, 52, 52, 26, 2.0f, 0xFFFFFFE0);
        Ui::triangle(cx - 8, cy - 12, cx + 13, cy, cx - 8, cy + 12, 0xFFFFFFFF);
        Ui::textCentered(cx, cy + 32, "Lire", 15, 0xFFFFFFFF);
    }

    // ---- Title ----
    int y = POSTER_Y;
    {
        bool jp = hasJapanese(detail.name);
        std::string title = jp ? detail.name : fitText(font, filterDejaVu(detail.name, 60), 22, INFO_W - 12);
        Text::print(INFO_X, y, jp ? jpFont : font, title.c_str(), 22, p.text);
    }
    y += 32;

    // ---- Year  Runtime  Rating (chips) ----
    {
        char chips[3][24];
        int nChips = 0;
        if (detail.year)
            snprintf(chips[nChips++], sizeof(chips[0]), "%d", detail.year);
        if (detail.runtimeTicks > 0) {
            int secs = (int)(detail.runtimeTicks / 10000000LL);
            int h = secs / 3600, m = (secs % 3600) / 60;
            if (h > 0) snprintf(chips[nChips++], sizeof(chips[0]), "%dh %02dmin", h, m);
            else       snprintf(chips[nChips++], sizeof(chips[0]), "%dmin", m);
        }
        if (!detail.officialRating.empty())
            snprintf(chips[nChips++], sizeof(chips[0]), "%s", detail.officialRating.c_str());
        int cx = INFO_X;
        for (int i = 0; i < nChips; ++i) {
            int cw = Ui::textWidth(chips[i], 13) + 18;
            Ui::roundRect(cx, y, cw, 20, 10, p.field);
            Ui::roundBorder(cx, y, cw, 20, 10, 1.0f, p.fieldBorder);
            Ui::textCentered(cx + cw / 2, y + 3, chips[i], 13, p.textDim);
            cx += cw + 6;
        }
        if (nChips) y += 28;
    }

    // ---- Resume hint (shown when playback position is saved) ----
    if (detail.playbackPositionTicks > 0 && detail.runtimeTicks > 0) {
        int secs = (int)(detail.playbackPositionTicks / 10000000LL);
        int h    = secs / 3600, m = (secs % 3600) / 60;
        int pct  = (int)(detail.playbackPositionTicks * 100LL / detail.runtimeTicks);
        char buf[48];
        if (h > 0) snprintf(buf, sizeof(buf), "Resume at %dh%02d (%d%%)", h, m, pct);
        else       snprintf(buf, sizeof(buf), "Resume at %dmin (%d%%)", m, pct);
        Ui::triangle(INFO_X, y + 3, INFO_X + 9, y + 8, INFO_X, y + 13, p.accentDark);
        Ui::text(INFO_X + 14, y, buf, 14, p.accentDark);
        y += 20;
    }

    // ---- Genres ----
    if (!detail.genres.empty()) {
        std::string g;
        for (size_t i = 0; i < detail.genres.size(); i++) {
            if (i) g += "  \xc2\xb7  ";
            g += detail.genres[i];
        }
        g = fitText(font, g, 14, INFO_W - 12);
        Ui::text(INFO_X, y, g.c_str(), 14, p.textDim);
        y += 18;
    }
    y += 6;

    // ---- Overview (pre-computed lines, no per-frame width measuring) ----
    if (!detailLines.empty()) {
        for (const auto& line : detailLines) {
            Ui::text(INFO_X, y, line.c_str(), 13, Ui::mix(p.text, p.textDim, 0.25f));
            y += 17;
        }
        y += 6;
    }

    // ---- Cast & crew (max 6) ----
    if (!detail.people.empty()) {
        Ui::text(INFO_X, y, "Cast & crew", 14, p.accentDark);
        y += 18;
        int shown = 0;
        for (const auto& pp : detail.people) {
            if (shown >= 6) break;
            if (pp.name.empty() && pp.character.empty()) continue;
            int rx = INFO_X + 8;
            if (!pp.name.empty() && hasJapanese(pp.name)) {
                // Japanese VA name: render with jpFont, then Latin suffix with font
                Text::print(rx, y, jpFont, pp.name.c_str(), 13, p.text);
                rx += Text::width(jpFont, pp.name.c_str(), 13);
                std::string suffix;
                if (pp.role == "Director")          suffix = " (director)";
                else if (!pp.character.empty())     suffix = " - " + pp.character;
                if (!suffix.empty())
                    Ui::text(rx, y, suffix.c_str(), 13, p.textDim);
            } else {
                // All-Latin line: name in text colour, role dimmed
                const std::string& nm = !pp.name.empty() ? pp.name : pp.character;
                Ui::text(rx, y, nm.c_str(), 13, p.text);
                std::string suffix;
                if (!pp.name.empty()) {
                    if (pp.role == "Director")          suffix = " (director)";
                    else if (!pp.character.empty())     suffix = " - " + pp.character;
                }
                if (!suffix.empty()) {
                    int nw = Ui::textWidth(nm.c_str(), 13);
                    suffix = fitText(font, suffix, 13, INFO_W - 16 - nw);
                    Ui::text(rx + nw, y, suffix.c_str(), 13, p.textDim);
                }
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
        Ui::roundRect(INFO_X, STREAM_Y0 - 8, INFO_W - 14, 1.5f, 0.75f, Ui::alpha(p.cardBorder, 0.6f));

        // Validate UTF-8 and truncate at a safe codepoint boundary so the
        // renderer never receives a broken multi-byte sequence.
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
            if (focused)
                Ui::roundRect(INFO_X - 6, ry - 3, INFO_W - 8, STREAM_ROW_H - 3, (STREAM_ROW_H - 3) * 0.5f,
                              Ui::mix(p.accent, 0xFFFFFFFF, 0.2f), p.accentDark);

            u32 labelCol = focused ? p.textOnAccent : p.textDim;
            u32 valueCol = focused ? p.textOnAccent : p.text;

            const char* label = row == 0 ? "Audio" : "Subtitles";
            const char* rawTitle;
            if (row == 0)
                rawTitle = detailAudioSel < (int)detail.audioStreams.size()
                    ? detail.audioStreams[detailAudioSel].displayTitle.c_str() : "-";
            else
                rawTitle = (detailSubSel == -1)
                    ? "Off"
                    : (detailSubSel < (int)detail.subtitleStreams.size()
                        ? detail.subtitleStreams[detailSubSel].displayTitle.c_str() : "-");
            Ui::text(INFO_X + 4, ry, label, 13, labelCol);
            std::string t = safeTitle(rawTitle, 36);
            char buf[64];
            snprintf(buf, sizeof(buf), "\xe2\x80\xb9 %s \xe2\x80\xba", t.c_str());
            GRRLIB_ttfFont* tf = hasJapanese(t) ? jpFont : font;
            Text::print(INFO_X + 74, ry, tf, buf, 13, valueCol);
        }
    }

    // ---- Footer ----
    const Ui::Hint l[] = { { "A", "Play" }, { "B", "Back" } };
    const Ui::Hint r[] = { { "UD", "Focus" }, { "LR", "Change" } };
    Ui::footer(l, 2, r, (hasAudio || hasSub) ? 2 : 0);
}

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
void LibraryView::renderSearchInput(ir_t& ir) {
    const Ui::Palette& p = Ui::pal();
    Ui::header("Search");

    // Search field: placeholder when empty, blinking caret
    {
        const float FX = 62, FY = 70, FW = 516, FH = 38;
        bool caretOn = (ticks_to_millisecs(gettime()) / 500) % 2 == 0;
        std::string display = searchQuery;
        while (!display.empty() && Text::width(font, display.c_str(), 18) > (u32)(FW - 44))
            display.erase(display.begin());
        Ui::field(FX, FY, FW, FH, nullptr, 18, true);
        if (searchQuery.empty())
            Ui::text(FX + 20, FY + 9, "Type a title, an actor...", 18, Ui::alpha(p.textDim, 0.8f));
        else
            Ui::text(FX + 20, FY + 9, display.c_str(), 18, p.text);
        if (caretOn) {
            float cx = FX + 20 + (searchQuery.empty() ? 0 : Ui::textWidth(display.c_str(), 18)) + 1;
            Ui::roundRect(cx, FY + 9, 2, 20, 1, p.accent);
        }
    }

    // Keyboard on its panel
    Ui::card((640 - searchKb.width()) * 0.5f - 10, 122,
             searchKb.width() + 20, searchKb.height() + 20, 16, 0.0f);
    searchKb.render(ir);

    const Ui::Hint l[] = { { "A", "Type" }, { "B", searchQuery.empty() ? "Back" : "Delete" } };
    const Ui::Hint r[] = { { "-", "Shift" }, { "+", "Search" } };
    Ui::footer(l, 2, r, 2);
}

// ---------------------------------------------------------------
void LibraryView::renderSearchResults(ir_t& ir) {
    const Ui::Palette& p = Ui::pal();
    int n = (int)searchResults.size();

    // Header
    {
        char sc[16] = "";
        if (n > SEARCH_VISIBLE) snprintf(sc, sizeof(sc), "%d / %d", searchSel + 1, n);
        std::string hdr = fitText(font, "Results: " + filterDejaVu(searchQuery, 40), 24, 460);
        Ui::header(hdr.c_str(), sc[0] ? sc : nullptr);
    }

    if (n == 0) {
        Ui::textCentered(320, 200, "No results.", 20, p.textDim);
    } else {
        for (int i = 0; i < SEARCH_VISIBLE; i++) {
            int idx = searchTop + i;
            if (idx >= n) break;
            int   ry  = LIST_Y + i * ROW_H;
            bool  sel = (idx == searchSel);
            bool  hov = ir.valid &&
                        ir.x >= LIST_X && ir.x <= LIST_X + LIST_W &&
                        ir.y >= ry && ir.y < ry + ROW_H;
            float f = focusOf(sel, hov);
            drawRow(LIST_X - 4, ry, LIST_W + 8, ROW_H, f);

            // Type badge
            const std::string& type = searchResults[idx].type;
            const char* badge = "?";
            u32 bCol = 0x446688FF;
            if      (type == "Movie")       { badge = "MOVIE";   bCol = 0x2D7DE0FF; }
            else if (type == "Series")      { badge = "SERIES";  bCol = 0xE0563FFF; }
            else if (type == "Episode")     { badge = "EP";      bCol = 0xC2453AFF; }
            else if (type == "MusicAlbum")  { badge = "ALBUM";   bCol = 0x3DAF5AFF; }
            else if (type == "Audio")       { badge = "TRACK";   bCol = 0x3DAF5AFF; }
            else if (type == "MusicArtist") { badge = "ARTIST";  bCol = 0x2C9450FF; }
            else if (type == "BoxSet")      { badge = "COLLEC";  bCol = 0x8E54D6FF; }
            else if (type == "Playlist")    { badge = "LIST";    bCol = 0x2A9FB0FF; }
            Ui::roundRect(LIST_X + 8, ry + 12, 54, 20, 10, Ui::mix(bCol, 0xFFFFFFFF, 0.15f), bCol);
            Ui::textCentered(LIST_X + 35, ry + 15, badge, 10, 0xFFFFFFFF);

            // Title
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
            label = fitText(font, label, 16, LIST_W - 90);
            Ui::text(LIST_X + 74, ry + 12, label.c_str(), 16, Ui::mix(p.text, p.accentDark, f));
        }
    }

    const Ui::Hint l[] = { { "A", "Open" }, { "1", "Nouvelle recherche" } };
    const Ui::Hint r[] = { { "B", "Back" } };
    Ui::footer(l, 2, r, 1);
}
