/* LibraryView: Library tabs with rows of cards: movie, TV and music suggestions, coming up. */
#include "LibraryView.h"
#include "LibraryDraw.h"
#include "Ui.h"
#include "../input/Input.h"
#include "../core/Utils.h"
#include "../core/Lang.h"

using namespace LibDraw;

// One horizontally scrolling row of poster cards with its end chevrons.
/* The pointer picks the card it is over (it only switched rows before:
 * A then opened the card chosen with the D-pad, the first one by default) */
int LibraryView::suggestionHover(const ir_t& ir, int rowY, int off, int n) const {
    if (!ir.valid || ir.y < rowY || ir.y >= rowY + SG_CH) return -1;
    for (int si = 0; si < SUGG_VISIBLE && off + si < n; si++) {
        int cx = SG_X0 + si * (SG_CW + SG_GAP);
        if (ir.x >= cx && ir.x < cx + SG_CW) return off + si;
    }
    return -1;
}

void LibraryView::drawSuggestionRow(ir_t& ir, int rowY, const std::vector<JellyfinItem>& list,
                                    GRRLIB_texImg* const* texs, int off, int selIdx, bool rowActive,
                                    bool preferSeries, bool showProgress) {
    const Ui::Palette& p = Ui::pal();
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
            titleText(1, cx + 1, rowY + SG_CH + 4, visW, filterDejaVu(nm, 200), 12, sel ? p.text : p.textDim, sel);
        }
    }
    int ay = rowY + SG_CH / 2;
    drawRowChevron(SG_X0 - 9, ay, true, off > 0);
    drawRowChevron(SG_X0 + SUGG_VISIBLE * (SG_CW + SG_GAP) - SG_GAP + 6, ay, false,
                   off + SUGG_VISIBLE < nItems);
}

bool LibraryView::updateMovieSuggestions(ir_t& ir, bool aPressed) {
    if (Input::isBackPressed()) {
        freeMovieSuggestions();
        state = State::LibsReady;
        return false;
    }
    // Tab switch with -/+
    if (Input::isLPressed()) { goToTab((movieTab + 3) % 4); return false; }
    if (Input::isRPressed()) { goToTab((movieTab + 1) % 4); return false; }
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
            if (irMode) {
                int h0 = suggestionHover(ir, SG_ROW0_YH, movieSuggestContOff, (int)movieContItems.size());
                int h1 = suggestionHover(ir, SG_ROW1_YH, movieSuggestRecOff, (int)movieRecentItems.size());
                if (h0 >= 0)      { movieSuggestRow = 0; movieSuggestContSel = h0; }
                else if (h1 >= 0) { movieSuggestRow = 1; movieSuggestRecSel  = h1; }
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

bool LibraryView::updateTVSuggestions(ir_t& ir, bool aPressed) {
    if (Input::isBackPressed()) {
        freeTVSuggestions();
        state = State::LibsReady;
        return false;
    }
    // Tab switch with -/+
    if (Input::isLPressed()) { goToTab((tvTab + 2) % 3); return false; }
    if (Input::isRPressed()) { goToTab((tvTab + 1) % 3); return false; }
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
            if (irMode) {
                int h0 = suggestionHover(ir, SG_ROW0_YH, tvSuggestContOff, (int)tvContItems.size());
                int h1 = suggestionHover(ir, SG_ROW1_YH, tvSuggestRecOff, (int)tvRecentItems.size());
                if (h0 >= 0)      { tvSuggestRow = 0; tvSuggestContSel = h0; }
                else if (h1 >= 0) { tvSuggestRow = 1; tvSuggestRecSel  = h1; }
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

bool LibraryView::updateTVUpcoming(ir_t& ir, bool aPressed) {
    if (Input::isBackPressed()) {
        freeTVUpcoming();
        state = State::LibsReady;
        return false;
    }
    // Tab switch with -/+
    if (Input::isLPressed()) { goToTab((tvTab + 2) % 3); return false; }
    if (Input::isRPressed()) { goToTab((tvTab + 1) % 3); return false; }
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
        if (irMode) {   /* the card under the pointer (row drawn at y 80) */
            int h = suggestionHover(ir, 80, tvUpcomingOff, nu);
            if (h >= 0) tvUpcomingSel = h;
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

bool LibraryView::updateMusicSuggestions(ir_t& ir, bool aPressed) {
    if (Input::isBackPressed()) {
        freeMusicSuggestions();
        state = State::LibsReady;
        return false;
    }
    // Tab switch with -/+
    if (Input::isLPressed()) {
        freeMusicSuggestions();
        musicTab = 0;   /* Albums */
        itemPage = 0;
        currentLibId = musicLibId;
        state = State::ItemsInit;
        return false;
    }
    if (Input::isRPressed()) {
        freeMusicSuggestions();
        musicTab = 2;   /* Artists */
        itemPage = 0;
        currentLibId = musicLibId;
        state = State::ItemsInit;
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
        /* the album under the pointer (grid as renderMusicSuggestions draws
         * it): only the D-pad moved the selection, so A opened the first */
        if (irMode && ir.valid) {
            const int AC = 120, GAP = 18, GRID_X = (640 - (4 * AC + 3 * GAP)) / 2;
            for (int i = 0; i < nr && i < cols * 2; i++) {
                int cx = GRID_X + (i % cols) * (AC + GAP), cy = i < cols ? 74 : 74 + AC + 16 + 18;
                if (ir.x >= cx && ir.x < cx + AC && ir.y >= cy && ir.y < cy + AC) musicSuggestSel = i;
            }
        }

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

// Movie Suggestions (continue watching + recently added)
void LibraryView::renderMovieSuggestions(ir_t& ir) {
    const Ui::Palette& p = Ui::pal();
    drawLibHeader(currentLibName, kMovieTabs, 4, movieTab);
    const int SG_ROW0_Y = 65, SG_ROW1_Y = 270;
    int nc = (int)movieContItems.size();
    int nr = (int)movieRecentItems.size();

    sectionLabel(SG_X0, 50, "IN PROGRESS", nc > 0);
    if (nc > 0)
        drawSuggestionRow(ir, SG_ROW0_Y, movieContItems, movieContTex, movieSuggestContOff,
                              movieSuggestContSel, movieSuggestRow == 0, false, true);
    else
        Ui::text(SG_X0 + 20, SG_ROW0_Y + 60, tr("No movie in progress"), 14, p.textDim);

    sectionLabel(SG_X0, 252, "RECENTLY ADDED", nr > 0);
    if (nr > 0)
        drawSuggestionRow(ir, SG_ROW1_Y, movieRecentItems, movieRecentTex, movieSuggestRecOff,
                              movieSuggestRecSel, movieSuggestRow == 1, false, true);
    else
        Ui::text(SG_X0 + 20, SG_ROW1_Y + 60, tr("No recent movies"), 14, p.textDim);

    const Ui::Hint l[] = { { "A", N_("Detail") }, { "B", N_("Back") } };
    const Ui::Hint r[] = { { "-/+", N_("Tab") } };
    Ui::footer(l, 2, r, 1);
}

// TV Suggestions (continue watching episodes + recently added series)
void LibraryView::renderTVSuggestions(ir_t& ir) {
    const Ui::Palette& p = Ui::pal();
    drawLibHeader(currentLibName, kTvTabs, 3, tvTab);
    const int SG_ROW0_Y = 65, SG_ROW1_Y = 270;
    int nc = (int)tvContItems.size();
    int nr = (int)tvRecentItems.size();

    sectionLabel(SG_X0, 50, "IN PROGRESS", nc > 0);
    if (nc > 0)
        drawSuggestionRow(ir, SG_ROW0_Y, tvContItems, tvContTex, tvSuggestContOff,
                              tvSuggestContSel, tvSuggestRow == 0, true, true);
    else
        Ui::text(SG_X0 + 20, SG_ROW0_Y + 60, tr("No episode in progress"), 14, p.textDim);

    sectionLabel(SG_X0, 252, "RECENT SERIES", nr > 0);
    if (nr > 0)
        drawSuggestionRow(ir, SG_ROW1_Y, tvRecentItems, tvRecentTex, tvSuggestRecOff,
                              tvSuggestRecSel, tvSuggestRow == 1, true, true);
    else
        Ui::text(SG_X0 + 20, SG_ROW1_Y + 60, tr("No recent series"), 14, p.textDim);

    const Ui::Hint l[] = { { "A", N_("Select") }, { "B", N_("Back") } };
    const Ui::Hint r[] = { { "-/+", N_("Tab") } };
    Ui::footer(l, 2, r, 1);
}

// TV Upcoming (unaired episodes)
void LibraryView::renderTVUpcoming(ir_t& ir) {
    const Ui::Palette& p = Ui::pal();
    drawLibHeader(currentLibName, kTvTabs, 3, tvTab);
    int nu = (int)tvUpcomingItems.size();
    if (nu == 0) {
        Ui::card(170, 160, 300, 90, 18, 0.0f);
        Ui::textCentered(320, 182, tr("No content."), 20, p.text);
        Ui::textCentered(320, 214, tr("Make sure metadata download is enabled."), 13, p.textDim);
    } else {
        sectionLabel(SG_X0, 55, "COMING UP", true);
        drawSuggestionRow(ir, 80, tvUpcomingItems, tvUpcomingTex, tvUpcomingOff,
                              tvUpcomingSel, true, true, false);
    }
    const Ui::Hint l[] = { { "A", N_("Detail") }, { "B", N_("Back") } };
    const Ui::Hint r[] = { { "-/+", N_("Tab") } };
    Ui::footer(l, 2, r, 1);
}

// Music Suggestions (recently added albums)
void LibraryView::renderMusicSuggestions(ir_t& ir) {
    const Ui::Palette& p = Ui::pal();
    Ui::tabs(320, 10, kMusicTabs, 5, musicTab, 14);
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
        titleText(1, cx + 1, cardY + AC_CH + 5, visW, filterDejaVu(musicRecentItems[i].name, 200), 12,
                  (sel || hov) ? p.text : p.textDim, sel);
    };

    if (nr == 0) {
        Ui::textCentered(320, 200, tr("No recent albums"), 16, p.textDim);
    } else {
        for (int pass = 0; pass < 2; pass++) {
            for (int i = 0; i < nr && i < COLS * 2; i++) {
                if ((i == musicSuggestSel) != (pass == 1)) continue;
                drawAlbumCard(i % COLS, i < COLS ? ROW0_Y : ROW1_Y, i);
            }
        }
    }
    const Ui::Hint l[] = { { "A", N_("Open") }, { "B", N_("Back") } };
    const Ui::Hint r[] = { { "-/+", N_("Tab") } };
    Ui::footer(l, 2, r, 1);
}
