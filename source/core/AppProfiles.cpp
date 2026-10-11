/* App: what a profile has of its own.  Settings from a profile's menu,
 * the passcode asked when it opens, and the questions a new profile is
 * asked: how it signs in, its home screen, its library view. */
#include "App.h"
#include "AppInternal.h"
#include "Lang.h"
#include "MusicBGM.h"
#include "SoundFX.h"
#include "../ui/Ui.h"
#include "../ui/PinPad.h"
#include "../ui/SettingsView.h"
#include "../input/Input.h"
#include <wiiuse/wpad.h>

extern volatile bool g_app_powerOff;
extern volatile bool g_app_reset;

static void drawPointer(const ir_t& ir, GRRLIB_texImg* tex) {
    if (ir.valid && tex) {
        orient_t orient; WPAD_Orientation(WPAD_CHAN_0, &orient);
        GRRLIB_DrawImg((int)ir.x - 20, (int)ir.y - 4, tex, orient.roll, 1, 1, 0xFFFFFFFF);
    }
}

void App::runSettings(SavedProfile* profile) {
    SettingsView sv(btnTex, font, jellyfinClient, musicEnabled, profile);
    while (true) {
        Input::update();
        Input::readIR(ir);
        if (g_app_powerOff || g_app_reset) { running = false; break; }
        if (Input::isHomePressed() && doShowHomeOverlay(font, btnTex, cursorPointerTex, musicEnabled)) {
            running = false;
            break;
        }
        if (sv.update(ir)) break;
        sv.render(ir);
        drawPointer(ir, cursorPointerTex);
        GRRLIB_Render();
    }
    /* the look set: the profile's own, or WiiFin's */
    if (profile) keepView(*profile);
    else         startView = currentView();
    saveSettings();
    logSettings();
    MusicBGM::setEnabled(musicEnabled);
}

bool App::askPasscode(const SavedProfile& p) {
    PinPad pin;
    pin.start(N_("Enter the passcode"), N_("This profile is locked"));
    for (;;) {
        Input::update();
        Input::readIR(ir);
        if (g_app_powerOff || g_app_reset) { running = false; return false; }
        const int r = pin.update(ir);
        if (r < 0) return false;
        if (r > 0) {
            if (PinPad::hash(p.passSalt, pin.code()) == p.passHash) {
                SYS_Report("[WiiFin] passcode right\n");
                SoundFX::play(SoundFX::FX::Start);
                return true;
            }
            SYS_Report("[WiiFin] passcode wrong\n");
            pin.reject();
        }
        Ui::background();
        pin.render(ir);
        drawPointer(ir, cursorPointerTex);
        GRRLIB_Render();
    }
}

/* ---------------------------------------------------------------
 * A new profile's questions
 * --------------------------------------------------------------- */
namespace {

/* the screens a choice shows, drawn small: home screen and library view */
enum Mock { M_NONE = -1, M_ROWS, M_GRID, M_POSTERS, M_LIST, M_LISTCOVER };

void drawMock(int kind, float x, float y, float w, float h) {
    const Ui::Palette& p = Ui::pal();
    const u32 line = Ui::alpha(p.textDim, 0.55f), tile = Ui::alpha(p.accent, 0.7f);
    Ui::roundRect(x, y, w, h, 8, p.field);
    Ui::roundRect(x + 6, y + 6, w - 12, 6, 3, line);   /* the header */
    const float ix = x + 8, iy = y + 20, iw = w - 16, ih = h - 28;
    switch (kind) {
    case M_ROWS:   /* rows of small posters */
        for (int r = 0; r < 3; ++r) {
            float ry = iy + r * ih / 3;
            Ui::roundRect(ix, ry, iw * 0.3f, 3, 1.5f, line);
            for (int c = 0; c < 6; ++c)
                Ui::roundRect(ix + c * iw / 6, ry + 6, iw / 6 - 3, ih / 3 - 10, 2, tile);
        }
        break;
    case M_GRID:   /* big library tiles */
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 3; ++c)
                Ui::roundRect(ix + c * iw / 3, iy + r * ih / 2, iw / 3 - 5, ih / 2 - 6, 4, tile);
        break;
    case M_POSTERS:
        for (int r = 0; r < 2; ++r)
            for (int c = 0; c < 5; ++c)
                Ui::roundRect(ix + c * iw / 5, iy + r * ih / 2, iw / 5 - 4, ih / 2 - 5, 2, tile);
        break;
    case M_LIST:
    case M_LISTCOVER: {
        const float lw = kind == M_LIST ? iw : iw * 0.62f;
        for (int r = 0; r < 6; ++r)
            Ui::roundRect(ix, iy + r * ih / 6, lw, ih / 6 - 4, 2, r == 1 ? tile : line);
        if (kind == M_LISTCOVER)
            Ui::roundRect(ix + iw * 0.68f, iy, iw * 0.32f, ih * 0.75f, 3, tile);
        break;
    }
    default: break;
    }
}

}  // namespace

/* title / note: N_ English; labels: N_ each; mocks: one per label, or null.
 * The index picked, -1 with B (or the power button). */
static int askChoice(ir_t& ir, GRRLIB_texImg* cursor, const char* title, const char* note,
                     const char* const* labels, int n, const int* mocks, int sel) {
    const bool big = mocks != nullptr;
    const float W = big ? (n == 2 ? 230 : 172) : 150, H = big ? 168 : 52, GAP = 20;
    const float x0 = 320 - (n * W + (n - 1) * GAP) / 2, Y = big ? 150 : 200;
    float focus[3] = {};
    for (;;) {
        Input::update();
        Input::readIR(ir);
        if (g_app_powerOff || g_app_reset) return -1;
        if (Input::isBackPressed()) { SoundFX::play(SoundFX::FX::Back); return -1; }
        const int was = sel;
        if (Input::isLeftPressed()  && sel > 0)     sel--;
        if (Input::isRightPressed() && sel < n - 1) sel++;
        int hover = -1;
        if (ir.valid)
            for (int i = 0; i < n; ++i) {
                float x = x0 + i * (W + GAP);
                if (ir.x >= x && ir.x <= x + W && ir.y >= Y && ir.y <= Y + H + (big ? 34 : 0)) hover = i;
            }
        if (hover >= 0) sel = hover;
        if (sel != was) SoundFX::play(SoundFX::FX::Move);
        if (Input::isAJustPressed() && (!ir.valid || hover >= 0)) { SoundFX::play(SoundFX::FX::Start); return sel; }

        const Ui::Palette& p = Ui::pal();
        Ui::background();
        Ui::textCentered(320, 70, tr(title), 22, p.text);
        if (note) Ui::textCentered(320, 104, tr(note), 14, p.textDim);
        for (int i = 0; i < n; ++i) {
            focus[i] = Ui::approach(focus[i], i == sel ? 1.0f : 0.0f);
            const float x = x0 + i * (W + GAP);
            if (big) {
                Ui::card(x, Y, W, H + 34, 14, focus[i] > 0.01f ? Ui::pulse() * focus[i] : 0.0f);
                drawMock(mocks[i], x + 12, Y + 12, W - 24, H - 18);
                Ui::textCentered(x + W / 2, Y + H + 4, tr(labels[i]), 17, i == sel ? p.text : p.textDim);
            } else {
                Ui::button(x, Y, W, H, labels[i], 19, focus[i] > 0.01f ? Ui::pulse() * focus[i] : 0.0f);
            }
        }
        const Ui::Hint l[] = { { "LR", N_("Choose") }, { "A", N_("Select") } };
        Ui::bottomBar(l, 2, nullptr, 0);
        drawPointer(ir, cursor);
        GRRLIB_Render();
    }
}

void App::runOnboarding(SavedProfile& p) {
    SYS_Report("[WiiFin] new profile: its questions\n");
    static const char* const YESNO[2] = { N_("Yes"), N_("No") };
    /* how it signs in */
    int a = askChoice(ir, cursorPointerTex, N_("Sign in automatically on this Wii?"),
                      N_("Anyone who opens WiiFin can then open this profile"), YESNO, 2, nullptr, 0);
    if (!running || g_app_powerOff || g_app_reset) return;
    if (a == 1) {
        int b = askChoice(ir, cursorPointerTex, N_("Protect this profile with a passcode?"),
                          N_("Four digits, asked each time it opens"), YESNO, 2, nullptr, 0);
        if (b == 0) {
            PinPad pin;
            pin.start(N_("Choose a passcode"), N_("Four digits, asked when this profile opens"));
            std::string first;
            for (;;) {
                Input::update();
                Input::readIR(ir);
                if (g_app_powerOff || g_app_reset) return;
                const int r = pin.update(ir);
                if (r < 0) { b = 1; break; }   /* none after all: the password each time */
                if (r > 0 && first.empty()) {
                    first = pin.code();
                    pin.start(N_("Enter it again"));
                } else if (r > 0 && pin.code() != first) {
                    first.clear();
                    pin.start(N_("Choose a passcode"), N_("The two didn't match: once more"));
                    pin.reject();
                } else if (r > 0) {
                    p.passSalt = PinPad::newSalt();
                    p.passHash = PinPad::hash(p.passSalt, pin.code());
                    p.signIn   = SavedProfile::Passcode;
                    break;
                }
                Ui::background();
                pin.render(ir);
                drawPointer(ir, cursorPointerTex);
                GRRLIB_Render();
            }
        }
        if (b != 0) {
            p.signIn = SavedProfile::Password;
            static const char* const OK[1] = { N_("OK") };
            askChoice(ir, cursorPointerTex, N_("The password each time, then"),
                      N_("Or Quick Connect: this profile keeps no sign-in on the Wii"), OK, 1, nullptr, 0);
        }
    }
    if (g_app_powerOff || g_app_reset) return;
    /* its home screen and library view, as small pictures */
    static const char* const HOMES[2] = { N_("Rows"), N_("Grid") };
    static const int HOME_MOCKS[2] = { M_ROWS, M_GRID };
    const ViewPrefs v = viewOf(p);
    int h = askChoice(ir, cursorPointerTex, N_("Which home screen?"), N_("Settings can change it later"),
                      HOMES, 2, HOME_MOCKS, v.homeLayout == 1 ? 0 : 1);
    if (h >= 0) p.homeLayout = h == 0 ? 1 : 0;
    if (g_app_powerOff || g_app_reset) return;
    static const char* const VIEWS[3] = { N_("Posters"), N_("List"), N_("List + Posters") };
    static const int VIEW_MOCKS[3] = { M_POSTERS, M_LIST, M_LISTCOVER };
    int l = askChoice(ir, cursorPointerTex, N_("Which library view?"), N_("Settings can change it later"),
                      VIEWS, 3, VIEW_MOCKS, v.libraryStyle);
    if (l >= 0) p.libraryStyle = l;
    SYS_Report("[WiiFin] new profile: sign-in %d, home %d, view %d\n", p.signIn, p.homeLayout, p.libraryStyle);
}
