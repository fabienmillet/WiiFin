/* LibraryView: special features of a film (2 on its page) or a series (the
 * last row of its seasons): trailers, featurettes, behind the scenes...
 * A list like the episodes'; many videos of several kinds get a folder per
 * kind first.  A opens a video's page (its length, picture), as a film's. */
#include "LibraryView.h"
#include "LibraryDraw.h"
#include "Ui.h"
#include "../input/Input.h"
#include <stdio.h>
#include <algorithm>

using namespace LibDraw;

namespace {

/* Jellyfin's ExtraType, in the order they are listed, and as shown */
const struct { const char* type; const char* label; } KINDS[] = {
    { "Trailer",         "Trailer" },
    { "Featurette",      "Featurette" },
    { "BehindTheScenes", "Behind the Scenes" },
    { "DeletedScene",    "Deleted Scene" },
    { "Interview",       "Interview" },
    { "Scene",           "Scene" },
    { "Short",           "Short" },
    { "Clip",            "Clip" },
    { "Sample",          "Sample" },
    { "ThemeVideo",      "Theme Video" },
};
const int KIND_COUNT = (int)(sizeof(KINDS) / sizeof(KINDS[0]));

int kindRank(const std::string& t) {
    for (int i = 0; i < KIND_COUNT; ++i) if (t == KINDS[i].type) return i;
    return KIND_COUNT;
}
const char* kindLabel(const std::string& t) {
    int r = kindRank(t);
    return r < KIND_COUNT ? KINDS[r].label : "Extra";
}

} // namespace

bool LibraryView::specialsFolders() const
{
    return specials.size() > 12 && specialsTypes.size() >= 2;
}

std::vector<int> LibraryView::specialsShown() const
{
    std::vector<int> rows;
    if (specialsFolders() && specialsGroup < 0) {
        for (int i = 0; i < (int)specialsTypes.size(); ++i) rows.push_back(i);
        return rows;
    }
    for (int i = 0; i < (int)specials.size(); ++i)
        if (!specialsFolders() || specials[i].extraType == specialsTypes[specialsGroup]) rows.push_back(i);
    return rows;
}

void LibraryView::openSpecials(const std::string& id, const std::string& name, State back)
{
    specialsOfId   = id;
    specialsOfName = name;
    specialsBack   = back;
    state = State::SpecialsLoad;
}

void LibraryView::loadSpecials()
{
    bool ok = false; std::string err;
    runWithLoading([&]() {
        ok = client.getSpecialFeatures(serverUrl, auth, specialsOfId, specials);
        if (!ok) err = client.lastError();
    });
    if (!ok || specials.empty()) { errMsg = ok ? "No special features" : err; state = State::Error; return; }
    std::stable_sort(specials.begin(), specials.end(), [](const JellyfinItem& a, const JellyfinItem& b) {
        return kindRank(a.extraType) < kindRank(b.extraType);
    });
    specialsTypes.clear();
    for (const JellyfinItem& it : specials)
        if (specialsTypes.empty() || specialsTypes.back() != it.extraType) specialsTypes.push_back(it.extraType);
    SYS_Report("[Specials] %u video(s) of %u kind(s)\n", (unsigned)specials.size(), (unsigned)specialsTypes.size());
    specialsSel = 0; specialsTop = 0; specialsGroup = -1;
    state = State::SpecialsReady;
}

bool LibraryView::updateSpecials(ir_t& ir, bool aPressed)
{
    std::vector<int> rows = specialsShown();
    int n = (int)rows.size();
    auto clamp = [&]() {
        if (specialsSel >= n) specialsSel = n - 1;
        if (specialsSel < 0) specialsSel = 0;
        if (specialsSel < specialsTop) specialsTop = specialsSel;
        if (specialsSel >= specialsTop + ITEMS_VISIBLE) specialsTop = specialsSel - ITEMS_VISIBLE + 1;
    };
    if (Input::isBackPressed()) {
        if (specialsFolders() && specialsGroup >= 0) {   /* back to the kinds */
            specialsSel = specialsGroup; specialsTop = 0; specialsGroup = -1;
            clamp();
        } else if (specialsBack == State::DetailReady) {   /* the film's page again */
            detailItemId        = specialsOfId;
            detailReturnState   = specialsFilmReturn;
            detailIsEpisodeHint = false;
            state = State::DetailLoad;
        } else {
            state = specialsBack;
        }
        return false;
    }
    if (Input::isUpPressed())   { specialsSel--; irMode = false; clamp(); }
    if (Input::isDownPressed()) { specialsSel++; irMode = false; clamp(); }
    if (irMode && ir.valid && !Input::isUpPressed() && !Input::isDownPressed()) {
        for (int i = 0; i < ITEMS_VISIBLE && specialsTop + i < n; i++) {
            int ry = LIST_Y + i * ROW_H;
            if (ir.x >= LIST_X && ir.x <= LIST_X + LIST_W && ir.y >= ry && ir.y < ry + ROW_H)
                specialsSel = specialsTop + i;
        }
    }
    if (aPressed && n > 0) {
        if (specialsFolders() && specialsGroup < 0) {   /* into a kind */
            specialsGroup = rows[specialsSel];
            specialsSel = 0; specialsTop = 0;
        } else {                                        /* the video's page */
            detailItemId        = specials[rows[specialsSel]].id;
            detailReturnState   = State::SpecialsReady;
            detailIsEpisodeHint = false;
            state = State::DetailLoad;
        }
    }
    return false;
}

void LibraryView::renderSpecials(ir_t& ir)
{
    const Ui::Palette& p = Ui::pal();
    std::string title = specialsOfName + "  \xc2\xb7  Special Features";
    if (specialsFolders() && specialsGroup >= 0) title += std::string("  \xc2\xb7  ") + kindLabel(specialsTypes[specialsGroup]);
    drawBreadcrumb(title, 20, 580);
    headerLine(46);

    std::vector<int> rows = specialsShown();
    const int n = (int)rows.size();
    const bool kinds = specialsFolders() && specialsGroup < 0;
    for (int i = 0; i < ITEMS_VISIBLE && specialsTop + i < n; i++) {
        int idx = specialsTop + i;
        bool sel   = idx == specialsSel;
        bool hover = ir.valid && ir.y >= LIST_Y + i * ROW_H && ir.y < LIST_Y + (i + 1) * ROW_H &&
                     ir.x >= LIST_X && ir.x <= LIST_X + LIST_W;
        int ry = LIST_Y + i * ROW_H;
        float f = focusOf(sel, hover);
        drawRow(LIST_X, ry, LIST_W, ROW_H, f);
        char right[48];
        std::string name;
        if (kinds) {   /* a kind: how many */
            const std::string& t = specialsTypes[rows[idx]];
            int count = 0;
            for (const JellyfinItem& it : specials) count += it.extraType == t;
            name = kindLabel(t);
            snprintf(right, sizeof(right), "%d video%s", count, count > 1 ? "s" : "");
        } else {       /* a video: its kind and length */
            const JellyfinItem& it = specials[rows[idx]];
            name = it.name;
            int mins = (int)((it.runtimeTicks / 10000000LL + 59) / 60);
            if (it.runtimeTicks > 0) snprintf(right, sizeof(right), "%s  \xc2\xb7  %d min", kindLabel(it.extraType), mins);
            else                     snprintf(right, sizeof(right), "%s", kindLabel(it.extraType));
        }
        int rw = Ui::textWidth(right, 14);
        std::string shown = fitText(font, filterDejaVu(name, 60), 18, LIST_W - 48 - rw);
        Ui::text(LIST_X + 16, ry + 11, shown.c_str(), 18, Ui::mix(p.text, p.accentDark, f));
        Ui::textRight(LIST_X + LIST_W - 16, ry + 14, right, 14, p.textDim);
    }
    Ui::scrollbar(614, LIST_Y + 2, ITEMS_VISIBLE * ROW_H - 6, specialsTop, ITEMS_VISIBLE, n);
    const Ui::Hint l[] = { { "A", kinds ? "Open" : "Details" }, { "B", "Back" } };
    Ui::footer(l, 2);
}
