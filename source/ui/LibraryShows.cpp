/* LibraryView: TV series: season and episode lists. */
#include "LibraryView.h"
#include "LibraryDraw.h"
#include "Ui.h"
#include "../input/Input.h"
#include <stdio.h>

using namespace LibDraw;

bool LibraryView::updateSeasons(ir_t& ir, bool aPressed) {
    int n = (int)seasons.size();
    if (Input::isBackPressed()) {
        state = seasonsCallerState;
        return false;
    }
    if (Input::isUpPressed())   { if (seasonSel > 0)   { seasonSel--; clampSeasonScroll(); } irMode = false; }
    if (Input::isDownPressed()) { if (seasonSel < n-1) { seasonSel++; clampSeasonScroll(); } irMode = false; }
    if (irMode && ir.valid && !Input::isUpPressed() && !Input::isDownPressed()) {
        for (int i = 0; i < ITEMS_VISIBLE; i++) {
            int idx = seasonTop + i;
            if (idx >= n) break;
            int ry = LIST_Y + i * ROW_H;
            if (ir.x >= LIST_X && ir.x <= LIST_X + LIST_W &&
                ir.y >= ry && ir.y < ry + ROW_H) {
                seasonSel = idx;
                irMode = true;
            }
        }
    }
    if (Input::is2Pressed() && n > 0) return startShuffle("");   /* whole series */
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
    const Ui::Hint l[] = { { "A", "Select" }, { "2", "Shuffle" }, { "B", "Back" } };
    Ui::footer(l, 3);
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
        std::string labelStr = fitText(font, filterDejaVu(episodes[idx].name, 60), 18,
                                       LIST_X + LIST_W - 16 - tx);
        Ui::text(tx, ry + 11, labelStr.c_str(), 18, Ui::mix(p.text, p.accentDark, f));
    }
    Ui::scrollbar(614, LIST_Y + 2, ITEMS_VISIBLE * ROW_H - 6, episodeTop, ITEMS_VISIBLE, n);
    const Ui::Hint l[] = { { "A", "Details" }, { "2", "Shuffle" }, { "B", "Back" } };
    Ui::footer(l, 3);
}
