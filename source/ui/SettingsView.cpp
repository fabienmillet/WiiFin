#include "SettingsView.h"
#include "Ui.h"
#include "../player/vo_wiifin.h"
#include "../core/Text.h"
#include "../input/Input.h"
#include "../version.h"
#include <stdio.h>
#include <ogc/lwp_watchdog.h>

static const int NUM_SETTINGS = 8; // extensible

/* Row layout (also used for pointer hit-testing).  Settings come in pages
 * of PER_PAGE rows, turned with arrows on the screen edges like the Wii
 * System Settings. */
static const int ROW_X = 64, ROW_W = 512, ROW_H = 54;
static const int ROW_Y0 = 72, ROW_STEP = 64;
static const int PER_PAGE = 4;
static const int PAGES = (NUM_SETTINGS + PER_PAGE - 1) / PER_PAGE;
/* page arrows sit on the screen edges (wider in 16:9) */
#define ARROW_L_CX (Ui::screenLeft() + 30.0f)
#define ARROW_R_CX (Ui::screenRight() - 30.0f)
static const int ARROW_CY  = ROW_Y0 + (PER_PAGE * ROW_STEP - 10) / 2;   /* middle of the rows */
static const int ARROW_HIT = 24;

static bool overArrow(const ir_t& ir, float cx)
{
    return ir.valid && ir.x >= cx - ARROW_HIT && ir.x < cx + ARROW_HIT &&
           ir.y >= ARROW_CY - ARROW_HIT && ir.y < ARROW_CY + ARROW_HIT;
}

SettingsView::SettingsView(GRRLIB_texImg* btn, GRRLIB_ttfFont* f, JellyfinClient& c, bool& music)
    : btnTex(btn), font(f), client(c), musicEnabled(music) {}

void SettingsView::drawRow(int i, const char* label, const char* desc,
                           const char* valStr, u32 pillCol) {
    const Ui::Palette& p = Ui::pal();
    float x = ROW_X + (i / PER_PAGE - pageAnim) * Ui::screenWidth();   /* pages slide sideways */
    float y = ROW_Y0 + (i % PER_PAGE) * ROW_STEP;
    if (x < Ui::screenLeft() - ROW_W - 20 || x > Ui::screenRight() + 20) return;
    Ui::card(x, y, ROW_W, ROW_H, 14, focusAnim[i]);
    Ui::text(x + 18, y + 8,  label, 18, p.text);
    Ui::text(x + 18, y + 31, desc,  12, p.textDim);

    // Value pill on the right
    int pw = Ui::textWidth(valStr, 15) + 28;
    float px = x + ROW_W - pw - 16, py = y + (ROW_H - 26) * 0.5f;
    Ui::roundRect(px, py, pw, 26, 13, Ui::mix(pillCol, 0xFFFFFFFF, 0.15f), pillCol);
    Ui::textCentered(px + pw * 0.5f, py + 4, valStr, 15, p.textOnAccent);
}

void SettingsView::activate(int index) {
    switch (index) {
        case 0: client.sslVerify = !client.sslVerify; break;
        case 1: musicEnabled     = !musicEnabled;     break;
        case 2: client.videoQuality = (client.videoQuality + 1) % JellyfinClient::VIDEO_QUALITY_COUNT; break;
        case 3: g_wiifin_smooth_motion = !g_wiifin_smooth_motion; break;
        case 4:
            Ui::setTheme(Ui::nextTheme(Ui::theme()));
            /* the Flix look is made for the rows of carousels */
            if (Ui::theme() == Ui::Theme::Flix) Ui::setHomeLayout(Ui::HomeLayout::Rows);
            break;
        case 5:
            Ui::setHomeLayout(Ui::homeLayout() == Ui::HomeLayout::Rows ? Ui::HomeLayout::Grid
                                                                       : Ui::HomeLayout::Rows);
            break;
        case 6:
            Ui::setLibraryStyle((Ui::LibraryStyle)(((int)Ui::libraryStyle() + 1) % Ui::LIBRARY_STYLE_COUNT));
            break;
        case 7: calibrating = true; calCorner = 0; break;
    }
}

/* ---------------------------------------------------------------
 * Screen area calibration: two corner brackets, moved with the d-pad until
 * they sit in the corners of the visible picture.  The GUI follows live.
 * --------------------------------------------------------------- */
void SettingsView::updateCalibration()
{
    if (Input::isBackPressed()) { calibrating = false; return; }
    if (Input::isAJustPressed()) calCorner ^= 1;
    if (Input::isLPressed()) Ui::setSafeArea(0, 0, 0, 0);   /* [-] reset */

    /* Single press = 1 px; holding repeats after 350 ms, faster after 1 s */
    u32 held = WPAD_ButtonsHeld(0);
    u32 dirs = held & (WPAD_BUTTON_UP | WPAD_BUTTON_DOWN | WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT);
    u64 now  = ticks_to_millisecs(gettime());
    int dx = 0, dy = 0, step = 1;
    if (Input::isLeftPressed())  dx = -1;
    if (Input::isRightPressed()) dx = +1;
    if (Input::isUpPressed())    dy = -1;
    if (Input::isDownPressed())  dy = +1;
    if (dx || dy) { calHeldSince = now; calLastStep = now; }
    else if (dirs && calHeldSince && now - calHeldSince > 350 && now - calLastStep > 40) {
        calLastStep = now;
        if (held & WPAD_BUTTON_LEFT)  dx = -1;
        if (held & WPAD_BUTTON_RIGHT) dx = +1;
        if (held & WPAD_BUTTON_UP)    dy = -1;
        if (held & WPAD_BUTTON_DOWN)  dy = +1;
        if (now - calHeldSince > 1000) step = 3;
    }
    if (!dirs && !(dx || dy)) calHeldSince = 0;
    if (!dx && !dy) return;

    int l, t, r, b;
    Ui::safeArea(l, t, r, b);
    if (calCorner == 0) { l += dx * step; t += dy * step; }   /* moving the corner inward grows the margin */
    else                { r -= dx * step; b -= dy * step; }
    Ui::setSafeArea(l, t, r, b);
}

void SettingsView::renderCalibration()
{
    const Ui::Palette& p = Ui::pal();
    int l, t, r, b;
    Ui::safeArea(l, t, r, b);

    Ui::background(false);

    Ui::card(110, 150, 420, 150, 18, 0.0f);
    Ui::textCentered(320, 166, "Screen Area", 22, p.text);
    Ui::textCentered(320, 200, "Move each arrow until its tip sits right in", 15, p.textDim);
    Ui::textCentered(320, 220, "the corner of your TV screen.", 15, p.textDim);
    char vals[96];
    snprintf(vals, sizeof(vals), "Left %d   \xc2\xb7   Top %d   \xc2\xb7   Right %d   \xc2\xb7   Bottom %d", l, t, r, b);
    Ui::textCentered(320, 258, vals, 14, p.accentDark);

    /* bar first: the brackets must stay visible on top of it */
    static const Ui::Hint left[]  = { { "UD", "Move" }, { "A", "Switch" }, { "-", "Reset" } };
    static const Ui::Hint right[] = { { "B", "Done" } };
    Ui::bottomBar(left, 3, right, 1);

    /* Corner brackets on the very edges of the picture: the whole picture
     * moves with the setting, so they show where the TV cuts it. */
    const float ARM = 46, TH = 5;
    const float SL = Ui::screenLeft(), SR = Ui::screenRight();
    Ui::roundBorder(SL, 0, SR - SL, 480, 2, 1.5f, Ui::alpha(p.textDim, 0.6f));
    for (int c = 0; c < 2; ++c) {
        bool on = (c == calCorner);
        u32 col = on ? Ui::mix(p.accent, 0xFFFFFFFF, 1.0f - Ui::pulse()) : p.textDim;
        float cx = c == 0 ? SL : SR, cy = c == 0 ? 0.0f : 480.0f;
        float sx = c == 0 ? 1.0f : -1.0f, sy = c == 0 ? 1.0f : -1.0f;
        /* L-shaped bracket hugging the corner */
        Ui::roundRect(sx > 0 ? cx : cx - ARM, sy > 0 ? cy : cy - TH, ARM, TH, 1, col);
        Ui::roundRect(sx > 0 ? cx : cx - TH, sy > 0 ? cy : cy - ARM, TH, ARM, 1, col);
        /* arrow pointing into the corner */
        float ax = cx + sx * 26, ay = cy + sy * 26;
        Ui::triangle(cx + sx * 9, cy + sy * 9, ax + sx * 6, ay - sy * 8, ax - sx * 8, ay + sy * 6, col);
        Ui::roundRect(ax - 3 + sx * 2, ay - 3 + sy * 2, 6, 6, 3, col);
    }
}

// ---------------------------------------------------------------
bool SettingsView::update(ir_t& ir) {
    bool aPressed = Input::isAJustPressed();

    if (calibrating) { updateCalibration(); return false; }

    if (ir.valid) irMode = true;

    if (Input::isBackPressed()) return true;

    const int page = selectedIndex / PER_PAGE;
    /* Up/Down walk through every row (pages follow); Left/Right turn pages */
    if (Input::isUpPressed())   { selectedIndex = (selectedIndex - 1 + NUM_SETTINGS) % NUM_SETTINGS; irMode = false; }
    if (Input::isDownPressed()) { selectedIndex = (selectedIndex + 1) % NUM_SETTINGS; irMode = false; }
    auto turn = [&](int to) {
        if (to < 0 || to >= PAGES) return;
        int slot = selectedIndex % PER_PAGE;
        selectedIndex = to * PER_PAGE + slot;
        if (selectedIndex >= NUM_SETTINGS) selectedIndex = NUM_SETTINGS - 1;
    };
    if (Input::isLeftPressed())  { turn(page - 1); irMode = false; }
    if (Input::isRightPressed()) { turn(page + 1); irMode = false; }

    int hovered = -1;
    if (ir.valid) {
        for (int i = page * PER_PAGE; i < NUM_SETTINGS && i < (page + 1) * PER_PAGE; i++) {
            int ry = ROW_Y0 + (i % PER_PAGE) * ROW_STEP;
            if (ir.x >= ROW_X && ir.x <= ROW_X + ROW_W && ir.y >= ry && ir.y <= ry + ROW_H)
                hovered = i;
        }
        if (hovered >= 0) { selectedIndex = hovered; irMode = true; }
    }

    if (aPressed) {
        if (overArrow(ir, ARROW_L_CX))      turn(page - 1);
        else if (overArrow(ir, ARROW_R_CX)) turn(page + 1);
        else if (ir.valid) { if (hovered >= 0) activate(hovered); }
        else if (!irMode) activate(selectedIndex);
    }
    return false;
}

void SettingsView::render(ir_t& ir) {
    if (calibrating) { renderCalibration(); return; }
    const Ui::Palette& p = Ui::pal();
    for (int i = 0; i < NUM_SETTINGS; ++i)
        focusAnim[i] = Ui::approach(focusAnim[i], i == selectedIndex ? 1.0f : 0.0f);

    const int page = selectedIndex / PER_PAGE;
    pageAnim = Ui::approach(pageAnim, (float)page, 0.25f);

    Ui::background();
    Ui::header("Settings");


    drawRow(0, "SSL Verification",
            "Validate the server HTTPS certificate (disable for self-signed certs)",
            client.sslVerify ? "ON" : "OFF", client.sslVerify ? p.ok : 0x8A9099FF);
    drawRow(1, "Background Music", "Play background music in menus",
            musicEnabled ? "ON" : "OFF", musicEnabled ? p.ok : 0x8A9099FF);
    {
        static const u32 qualityCol[JellyfinClient::VIDEO_QUALITY_COUNT] =
            { 0xD98A2EFF, 0x34A8DDFF, 0x3DAF5AFF };
        char desc[96];
        snprintf(desc, sizeof(desc),
                 "Video bitrate: %.1f Mb/s. Lower it if playback keeps buffering",
                 client.videoBitrate() / 1000000.0);
        drawRow(2, "Video Quality", desc,
                JellyfinClient::videoQualityName(client.videoQuality),
                qualityCol[client.videoQuality]);
    }
    drawRow(3, "Smooth Motion",
            g_wiifin_smooth_motion ? "Blends frames on picture changes: films move evenly on 60 Hz TVs"
                                   : "Each frame shown as is (24 fps films judder slightly on 60 Hz)",
            g_wiifin_smooth_motion ? "ON" : "OFF", g_wiifin_smooth_motion ? p.ok : 0x8A9099FF);
    {
        static const char* themeDesc[Ui::THEME_COUNT] = {
            "Light, like the Wii Menu",
            "Dark, for movie nights",
            "Red and charcoal, browse in rows like the old video channels",
        };
        static const u32 themeCol[Ui::THEME_COUNT] = { 0x34A8DDFF, 0x45546BFF, 0xD81F26FF };
        int t = (int)Ui::theme();
        drawRow(4, "Theme", themeDesc[t], Ui::themeName(Ui::theme()), themeCol[t]);
    }
    {
        bool rows = Ui::homeLayout() == Ui::HomeLayout::Rows;
        drawRow(5, "Home Screen", rows ? "Rows of posters to browse, like the old video channels"
                                       : "A grid of your libraries",
                rows ? "Rows" : "Grid", rows ? p.accent : 0x8A9099FF);
    }
    {
        static const char* name[Ui::LIBRARY_STYLE_COUNT] = { "Posters", "List", "List + Cover" };
        static const char* desc[Ui::LIBRARY_STYLE_COUNT] = {
            "Browse libraries with their artwork",
            "Libraries as text lists: quickest to browse big ones",
            "Text lists, with the cover of the selected title",
        };
        int v = (int)Ui::libraryStyle();
        drawRow(6, "Library View", desc[v], name[v], v == 0 ? 0x8A9099FF : p.accent);
    }
    {
        int l, t, r, b;
        Ui::safeArea(l, t, r, b);
        bool full = !(l | t | r | b);
        drawRow(7, "Screen Area", "Shrink the interface if your TV crops the edges (overscan)",
                full ? "Full" : "Adjusted", full ? 0x8A9099FF : p.accent);
    }

    /* page arrows on the screen edges + page dots */
    if (PAGES > 1) {
        Ui::pageArrow(ARROW_L_CX, ARROW_CY, true,  page > 0,         overArrow(ir, ARROW_L_CX));
        Ui::pageArrow(ARROW_R_CX, ARROW_CY, false, page < PAGES - 1, overArrow(ir, ARROW_R_CX));
        const float GAP = 16, y = ROW_Y0 + PER_PAGE * ROW_STEP + 2;
        float x0 = 320 - (PAGES - 1) * GAP * 0.5f;
        for (int i = 0; i < PAGES; ++i)
            Ui::circle(x0 + i * GAP, y, i == page ? 4.5f : 3.5f,
                       i == page ? p.accent : Ui::alpha(p.cardBorder, 0.9f));
    }

    // Credits
    const char* version = "WiiFin v" WIIFIN_VERSION;
    const char* credits = "Made with \xe2\x9d\xa4 for Jellyfin  \xe2\x80\xa2  github.com/fabienmillet/WiiFin";
    Ui::textCentered(320, 348, version, 15, p.accentDark);
    Ui::textCentered(320, 370, credits, 12, p.textDim);

    static const Ui::Hint left[]  = { { "UD", "Choose" }, { "A", "Change" }, { "LR", "Page" } };
    static const Ui::Hint right[] = { { "B", "Back" } };
    Ui::bottomBar(left, PAGES > 1 ? 3 : 2, right, 1);
}
