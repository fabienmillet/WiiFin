/* LibraryView: A library's content: the text list and the poster grid. */
#include "LibraryView.h"
#include "LibraryDraw.h"
#include "Ui.h"
#include "../input/Input.h"
#include <stdio.h>
#include <string.h>
#include <ogc/lwp_watchdog.h>   // gettime(), ticks_to_millisecs()

using namespace LibDraw;

bool LibraryView::updateItemList(ir_t& ir, bool aPressed) {
    // Text list of a whole library (ListFeed): no pages, chunks are
    // fetched in the background as it scrolls.
    int n = feed.total();
    feed.startWorker();
    const int LW = listWidth();
    if (sortPanel) { updateSortPanel(); return false; }
    if (Input::is2Pressed() && sortable()) { openSortPanel(); return false; }

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
    // Left/Right: previous/next letter (sorted by name only)
    const bool letters = !sortable() || listSort.sort == 0;
    if (letters && Input::isLeftPressed())  { feed.requestJump(-1, itemSel); irMode = false; }
    if (letters && Input::isRightPressed()) { feed.requestJump(+1, itemSel); irMode = false; }
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
            musicTab = Input::isLPressed() ? (musicTab + 4) % 5 : (musicTab + 1) % 5;
            itemPage = 0;
            /* every tab but Suggestions is a list of the library (loadItems) */
            state = musicTab == 1 ? State::MusicSuggestionsLoad : State::ItemsInit;
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
                // A track of a flat library (files without albums): it and
                // the tracks around it, in list order, make the queue
                const int BEFORE = 20, COUNT = 220;
                int first = itemSel > BEFORE ? itemSel - BEFORE : 0;
                std::vector<JellyfinItem> range;
                feed.stopWorker();
                runWithLoading([&]() { feed.fetchRange(first, COUNT, range); });
                pendingMusicTracks.clear();
                pendingMusicTrackIdx = 0;
                for (const JellyfinItem& it : range) {
                    if (it.type != "Audio") continue;
                    if (it.id == sel.id) pendingMusicTrackIdx = (int)pendingMusicTracks.size();
                    MusicTrack t;
                    t.id           = it.id;
                    t.title        = it.name;
                    t.runtimeTicks = it.runtimeTicks;
                    pendingMusicTracks.push_back(t);
                }
                if (pendingMusicTracks.empty() || pendingMusicTracks[pendingMusicTrackIdx].id != sel.id) {
                    MusicTrack t;
                    t.id = sel.id; t.title = sel.name; t.runtimeTicks = sel.runtimeTicks;
                    pendingMusicTracks.assign(1, t);
                    pendingMusicTrackIdx = 0;
                }
                pendingPlayIsMusic = true;
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

bool LibraryView::updatePosterGrid(ir_t& ir, bool aPressed) {
    int n = (int)items.size();
    if (sortPanel) { updateSortPanel(); return false; }
    if (Input::is2Pressed() && sortable()) { openSortPanel(); return false; }
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

// Items list (whole library as text)
void LibraryView::renderItemList(ir_t& ir) {
    const Ui::Palette& p = Ui::pal();
    const int n  = feed.total();
    const int LW = listWidth();
    const bool coverPanel = Ui::libraryStyle() == Ui::LibraryStyle::ListCover;

    // Header — music gets a tab bar; everything else gets a breadcrumb
    JellyfinItem selItem;
    bool haveSel = n > 0 && feed.get(itemSel, selItem);
    if (currentLibType == "music" && !inItemsDrilldown) {
        Ui::tabs(320, 10, kMusicTabs, 5, musicTab, 14);
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

    // Footer: the sort in force beside its button
    const std::string sum = sortSummary();
    const Ui::Hint l[] = { { "A", "Open" }, { "B", "Back" }, { "2", sum.empty() ? "Sort" : sum.c_str() } };
    const int nl = sortable() ? 3 : 2;
    if (currentLibType == "music" && !inItemsDrilldown) {
        const Ui::Hint r[] = { { "LR", "A-Z" }, { "-/+", "Tab" } };
        Ui::footer(l, nl, r, 2);
    } else if (sortable() && listSort.sort != 0) {
        const Ui::Hint r[] = { { "-/+", "Page" } };
        Ui::footer(l, nl, r, 1);
    } else {
        const Ui::Hint r[] = { { "LR", "A-Z" }, { "-/+", "Page" } };
        Ui::footer(l, nl, r, 2);
    }
    if (sortPanel) renderSortPanel();
}

void LibraryView::renderPosterGrid(ir_t& ir) {
    const Ui::Palette& p = Ui::pal();
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

    // Footer: selected title centred, page on the right, the sort in force
    const std::string sum = sortSummary();
    const Ui::Hint l[] = { { "B", "Back" }, { "2", sum.empty() ? "Sort" : sum.c_str() } };
    char pageStr[32];
    snprintf(pageStr, sizeof(pageStr), "Page %d / %d", itemPage + 1, totalPages);
    const Ui::Hint r[] = { { "", pageStr } };
    const char* center = (posterSel >= 0 && posterSel < n) ? items[posterSel].name.c_str() : nullptr;
    Ui::footer(l, sortable() ? 2 : 1, r, totalPages > 1 ? 1 : 0, center);
    if (sortPanel) renderSortPanel();
}
