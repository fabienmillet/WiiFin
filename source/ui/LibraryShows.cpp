/* LibraryView: TV series: season and episode lists. */
#include "LibraryView.h"
#include "LibraryDraw.h"
#include "Ui.h"
#include "../input/Input.h"
#include "../core/Lang.h"
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
/* the List + Posters view: the list narrower, the poster on its right */
static const int SEASON_LW = 384;

/* the series' page: its poster where A plays it from the start */
static const float SP_X = 40, SP_Y = 62, SP_W = 170, SP_H = 255;
static const float PAGE_H = 420;   /* the overview over the seasons: one page down */

/* the first episode of its first season (not the specials), played; the
 * season's others after it */
bool LibraryView::startSeriesFromStart()
{
    std::string sid;
    for (const JellyfinSeason& se : seasons)
        if (se.id != SPECIALS_ROW && se.indexNumber > 0) { sid = se.id; break; }
    if (sid.empty())
        for (const JellyfinSeason& se : seasons) if (se.id != SPECIALS_ROW) { sid = se.id; break; }
    if (sid.empty()) return false;
    std::vector<JellyfinEpisode> eps;
    bool ok = false;
    std::string err;
    freeDetail();
    runWithLoading([&]() {
        ok = client.getEpisodes(serverUrl, auth, currentSeriesId, sid, eps);
        if (ok && !eps.empty()) ok = client.getItemDetail(serverUrl, auth, eps[0].id, detail);
        if (!ok) err = client.lastError();
    });
    if (!ok) { errMsg = err; state = State::Error; return false; }
    if (eps.empty()) return false;
    SYS_Report("[Series] from the start: %s\n", eps[0].name.c_str());
    detailItemId    = eps[0].id;
    detailIsEpisode = true;
    preparePlay(0LL, &eps);
    return true;
}

bool LibraryView::updateSeasons(ir_t& ir, bool aPressed) {
    int n = (int)seasons.size();
    if (Input::isBackPressed()) {
        freeSeasonPosters();
        state = seasonsCallerState;
        return false;
    }
    seriesScrollK = Ui::approach(seriesScrollK, seriesTop ? 0.0f : 1.0f, 0.22f);
    if (seriesTop) {
        /* the overview: A on the poster plays the series from the start,
         * Down (or the pointer low) to the seasons */
        const bool onPoster = ir.valid && ir.x >= SP_X && ir.x < SP_X + SP_W && ir.y >= SP_Y && ir.y < SP_Y + SP_H;
        if (Input::isDownPressed() || (aPressed && ir.valid && ir.y > PAGE_H - 40)) { seriesTop = false; irMode = false; return false; }
        if (Input::is2Pressed() && n > 0) return startShuffle("");
        if (Input::is1Pressed()) toggleFavorite(currentSeriesId, seriesFavorite);
        if (aPressed && (!ir.valid || onPoster) && n > 0) return startSeriesFromStart();
        return false;
    }
    /* back up to the overview: Up from the first row, or A on the header */
    {
        const bool firstRow = seasonGrid() ? seasonSel < SG_COLS : seasonSel == 0;
        if ((Input::isUpPressed() && firstRow) || (aPressed && ir.valid && ir.y < 46)) {
            seriesTop = true;
            return false;
        }
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

/* the series' page: its poster (A plays it from the start), its name,
 * year, rating and seasons, genres, overview, cast; the seasons below */
void LibraryView::drawSeriesOverview(ir_t& ir)
{
    const Ui::Palette& p = Ui::pal();
    const JellyfinItemDetail& d = seriesDetail;
    /* the poster, focused: a play mark over it */
    const bool hover = ir.valid && ir.x >= SP_X && ir.x < SP_X + SP_W && ir.y >= SP_Y && ir.y < SP_Y + SP_H;
    const float f = seriesTop ? (ir.valid && !hover ? 0.35f : Ui::pulse()) : 0.0f;
    drawThumb(seriesPosterTex, SP_X, SP_Y, SP_W, SP_H, f, -1.0f, seriesPosterTex ? nullptr : currentSeriesName.c_str());
    if (seriesTop) {
        const float cx = SP_X + SP_W / 2, cy = SP_Y + SP_H / 2;
        Ui::circle(cx, cy, 30, Ui::alpha(0x000000FF, 0.55f));
        Ui::triangle(cx - 9, cy - 14, cx - 9, cy + 14, cx + 15, cy, 0xFFFFFFFF);
    }
    /* TRANSLATORS: under the series' poster: A on it plays the first episode */
    Ui::textCentered(SP_X + SP_W / 2, SP_Y + SP_H + 10, tr("Play from the start"), 14,
                     seriesTop ? p.accentDark : p.textDim);
    /* its facts */
    const float X = SP_X + SP_W + 26, W = 600 - X;
    float y = SP_Y - 2;
    for (const std::string& line : wrapText(filterDejaVu(currentSeriesName, 200), 24, W, 2)) {
        Ui::text(X, y, line.c_str(), 24, p.text);
        y += 30;
    }
    {
        std::string meta;
        auto add = [&](const std::string& part) { if (!part.empty()) meta += (meta.empty() ? "" : "  \xc2\xb7  ") + part; };
        if (d.year > 0) add(std::to_string(d.year));
        add(d.officialRating);
        int real = 0;
        for (const JellyfinSeason& se : seasons) real += se.id != SPECIALS_ROW;
        char ns[48];
        /* TRANSLATORS: how many seasons a series has */
        snprintf(ns, sizeof ns, real == 1 ? tr("%d season") : tr("%d seasons"), real);
        if (real > 0) add(ns);
        Ui::text(X, y + 2, fitText(font, meta, 15, (int)W).c_str(), 15, p.textDim);
        y += 24;
    }
    if (!d.genres.empty()) {
        std::string g;
        for (size_t i = 0; i < d.genres.size(); i++) g += (i ? "  \xc2\xb7  " : "") + d.genres[i];
        Ui::text(X, y, fitText(font, g, 14, (int)W).c_str(), 14, p.accentDark);
        y += 22;
    }
    y += 4;
    for (const std::string& line : seriesLines) {
        Ui::text(X, y, line.c_str(), 14, Ui::mix(p.text, p.textDim, 0.2f));
        y += 18;
    }
    /* the cast: its actors, the first few */
    std::string cast;
    int shown = 0;
    for (const JellyfinPerson& pp : d.people) {
        if (pp.role != "Actor" || shown >= 5) continue;
        cast += (shown++ ? ", " : "") + pp.name;
    }
    if (!cast.empty()) {
        y += 8;
        /* TRANSLATORS: before the series' actors' names */
        const std::string with = std::string(tr("With")) + "  ";
        Ui::text(X, y, with.c_str(), 14, p.accentDark);
        Ui::text(X + Ui::textWidth(with.c_str(), 14), y,
                 fitText(font, filterDejaVu(cast, 200), 14, (int)(W - Ui::textWidth(with.c_str(), 14))).c_str(), 14, p.text);
    }
    /* the seasons, below: Down */
    const float cy = PAGE_H - 22;
    /* TRANSLATORS: at the bottom of a series' page: Down shows its seasons */
    const char* down = tr("Seasons");
    const float w = Ui::textWidth(down, 16);
    Ui::text(320 - w / 2 - 10, cy - 9, down, 16, p.accentDark);
    Ui::triangle(320 + w / 2 - 2, cy - 4, 320 + w / 2 + 12, cy - 4, 320 + w / 2 + 5, cy + 4, p.accentDark);
}

// Season list
void LibraryView::renderSeasons(ir_t& ir) {
    drawBreadcrumb(currentSeriesName, 20, seriesFavorite ? 480 : 580);
    if (seriesFavorite) Ui::textRight(620, 16, (std::string("\xe2\x99\xa5 ") + tr("Favorite")).c_str(), 16, 0xE8455AFF);
    headerLine(46);
    /* the overview over the seasons, one sliding under the other */
    const float k = seriesScrollK * seriesScrollK * (3 - 2 * seriesScrollK);
    const bool sliding = k > 0.001f && k < 0.999f;
    if (sliding) Ui::clip(Ui::screenLeft(), 48, Ui::screenWidth(), PAGE_H - 48 + 10);   /* under the header */
    if (k < 0.999f) {
        Ui::pushOffset(0, -k * PAGE_H);
        drawSeriesOverview(ir);
        Ui::popOffset();
    }
    if (k > 0.001f) {
        Ui::pushOffset(0, (1 - k) * PAGE_H);
        drawSeasonsBody(ir);
        Ui::popOffset();
    }
    if (sliding) Ui::clipReset();
    if (seriesTop) {
        const Ui::Hint l[] = { { "A", N_("Play") }, { "2", N_("Shuffle") }, { "B", N_("Back") } };
        const Ui::Hint r[] = { { "UD", N_("Seasons") }, { "1", seriesFavorite ? N_("Unfavorite") : N_("Favorite") } };
        Ui::footer(l, 3, r, 2);
        return;
    }
    const Ui::Hint l[] = { { "A", N_("Select") }, { "2", N_("Shuffle") }, { "B", N_("Back") } };
    const Ui::Hint r[] = { { "1", seriesFavorite ? N_("Unfavorite") : N_("Favorite") } };
    Ui::footer(l, 3, r, 1);
}

/* the seasons themselves: posters, or a list */
void LibraryView::drawSeasonsBody(ir_t& ir) {
    const Ui::Palette& p = Ui::pal();
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
        /* the selected season's poster (List + Posters) */
        if (cover && seasonSel >= 0 && seasonSel < n) {
            GRRLIB_texImg* t = seasonPoster(seasonSel);
            const float PX = LIST_X + lw + 26, PW = 600 - PX, PH = fitHeight(t, PW, PW * 1.5f), PY = LIST_Y + 2;
            drawThumb(t, PX, PY, PW, PH, 0.0f, -1.0f, t ? nullptr : seasons[seasonSel].name.c_str(), false);
            std::string name = fitText(font, filterDejaVu(seasons[seasonSel].name, 45), 15, (int)PW);
            Ui::text(PX, PY + PH + 10, name.c_str(), 15, p.text);
        }
    }
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
            /* TRANSLATORS: E = episode, its number in the season */
            snprintf(num, sizeof(num), tr("E%02d"), episodes[idx].indexNumber);
            int bw = Ui::textWidth(num, 12) + 14;
            Ui::roundRect(tx, ry + 12, bw, 20, 10, Ui::mix(p.accent, 0xFFFFFFFF, 0.2f), p.accentDark);
            Ui::textCentered(tx + bw / 2, ry + 15, num, 12, p.textOnAccent);
            tx += bw + 10;
        }
        const bool seen = episodes[idx].played;
        const int  avail = LIST_X + LIST_W - 16 - tx - (seen ? 34 : 0);
        const u32  col = seen ? Ui::mix(p.textDim, p.accentDark, f) : Ui::mix(p.text, p.accentDark, f);
        titleText(0, tx, ry + 11, avail, filterDejaVu(episodes[idx].name, 200), 18, col, sel);
        if (seen)   /* watched: a check on the right */
            Ui::textRight(LIST_X + LIST_W - 16, ry + 11, "\xe2\x9c\x93", 18, p.ok);
    }
    Ui::scrollbar(614, LIST_Y + 2, ITEMS_VISIBLE * ROW_H - 6, episodeTop, ITEMS_VISIBLE, n);
    const bool seenSel = episodeSel < n && episodes[episodeSel].played;
    const Ui::Hint l[] = { { "A", N_("Details") }, { "2", N_("Shuffle") }, { "B", N_("Back") } };
    const Ui::Hint r[] = { { "+!", seenSel ? N_("Unwatched") : N_("Watched") } };
    Ui::footer(l, 3, r, 1);
}
