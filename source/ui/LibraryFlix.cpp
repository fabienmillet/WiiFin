/* LibraryView: the Flix theme's pages, laid out like the old Wii video
 * channels: a season's episodes as a row of numbered tiles with the
 * selected one's card below, and a title's page with its picture, a big
 * Play button and its facts under it.  The other themes keep the lists
 * and pages of LibraryShows.cpp and LibraryDetail.cpp. */
#include "LibraryView.h"
#include "LibraryDraw.h"
#include "Ui.h"
#include "../input/Input.h"
#include "../core/Text.h"
#include "../core/Lang.h"
#include "../core/Utils.h"
#include <algorithm>
#include <math.h>
#include <stdio.h>

using namespace LibDraw;

/* the chosen theme: December's Christmas colours keep the layout */
bool LibraryView::flixLayout() { return Ui::chosenTheme() == Ui::Theme::Flix; }

/* chips: "TV-PG" in a box, then the year, the running time, "watched" */
static float drawFlixChips(float x, float y, const std::string& rating, int year, long long runtimeTicks,
                           bool played)
{
    const Ui::Palette& p = Ui::pal();
    if (!rating.empty()) {
        const float w = Ui::textWidth(rating.c_str(), 13) + 12;
        Ui::roundBorder(x, y, w, 20, 3, 1.5f, p.textDim);
        Ui::textCentered(x + w / 2, y + 3, rating.c_str(), 13, p.text);
        x += w + 14;
    }
    auto plain = [&](const std::string& s, u32 c) {
        Ui::text(x, y + 2, s.c_str(), 15, c);
        x += Ui::textWidth(s.c_str(), 15) + 18;
    };
    if (year > 0) plain(std::to_string(year), p.text);
    if (runtimeTicks > 0) {
        const int secs = (int)(runtimeTicks / 10000000LL);
        char buf[24];
        if (secs >= 60) snprintf(buf, sizeof buf, "%s", Lang::runTime(secs / 60).c_str());
        else            snprintf(buf, sizeof buf, tr("%ds"), secs);
        plain(buf, p.text);
    }
    if (played) plain(std::string("\xe2\x9c\x93 ") + tr("Watched"), p.ok);
    return x;
}

// ---------------------------------------------------------------
// A season's episodes: numbered tiles, the card of the selected one
// ---------------------------------------------------------------
static const float EP_TW = 70, EP_TH = 64, EP_STRIDE = 84, EP_Y = 84;
static const float EP_CARD_X = 24, EP_CARD_W = 592, EP_CARD_Y = 176, EP_CARD_H = 260;   /* to 436 */
static const float EP_EDGE = 52;   /* room for the arrows at both ends */

/* tile i at base + i * EP_STRIDE - scroll; scroll in sMin..sMax keeps the
 * row against both ends (centred when it all fits) */
void LibraryView::flixEpGeometry(float& base, float& sMin, float& sMax) const
{
    const int n = (int)episodes.size();
    const float left = Ui::screenLeft() + EP_EDGE, right = Ui::screenRight() - EP_EDGE;
    base = 320 - EP_TW / 2;
    sMin = base - left;
    sMax = base + (n - 1) * EP_STRIDE + EP_TW - right;
    if (sMax < sMin) sMin = sMax = (sMin + sMax) * 0.5f;
}

/* the selected tile in the middle, as far as the ends allow */
float LibraryView::flixEpTarget() const
{
    float base, sMin, sMax;
    flixEpGeometry(base, sMin, sMax);
    const float t = episodeSel * EP_STRIDE;
    return t < sMin ? sMin : t > sMax ? sMax : t;
}

bool LibraryView::updateEpisodesFlix(ir_t& ir, bool aPressed)
{
    const int n = (int)episodes.size();
    if (Input::isBackPressed()) {
        if (episodesFromHome) { episodesFromHome = false; state = State::LibsReady; }
        else                  state = State::SeasonsReady;
        return false;
    }
    float base, sMin, sMax;
    flixEpGeometry(base, sMin, sMax);
    const int page = (int)((Ui::screenWidth() - 2 * EP_EDGE) / EP_STRIDE);
    const bool prev = Input::isLeftPressed() || Input::isUpPressed();
    const bool next = Input::isRightPressed() || Input::isDownPressed();
    if (prev && episodeSel > 0)     episodeSel--;
    if (next && episodeSel < n - 1) episodeSel++;
    if (prev || next) irMode = false;
    /* the pointer: on a tile selects it, A on an arrow turns a page */
    int arrow = 0;
    if (ir.valid) {
        const float cy = EP_Y + EP_TH / 2;
        if (fabsf(ir.y - cy) < 26) {
            if (fabsf(ir.x - (Ui::screenLeft() + 26)) < 24 && flixEpScroll > sMin + 1)  arrow = -1;
            if (fabsf(ir.x - (Ui::screenRight() - 26)) < 24 && flixEpScroll < sMax - 1) arrow = 1;
        }
        if (irMode && !prev && !next && !arrow)
            for (int i = 0; i < n; i++) {
                const float x = base + i * EP_STRIDE - flixEpScroll;
                if (ir.x >= x && ir.x < x + EP_TW && ir.y >= EP_Y && ir.y < EP_Y + EP_TH) episodeSel = i;
            }
    }
    if (aPressed && arrow) {
        episodeSel += arrow * (page > 1 ? page : 1);
        clampEpisodeScroll();
        return false;
    }
    clampEpisodeScroll();   /* the list's own top too, should the theme change */
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

void LibraryView::renderEpisodesFlix(ir_t& ir)
{
    const Ui::Palette& p = Ui::pal();
    drawBreadcrumb(currentSeriesName + "  /  " + currentSeasonName, 18, 580);
    headerLine(46);
    const int n = (int)episodes.size();
    float base, sMin, sMax;
    flixEpGeometry(base, sMin, sMax);
    const float target = flixEpTarget();
    flixEpScroll = flixEpScrollSet ? Ui::approach(flixEpScroll, target, 0.25f) : target;
    flixEpScrollSet = true;

    /* the tiles, between the arrows; the selected one last (its halo on top) */
    const float left = Ui::screenLeft() + EP_EDGE - 8, right = Ui::screenRight() - EP_EDGE + 8;
    Ui::clip(left, EP_Y - 16, right - left, EP_TH + 32);
    for (int pass = 0; pass < 2; pass++) {
        for (int i = 0; i < n; i++) {
            const bool sel = i == episodeSel;
            if (sel != (pass == 1)) continue;
            const float x = base + i * EP_STRIDE - flixEpScroll;
            if (x + EP_TW < left || x > right) continue;
            const bool hover = ir.valid && ir.x >= x && ir.x < x + EP_TW && ir.y >= EP_Y && ir.y < EP_Y + EP_TH;
            Ui::card(x, EP_Y, EP_TW, EP_TH, 8, sel ? Ui::pulse() : hover ? 0.45f : 0.0f);
            char num[12];
            snprintf(num, sizeof num, "%d", episodes[i].indexNumber > 0 ? episodes[i].indexNumber : i + 1);
            Ui::textCentered(x + EP_TW / 2, EP_Y + 9, num, 28, sel ? p.text : Ui::mix(p.text, p.textDim, 0.35f));
            /* how much of it is watched: the red bar under the number */
            float frac = episodes[i].played ? 1.0f : progressOf(episodes[i]);
            const float bx = x + 9, by = EP_Y + EP_TH - 15, bw = EP_TW - 18, bh = 7;
            Ui::roundRect(bx, by, bw, bh, 2, 0x00000080);
            if (frac > 0) Ui::roundRect(bx, by, bw * frac < bh ? bh : bw * frac, bh, 2, 0xE52A30FF, 0xB0141AFF);
        }
    }
    Ui::clipReset();
    {
        const float cy = EP_Y + EP_TH / 2;
        const float lx = Ui::screenLeft() + 26, rx = Ui::screenRight() - 26;
        const bool canL = flixEpScroll > sMin + 1, canR = flixEpScroll < sMax - 1;
        Ui::pageArrow(lx, cy, true,  canL, canL && ir.valid && fabsf(ir.x - lx) < 24 && fabsf(ir.y - cy) < 26);
        Ui::pageArrow(rx, cy, false, canR, canR && ir.valid && fabsf(ir.x - rx) < 24 && fabsf(ir.y - cy) < 26);
    }

    /* the selected episode's card, pointing at its tile */
    if (n > 0 && episodeSel < n) {
        const JellyfinEpisode& e = episodes[episodeSel];
        Ui::card(EP_CARD_X, EP_CARD_Y, EP_CARD_W, EP_CARD_H, 14, 0.0f);
        float cx = base + episodeSel * EP_STRIDE - flixEpScroll + EP_TW / 2;
        if (cx < EP_CARD_X + 24) cx = EP_CARD_X + 24;
        if (cx > EP_CARD_X + EP_CARD_W - 24) cx = EP_CARD_X + EP_CARD_W - 24;
        Ui::triangle(cx - 13, EP_CARD_Y + 2, cx, EP_CARD_Y - 11, cx + 13, EP_CARD_Y + 2, p.cardTop);

        const float X = EP_CARD_X + 20, W = EP_CARD_W - 40;
        float y = EP_CARD_Y + 16;
        char pos[48];
        /* TRANSLATORS: the Flix theme's episode card: its place in the season */
        snprintf(pos, sizeof pos, tr("episode %d / %d"), episodeSel + 1, n);
        const float pw = Ui::textWidth(pos, 14);
        Ui::textRight(X + W, y + 3, pos, 14, p.textDim);
        std::string head;
        if (e.indexNumber > 0) {
            char se[32];
            /* TRANSLATORS: season and episode numbers before an episode's name (S1:E9) */
            snprintf(se, sizeof se, tr("S%d:E%d"), e.seasonNumber, e.indexNumber);
            head = std::string(se) + " - ";
        }
        head += filterDejaVu(e.name, 200);
        titleText(2, X, y, W - pw - 16, head, 18, p.text, true);
        y += 30;
        if (!e.officialRating.empty() || e.year > 0 || e.runtimeTicks > 0 || e.played) {
            drawFlixChips(X, y, e.officialRating, e.year, e.runtimeTicks, e.played);
            y += 28;
        }
        if (!e.played && e.playbackPositionTicks > 0 && e.runtimeTicks > 0) {
            const int secs = (int)(e.playbackPositionTicks / 10000000LL);
            const int h = secs / 3600, m = (secs % 3600) / 60;
            const int pct = (int)(e.playbackPositionTicks * 100LL / e.runtimeTicks);
            char buf[48];
            if (h > 0) snprintf(buf, sizeof buf, tr("Resume at %dh%02d (%d%%)"), h, m, pct);
            else       snprintf(buf, sizeof buf, tr("Resume at %dmin (%d%%)"), m, pct);
            Ui::triangle(X, y + 3, X + 9, y + 8, X, y + 13, p.accentDark);
            Ui::text(X + 14, y, buf, 14, p.accentDark);
            y += 22;
        }
        /* its overview, wrapped once per selection */
        const int lines = (int)((EP_CARD_Y + EP_CARD_H - 14 - y) / 19);
        if (flixEpLinesFor != episodeSel) {
            flixEpLines    = wrapText(e.overview, 15, W, lines > 0 ? lines : 1);
            flixEpLinesFor = episodeSel;
        }
        for (const std::string& line : flixEpLines) {
            Ui::text(X, y, line.c_str(), 15, Ui::mix(p.text, p.textDim, 0.2f));
            y += 19;
        }
    }

    const bool seenSel = episodeSel < n && episodes[episodeSel].played;
    const Ui::Hint l[] = { { "A", N_("Details") }, { "2", N_("Shuffle") }, { "B", N_("Back") } };
    const Ui::Hint r[] = { { "LR", N_("Select") }, { "+!", seenSel ? N_("Unwatched") : N_("Watched") } };
    Ui::footer(l, 3, r, 2);
}

// ---------------------------------------------------------------
// A title's page: picture, Play and Favorite buttons, facts, overview
// ---------------------------------------------------------------
void LibraryView::drawDetailFlix(ir_t& ir)
{
    const Ui::Palette& p = Ui::pal();
    const float X0 = 24, RIGHT = 616, TITLE_Y = 54, PY = 90;
    const bool wide = detailWideThumb();
    const float PW = wide ? 192 : 130, PH = wide ? 108 : 195;
    const float visW = PW * WiiUtils::wsScaleX();

    /* the name, a heart after it when a favourite */
    {
        const std::string title = filterDejaVu(detail.name, 200);
        const float w = RIGHT - X0 - (detail.isFavorite ? 30 : 0);
        titleText(2, X0, TITLE_Y, w, title, 22, p.text, true);
        if (detail.isFavorite) {
            const float tw = Text::width(font, title.c_str(), 22);
            Text::print(X0 + (tw < w ? tw : w) + 8, TITLE_Y, font, "\xe2\x99\xa5", 22, 0xE8455AFF);
        }
    }

    /* the picture: A on it plays too */
    const bool posterHover = ir.valid && ir.x >= X0 && ir.x < X0 + visW && ir.y >= PY && ir.y < PY + PH;
    drawThumb(detailTex, X0, PY, PW, PH, posterHover ? 0.8f : 0.0f, progressOf(detail), detail.name.c_str(), false);

    /* the buttons beside it: Play (Resume), the favourite */
    flixBtnX = X0 + visW + 20;
    flixBtnW = 230;
    if (flixBtnX + flixBtnW > RIGHT) flixBtnW = RIGHT - flixBtnX;
    flixPlayY = PY;
    flixFavY  = PY + FLIX_PLAY_H + 10;
    {
        const float f = flixBtn == 0 ? Ui::pulse() : 0.0f;
        const float x = flixBtnX, y = flixPlayY, w = flixBtnW, h = FLIX_PLAY_H;
        if (f > 0) Ui::shadow(x, y, w, h, 8, 10.0f, Ui::alpha(p.glow, f));
        Ui::roundRect(x, y, w, h, 8, Ui::mix(p.accent, 0xFFFFFFFF, 0.22f + 0.12f * f), Ui::mix(p.accent, 0x000000FF, 0.15f));
        if (f > 0) Ui::roundBorder(x, y, w, h, 8, 2.0f, Ui::alpha(0xFFFFFFFF, 0.9f * f));
        /* TRANSLATORS: the Flix theme's big button when the video has a resume point */
        const char* label = detail.playbackPositionTicks > 0 ? tr("Resume") : tr("Play");
        const float tw = Ui::textWidth(label, 19);
        const float cx = x + w / 2, ty = y + (h - 19) / 2 - 1;
        Ui::triangle(cx - tw / 2 - 18, ty + 3, cx - tw / 2 - 18, ty + 17, cx - tw / 2 - 6, ty + 10, 0xFFFFFFFF);
        Ui::text(cx - tw / 2 + 4, ty, label, 19, 0xFFFFFFFF);
    }
    Ui::button(flixBtnX, flixFavY, flixBtnW, FLIX_FAV_H,
               detail.isFavorite ? N_("Unfavorite") : N_("Favorite"), 15, flixBtn == 1 ? Ui::pulse() : 0.0f);

    /* the facts under the buttons */
    float y = flixFavY + FLIX_FAV_H + 14;
    const float CX = flixBtnX, CW = RIGHT - CX;
    if (!detail.officialRating.empty() || detail.year > 0 || detail.runtimeTicks > 0 || detail.played) {
        drawFlixChips(CX, y, detail.officialRating, detail.year, detail.runtimeTicks, detail.played);
        y += 28;
    }
    if (detail.playbackPositionTicks > 0 && detail.runtimeTicks > 0) {
        const int secs = (int)(detail.playbackPositionTicks / 10000000LL);
        const int h = secs / 3600, m = (secs % 3600) / 60;
        const int pct = (int)(detail.playbackPositionTicks * 100LL / detail.runtimeTicks);
        char buf[48];
        if (h > 0) snprintf(buf, sizeof buf, tr("Resume at %dh%02d (%d%%)"), h, m, pct);
        else       snprintf(buf, sizeof buf, tr("Resume at %dmin (%d%%)"), m, pct);
        Ui::triangle(CX, y + 3, CX + 9, y + 8, CX, y + 13, p.accentDark);
        Ui::text(CX + 14, y, buf, 14, p.accentDark);
        y += 22;
    }

    /* the rows at the bottom (Version, Audio, Subtitles, Scenes): the cast
     * and genres just above them, the overview between */
    const int ROW_H = 24;
    const bool hasVersions = !detail.versions.empty(), hasScenes = !detail.chapters.empty();
    const bool hasRows = hasVersions || hasScenes || !detail.audioStreams.empty() || !detail.subtitleStreams.empty();
    const float rowsTop = hasRows ? 390 - (hasVersions ? ROW_H : 0) - (hasScenes ? ROW_H : 0) - 8 : 438;
    std::string cast;
    std::vector<std::string> named;   /* each name once (a director who wrote it too) */
    for (const JellyfinPerson& pp : detail.people) {
        if (pp.name.empty() || std::find(named.begin(), named.end(), pp.name) != named.end()) continue;
        named.push_back(pp.name);
        cast += (cast.empty() ? "" : "  \xc2\xb7  ") + pp.name;
    }
    std::string genres;
    for (const std::string& g : detail.genres) genres += (genres.empty() ? "" : "  \xc2\xb7  ") + g;
    const float castY   = rowsTop - 8 - (!cast.empty() ? 19 : 0) - (!genres.empty() ? 19 : 0);
    const float bottomY = castY > PY + PH + 6 ? castY : PY + PH + 6;
    if (!detail.overview.empty()) {
        /* beside the picture, then the whole width under it when it ends first */
        y += 2;
        const int lines = (int)((castY - 6 - y) / 17);
        if (flixOvW != CW || flixOvN != lines) {
            flixOvLines.clear();
            if (lines > 0) flixOvLines = wrapText(detail.overview, 13, CW, lines);
            overviewCut = lines <= 0 ||
                          (int)wrapText(detail.overview, 13, CW, (lines > 0 ? lines : 0) + 1).size() > lines;
            flixOvW = CW;
            flixOvN = lines;
        }
        detailOvX0 = CX;
        detailOvY0 = y;
        const bool hover = overviewCut && ir.valid && ir.x >= CX && ir.x < RIGHT && ir.y >= detailOvY0 && ir.y < detailOvY1;
        for (const std::string& line : flixOvLines) {
            Ui::text(CX, y, line.c_str(), 13, hover ? p.accentDark : Ui::mix(p.text, p.textDim, 0.25f));
            y += 17;
        }
        detailOvY1 = y;
    }
    float ly = bottomY;
    if (overviewCut) {   /* the rest of it: - (or A on the text) */
        const Ui::Hint more = { "-", N_("Read more") };
        Ui::hint(RIGHT - Ui::hintWidth(more) + 14, ly - 18 > y ? ly - 18 : y, more);
    }
    if (!cast.empty()) {
        const std::string lab = std::string(tr("Cast & crew")) + "  ";
        const float lw = Ui::textWidth(lab.c_str(), 13);
        Ui::text(X0, ly, lab.c_str(), 13, p.accentDark);
        Ui::text(X0 + lw, ly, fitText(font, filterDejaVu(cast, 300), 13, (int)(RIGHT - X0 - lw)).c_str(), 13, p.text);
        ly += 19;
    }
    if (!genres.empty())
        Ui::text(X0, ly, fitText(font, genres, 13, (int)(RIGHT - X0)).c_str(), 13, p.textDim);

    drawDetailRows((int)X0 + 6, (int)(RIGHT - X0 - 6), flixBtn < 0);
}
