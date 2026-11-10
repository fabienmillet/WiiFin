#include "SettingsView.h"
#include "Ui.h"
#include "../player/vo_wiifin.h"
#include "../player/WiiPlayer.h"
#include "../core/SoundFX.h"
#include "../core/MusicBGM.h"
#include "../core/Text.h"
#include "../core/Lang.h"
#include "../input/Input.h"
#include "../core/SavedProfile.h"
#include "../core/ExitZone.h"
#include "../version.h"
#include <stdio.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/lwp.h>
#include <ogc/mutex.h>

extern volatile bool g_app_powerOff;
extern volatile bool g_app_reset;

namespace {

struct CacheClearJob {
    JellyfinClient* client;
    mutex_t lock;
    bool done = false, stop = false;
    int removed = 0, result = -1;
    std::string filename;
};

static u8 s_cacheClearStack[16 * 1024] DEAD_AT_EXIT __attribute__((aligned(32)));

void* clearCacheWorker(void* arg) {
    CacheClearJob& job = *static_cast<CacheClearJob*>(arg);
    int result = job.client->clearImageCache([&](int removed, const char* filename) {
        LWP_MutexLock(job.lock);
        job.removed = removed;
        job.filename = filename;
        bool keepGoing = !job.stop;
        LWP_MutexUnlock(job.lock);
        return keepGoing;
    });
    LWP_MutexLock(job.lock);
    job.result = result;
    job.done = true;
    LWP_MutexUnlock(job.lock);
    return nullptr;
}

} // namespace

/* The rows, by what they set (drawRow and activate take these) */
enum Row { R_SSL, R_MUSIC, R_SOUNDS, R_QUALITY, R_DIRECT, R_SMOOTH, R_THEME, R_HOME, R_VIEW,
           R_LIGHT, R_AREA, R_BACKGROUNDS, R_CLOCK, R_LANGUAGE, R_SIGNIN, R_CACHE, R_CACHE_LOCATION };

/* the sections: a profile's own look (WiiFin's menu: the start-up one,
 * theme and backgrounds only), what every profile shares, how a profile
 * signs in */
static const char* const SECTIONS[3] = { N_("View Settings"), N_("System Settings"), N_("Sign-In Settings") };
/* in the menu, a profile's two sections first: View, Sign-In, System */
static int sectionAt(int i, bool profile) { return profile && i > 0 ? 3 - i : i; }

/* Row layout (also used for pointer hit-testing).  Settings come in pages
 * of PER_PAGE rows, turned with arrows on the screen edges like the Wii
 * System Settings. */
static const int ROW_X = 64, ROW_W = 512, ROW_H = 54;
static const int ROW_Y0 = 72, ROW_STEP = 64;
static const int PER_PAGE = 4;
/* the sections' buttons */
static const int SB_W = 320, SB_H = 52, SB_Y0 = 110, SB_STEP = 72;
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

SettingsView::SettingsView(GRRLIB_texImg* btn, GRRLIB_ttfFont* f, JellyfinClient& c, bool& music,
                           SavedProfile* prof)
    : btnTex(btn), font(f), client(c), musicEnabled(music), profile(prof) {}

int SettingsView::sectionCount() const { return profile ? 3 : 2; }
int SettingsView::pages() const { return ((int)rows.size() + PER_PAGE - 1) / PER_PAGE; }

void SettingsView::openSection(int s) {
    section = s;
    selectedIndex = 0;
    pageAnim = 0.0f;
    for (float& f : focusAnim) f = 0.0f;
    if (s == 0) rows = profile ? std::vector<int>{ R_THEME, R_BACKGROUNDS, R_HOME, R_VIEW }
                               : std::vector<int>{ R_THEME, R_BACKGROUNDS };
    else if (s == 1) rows = { R_MUSIC, R_SOUNDS, R_CLOCK, R_LANGUAGE, R_AREA,
                              R_SSL, R_QUALITY, R_DIRECT, R_SMOOTH, R_LIGHT, R_CACHE_LOCATION, R_CACHE };
    else rows = { R_SIGNIN };
    signInWant = profile ? profile->signIn : -1;
    cacheCleared = -1;
}

/* B in the Sign-In section: its choice applied; a passcode new to the
 * profile is typed first (twice), the section left once it is.  False:
 * the pad is up */
bool SettingsView::leaveSignIn() {
    if (!profile || signInWant < 0 || signInWant == profile->signIn) return true;
    if (signInWant == SavedProfile::Passcode) {
        pinStep = 1;
        pin.start(N_("Choose a passcode"), N_("Four digits, asked when this profile opens"));
        return false;
    }
    profile->signIn = signInWant;
    SYS_Report("[Settings] sign-in %d\n", profile->signIn);
    return true;
}

bool SettingsView::updateMenu(ir_t& ir) {
    if (Input::isBackPressed()) { SoundFX::play(SoundFX::FX::Back); return true; }
    const int n = sectionCount(), was = menuSel;
    if (Input::isUpPressed())   menuSel = (menuSel + n - 1) % n;
    if (Input::isDownPressed()) menuSel = (menuSel + 1) % n;
    int hover = -1;
    if (ir.valid)
        for (int i = 0; i < n; ++i) {
            int by = SB_Y0 + i * SB_STEP;
            if (ir.x >= 320 - SB_W / 2 && ir.x <= 320 + SB_W / 2 && ir.y >= by && ir.y <= by + SB_H) hover = i;
        }
    if (hover >= 0) menuSel = hover;
    if (menuSel != was) SoundFX::play(SoundFX::FX::Move);
    if (Input::isAJustPressed() && (!ir.valid || hover >= 0)) {
        SoundFX::play(SoundFX::FX::Open);
        openSection(sectionAt(menuSel, profile));
    }
    return false;
}

void SettingsView::renderMenu(ir_t& ir) {
    const Ui::Palette& p = Ui::pal();
    Ui::background();
    Ui::header(tr("Settings"));
    for (int i = 0; i < sectionCount(); ++i) {
        menuFocus[i] = Ui::approach(menuFocus[i], i == menuSel ? 1.0f : 0.0f);
        float grow = 6.0f * menuFocus[i];
        Ui::button(320 - SB_W / 2 - grow, SB_Y0 + i * SB_STEP - grow * 0.25f, SB_W + grow * 2, SB_H + grow * 0.5f,
                   SECTIONS[sectionAt(i, profile)], 20, menuFocus[i]);
    }
    const char* version = "WiiFin v" WIIFIN_VERSION;
    std::string credits = std::string(tr("Made with \xe2\x9d\xa4 for Jellyfin")) + "  \xe2\x80\xa2  github.com/fabienmillet/WiiFin";
    Ui::textCentered(320, 348, version, 15, p.accentDark);
    Ui::textCentered(320, 370, credits.c_str(), 12, p.textDim);
    static const Ui::Hint left[]  = { { "UD", N_("Choose") }, { "A", N_("Open") } };
    static const Ui::Hint right[] = { { "B", N_("Back") } };
    Ui::bottomBar(left, 2, right, 1);
}

void SettingsView::drawRow(int id, const char* label, const char* desc,
                           const char* valStr, u32 pillCol) {
    const Ui::Palette& p = Ui::pal();
    int i = -1;   /* its place in the open section */
    for (int k = 0; k < (int)rows.size(); ++k) if (rows[k] == id) i = k;
    if (i < 0) return;
    float x = ROW_X + (i / PER_PAGE - pageAnim) * Ui::screenWidth();   /* pages slide sideways */
    float y = ROW_Y0 + (i % PER_PAGE) * ROW_STEP;
    if (x < Ui::screenLeft() - ROW_W - 20 || x > Ui::screenRight() + 20) return;
    Ui::card(x, y, ROW_W, ROW_H, 14, focusAnim[i]);
    label = tr(label); desc = tr(desc); valStr = tr(valStr);
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
        case 1: musicEnabled = !musicEnabled; MusicBGM::setEnabled(musicEnabled); break;
        case 2: SoundFX::setEnabled(!SoundFX::enabled()); break;
        case 3: client.videoQuality = (client.videoQuality + 1) % JellyfinClient::VIDEO_QUALITY_COUNT; break;
        case 4: client.directPlay = !client.directPlay; break;
        case 5: g_wiifin_smooth_motion = !g_wiifin_smooth_motion; break;
        case 6:
            Ui::setTheme(Ui::nextTheme(Ui::theme()));
            /* the Flix look is made for the rows of carousels */
            if (Ui::theme() == Ui::Theme::Flix) Ui::setHomeLayout(Ui::HomeLayout::Rows);
            break;
        case 7:
            Ui::setHomeLayout(Ui::homeLayout() == Ui::HomeLayout::Rows ? Ui::HomeLayout::Grid
                                                                       : Ui::HomeLayout::Rows);
            break;
        case 8:
            Ui::setLibraryStyle((Ui::LibraryStyle)(((int)Ui::libraryStyle() + 1) % Ui::LIBRARY_STYLE_COUNT));
            break;
        case 9: g_wiifin_disc_light = !g_wiifin_disc_light; break;
        case 10: calibrating = true; calCorner = 0; break;
        case 11: Ui::setSolidColors(!Ui::solidColors()); break;
        case 12: Ui::setClock12h(!Ui::clock12h()); break;
        case 13:   /* the Wii's, then each language in turn */
            Lang::choose(Lang::next(Lang::choice()));
            break;
        case R_CACHE:    /* once per visit: "Cleared N" stays until the section is reopened */
            if (cacheCleared < 0 && !client.cacheDir.empty()) clearCache();
            break;
        case R_CACHE_LOCATION:
            client.setImageCacheLocation(client.imageCacheLocation == JellyfinClient::CacheSD
                ? JellyfinClient::CacheOff : JellyfinClient::CacheSD);
            cacheCleared = -1;
            break;
        case R_SIGNIN:   /* at once, a passcode, the password: chosen, applied on leaving */
            if (!profile) break;
            signInWant = (signInWant + 1) % 3;
            break;
    }
}

void SettingsView::clearCache() {
    CacheClearJob job;
    job.client = &client;
    cacheCleared = -2;
    if (LWP_MutexInit(&job.lock, false) < 0) return;
    lwp_t thread;
    if (LWP_CreateThread(&thread, clearCacheWorker, &job,
                         s_cacheClearStack, sizeof(s_cacheClearStack), 64) < 0) {
        LWP_MutexDestroy(job.lock);
        return;
    }
    SoundFX::play(SoundFX::FX::Loading);
    const Ui::Palette& p = Ui::pal();
    for (;;) {
        Input::update();
        LWP_MutexLock(job.lock);
        if (g_app_powerOff || g_app_reset) job.stop = true;
        bool done = job.done;
        int removed = job.removed;
        std::string filename = job.filename;
        LWP_MutexUnlock(job.lock);
        if (done) break;

        Ui::background();
        Ui::header(tr("Settings"), tr("Image Cache"));
        Ui::card(80, 145, 480, 180, 18, Ui::pulse());
        Ui::textCentered(320, 168, tr("Clearing image cache..."), 22, p.text);
        char count[96];
        snprintf(count, sizeof(count), tr("Removed %d image(s)"), removed);
        Ui::textCentered(320, 212, count, 18, p.accent);
        size_t slash = filename.rfind('/');
        std::string server;
        if (slash != std::string::npos) {
            server = filename.substr(0, slash);
            filename = filename.substr(slash + 1);
        }
        /* Server folders and image names are ASCII; keep both in the card. */
        auto fit = [](std::string text) {
            bool shortened = false;
            while (!text.empty() && Ui::textWidth((text + "...").c_str(), 12) > 420) {
                text.pop_back();
                shortened = true;
            }
            return shortened ? text + "..." : text;
        };
        Ui::textCentered(320, 244, fit(server).c_str(), 12, p.textDim);
        Ui::textCentered(320, 266, fit(filename).c_str(), 12, p.textDim);
        int active = (ticks_to_millisecs(gettime()) / 200) % 3;
        for (int i = 0; i < 3; ++i)
            Ui::circle(300 + i * 20, 292, 4, Ui::alpha(p.accent, i == active ? 1.0f : 0.25f));
        GRRLIB_Render();
    }
    LWP_JoinThread(thread, nullptr);
    SoundFX::stopLoading();
    cacheCleared = job.result >= 0 ? job.result : -2;
    LWP_MutexDestroy(job.lock);
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
    u32 held = Input::held();
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
    Ui::textCentered(320, 166, tr("Screen Area"), 22, p.text);
    Ui::textCentered(320, 200, tr("Move each arrow until its tip sits right in"), 15, p.textDim);
    Ui::textCentered(320, 220, tr("the corner of your TV screen."), 15, p.textDim);
    char vals[96];
    snprintf(vals, sizeof(vals), tr("Left %d   \xc2\xb7   Top %d   \xc2\xb7   Right %d   \xc2\xb7   Bottom %d"), l, t, r, b);
    Ui::textCentered(320, 258, vals, 14, p.accentDark);

    /* bar first: the brackets must stay visible on top of it */
    static const Ui::Hint left[]  = { { "UD", N_("Move") }, { "A", N_("Switch") }, { "-", N_("Reset") } };
    static const Ui::Hint right[] = { { "B", N_("Done") } };
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
    if (pinStep) {
        const int r = pin.update(ir);
        if (r < 0) { pinStep = 0; signInWant = profile->signIn; return false; }   /* the section, as it was */
        if (r == 0) return false;
        if (pinStep == 1) {
            pinFirst = pin.code();
            pinStep = 2;
            pin.start(N_("Enter it again"));
        } else if (pin.code() != pinFirst) {
            pinStep = 1;
            pin.start(N_("Choose a passcode"), N_("The two didn't match: once more"));
            pin.reject();
        } else {
            profile->passSalt = PinPad::newSalt();
            profile->passHash = PinPad::hash(profile->passSalt, pin.code());
            profile->signIn   = SavedProfile::Passcode;
            pinStep = 0;
            section = -1;
            SoundFX::play(SoundFX::FX::Start);
            SYS_Report("[Settings] sign-in %d\n", profile->signIn);
        }
        return false;
    }
    if (section < 0) return updateMenu(ir);

    if (ir.valid) irMode = true;
    else          irMode = false;   /* no pointer on the screen: the D-pad drives, A acts on the highlighted item */

    if (Input::isBackPressed()) {
        SoundFX::play(SoundFX::FX::Back);
        if (section != 2 || leaveSignIn()) section = -1;
        return false;
    }
    const int NUM_SETTINGS = (int)rows.size(), PAGES = pages();

    const int page = selectedIndex / PER_PAGE;
    const int selBefore = selectedIndex;
    bool activated = false;
    /* Up/Down walk through every row (pages follow); Left/Right and -/+
     * (L/R on the Classic and GameCube controllers) turn pages */
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
    if (Input::isLPressed())     { turn(page - 1); irMode = false; }
    if (Input::isRPressed())     { turn(page + 1); irMode = false; }

    /* the pointer picks among the rows of the page shown now: of the one
     * before, the row under it took the page back (-/+ did nothing) */
    const int shown = selectedIndex / PER_PAGE;
    int hovered = -1;
    if (ir.valid) {
        for (int i = shown * PER_PAGE; i < NUM_SETTINGS && i < (shown + 1) * PER_PAGE; i++) {
            int ry = ROW_Y0 + (i % PER_PAGE) * ROW_STEP;
            if (ir.x >= ROW_X && ir.x <= ROW_X + ROW_W && ir.y >= ry && ir.y <= ry + ROW_H)
                hovered = i;
        }
        if (hovered >= 0) { selectedIndex = hovered; irMode = true; }
    }

    if (aPressed) {
        if (overArrow(ir, ARROW_L_CX))      turn(page - 1);
        else if (overArrow(ir, ARROW_R_CX)) turn(page + 1);
        else if (ir.valid) { if (hovered >= 0) { activate(rows[hovered]); activated = true; } }
        else if (!irMode) { activate(rows[selectedIndex]); activated = true; }
    }
    /* turning Interface Sounds on plays this one: what they sound like */
    if (activated)                                    SoundFX::play(SoundFX::FX::Start);
    else if (selectedIndex / PER_PAGE != page)        SoundFX::play(SoundFX::FX::Page);
    else if (selectedIndex != selBefore)              SoundFX::play(SoundFX::FX::Move);
    return false;
}

void SettingsView::render(ir_t& ir) {
    if (calibrating) { renderCalibration(); return; }
    if (pinStep) { Ui::background(); pin.render(ir); return; }
    if (section < 0) { renderMenu(ir); return; }
    const Ui::Palette& p = Ui::pal();
    const int NUM_SETTINGS = (int)rows.size(), PAGES = pages();
    for (int i = 0; i < NUM_SETTINGS; ++i)
        focusAnim[i] = Ui::approach(focusAnim[i], i == selectedIndex ? 1.0f : 0.0f);

    const int page = selectedIndex / PER_PAGE;
    pageAnim = Ui::approach(pageAnim, (float)page, 0.25f);

    Ui::background();
    Ui::header(tr("Settings"), tr(SECTIONS[section]));


    drawRow(0, N_("SSL Verification"),
            N_("Validate the server HTTPS certificate (disable for self-signed certs)"),
            client.sslVerify ? N_("ON") : N_("OFF"), client.sslVerify ? p.ok : 0x8A9099FF);
    drawRow(1, N_("Background Music"), N_("Play background music in menus"),
            musicEnabled ? N_("ON") : N_("OFF"), musicEnabled ? p.ok : 0x8A9099FF);
    drawRow(2, N_("Interface Sounds"),
            SoundFX::enabled() ? N_("Sounds in the menus; your own in sounds/ on the SD card")
                               : N_("The menus stay silent"),
            SoundFX::enabled() ? N_("ON") : N_("OFF"), SoundFX::enabled() ? p.ok : 0x8A9099FF);
    {
        static const u32 qualityCol[JellyfinClient::VIDEO_QUALITY_COUNT] =
            { 0xD98A2EFF, 0x34A8DDFF, 0x3DAF5AFF, 0x9A5BD6FF };
        /* the chosen bitrate, and what the connection measured at the first
         * video holds it to (it is not the setting changing on its own) */
        char desc[128];
        const int q = client.videoQuality;
        if (client.measuredKbps() > 0 && client.linkQuality() < q)
            snprintf(desc, sizeof(desc),
                     tr("%.1f Mb/s; your connection (%.1f Mb/s measured) holds videos to %.1f"),
                     JellyfinClient::qualityBitrate(q) / 1000000.0, client.measuredKbps() / 1000.0,
                     client.videoBitrate() / 1000000.0);
        else if (q == JellyfinClient::VIDEO_QUALITY_COUNT - 1)
            snprintf(desc, sizeof(desc),
                     tr("%.1f Mb/s: sharpest, for a wired adapter. Lower it if videos stutter"),
                     JellyfinClient::qualityBitrate(q) / 1000000.0);
        else
            snprintf(desc, sizeof(desc),
                     tr("Video bitrate: %.1f Mb/s. Lower it if playback keeps buffering"),
                     JellyfinClient::qualityBitrate(q) / 1000000.0);
        drawRow(3, N_("Video Quality"), desc,
                JellyfinClient::videoQualityName(client.videoQuality),
                qualityCol[client.videoQuality]);
    }
    drawRow(4, N_("Direct Play"),
            client.directPlay ? N_("Files the Wii can decode play as they are (SD DivX, DVD...): no server work")
                              : N_("Every video is converted by the server"),
            client.directPlay ? N_("Auto") : N_("OFF"), client.directPlay ? p.ok : 0x8A9099FF);
    drawRow(5, N_("Smooth Motion"),
            g_wiifin_smooth_motion ? N_("Blends frames on picture changes: films move evenly on 60 Hz TVs")
                                   : N_("Each frame shown as is (24 fps films judder slightly on 60 Hz)"),
            g_wiifin_smooth_motion ? N_("ON") : N_("OFF"), g_wiifin_smooth_motion ? p.ok : 0x8A9099FF);
    {
        static const char* themeDesc[Ui::THEME_COUNT] = {
            N_("Light, like the Wii Menu"),
            N_("Dark, for movie nights"),
            N_("Red and charcoal, browse in rows like the old video channels"),
            N_("Black, silver and green"),
            N_("Dark grey and orange"),
        };
        static const u32 themeCol[Ui::THEME_COUNT] = { 0x34A8DDFF, 0x45546BFF, 0xD81F26FF, 0x7DC242FF, 0xF47521FF };
        int t = (int)Ui::theme();
        drawRow(6, N_("Theme"), themeDesc[t], Ui::themeName(Ui::theme()), themeCol[t]);
    }
    {
        bool rows = Ui::homeLayout() == Ui::HomeLayout::Rows;
        drawRow(7, N_("Home Screen"), rows ? N_("Rows of posters to browse, like the old video channels")
                                       : N_("A grid of your libraries"),
                rows ? N_("Rows") : N_("Grid"), rows ? p.accent : 0x8A9099FF);
    }
    {
        static const char* name[Ui::LIBRARY_STYLE_COUNT] = { N_("Posters"), N_("List"), N_("List + Posters") };
        static const char* desc[Ui::LIBRARY_STYLE_COUNT] = {
            N_("Browse libraries with their artwork"),
            N_("Libraries as text lists: quickest to browse big ones"),
            N_("Text lists, with the cover of the selected title"),
        };
        int v = (int)Ui::libraryStyle();
        drawRow(8, N_("Library View"), desc[v], name[v], v == 0 ? 0x8A9099FF : p.accent);
    }
    drawRow(9, N_("Disc Slot Light"),
            g_wiifin_disc_light ? N_("Pulses with the sound, as bright as the Wii Settings say")
                                : N_("Stays dark during playback"),
            g_wiifin_disc_light ? N_("ON") : N_("OFF"), g_wiifin_disc_light ? p.ok : 0x8A9099FF);
    {
        int l, t, r, b;
        Ui::safeArea(l, t, r, b);
        bool full = !(l | t | r | b);
        drawRow(10, N_("Screen Area"), N_("Shrink the interface if your TV crops the edges (overscan)"),
                full ? N_("Full") : N_("Adjusted"), full ? 0x8A9099FF : p.accent);
    }
    drawRow(11, N_("Backgrounds"),
            Ui::solidColors() ? N_("Solid colours: no bands on TVs that show them in gradients")
                              : N_("Soft gradients, like the Wii Menu"),
            Ui::solidColors() ? N_("Solid") : N_("Gradient"), Ui::solidColors() ? p.accent : 0x8A9099FF);
    drawRow(12, N_("Clock"),
            Ui::clock12h() ? N_("1:30 PM, the date as month/day") : N_("13:30, the date as day/month"),
            Ui::clock12h() ? N_("12-hour") : N_("24-hour"), p.accent);
    /* each language by its own name */
    drawRow(13, N_("Language"),
            Lang::choice() < 0 ? N_("The Wii's language, as set in the Wii Settings") : N_("Chosen here, whatever the Wii's language"),
            Lang::name(Lang::current()), Lang::choice() < 0 ? 0x8A9099FF : p.accent);
    {
        const char* desc = client.imageCacheLocation == JellyfinClient::CacheOff
            ? N_("Download artwork without saving it")
            : client.cacheDir.empty() ? N_("Selected drive is unavailable or not writable")
                                      : N_("Save artwork here for faster browsing");
        drawRow(R_CACHE_LOCATION, N_("Image Cache"), desc,
                client.imageCacheLocation == JellyfinClient::CacheSD ? N_("SD") : N_("OFF"),
                client.cacheDir.empty() ? 0x8A9099FF : p.accent);
    }
    {
        char desc[96];
        if (client.imageCacheLocation == JellyfinClient::CacheOff)
            snprintf(desc, sizeof(desc), "%s", tr("Choose SD to clear saved artwork"));
        else if (client.cacheDir.empty())
            snprintf(desc, sizeof(desc), "%s", tr("Selected drive is unavailable or not writable"));
        else if (cacheCleared == -2)
            snprintf(desc, sizeof(desc), "%s", tr("Could not clear image cache. Try again"));
        else if (cacheCleared >= 0)
            snprintf(desc, sizeof(desc), tr("Removed %d image(s)"), cacheCleared);
        else
            snprintf(desc, sizeof(desc), "%s", tr("Clear saved artwork to refresh it or free space"));
        const char* val = client.cacheDir.empty() ? N_("OFF") : cacheCleared >= 0 ? N_("Cleared") : N_("Clear");
        drawRow(R_CACHE, N_("Clear Image Cache"), desc, val,
                cacheCleared < 0 && !client.cacheDir.empty() ? p.accent : 0x8A9099FF);
    }
    if (profile) {
        static const char* const how[3]  = { N_("Automatic"), N_("Passcode"), N_("Password") };
        static const char* const desc[3] = {
            N_("Opens at once on this Wii"),
            N_("Asks the profile's four-digit passcode first"),
            N_("Asks the password (or Quick Connect) each time"),
        };
        const int m = signInWant >= 0 && signInWant < 3 ? signInWant : 0;
        drawRow(R_SIGNIN, N_("Sign-In"), desc[m], how[m], m == 0 ? 0x8A9099FF : p.accent);
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

    static const Ui::Hint left[]  = { { "UD", N_("Choose") }, { "A", N_("Change") }, { "-/+", N_("Page") } };
    static const Ui::Hint right[] = { { "B", N_("Back") } };
    Ui::bottomBar(left, PAGES > 1 ? 3 : 2, right, 1);
}
