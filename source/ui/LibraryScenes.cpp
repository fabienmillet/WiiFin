/* LibraryView: a film's or an episode's scenes (its chapters), from the
 * Scenes row of its page: the list, the picture Jellyfin took of the one
 * selected, A plays from it with the version and tracks the page set. */
#include "LibraryView.h"
#include "LibraryDraw.h"
#include "Ui.h"
#include "JpegTexture.h"
#include "../input/Input.h"
#include "../core/Lang.h"
#include <stdio.h>

using namespace LibDraw;

static const int SC_LW = 360;                 /* the list, the picture on its right */
static const int SC_MAX_PICTURES = 60;        /* fetched with the list: 16:9, small */

void LibraryView::freeScenes() {
    for (GRRLIB_texImg*& t : sceneTex) if (t) { GRRLIB_FreeTexture(t); t = nullptr; }
    sceneTex.clear();
    scenes.clear();
}

void LibraryView::loadScenes() {
    freeScenes();
    const std::string id = detailSourceId();   /* the version chosen: its own chapters */
    bool ok = false; std::string err;
    std::vector<std::string> bytes;
    runWithLoading([&]() {
        ok = client.getChapters(serverUrl, auth, id, scenes);
        if (!ok) { err = client.lastError(); return; }
        const int n = (int)scenes.size() < SC_MAX_PICTURES ? (int)scenes.size() : SC_MAX_PICTURES;
        bytes.resize(n);
        for (int i = 0; i < n; ++i)
            if (scenes[i].hasImage) client.getChapterImageBytes(serverUrl, auth, id, i, 256, 144, bytes[i]);
    });
    if (!ok || scenes.empty()) { state = State::DetailReady; return; }
    sceneTex.assign(scenes.size(), nullptr);
    for (int i = 0; i < (int)bytes.size(); ++i)
        if (!bytes[i].empty()) sceneTex[i] = loadJPEGTexture((const u8*)bytes[i].data(), (u32)bytes[i].size());
    SYS_Report("[Scenes] %u chapter(s)\n", (unsigned)scenes.size());
    sceneSel = 0; sceneTop = 0;
    state = State::ScenesReady;
}

bool LibraryView::updateScenes(ir_t& ir, bool aPressed) {
    const int n = (int)scenes.size();
    auto clamp = [&]() {
        if (sceneSel >= n) sceneSel = n - 1;
        if (sceneSel < 0) sceneSel = 0;
        if (sceneSel < sceneTop) sceneTop = sceneSel;
        if (sceneSel >= sceneTop + ITEMS_VISIBLE) sceneTop = sceneSel - ITEMS_VISIBLE + 1;
    };
    if (Input::isBackPressed()) { freeScenes(); state = State::DetailReady; return false; }
    if (Input::isUpPressed())   { sceneSel--; irMode = false; clamp(); }
    if (Input::isDownPressed()) { sceneSel++; irMode = false; clamp(); }
    if (Input::isLPressed())    { sceneSel -= ITEMS_VISIBLE; irMode = false; clamp(); }
    if (Input::isRPressed())    { sceneSel += ITEMS_VISIBLE; irMode = false; clamp(); }
    int hover = -1;
    if (ir.valid)
        for (int i = 0; i < ITEMS_VISIBLE && sceneTop + i < n; i++) {
            int ry = LIST_Y + i * ROW_H;
            if (ir.x >= LIST_X && ir.x <= LIST_X + SC_LW && ir.y >= ry && ir.y < ry + ROW_H) hover = sceneTop + i;
        }
    if (hover >= 0 && irMode) sceneSel = hover;
    if (aPressed && n > 0 && (!ir.valid || hover >= 0)) {
        const long long at = scenes[sceneSel].startTicks;
        SYS_Report("[Scenes] play from chapter %d (%lld s)\n", sceneSel + 1, at / 10000000LL);
        freeScenes();
        state = State::DetailReady;
        preparePlay(at);
        return true;
    }
    return false;
}

void LibraryView::renderScenes(ir_t& ir) {
    const Ui::Palette& p = Ui::pal();
    const int n = (int)scenes.size();
    drawBreadcrumb(detail.name + "  \xc2\xb7  " + tr("Scenes"), 20, 580);
    headerLine(46);
    for (int i = 0; i < ITEMS_VISIBLE && sceneTop + i < n; i++) {
        const int idx = sceneTop + i, ry = LIST_Y + i * ROW_H;
        const bool sel = idx == sceneSel;
        const bool hover = ir.valid && ir.y >= ry && ir.y < ry + ROW_H && ir.x >= LIST_X && ir.x <= LIST_X + SC_LW;
        const float f = focusOf(sel, hover);
        drawRow(LIST_X, ry, SC_LW, ROW_H, f);
        const int secs = (int)(scenes[idx].startTicks / 10000000LL);
        char at[16];
        if (secs >= 3600) snprintf(at, sizeof(at), "%d:%02d:%02d", secs / 3600, secs / 60 % 60, secs % 60);
        else              snprintf(at, sizeof(at), "%d:%02d", secs / 60, secs % 60);
        const int aw = Ui::textWidth(at, 14);
        Ui::textRight(LIST_X + SC_LW - 14, ry + 14, at, 14, p.textDim);
        char num[12];
        snprintf(num, sizeof(num), "%d", idx + 1);
        Ui::textCentered(LIST_X + 24, ry + 13, num, 15, sel ? p.accent : p.textDim);
        std::string name = scenes[idx].name;
        if (name.empty()) {
            char b[48];
            /* TRANSLATORS: a chapter without a name, its number */
            snprintf(b, sizeof(b), tr("Chapter %d"), idx + 1);
            name = b;
        }
        titleText(0, LIST_X + 44, ry + 11, SC_LW - 44 - aw - 24, filterDejaVu(name, 200), 18,
                  Ui::mix(p.text, p.accentDark, f), sel);
    }
    Ui::scrollbar(LIST_X + SC_LW + 8, LIST_Y + 2, ITEMS_VISIBLE * ROW_H - 6, sceneTop, ITEMS_VISIBLE, n);
    /* the picture of the one selected, 16:9 */
    if (sceneSel >= 0 && sceneSel < n) {
        const float PX = LIST_X + SC_LW + 26, PW = 600 - PX, PH = PW * 9.0f / 16.0f, PY = LIST_Y + 2;
        GRRLIB_texImg* t = sceneSel < (int)sceneTex.size() ? sceneTex[sceneSel] : nullptr;
        drawThumb(t, PX, PY, PW, PH, 0.0f, -1.0f, t ? nullptr : scenes[sceneSel].name.c_str(), false);
    }
    const Ui::Hint l[] = { { "A", N_("Play") }, { "B", N_("Back") } };
    const Ui::Hint r[] = { { "-/+", N_("Page") } };
    Ui::footer(l, 2, r, n > ITEMS_VISIBLE ? 1 : 0);
}
