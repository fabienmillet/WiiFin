/* LibraryView: TV series: season and episode lists. */
#include "LibraryView.h"
#include "LibraryDraw.h"
#include "Ui.h"
#include "../input/Input.h"
#include <stdio.h>

using namespace LibDraw;

/* Seasons as posters (Posters view): two rows of four, the name under
 * each, centred when there are fewer than four */
static const int SG_COLS = 4, SG_ROWS = 2, SG_W = 112, SG_H = 160;
static const int SG_STRIDE_X = 150, SG_STRIDE_Y = 188, SG_Y0 = 56;
static int seasonGridX0(int n)
{
    int cols = n < SG_COLS ? (n > 0 ? n : 1) : SG_COLS;
    return (640 - ((cols - 1) * SG_STRIDE_X + SG_W)) / 2;
}
/* the List + Cover view: the list narrower, the poster on its right */
static const int SEASON_LW = 384;

bool LibraryView::updateSeasons(ir_t& ir, bool aPressed) {
    int n = (int)seasons.size();
    if (Input::isBackPressed()) {
        freeSeasonPosters();
        state = seasonsCallerState;
        return false;
    }
    if (seasonGrid()) {
        const bool moved = Input::isLeftPressed() || Input::isRightPressed() ||
                           Input::isUpPressed() || Input::isDownPressed();
        if (Input::isLeftPressed()  && seasonSel % SG_COLS > 0) seasonSel--;
        if (Input::isRightPressed() && seasonSel % SG_COLS < SG_COLS - 1 && seasonSel + 1 < n) seasonSel++;
        if (Input::isUpPressed()    && seasonSel >= SG_COLS) seasonSel -= SG_COLS;
        if (Input::isDownPressed()  && seasonSel / SG_COLS < (n - 1) / SG_COLS)
            seasonSel = seasonSel + SG_COLS < n ? seasonSel + SG_COLS : n - 1;   /* a shorter last row */
        if (moved) irMode = false;
        if (irMode && ir.valid && !moved) {
            const int x0 = seasonGridX0(n);
            for (int k = 0; k < SG_COLS * SG_ROWS && seasonTop + k < n; k++) {
                int px = x0 + (k % SG_COLS) * SG_STRIDE_X, py = SG_Y0 + (k / SG_COLS) * SG_STRIDE_Y;
                if (ir.x >= px && ir.x < px + SG_W && ir.y >= py && ir.y < py + SG_H) seasonSel = seasonTop + k;
            }
        }
        /* scroll a row at a time */
        if (seasonSel < seasonTop) seasonTop = seasonSel / SG_COLS * SG_COLS;
        if (seasonSel >= seasonTop + SG_COLS * SG_ROWS) seasonTop = (seasonSel / SG_COLS - (SG_ROWS - 1)) * SG_COLS;
    } else {
        const int lw = Ui::libraryStyle() == Ui::LibraryStyle::ListCover ? SEASON_LW : LIST_W;
        if (Input::isUpPressed())   { if (seasonSel > 0)   { seasonSel--; clampSeasonScroll(); } irMode = false; }
        if (Input::isDownPressed()) { if (seasonSel < n-1) { seasonSel++; clampSeasonScroll(); } irMode = false; }
        if (irMode && ir.valid && !Input::isUpPressed() && !Input::isDownPressed()) {
            for (int i = 0; i < ITEMS_VISIBLE; i++) {
                int idx = seasonTop + i;
                if (idx >= n) break;
                int ry = LIST_Y + i * ROW_H;
                if (ir.x >= LIST_X && ir.x <= LIST_X + lw &&
                    ir.y >= ry && ir.y < ry + ROW_H) {
                    seasonSel = idx;
                    irMode = true;
                }
            }
        }
    }
    if (Input::is2Pressed() && n > 0) return startShuffle("");   /* whole series */
    if (Input::is1Pressed()) toggleFavorite(currentSeriesId, seriesFavorite);
    if (aPressed && n > 0 && seasonSel < n && seasons[seasonSel].id == SPECIALS_ROW) {
        openSpecials(currentSeriesId, currentSeriesName, State::SeasonsReady);
        return false;
    }
    if (aPressed && n > 0 && seasonSel < n) {
        currentSeasonId   = seasons[seasonSel].id;
        currentSeasonName = seasons[seasonSel].name;
        episodes.clear(); episodeSel = 0; episodeTop = 0;
        state = State::EpisodesLoad;
    }
    return false;
}

bool LibraryView::updateEpisodes(ir_t& ir, bool aPressed) {
    int n = (int)episodes.size();
    if (Input::isBackPressed()) {
        if (episodesFromHome) { episodesFromHome = false; state = State::LibsReady; }
        else                  state = State::SeasonsReady;
        return false;
    }
    if (Input::isUpPressed())   { if (episodeSel > 0)   { episodeSel--; clampEpisodeScroll(); } irMode = false; }
    if (Input::isDownPressed()) { if (episodeSel < n-1) { episodeSel++; clampEpisodeScroll(); } irMode = false; }
    if (irMode && ir.valid && !Input::isUpPressed() && !Input::isDownPressed()) {
        for (int i = 0; i < ITEMS_VISIBLE; i++) {
            int idx = episodeTop + i;
            if (idx >= n) break;
            int ry = LIST_Y + i * ROW_H;
            if (ir.x >= LIST_X && ir.x <= LIST_X + LIST_W &&
                ir.y >= ry && ir.y < ry + ROW_H) {
                episodeSel = idx;
                irMode = true;
            }
        }
    }
    if (Input::is2Pressed() && n > 0) return startShuffle(currentSeasonId);
    if (Input::isActionPressed() && n > 0 && episodeSel < n) {   /* +: watched or not */
        togglePlayed(episodes[episodeSel].id, episodes[episodeSel].played);
        if (episodes[episodeSel].played) episodes[episodeSel].playbackPositionTicks = 0;
    }
    if (aPressed && n > 0 && episodeSel < n) {
        detailItemId = episodes[episodeSel].id;
        detailReturnState = State::EpisodesReady;
        state = State::DetailLoad;
    }
    return false;
}

// Season list
void LibraryView::renderSeasons(ir_t& ir) {
    const Ui::Palette& p = Ui::pal();
    drawBreadcrumb(currentSeriesName, 20, seriesFavorite ? 480 : 580);
    if (seriesFavorite) Ui::textRight(620, 16, "\xe2\x99\xa5 Favourite", 16, 0xE8455AFF);
    headerLine(46);

    int n = (int)seasons.size();
    if (seasonGrid()) {
        const int x0 = seasonGridX0(n);
        /* the focused poster last: its halo over its neighbours */
        for (int pass = 0; pass < 2; pass++) {
            for (int k = 0; k < SG_COLS * SG_ROWS && seasonTop + k < n; k++) {
                const int idx = seasonTop + k;
                const bool sel = idx == seasonSel;
                if (sel != (pass == 1)) continue;
                int px = x0 + (k % SG_COLS) * SG_STRIDE_X, py = SG_Y0 + (k / SG_COLS) * SG_STRIDE_Y;
                GRRLIB_texImg* t = seasonPoster(idx);
                drawThumb(t, px, py, SG_W, SG_H, sel ? Ui::pulse() : 0.0f, -1.0f,
                          t ? nullptr : seasons[idx].name.c_str());
                std::string name = fitText(font, filterDejaVu(seasons[idx].name, 45), 14, SG_STRIDE_X - 8);
                Ui::textCentered(px + SG_W / 2, py + SG_H + 6, name.c_str(), 14,
                                 sel ? p.accentDark : p.text);
            }
        }
        const int rows = (n + SG_COLS - 1) / SG_COLS;
        if (rows > SG_ROWS)
            Ui::scrollbar(614, SG_Y0, SG_ROWS * SG_STRIDE_Y - 8, seasonTop / SG_COLS, SG_ROWS, rows);
    } else {
        const bool cover = Ui::libraryStyle() == Ui::LibraryStyle::ListCover;
        const int lw = cover ? SEASON_LW : LIST_W;
        for (int i = 0; i < ITEMS_VISIBLE; i++) {
            int idx = seasonTop + i;
            if (idx >= n) break;
            bool sel   = (idx == seasonSel);
            bool hover = ir.valid &&
                         ir.y >= LIST_Y + i * ROW_H &&
                         ir.y <  LIST_Y + (i + 1) * ROW_H &&
                         ir.x >= LIST_X && ir.x <= LIST_X + lw;
            int ry = LIST_Y + i * ROW_H;
            float f = focusOf(sel, hover);
            drawRow(LIST_X, ry, lw, ROW_H, f);
            std::string name = fitText(font, filterDejaVu(seasons[idx].name, 45), 18, lw - 32);
            Ui::text(LIST_X + 16, ry + 11, name.c_str(), 18, Ui::mix(p.text, p.accentDark, f));
        }
        Ui::scrollbar(LIST_X + lw + 8, LIST_Y + 2, ITEMS_VISIBLE * ROW_H - 6, seasonTop, ITEMS_VISIBLE, n);
        /* the selected season's poster (List + Cover) */
        if (cover && seasonSel >= 0 && seasonSel < n) {
            const float PX = LIST_X + lw + 26, PW = 600 - PX, PH = PW * 1.5f, PY = LIST_Y + 2;
            GRRLIB_texImg* t = seasonPoster(seasonSel);
            drawThumb(t, PX, PY, PW, PH, 0.0f, -1.0f, t ? nullptr : seasons[seasonSel].name.c_str(), false);
            std::string name = fitText(font, filterDejaVu(seasons[seasonSel].name, 45), 15, (int)PW);
            Ui::text(PX, PY + PH + 10, name.c_str(), 15, p.text);
        }
    }
    const Ui::Hint l[] = { { "A", "Select" }, { "2", "Shuffle" }, { "B", "Back" } };
    const Ui::Hint r[] = { { "1", seriesFavorite ? "Unfavourite" : "Favourite" } };
    Ui::footer(l, 3, r, 1);
}

// Episode list
void LibraryView::renderEpisodes(ir_t& ir) {
    const Ui::Palette& p = Ui::pal();
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
            char num[16];
            snprintf(num, sizeof(num), "E%02d", episodes[idx].indexNumber);
            int bw = Ui::textWidth(num, 12) + 14;
            Ui::roundRect(tx, ry + 12, bw, 20, 10, Ui::mix(p.accent, 0xFFFFFFFF, 0.2f), p.accentDark);
            Ui::textCentered(tx + bw / 2, ry + 15, num, 12, p.textOnAccent);
            tx += bw + 10;
        }
        const bool seen = episodes[idx].played;
        std::string labelStr = fitText(font, filterDejaVu(episodes[idx].name, 60), 18,
                                       LIST_X + LIST_W - 16 - tx - (seen ? 34 : 0));
        Ui::text(tx, ry + 11, labelStr.c_str(), 18,
                 seen ? Ui::mix(p.textDim, p.accentDark, f) : Ui::mix(p.text, p.accentDark, f));
        if (seen)   /* watched: a check on the right */
            Ui::textRight(LIST_X + LIST_W - 16, ry + 11, "\xe2\x9c\x93", 18, p.ok);
    }
    Ui::scrollbar(614, LIST_Y + 2, ITEMS_VISIBLE * ROW_H - 6, episodeTop, ITEMS_VISIBLE, n);
    const bool seenSel = episodeSel < n && episodes[episodeSel].played;
    const Ui::Hint l[] = { { "A", "Details" }, { "2", "Shuffle" }, { "B", "Back" } };
    const Ui::Hint r[] = { { "+!", seenSel ? "Unwatched" : "Watched" } };
    Ui::footer(l, 3, r, 1);
}
