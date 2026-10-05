/* LibraryView: Home: the libraries grid, the Activity and Favorites tabs, the carousel home. */
#include "LibraryView.h"
#include "LibraryDraw.h"
#include "Ui.h"
#include "../input/Input.h"
#include "../core/SoundFX.h"
#include <cmath>

using namespace LibDraw;

bool LibraryView::updateHome(ir_t& ir, bool aPressed) {
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

        // [1] (Classic / GameCube: Y) → open search
        if (Input::is1Pressed()) {
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
        if (irMode && ir.valid && !Input::isUpPressed() && !Input::isDownPressed()
                     && !Input::isLeftPressed() && !Input::isRightPressed()) {
            for (int i = 0; i < nc; i++) {
                int cx = ACT_X0 + i * (ACT_CARD_W + ACT_CARD_GAP);
                if (ir.x >= cx && ir.x < cx + ACT_CARD_W &&
                    ir.y >= ACT_ROW0_Y && ir.y < ACT_ROW0_Y + ACT_CARD_H) {
                    continueSel = i; actRow = 0; irMode = true;
                }
            }
            for (int i = 0; i < nu; i++) {
                int cx = ACT_X0 + i * (ACT_CARD_W + ACT_CARD_GAP);
                if (ir.x >= cx && ir.x < cx + ACT_CARD_W &&
                    ir.y >= ACT_ROW1_Y && ir.y < ACT_ROW1_Y + ACT_CARD_H) {
                    nextUpSel = i; actRow = 1; irMode = true;
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

bool LibraryView::updateGlobalFavorites(ir_t& ir, bool aPressed) {
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

// Home: the carousel (Flix theme), else the library grid / Activity
void LibraryView::renderHome(ir_t& ir) {
    if (flixHome() && (browsePage ? catalog : browse).built()) {
        (browsePage ? catalog : browse).render(ir);
        return;
    }

    const Ui::Palette& p = Ui::pal();
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
            Ui::text(ACT_X0 + 20, ACT_ROW0_Y + 40, "Nothing in progress", 14, p.textDim);
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

// Global Favourites poster grid
void LibraryView::renderGlobalFavorites(ir_t& ir) {
    float tabX0 = Ui::tabs(320, 10, kHomeTabs, 3, 2, 16);
    std::string srv = fitText(font, filterDejaVu(auth.serverName, 40), 14, (int)tabX0 - 30);
    Ui::text(20, 18, srv.c_str(), 14, headerDim());
    headerLine(51);

    // Count top-right
    if (itemTotal > 0) drawCount(itemPage, POSTERS_PER_PAGE, (int)items.size(), itemTotal);
    else               Ui::textRight(620, 18, "No favorites", 15, headerDim());

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
