#include "BrowseHome.h"
#include "../core/ExitZone.h"
#include "Ui.h"
#include "../core/Text.h"
#include "../core/Utils.h"
#include "../input/Input.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <unistd.h>
#include <ogc/lwp_watchdog.h>

/* WiiFin logo (data/images/logo_wiifin.png), shared by both pages */
extern unsigned char data_logo_wiifin_png[];
static GRRLIB_texImg* s_logo = nullptr;
static int            s_logoUsers = 0;

/* Defined in LibraryView.cpp: decodes a JPEG into an RGBA8 texture. */
GRRLIB_texImg* loadJPEGTexture(const u8* data, u32 size);

/* ---- Layout (640x480; rows, band and header span the whole screen, which
 * is wider in 16:9 — see Ui::screenLeft/Right) ----------------------------- */
static const float BAND_Y     = 48,  BAND_H = 252;   /* focused row panel        */
static const float POSTER_Y   = 80;                  /* top of the focused row   */
static const float POSTER_H   = 146, POSTER_W = 98;  /* 2:3 posters              */
static const float GAP        = 9;
#define X0 (Ui::screenLeft() + 30.0f)              /* first poster of a row */
static const float ROW_STRIDE = 252;                 /* poster top to poster top */
static const float INFO_Y     = 240, INFO_H = 52;
#define INFO_X (Ui::screenLeft() + 28.0f)
#define INFO_W (Ui::screenWidth() - 56.0f)
static const int   MAX_ITEMS  = 20;                  /* per row (home)           */
static const int   CATALOG_ITEMS = 60;               /* per row (browse page)    */

/* Header pills */
static const float PILL_Y = 9, PILL_H = 26;
#define AVATAR_CX (Ui::screenRight() - 24.0f)
static const float AVATAR_CY = 22, AVATAR_R = 14;
static const char* pillLabel(int i, bool catalog)
{
    return i == 0 ? "search" : (catalog ? "home" : "browse");
}

static u32 lerpCol(u32 a, u32 b, float t) { return Ui::mix(a, b, t); }

/* Colours of the carousel home.  Flix keeps the look of the old video
 * channels (grey band, light info panel, white pills on the red header);
 * the other themes take everything from their palette. */
struct Look {
    bool band;                                   /* red header band (Flix) */
    u32  bandTop, bandBottom, bandEdge;          /* focused-row panel      */
    u32  title, titleDim, dimTint;
    u32  phTop, phBottom, phTopDim, phBottomDim, phText, phTextDim;
    u32  selBorder, tileBorder, selShadow, tileShadow, arrow;
    u32  panelTop, panelBottom, panelBorder, panelText, panelMeta;
    u32  starOn, starOff;
    u32  word, wordShadow;
    u32  pillTop, pillBottom, pillBorder, pillFocus, pillText, pillGlow;
    u32  floor;
};

static Look look()
{
    const Ui::Palette& p = Ui::pal();
    Look l;
    if (Ui::theme() == Ui::Theme::Flix) {
        l = Look{ true,
            0x6A6A6AFF, 0x474747FF, 0xFFFFFF30,
            0xFFFFFFFF, 0xA0A0A0FF, 0x6A6A6AFF,
            0x505050FF, 0x3A3A3AFF, 0x2A2A2AFF, 0x202020FF, 0xC8C8C8FF, 0x707070FF,
            0xFFFFFFFF, 0x00000060, 0x00000090, 0x00000070, 0xFFFFFFE0,
            0xF5F5F5FF, 0xC6C6C6FF, 0x8A8A8AFF, 0x262626FF, 0x3E3E3EFF,
            0xD81F26FF, 0xA9A9A9FF,
            0xFFFFFFFF, 0x00000070,
            0xFFFFFFFF, 0xCFCFCFFF, 0x5E5E5EFF, 0xFFFFFFFF, 0x333333FF, 0xFFFFFFB0,
            0x111111FF };
        return l;
    }
    const bool light = Ui::theme() == Ui::Theme::Light;
    l.band        = false;
    l.bandTop     = Ui::alpha(p.cardTop, 0.80f);
    l.bandBottom  = Ui::alpha(p.cardBottom, 0.80f);
    l.bandEdge    = Ui::alpha(p.cardBorder, 0.70f);
    l.title       = p.text;
    l.titleDim    = p.textDim;
    l.dimTint     = light ? 0xC4C4C4FF : 0x6A6A6AFF;
    l.phTop       = p.cardTop;     l.phBottom    = p.cardBottom;
    l.phTopDim    = Ui::mix(p.cardTop, p.bgBottom, 0.5f);
    l.phBottomDim = Ui::mix(p.cardBottom, p.bgBottom, 0.5f);
    l.phText      = p.textDim;     l.phTextDim   = Ui::alpha(p.textDim, 0.6f);
    l.selBorder   = p.accent;      l.tileBorder  = Ui::alpha(p.cardBorder, 0.7f);
    l.selShadow   = p.glow;        l.tileShadow  = p.shadow;
    l.arrow       = p.accent;
    l.panelTop    = p.cardTop;     l.panelBottom = p.cardBottom;  l.panelBorder = p.cardBorder;
    l.panelText   = p.text;        l.panelMeta   = p.textDim;
    l.starOn      = 0xF2B01EFF;    l.starOff     = light ? 0xC5C9CEFF : 0x4A5668FF;
    l.word        = p.text;        l.wordShadow  = 0x00000000;
    l.pillTop     = p.cardTop;     l.pillBottom  = p.cardBottom;  l.pillBorder  = p.cardBorder;
    l.pillFocus   = p.accent;      l.pillText    = p.text;        l.pillGlow    = p.glow;
    l.floor       = p.bgBottom;
    return l;
}

static std::string fit(const std::string& s, int size, int maxW)
{
    if (Ui::textWidth(s.c_str(), size) <= maxW) return s;
    std::string t = s;
    while (!t.empty() && Ui::textWidth((t + "...").c_str(), size) > maxW) {
        while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();
        if (!t.empty()) t.pop_back();
    }
    return t + "...";
}

static float pillWidth(int i, bool catalog) { return Ui::textWidth(pillLabel(i, catalog), 15) + (i == 0 ? 40 : 28); }
static float pillX(int i, bool catalog)
{
    float right = AVATAR_CX - AVATAR_R - 10;
    float w1 = pillWidth(1, catalog), w0 = pillWidth(0, catalog);
    return i == 1 ? right - w1 : right - w1 - 8 - w0;
}

/* ------------------------------------------------------------------------ */

BrowseHome::BrowseHome(JellyfinClient& c, const std::string& url, const JellyfinAuth& a, Mode m)
    : client(c), mode(m), serverUrl(url), auth(a)
{
    LWP_MutexInit(&texLock, false);
    if (s_logoUsers++ == 0) s_logo = GRRLIB_LoadTexture(data_logo_wiifin_png);
}

BrowseHome::~BrowseHome()
{
    stopLoader();
    for (auto& r : rows) freeTextures(r);
    if (texLock != LWP_MUTEX_NULL) LWP_MutexDestroy(texLock);
    if (--s_logoUsers == 0 && s_logo) { GRRLIB_FreeTexture(s_logo); s_logo = nullptr; }
}

void BrowseHome::invalidate()
{
    stopLoader();
    for (auto& r : rows) freeTextures(r);
    rows.clear();
    isBuilt = false;
}

void BrowseHome::freeTextures(Row& r)
{
    LWP_MutexLock(texLock);
    for (size_t i = 0; i < r.tex.size(); ++i) {
        if (r.tex[i]) { GRRLIB_FreeTexture(r.tex[i]); r.tex[i] = nullptr; }
        r.st[i] = TILE_NONE;
    }
    LWP_MutexUnlock(texLock);
}

/* ---- Rows ---------------------------------------------------------------- */

void BrowseHome::build(const std::vector<JellyfinLibrary>& libs)
{
    stopLoader();
    /* keep the selection across rebuilds (matched by row title) */
    std::string keepTitle = rowSel < (int)rows.size() ? rows[rowSel].title : "";
    for (auto& r : rows) freeTextures(r);
    rows.clear();

    if (mode == Mode::Catalog) buildCatalog(libs);
    else                       buildHome(libs);

    rowSel = 0;
    for (int i = 0; i < (int)rows.size(); ++i)
        if (rows[i].title == keepTitle) { rowSel = i; break; }
    rowAnim = (float)rowSel;
    headerFocus = -1;
    isBuilt = true;
}

/* Row from a query; empty rows are dropped. */
#define ADD_ROW(T_, Q_, SQ_, CAP_) do {                                   \
        Row r_;                                                              \
        r_.title  = (T_);                                                    \
        r_.square = (SQ_);                                                   \
        client.getItemsByQuery(serverUrl, auth, (Q_), r_.items);             \
        if (!r_.items.empty()) {                                             \
            if ((int)r_.items.size() > (CAP_)) r_.items.resize(CAP_);        \
            r_.tex.assign(r_.items.size(), nullptr);                         \
            r_.st.assign(r_.items.size(), TILE_NONE);                        \
            rows.push_back(std::move(r_));                                   \
        }                                                                    \
    } while (0)

void BrowseHome::buildHome(const std::vector<JellyfinLibrary>& libs)
{
    const std::string& uid = auth.userId;
    auto add = [&](const std::string& title, const std::string& query, bool square) {
        ADD_ROW(title, query, square, MAX_ITEMS);
    };

    char q[320];
    snprintf(q, sizeof(q), "/Users/%s/Items/Resume?Limit=%d&MediaTypes=Video", uid.c_str(), MAX_ITEMS);
    add("Continue Watching", q, false);
    snprintf(q, sizeof(q), "/Shows/NextUp?UserId=%s&Limit=%d", uid.c_str(), MAX_ITEMS);
    add("Next Up", q, false);

    for (const auto& lib : libs) {
        const std::string& t = lib.collectionType;
        bool music = (t == "music");
        if (!(t == "movies" || t == "tvshows" || music || t == "homevideos" ||
              t == "musicvideos" || t.empty() || t == "mixed"))
            continue;
        snprintf(q, sizeof(q), "/Users/%s/Items/Latest?ParentId=%s&Limit=%d",
                 uid.c_str(), lib.id.c_str(), MAX_ITEMS);
        add("New in " + lib.name, q, music);
        if (t == "movies" || t == "tvshows") {
            snprintf(q, sizeof(q),
                     "/Users/%s/Items?ParentId=%s&Recursive=true&IncludeItemTypes=%s"
                     "&SortBy=CommunityRating,SortName&SortOrder=Descending&Limit=%d",
                     uid.c_str(), lib.id.c_str(), t == "movies" ? "Movie" : "Series", MAX_ITEMS);
            add("Top Rated " + lib.name, q, false);
        }
    }

    snprintf(q, sizeof(q),
             "/Users/%s/Items?Filters=IsFavorite&Recursive=true"
             "&IncludeItemTypes=Movie,Series,Episode&SortBy=SortName&Limit=%d", uid.c_str(), MAX_ITEMS);
    add("My Favorites", q, false);
    snprintf(q, sizeof(q),
             "/Users/%s/Items?Filters=IsFavorite&Recursive=true"
             "&IncludeItemTypes=MusicAlbum&SortBy=SortName&Limit=%d", uid.c_str(), MAX_ITEMS);
    add("Favorite Albums", q, true);
    int favs = 0;
    for (const auto& r : rows) if (r.title == "My Favorites") favs = (int)r.items.size();
    SYS_Report("[Home] %d rows, %d favourite titles\n", (int)rows.size(), favs);
}

/* "browse" page: the libraries themselves, then every title of each library
 * (A-Z) and a few genre rows for films and series. */
void BrowseHome::buildCatalog(const std::vector<JellyfinLibrary>& libs)
{
    const std::string& uid = auth.userId;
    char q[384];

    {
        Row r;
        r.title = "Libraries";
        /* Jellyfin draws each library's picture itself, 16:9 with the name
         * across the middle: shown whole, not cut to a portrait poster (that
         * kept a blurred strip of it, a few letters of the name) */
        r.wide  = true;
        for (const auto& lib : libs) {
            JellyfinItem it;
            it.id   = lib.id;
            it.name = lib.name;
            it.type = "CollectionFolder";
            it.seriesName = lib.collectionType;   /* shown in the info panel */
            r.items.push_back(it);
        }
        if (!r.items.empty()) {
            r.tex.assign(r.items.size(), nullptr);
            r.st.assign(r.items.size(), TILE_NONE);
            rows.push_back(std::move(r));
        }
    }

    for (const auto& lib : libs) {
        const std::string& t = lib.collectionType;
        const char* types = t == "movies"  ? "Movie"
                          : t == "tvshows" ? "Series"
                          : t == "music"   ? "MusicAlbum"
                          : (t == "homevideos" || t == "musicvideos") ? "Video,MusicVideo,Movie"
                          : nullptr;
        if (!types) continue;
        snprintf(q, sizeof(q),
                 "/Users/%s/Items?ParentId=%s&Recursive=true&IncludeItemTypes=%s"
                 "&SortBy=SortName&SortOrder=Ascending&Limit=%d",
                 uid.c_str(), lib.id.c_str(), types, CATALOG_ITEMS);
        ADD_ROW("All " + lib.name, q, t == "music", CATALOG_ITEMS);

        if (t != "movies" && t != "tvshows") continue;
        std::vector<JellyfinItem> genres;
        snprintf(q, sizeof(q), "/Genres?ParentId=%s&UserId=%s&SortBy=SortName&Limit=8",
                 lib.id.c_str(), uid.c_str());
        client.getItemsByQuery(serverUrl, auth, q, genres);
        for (const auto& g : genres) {
            snprintf(q, sizeof(q),
                     "/Users/%s/Items?ParentId=%s&Recursive=true&IncludeItemTypes=%s"
                     "&GenreIds=%s&SortBy=Random&Limit=%d",
                     uid.c_str(), lib.id.c_str(), types, g.id.c_str(), MAX_ITEMS);
            ADD_ROW(g.name + " \xc2\xb7 " + lib.name, q, false, MAX_ITEMS);
        }
    }
}

const JellyfinItem* BrowseHome::selectedItem() const
{
    if (rowSel < 0 || rowSel >= (int)rows.size()) return nullptr;
    const Row& r = rows[rowSel];
    if (r.sel < 0 || r.sel >= (int)r.items.size()) return nullptr;
    return &r.items[r.sel];
}

/* ---- Geometry ------------------------------------------------------------ */

float BrowseHome::tileW(const Row& r) const
{
    return (r.wide ? POSTER_H * 16.0f / 9.0f : r.square ? POSTER_H : POSTER_W) * WiiUtils::wsScaleX();
}

float BrowseHome::stride(const Row& r) const
{
    return tileW(r) + GAP * WiiUtils::wsScaleX();
}

int BrowseHome::fullyVisible(const Row& r) const
{
    int n = (int)((Ui::screenRight() - 28.0f - X0 + GAP) / stride(r));
    return n < 1 ? 1 : n;
}

float BrowseHome::targetScroll(const Row& r) const
{
    int n = (int)r.items.size(), vis = fullyVisible(r);
    int t = r.first;
    if (t > n - vis) t = n - vis;
    if (t < 0) t = 0;
    return (float)t;
}

/* The view only scrolls on purpose: d-pad moves past the edge, or the side
 * arrows.  Hovering a poster selects it without moving anything, otherwise
 * the poster under the pointer would keep changing as the row slides. */
void BrowseHome::followSelection(Row& r)
{
    int n = (int)r.items.size(), vis = fullyVisible(r);
    if (r.sel < r.first + 1)       r.first = r.sel - 1;          /* keep one poster of context */
    if (r.sel > r.first + vis - 2) r.first = r.sel - vis + 2;
    if (r.first > n - vis) r.first = n - vis;
    if (r.first < 0) r.first = 0;
}

void BrowseHome::scrollRow(Row& r, int delta)
{
    int n = (int)r.items.size(), vis = fullyVisible(r);
    r.first += delta;
    if (r.first > n - vis) r.first = n - vis;
    if (r.first < 0) r.first = 0;
    /* keep the selection on a visible poster */
    if (r.sel < r.first) r.sel = r.first;
    if (r.sel > r.first + vis - 1) r.sel = r.first + vis - 1;
    if (r.sel > n - 1) r.sel = n - 1;
}

/* ---- Poster loader --------------------------------------------------------- */

void BrowseHome::startLoader()
{
    if (loaderThread != LWP_THREAD_NULL || rows.empty()) return;
    static u8 stack[64 * 1024] DEAD_AT_EXIT __attribute__((aligned(32)));
    stopReq = false;
    /* Below the main thread: it runs while the UI waits for vsync. */
    if (LWP_CreateThread(&loaderThread, loaderMain, this, stack, sizeof(stack), 40) < 0)
        loaderThread = LWP_THREAD_NULL;
}

void BrowseHome::stopLoader()
{
    if (loaderThread == LWP_THREAD_NULL) return;
    stopReq = true;
    LWP_JoinThread(loaderThread, nullptr);
    loaderThread = LWP_THREAD_NULL;
}

void* BrowseHome::loaderMain(void* self)
{
    static_cast<BrowseHome*>(self)->loaderLoop();
    return nullptr;
}

/* Tiles worth keeping around a row's scroll position (long catalogue rows
 * would not fit in memory otherwise). */
void BrowseHome::tileWindow(const Row& r, int& first, int& last) const
{
    first = (int)r.scroll - 6;
    last  = (int)r.scroll + fullyVisible(r) + 10;
}

/* Next tile to fetch: what is on screen first (focused row, then the one
 * below, then the one above), then the rest of those rows. */
bool BrowseHome::pickNext(int& outR, int& outI)
{
    const int nRows = (int)rows.size();
    const int order[4] = { rowSel, rowSel + 1, rowSel - 1, rowSel + 2 };
    for (int pass = 0; pass < 2; ++pass) {
        for (int o = 0; o < 4; ++o) {
            int r = order[o];
            if (r < 0 || r >= nRows) continue;
            Row& row = rows[r];
            int n = (int)row.items.size();
            int first = (int)row.scroll - 1;
            int last  = (int)row.scroll + fullyVisible(row) + 1;
            int wFirst, wLast;
            tileWindow(row, wFirst, wLast);
            for (int i = 0; i < n; ++i) {
                if (i < wFirst || i > wLast) continue;
                bool onScreen = i >= first && i <= last;
                if (onScreen != (pass == 0)) continue;
                LWP_MutexLock(texLock);
                bool take = row.st[i] == TILE_NONE;
                if (take) row.st[i] = TILE_LOADING;
                LWP_MutexUnlock(texLock);
                if (take) { outR = r; outI = i; return true; }
            }
        }
    }
    return false;
}

void BrowseHome::loaderLoop()
{
    while (!stopReq) {
        int r, i;
        if (!pickNext(r, i)) { usleep(30 * 1000); continue; }
        const JellyfinItem& it = rows[r].items[i];
        /* Episodes: the series poster fits a portrait tile better */
        const std::string& id = (it.type == "Episode" && !it.seriesId.empty()) ? it.seriesId : it.id;
        /* the size drawn: Jellyfin fits the picture inside it, so a smaller
         * or narrower request came back blurred once cropped */
        int h = (int)POSTER_H,
            w = rows[r].wide ? (int)(POSTER_H * 16.0f / 9.0f) : rows[r].square ? h : (int)POSTER_W;
        std::string bytes;
        client.getItemImageBytes(serverUrl, auth, id, w, h, bytes);
        /* seasons without artwork of their own: use the series poster */
        if (bytes.empty() && it.type == "Season" && !it.seriesId.empty())
            client.getItemImageBytes(serverUrl, auth, it.seriesId, w, h, bytes);
        GRRLIB_texImg* t = bytes.empty() ? nullptr
                         : loadJPEGTexture((const u8*)bytes.data(), (u32)bytes.size());
        LWP_MutexLock(texLock);
        if (rows[r].st[i] == TILE_LOADING) {          /* not evicted meanwhile */
            rows[r].tex[i] = t;
            rows[r].st[i]  = t ? TILE_READY : TILE_FAILED;
            t = nullptr;
        }
        LWP_MutexUnlock(texLock);
        if (t) GRRLIB_FreeTexture(t);
    }
}

/* Keep textures for the rows around the focus only (memory: MPlayer needs
 * room when playback starts).  Runs on the main thread between frames, so
 * the GPU is done with the textures it frees. */
void BrowseHome::evictFarRows()
{
    for (int r = 0; r < (int)rows.size(); ++r) {
        Row& row = rows[r];
        bool keepRow = r >= rowSel - 1 && r <= rowSel + 2;
        int first, last;
        tileWindow(row, first, last);
        LWP_MutexLock(texLock);
        for (int i = 0; i < (int)row.st.size(); ++i) {
            if (row.st[i] == TILE_NONE) continue;
            if (keepRow && i >= first - 4 && i <= last + 4) continue;   /* hysteresis */
            if (row.tex[i]) { GRRLIB_FreeTexture(row.tex[i]); row.tex[i] = nullptr; }
            row.st[i] = TILE_NONE;
        }
        LWP_MutexUnlock(texLock);
    }
}

/* ---- Input -------------------------------------------------------------------- */

BrowseHome::Action BrowseHome::update(const ir_t& ir, bool& irMode)
{
    const int nRows = (int)rows.size();
    evictFarRows();

    /* Animation (targets from the previous frame's selection) */
    rowAnim = Ui::approach(rowAnim, (float)rowSel, 0.22f);
    for (auto& r : rows) r.scroll = Ui::approach(r.scroll, targetScroll(r), 0.25f);

    bool up    = Input::isUpPressed(),   down  = Input::isDownPressed();
    bool left  = Input::isLeftPressed(), right = Input::isRightPressed();
    bool a     = Input::isAJustPressed();
    if (up || down || left || right) irMode = false;

    if (Input::is1Pressed()) return Action::Search;
    if (Input::isActionPressed()) return Action::Browse;
    if (mode == Mode::Catalog && Input::isBackPressed()) return Action::Back;

    /* ---- Pointer ---- */
    hoverHeader = -1;
    int  hoverTile = -1;
    bool hoverNextRow = false, hoverUpArrow = false, hoverLeft = false, hoverRight = false;
    if (ir.valid && irMode) {
        for (int i = 0; i < 2; ++i)
            if (ir.x >= pillX(i, mode == Mode::Catalog) && ir.x < pillX(i, mode == Mode::Catalog) + pillWidth(i, mode == Mode::Catalog) && ir.y >= PILL_Y && ir.y < PILL_Y + PILL_H)
                hoverHeader = i;
        if (fabsf(ir.x - AVATAR_CX) < AVATAR_R + 4 && fabsf(ir.y - AVATAR_CY) < AVATAR_R + 4)
            hoverHeader = 2;
        hoverUpArrow = rowSel > 0 && fabsf(ir.x - 320) < 20 && ir.y < 44;
        if (nRows > 0) {
            Row& row = rows[rowSel];
            float tw = tileW(row), st = stride(row);
            if (ir.y >= POSTER_Y && ir.y < POSTER_Y + POSTER_H) {
                hoverLeft  = ir.x < X0 - 4;
                hoverRight = ir.x > Ui::screenRight() - 24.0f;
                for (int i = 0; i < (int)row.items.size(); ++i) {
                    float x = X0 + (i - row.scroll) * st;
                    if (x < X0 - 2 || x + tw > Ui::screenRight() - 24.0f) continue;   /* only whole tiles */
                    if (ir.x >= x && ir.x < x + tw) hoverTile = i;
                }
            }
            hoverNextRow = rowSel + 1 < nRows && ir.y >= POSTER_Y + ROW_STRIDE - 26;
        }
        if (hoverHeader >= 0) headerFocus = hoverHeader;
        else if (ir.y > 44) headerFocus = -1;
        if (hoverTile >= 0) rows[rowSel].sel = hoverTile;
    }

    /* Pointer resting on a side arrow scrolls the row, one poster at a time */
    {
        int arrow = hoverLeft ? -1 : (hoverRight ? 1 : 0);
        unsigned long long now = ticks_to_millisecs(gettime());
        if (arrow != hoverArrow) { hoverArrow = arrow; arrowSince = now; arrowLast = 0; }
        if (arrow && nRows > 0 && headerFocus < 0 && now - arrowSince > 350 && now - arrowLast > 260) {
            arrowLast = now;
            scrollRow(rows[rowSel], arrow);
        }
    }

    /* ---- Header ---- */
    if (headerFocus >= 0) {
        if (left  && headerFocus > 0) headerFocus--;
        if (right && headerFocus < 2) headerFocus++;
        if (down && nRows > 0) headerFocus = -1;
        if (a) {
            int f = headerFocus;
            if (!irMode || hoverHeader >= 0) {
                if (f == 0) return Action::Search;
                if (f == 1) return Action::Browse;
                return Action::Profiles;
            }
        }
        return Action::None;
    }
    if (nRows == 0) {
        if (up) headerFocus = 1;
        return Action::None;
    }

    /* ---- Rows ---- */
    Row& row = rows[rowSel];
    int n = (int)row.items.size();
    if (up) {
        if (rowSel > 0) rowSel--;
        else headerFocus = 1;
    }
    if (down && rowSel + 1 < nRows) rowSel++;
    if (left  && row.sel > 0)     row.sel--;
    if (right && row.sel < n - 1) row.sel++;
    if (left || right) followSelection(row);

    if (a) {
        if (hoverUpArrow)            { rowSel--; return Action::None; }
        if (hoverLeft)               { scrollRow(row, -1); arrowLast = ticks_to_millisecs(gettime()); return Action::None; }
        if (hoverRight)              { scrollRow(row, +1); arrowLast = ticks_to_millisecs(gettime()); return Action::None; }
        if (hoverNextRow && hoverTile < 0) { rowSel++; return Action::None; }
        if (!irMode || hoverTile >= 0) return Action::Open;
    }
    return Action::None;
}

/* ---- Drawing --------------------------------------------------------------- */

/* Five-pointed star centred on cx, cy. */
static void star(float cx, float cy, float rad, u32 col)
{
    float px[10], py[10];
    for (int i = 0; i < 10; ++i) {
        float a = -1.5707963f + i * 0.6283185f;
        float r = (i & 1) ? rad * 0.45f : rad;
        px[i] = cx + cosf(a) * r;
        py[i] = cy + sinf(a) * r;
    }
    for (int i = 0; i < 10; ++i) {
        int j = (i + 1) % 10;
        Ui::triangle(cx, cy, px[i], py[i], px[j], py[j], col);
    }
}

/* 0..5 stars with halves, from a 0..10 community rating. */
static float drawStars(float x, float cy, float rating10, u32 on, u32 off)
{
    const float R = 7, STEP = 17;
    float v = rating10 / 2.0f;
    for (int i = 0; i < 5; ++i) {
        float cx = x + R + i * STEP;
        star(cx, cy, R, off);
        float fill = v - i;
        if (fill >= 0.75f) {
            star(cx, cy, R, on);
        } else if (fill >= 0.25f) {
            Ui::clip(cx - R, cy - R, R, R * 2);
            star(cx, cy, R, on);
            Ui::clipReset();
        }
    }
    return 5 * STEP;
}

void BrowseHome::drawRow(int r, float offY, float focus)
{
    Row& row = rows[r];
    const float ws = WiiUtils::wsScaleX();
    const float tw = tileW(row), st = stride(row);
    const float y = POSTER_Y + offY;
    const bool  focused = focus > 0.5f && headerFocus < 0;
    const Look  L = look();

    /* Row title (bold for the focused row) */
    u32 tc = lerpCol(L.titleDim, L.title, focus);
    Ui::text(X0 - 2, y - 25, row.title.c_str(), 16, tc);
    if (focus > 0.5f) Ui::text(X0 - 1, y - 25, row.title.c_str(), 16, tc);

    u32 tint = lerpCol(L.dimTint, 0xFFFFFFFF, focus);
    int n = (int)row.items.size();
    int first = (int)floorf(row.scroll) - 1;
    for (int pass = 0; pass < 2; ++pass) {           /* selected tile last */
        for (int i = first < 0 ? 0 : first; i < n; ++i) {
            float x = X0 + (i - row.scroll) * st;
            if (x > Ui::screenRight()) break;
            if (x + tw < Ui::screenLeft()) continue;
            bool sel = focused && i == row.sel;
            if (sel != (pass == 1)) continue;

            float dx = x, dy = y, dw = tw, dh = POSTER_H;
            if (sel) {                                 /* pop out a little */
                float g = 5;
                dx -= g * ws * 0.5f; dy -= g * 0.5f; dw += g * ws; dh += g;
                Ui::shadow(dx - 3, dy - 3, dw + 6, dh + 6, 8, 10.0f, L.selShadow);
            } else {
                Ui::shadow(dx + 1, dy + 3, dw - 2, dh - 2, 5, 5.0f, L.tileShadow);
            }
            LWP_MutexLock(texLock);
            GRRLIB_texImg* tex = row.tex[i];
            LWP_MutexUnlock(texLock);
            if (tex) {
                Ui::texCover(tex, dx, dy, dw, dh, 5, (dw / ws) / dh, tint);
            } else {
                Ui::roundRect(dx, dy, dw, dh, 5, lerpCol(L.phTopDim, L.phTop, focus),
                              lerpCol(L.phBottomDim, L.phBottom, focus));
                const JellyfinItem& it = row.items[i];
                const std::string& nm = (it.type == "Episode" || it.type == "Season") && !it.seriesName.empty()
                                        ? it.seriesName : it.name;
                std::string cap = fit(nm, 12, (int)dw - 10);
                Ui::textCentered(dx + dw * 0.5f, dy + dh * 0.5f - 7, cap.c_str(), 12,
                                 lerpCol(L.phTextDim, L.phText, focus));
            }
            const JellyfinItem& it = row.items[i];
            if (it.playbackPositionTicks > 0 && it.runtimeTicks > 0)
                Ui::progress(dx + 5, dy + dh - 9, dw - 10, 4,
                             (float)it.playbackPositionTicks / (float)it.runtimeTicks);
            if (sel) Ui::roundBorder(dx, dy, dw, dh, 5, 3.0f, L.selBorder);
            else     Ui::roundBorder(dx, dy, dw, dh, 5, 1.0f, L.tileBorder);
        }
    }

    /* Scroll arrows (white triangles on the sides) */
    if (focus > 0.5f) {
        float cy = y + POSTER_H * 0.5f;
        auto arrow = [&](bool leftSide) {
            bool hot = hoverArrow == (leftSide ? -1 : 1);
            float g = hot ? 1.35f : 1.0f;           /* grows under the pointer */
            float x = leftSide ? Ui::screenLeft() + 15.0f : Ui::screenRight() - 15.0f, s = leftSide ? -1.0f : 1.0f;
            if (hot) Ui::circle(x, cy, 16, Ui::alpha(L.arrow, 0.25f));
            Ui::triangle(x + s * 6 * g, cy, x - s * 6 * g, cy - 11 * g, x - s * 6 * g, cy + 11 * g, L.arrow);
        };
        if (row.scroll > 0.05f) arrow(true);
        if (row.scroll + fullyVisible(row) < n - 0.05f) arrow(false);
    }
}

void BrowseHome::drawInfoPanel(float k)
{
    const JellyfinItem* itp = selectedItem();
    if (!itp || k <= 0.01f) return;
    const JellyfinItem& it = *itp;
    const Row& row = rows[rowSel];

    /* Pointer tab from the selected poster into the panel */
    float cx = X0 + (row.sel - row.scroll) * stride(row) + tileW(row) * 0.5f;
    if (cx < INFO_X + 20) cx = INFO_X + 20;
    if (cx > INFO_X + INFO_W - 20) cx = INFO_X + INFO_W - 20;

    const Look L = look();
    Ui::shadow(INFO_X, INFO_Y + 2, INFO_W, INFO_H, 8, 6.0f, Ui::alpha(0x00000080, k * (L.band ? 1.0f : 0.5f)));
    Ui::roundRect(INFO_X, INFO_Y, INFO_W, INFO_H, 8, Ui::alpha(L.panelTop, k), Ui::alpha(L.panelBottom, k));
    Ui::roundBorder(INFO_X, INFO_Y, INFO_W, INFO_H, 8, 1.0f, Ui::alpha(L.panelBorder, k));
    Ui::triangle(cx - 10, INFO_Y + 1, cx + 10, INFO_Y + 1, cx, INFO_Y - 9, Ui::alpha(L.panelTop, k));

    /* Title */
    std::string title = it.name;
    if ((it.type == "Episode" || it.type == "Season") && !it.seriesName.empty()) title = it.seriesName;
    if (it.type == "CollectionFolder") title = it.name;
    title = fit(title, 17, (int)INFO_W - 32);
    Ui::text(INFO_X + 16, INFO_Y + 6, title.c_str(), 17, Ui::alpha(L.panelText, k));

    /* Stars, rating chip, details */
    float x = INFO_X + 16, cy = INFO_Y + 38;
    if (it.communityRating > 0.0f) x += drawStars(x, cy, it.communityRating, L.starOn, L.starOff) + 10;
    if (!it.officialRating.empty()) {
        float w = Ui::textWidth(it.officialRating.c_str(), 12) + 12;
        Ui::roundBorder(x, cy - 9, w, 18, 3, 1.5f, Ui::alpha(L.panelMeta, k));
        Ui::textCentered(x + w * 0.5f, cy - 7, it.officialRating.c_str(), 12, Ui::alpha(L.panelMeta, k));
        x += w + 12;
    }
    char meta[160] = "";
    if (it.type == "CollectionFolder") {
        const std::string& t = it.seriesName;
        snprintf(meta, sizeof(meta), "%s library  \xc2\xb7  open it with A",
                 t == "movies" ? "Movie" : t == "tvshows" ? "TV" : t == "music" ? "Music"
                 : t == "boxsets" ? "Collection" : "Media");
    } else if (it.type == "Series") {
        if (it.recursiveItemCount > 0)
            snprintf(meta, sizeof(meta), "%d episode%s", it.recursiveItemCount, it.recursiveItemCount > 1 ? "s" : "");
        else if (it.childCount > 0)
            snprintf(meta, sizeof(meta), "%d season%s", it.childCount, it.childCount > 1 ? "s" : "");
        if (it.year > 0) {
            size_t l = strlen(meta);
            snprintf(meta + l, sizeof(meta) - l, "%s%d", l ? "   \xc2\xb7   " : "", it.year);
        }
    } else if (it.type == "Season") {
        /* e.g. "Season 2  ·  12 episodes  ·  2026" */
        int eps = it.recursiveItemCount > 0 ? it.recursiveItemCount : it.childCount;
        snprintf(meta, sizeof(meta), "%s", it.name.c_str());
        size_t l = strlen(meta);
        if (eps > 0) snprintf(meta + l, sizeof(meta) - l, "   \xc2\xb7   %d episode%s", eps, eps > 1 ? "s" : "");
        l = strlen(meta);
        if (it.year > 0) snprintf(meta + l, sizeof(meta) - l, "   \xc2\xb7   %d", it.year);
    } else if (it.type == "Episode") {
        snprintf(meta, sizeof(meta), "S%d E%d  \xc2\xb7  %s", it.seasonNumber, it.episodeNumber, it.name.c_str());
    } else {
        int mins = (int)(it.runtimeTicks / 600000000LL);
        if (it.year > 0 && mins > 0)
            snprintf(meta, sizeof(meta), "%d   \xc2\xb7   %dh %02dmin", it.year, mins / 60, mins % 60);
        else if (it.year > 0)
            snprintf(meta, sizeof(meta), "%d", it.year);
        else if (mins > 0)
            snprintf(meta, sizeof(meta), "%dh %02dmin", mins / 60, mins % 60);
    }
    if (it.playbackPositionTicks > 0 && it.runtimeTicks > 0) {
        int left = (int)((it.runtimeTicks - it.playbackPositionTicks) / 600000000LL);
        size_t l = strlen(meta);
        if (left > 0) snprintf(meta + l, sizeof(meta) - l, "%s%d min left", l ? "   \xc2\xb7   " : "", left);
    }
    if (meta[0]) {
        std::string m = fit(meta, 14, (int)(INFO_X + INFO_W - 16 - x));
        Ui::text(x, cy - 8, m.c_str(), 14, Ui::alpha(L.panelMeta, k));
    }
}

void BrowseHome::drawHeader()
{
    const Look L = look();
    if (L.band) {
        Ui::drawHeaderBand();
    } else {
        /* no band: a plain strip of background, edge to edge, so rows that
         * scroll up don't show under the header (the row band right below
         * draws its own edge) */
        GRRLIB_Rectangle(Ui::screenLeft(), 0, Ui::screenWidth(), 46, Ui::pal().bgTop, 1);
    }

    /* Logo, top left.  On the red Flix band it sits on a white glossy pill
     * like the search/browse buttons (its grey would vanish on the red). */
    if (s_logo) {
        const float h = 30, w = h * s_logo->w / s_logo->h * WiiUtils::wsScaleX();
        float x = Ui::screenLeft() + 20, y = 7;
        if (L.band) {
            const float px = x - 4, py = y - 1, pw = w + 14, ph = h + 2;
            Ui::shadow(px, py + 2, pw, ph, ph * 0.5f, 4.0f, 0x00000070);
            Ui::roundRect(px, py, pw, ph, ph * 0.5f, 0xFFFFFFFF, 0xDCDCDCFF);
            Ui::roundBorder(px, py, pw, ph, ph * 0.5f, 1.5f, 0x5E5E5EFF);
            x += 3;
        }
        GRRLIB_DrawImg(x, y, s_logo, 0, w / s_logo->w, h / s_logo->h, 0xFFFFFFFF);
    }

    /* "More rows above" arrow */
    if (rowSel > 0)
        Ui::triangle(320, 13, 332, 29, 308, 29, L.band ? 0xFFFFFFE8 : L.arrow);

    /* search / browse pills */
    for (int i = 0; i < 2; ++i) {
        float x = pillX(i, mode == Mode::Catalog), w = pillWidth(i, mode == Mode::Catalog);
        bool f = headerFocus == i;
        if (f) Ui::shadow(x - 3, PILL_Y - 3, w + 6, PILL_H + 6, PILL_H * 0.5f + 3, 8.0f, L.pillGlow);
        else   Ui::shadow(x, PILL_Y + 2, w, PILL_H, PILL_H * 0.5f, 4.0f, L.tileShadow);
        Ui::roundRect(x, PILL_Y, w, PILL_H, PILL_H * 0.5f, L.pillTop, L.pillBottom);
        Ui::roundBorder(x, PILL_Y, w, PILL_H, PILL_H * 0.5f, f ? 2.0f : 1.5f, f ? L.pillFocus : L.pillBorder);
        float tx = x + w * 0.5f;
        if (i == 0) {
            /* magnifier */
            float mx = x + 15, my = PILL_Y + 12;
            Ui::roundBorder(mx - 5, my - 5, 10, 10, 5, 2.0f, L.pillText);
            Ui::triangle(mx + 3, my + 4, mx + 5, my + 2, mx + 9, my + 8, L.pillText);
            Ui::triangle(mx + 3, my + 4, mx + 9, my + 8, mx + 7, my + 10, L.pillText);
            tx += 7;
        }
        Ui::textCentered(tx, PILL_Y + 4, pillLabel(i, mode == Mode::Catalog), 15, L.pillText);
    }

    /* profile avatar */
    {
        bool f = headerFocus == 2;
        Ui::avatar(AVATAR_CX, AVATAR_CY, f ? AVATAR_R + 1 : AVATAR_R, userName.c_str(), f ? 1.0f : 0.0f);
    }
}

void BrowseHome::render(const ir_t& ir)
{
    (void)ir;
    Ui::background();

    /* Focused-row band */
    const Look L = look();
    Ui::roundRect(Ui::screenLeft() - 20, BAND_Y, Ui::screenWidth() + 40, BAND_H, 16, L.bandTop, L.bandBottom);
    Ui::roundRect(Ui::screenLeft() - 20, BAND_Y, Ui::screenWidth() + 40, 1.5f, 0.75f, L.bandEdge);

    if (rows.empty()) {
        Ui::textCentered(320, 150, "Nothing to show yet", 20, L.title);
        Ui::textCentered(320, 180, "Use \"browse\" to open your libraries.", 14, L.titleDim);
    }

    /* Rows around the focus, sliding with rowAnim */
    for (int r = rowSel - 2; r <= rowSel + 2; ++r) {
        if (r < 0 || r >= (int)rows.size()) continue;
        float d = r - rowAnim;
        float off = d * ROW_STRIDE;
        if (off < -ROW_STRIDE - 40 || off > 480 - POSTER_Y + 30) continue;
        float focus = 1.0f - fabsf(d);
        if (focus < 0) focus = 0;
        drawRow(r, off, focus);
    }

    /* Info panel once the rows have settled */
    float settle = 1.0f - fabsf(rowAnim - rowSel) * 4.0f;
    if (headerFocus < 0) drawInfoPanel(settle < 0 ? 0 : settle);

    /* Fade the peeking row into the floor */
    {
        const u32 c = L.floor;
        GRRLIB_Rectangle(Ui::screenLeft(), 470, Ui::screenWidth(), 10, c, 1);
        for (int i = 0; i < 16; ++i) {
            float y = 386 + i * 5.25f;
            GRRLIB_Rectangle(Ui::screenLeft(), y, Ui::screenWidth(), 5.25f, Ui::alpha(c, (i + 1) / 17.0f), 1);
        }
    }

    /* Button hints along the bottom */
    {
        const Ui::Hint hHome[] = { { "A", "Open" }, { "1", "Search" }, { "+!", "Browse" } };
        const Ui::Hint hCat[]  = { { "A", "Open" }, { "1", "Search" }, { "B", "Home" } };
        const Ui::Hint* h = mode == Mode::Catalog ? hCat : hHome;
        float w = 0;
        for (int i = 0; i < 3; ++i) w += Ui::hintWidth(h[i]);
        float x = Ui::screenRight() - 14 - w;
        for (int i = 0; i < 3; ++i) x += Ui::hint(x, 454, h[i]);
    }

    drawHeader();
}
