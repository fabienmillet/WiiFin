/* LibraryView: Search: query keyboard and results. */
#include "LibraryView.h"
#include "LibraryDraw.h"
#include "Ui.h"
#include "../input/Input.h"
#include "../core/Text.h"
#include <stdio.h>
#include <ogc/lwp_watchdog.h>   // gettime(), ticks_to_millisecs()

using namespace LibDraw;

bool LibraryView::updateSearchInput(ir_t& ir) {
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

bool LibraryView::updateSearchResults(ir_t& ir, bool aPressed) {
    int n = (int)searchResults.size();
    // B → back to search input
    if (Input::isBackPressed()) {
        state = State::SearchInput;
        return false;
    }
    // [1] → new search
    if (Input::is1Pressed()) {
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
        char sc[32] = "";
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

    const Ui::Hint l[] = { { "A", "Open" }, { "1", "New search" } };
    const Ui::Hint r[] = { { "B", "Back" } };
    Ui::footer(l, 2, r, 1);
}
