#include "LibraryView.h"
#include "LibraryDraw.h"
#include "JpegTexture.h"
#include "../version.h"
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
#include <ogc/lwp_watchdog.h> // gettime(), ticks_to_millisecs()
#include <ogc/lwp.h>          // LWP_CreateThread / LWP_JoinThread

// widescreen flag definition (declared in Utils.h)
namespace WiiUtils { bool widescreen = false; }

using namespace LibDraw;

// ---------------------------------------------------------------
LibraryView::LibraryView(GRRLIB_ttfFont* f, GRRLIB_ttfFont* jf, GRRLIB_texImg* cursor,
                          GRRLIB_texImg* ring,
                          JellyfinClient& c,
                          const JellyfinAuth& a, const std::string& url)
    : font(f), jpFont(jf), cursorTex(cursor), ringTex(ring), client(c), auth(a), serverUrl(url),
      browse(c, url, a), catalog(c, url, a, BrowseHome::Mode::Catalog), feed(c, url, a) {
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

/* Fills pendingPlay* for the item in `detail` (App then plays it).  queue:
 * the episodes previous/next walk through, instead of the season list. */
void LibraryView::preparePlay(long long startTicks, const std::vector<JellyfinEpisode>* queue)
{
    int audioIdx = (!detail.audioStreams.empty())
        ? detail.audioStreams[detailAudioSel].index : 0;
    int subIdx = (detailSubSel >= 0 && !detail.subtitleStreams.empty())
        ? detail.subtitleStreams[detailSubSel].index : -1;

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
    if (queue && !queue->empty()) {
        /* shuffle: the drawn list is the queue, playing from its start */
        pendingPlayEpisodes   = *queue;
        pendingPlaySeriesId   = currentSeriesId;
        pendingPlayEpisodeIdx = 0;
    } else if (detailIsEpisode && !episodes.empty()) {
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
}

/* Shuffle (2 on the season or episode list): the server draws up to 100
 * episodes of the series, or of one season, in random order; the first one
 * starts and the rest become the queue for next / previous. */
bool LibraryView::startShuffle(const std::string& seasonId)
{
    std::vector<JellyfinEpisode> drawn;
    bool ok = false;
    std::string err;
    freeDetail();
    runWithLoading([&]() {
        ok = client.getShuffledEpisodes(serverUrl, auth, currentSeriesId, seasonId, 100, drawn);
        if (ok && !drawn.empty())
            ok = client.getItemDetail(serverUrl, auth, drawn[0].id, detail);
        if (!ok) err = client.lastError();
    });
    if (!ok) { errMsg = err; state = State::Error; return false; }
    if (drawn.empty()) return false;
    SYS_Report("[Shuffle] %d episode(s) drawn%s\n", (int)drawn.size(), seasonId.empty() ? "" : " (one season)");
    detailItemId    = drawn[0].id;
    detailIsEpisode = true;
    preparePlay(0LL, &drawn);
    return true;
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
        client.logServerInfo(serverUrl);
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
        // Loading screens: the spinner frame is up, now fetch
        case State::LibsInit:              state = State::LibsLoad;  return false;
        case State::ItemsInit:             state = State::ItemsLoad; return false;
        case State::LibsLoad:              loadLibraries();          return false;
        case State::ItemsLoad:             loadItems();              return false;
        case State::PostersLoad:           loadPosters();            return false;
        case State::SeasonsLoad:           loadSeasons();            return false;
        case State::EpisodesLoad:          loadEpisodes();           return false;
        case State::DetailLoad:            loadDetail();             return false;
        case State::MusicTracksLoad:       loadMusicTracks();        return false;
        case State::CollectionsLoad:       loadMovieCollections();   return false;
        case State::FavoritesLoad:         loadMovieFavorites();     return false;
        case State::MovieSuggestionsLoad:  loadMovieSuggestions();   return false;
        case State::TVSuggestionsLoad:     loadTVSuggestions();      return false;
        case State::TVUpcomingLoad:        loadTVUpcoming();         return false;
        case State::MusicSuggestionsLoad:  loadMusicSuggestions();   return false;
        case State::PlaylistsLoad:         loadPlaylistsTab();       return false;
        case State::GlobalFavoritesLoad:   loadGlobalFavorites();    return false;
        case State::SearchLoad:            performSearch();          return false;

        // Screens
        case State::LibsReady:             return updateHome(ir, aPressed);
        case State::GlobalFavoritesReady:  return updateGlobalFavorites(ir, aPressed);
        case State::ItemsReady:            return updateItemList(ir, aPressed);
        case State::PostersReady:          return updatePosterGrid(ir, aPressed);
        case State::SeasonsReady:          return updateSeasons(ir, aPressed);
        case State::EpisodesReady:         return updateEpisodes(ir, aPressed);
        case State::MusicTracksReady:      return updateMusicTracks(ir, aPressed);
        case State::MovieSuggestionsReady: return updateMovieSuggestions(ir, aPressed);
        case State::TVSuggestionsReady:    return updateTVSuggestions(ir, aPressed);
        case State::TVUpcomingReady:       return updateTVUpcoming(ir, aPressed);
        case State::MusicSuggestionsReady: return updateMusicSuggestions(ir, aPressed);
        case State::DetailReady:           return updateDetail(ir, aPressed);
        case State::ResumePrompt:          return updateResumePrompt(ir, aPressed);
        case State::SearchInput:           return updateSearchInput(ir);
        case State::SearchReady:           return updateSearchResults(ir, aPressed);

        case State::Error:
            if (Input::isBackPressed() || (aPressed && (!irMode || ir.valid))) return true;
            return false;

        default:   // PlaylistsReady: playlists open in the item list
            return false;
    }
    return false;
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
        Ui::textRight(EX + EW - 16, EY + 22, "WiiFin v" WIIFIN_VERSION, 11, p.textDim);   /* for bug reports */
        const Ui::Hint h[] = { { "A", "Back" } };
        Ui::footer(h, 1);
        drawCursor(ir);
        return;
    }

    switch (state) {
    case State::LibsReady:              renderHome(ir); break;
    case State::GlobalFavoritesReady:   renderGlobalFavorites(ir); break;
    case State::ItemsReady:             renderItemList(ir); break;
    case State::PostersReady:           renderPosterGrid(ir); break;
    case State::SeasonsReady:           renderSeasons(ir); break;
    case State::EpisodesReady:          renderEpisodes(ir); break;
    case State::MusicTracksReady:       renderMusicTracks(ir); break;
    case State::MovieSuggestionsReady:  renderMovieSuggestions(ir); break;
    case State::TVSuggestionsReady:     renderTVSuggestions(ir); break;
    case State::TVUpcomingReady:        renderTVUpcoming(ir); break;
    case State::MusicSuggestionsReady:  renderMusicSuggestions(ir); break;
    case State::DetailReady:            drawDetailView(ir); break;
    case State::ResumePrompt:           drawDetailView(ir); renderResumePrompt(); break;
    case State::SearchInput:            renderSearchInput(ir); break;
    case State::SearchReady:            renderSearchResults(ir); break;
    default: break;
    }

    drawCursor(ir);
}

