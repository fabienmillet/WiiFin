/* The Wii-style HOME menu (Wii Menu / Reset). */
#include "AppInternal.h"
#include "Input.h"
#include "MusicBGM.h"
#include "SoundFX.h"
#include "Text.h"
#include "Utils.h"
#include "../ui/Ui.h"
#include <grrlib.h>
#include <wiiuse/wpad.h>
#include <cmath>

/* -----------------------------------------------------------------------
 * doShowHomeOverlay — full-screen Wii-style HOME menu with confirmation popup.
 * Can be called from App::loop() (normal UI) or from runPlaySession() (after
 * the player stops with PLAYER_STOP_HOME) so both contexts share identical UX.
 * Returns true if the user wants to exit (Wii Menu or Reset).
 * Sets g_restartApp = true when "Reset" is chosen.
 * Pauses BGM on entry; resumes only if musicEnabled is true (pass false
 * when calling from the player, since BGM is already paused for playback).
 * ----------------------------------------------------------------------- */
bool doShowHomeOverlay(GRRLIB_ttfFont* font, GRRLIB_texImg* btnTex,
                       GRRLIB_texImg* cursorPointerTex, bool musicEnabled)
{
    ir_t ir;
    ir.valid = false;
    int   hmSel     = 0;    /* 0 = Wii Menu, 1 = Reset */
    int   state     = 0;    /* 0 = home menu, 1 = confirm popup */
    int   confirmFor = 0;   /* which button triggered the popup */

    /* Main button layout */
    const int BTN_W = 230, BTN_H = 80;
    const int BTN_Y = 195;
    const int B0X   =  65;
    const int B1X   = 345;
    float bcx[2] = { B0X + BTN_W * 0.5f, B1X + BTN_W * 0.5f };
    float bcy    = BTN_Y + BTN_H * 0.5f;

    /* Confirmation dialog layout — centred on 640×480 */
    const int DW = 370, DH = 218;
    const int DX = (640 - DW) / 2;       /* 135 */
    const int DY = (480 - DH) / 2;       /* 131 */
    const int PBW = 150, PBH = 56;
    const int PBY = DY + DH - PBH - 20;
    const int PB0X = DX + 24;
    const int PB1X = DX + DW - PBW - 24;
    float pbcx[2] = { PB0X + PBW * 0.5f, PB1X + PBW * 0.5f };
    float pbcy    = PBY + PBH * 0.5f;

    /* Animation state */
    float openAnim      = 0.0f;
    float hoverSc[2]    = { 1.0f, 1.0f };
    float popAnim       = 0.0f;
    int   popSel        = 1;             /* default: "No" */
    float popHoverSc[2] = { 1.0f, 1.0f };

    /* Hover-change trackers for Select sound */
    int prevHoverMain = -1;
    int prevHoverPop  = -1;

    MusicBGM::pause();
    SoundFX::play(SoundFX::FX::MenuEnter);

    while (true) {
        Input::update();
        Input::readIR(ir);
        orient_t orient; WPAD_Orientation(WPAD_CHAN_0, &orient);
        if (g_app_powerOff || g_app_reset) return true;

        float irX = ir.valid ? ir.x : -1.f;
        float irY = ir.valid ? ir.y : -1.f;

        /* Per-frame hover results (reset each frame) */
        int hover    = -1;
        int popHover = -1;

        /* ---- Input ---- */
        if (state == 0) {
            if (Input::isBPressed()) { if (musicEnabled) MusicBGM::resume(); return false; }

            if (ir.valid) {
                for (int i = 0; i < 2; i++) {
                    float hw = BTN_W * hoverSc[i] * 0.5f;
                    float hh = BTN_H * hoverSc[i] * 0.5f;
                    if (irX >= bcx[i] - hw && irX <= bcx[i] + hw &&
                        irY >= bcy    - hh && irY <= bcy    + hh)
                        hover = i;
                }
            }
            /* Select sound: fire once when IR cursor first enters a button */
            if (hover >= 0 && hover != prevHoverMain)
                SoundFX::play(SoundFX::FX::Select);
            prevHoverMain = hover;

            if (hover >= 0) hmSel = hover;    /* one highlight: the pointer's */
            /* With the pointer on screen only a button under it counts;
             * the d-pad selection is used when pointing away. */
            if (Input::isAJustPressed() && (!ir.valid || hover >= 0)) {
                SoundFX::play(SoundFX::FX::Start); /* clicking Wii Menu / Reset */
                confirmFor = (hover >= 0) ? hover : hmSel;
                state      = 1;
                popAnim    = 0.0f;
                popSel     = 1;
            }
            if (Input::isLeftPressed()  || Input::isUpPressed())   hmSel = 0;
            if (Input::isRightPressed() || Input::isDownPressed())  hmSel = 1;
            for (int i = 0; i < 2; i++) {
                float t = (hover == i) ? 1.10f : 1.0f;
                hoverSc[i] += (t - hoverSc[i]) * 0.18f;
            }
        } else {
            /* popup state */
            if (Input::isBPressed()) { state = 0; }
            if (ir.valid) {
                for (int i = 0; i < 2; i++) {
                    if (irX >= pbcx[i] - PBW * 0.5f && irX <= pbcx[i] + PBW * 0.5f &&
                        irY >= pbcy    - PBH * 0.5f && irY <= pbcy    + PBH * 0.5f)
                        popHover = i;
                }
            }
            /* Select sound: fire once when IR cursor first enters a popup button */
            if (popHover >= 0 && popHover != prevHoverPop)
                SoundFX::play(SoundFX::FX::Select);
            prevHoverPop = popHover;

            if (popHover >= 0) popSel = popHover;
            if (Input::isAJustPressed() && (!ir.valid || popHover >= 0)) {
                int sel = (popHover >= 0) ? popHover : popSel;
                if (sel == 0) { /* Yes */
                    SoundFX::play(SoundFX::FX::MenuExit);
                    SoundFX::waitDone(SoundFX::FX::MenuExit);
                    if (confirmFor == 1) g_restartApp = true;
                    return true;
                } else {        /* No */
                    SoundFX::play(SoundFX::FX::Back);
                    state = 0;
                }
            }
            if (Input::isLeftPressed()  || Input::isUpPressed())   popSel = 0;
            if (Input::isRightPressed() || Input::isDownPressed())  popSel = 1;
            for (int i = 0; i < 2; i++) {
                float t = (popHover == i) ? 1.08f : 1.0f;
                popHoverSc[i] += (t - popHoverSc[i]) * 0.18f;
            }
        }

        /* ---- Update animations ---- */
        if (openAnim < 1.0f) openAnim += 0.075f;
        if (openAnim > 1.0f) openAnim = 1.0f;
        float oc = openAnim * openAnim * (3.0f - 2.0f * openAnim); /* smoothstep */

        if (state == 1) {
            if (popAnim < 1.0f) popAnim += 0.12f;
            if (popAnim > 1.0f) popAnim = 1.0f;
        }
        float pc = popAnim * popAnim * (3.0f - 2.0f * popAnim);

        int slideH = (int)((1.0f - oc) * 90);
        int slideF = (int)((1.0f - oc) * 60);

        /* ---- Draw HOME menu ---- */
        const Ui::Palette& P = Ui::pal();
        Ui::background(false);

        /* Top bar slides down, bottom bar (clock) slides up */
        Ui::pushOffset(0, -slideH);
        Ui::shadow(Ui::screenLeft() - 20, -30, Ui::screenWidth() + 40, 92, 22, 8.0f, P.shadow);
        Ui::roundRect(Ui::screenLeft() - 20, -30, Ui::screenWidth() + 40, 92, 22, P.barTop, P.barBottom);
        Ui::roundBorder(Ui::screenLeft() - 20, -30, Ui::screenWidth() + 40, 92, 22, 1.5f, P.barBorder);
        Ui::text(28, 17, "HOME Menu", 26, P.text);
        {
            const Ui::Hint close = { "B", "Close" };
            Ui::hint(626 - Ui::hintWidth(close), 22, close);
        }
        Ui::popOffset();

        Ui::pushOffset(0, slideF);
        {
            const Ui::Hint l[] = { { "A", "Confirm" } };
            const Ui::Hint r[] = { { "B", "Close" } };
            Ui::bottomBar(l, 1, r, 1);
        }
        Ui::popOffset();

        /* Wii Menu / Reset buttons */
        for (int i = 0; i < 2; i++) {
            float sc = hoverSc[i];
            float dw = BTN_W * sc, dh = BTN_H * sc;
            bool  sel = (hmSel == i);
            Ui::button(bcx[i] - dw * 0.5f, bcy - dh * 0.5f, dw, dh,
                       i == 0 ? "Wii Menu" : "Reset", (int)(22 * sc),
                       sel ? Ui::pulse() : 0.0f);
        }

        /* ---- Draw confirmation popup (state == 1) ---- */
        if (state == 1 && pc > 0.01f) {
            GRRLIB_Rectangle(Ui::screenLeft(), 0, Ui::screenWidth(), 480, Ui::alpha(P.dim, pc), 1);

            /* Pop-in: scale from 0.82 → 1.0 around screen centre */
            float psc = 0.82f + 0.18f * pc;
            float adw = DW * psc, adh = DH * psc;
            float adx = 320 - adw * 0.5f, ady = 240 - adh * 0.5f;
            Ui::card(adx, ady, adw, adh, 20 * psc, 0.0f);

            const char* line1 = (confirmFor == 0)
                ? "Return to the Wii Menu?"
                : "Reset the application?";
            Ui::textCentered(320, ady + 32 * psc, line1, (int)(20 * psc), P.text);
            Ui::textCentered(320, ady + 62 * psc, "(Anything not saved will be lost.)",
                             (int)(14 * psc), P.textDim);

            /* Yes / No buttons */
            for (int i = 0; i < 2; i++) {
                float bsc = popHoverSc[i] * psc;
                float bdw = PBW * bsc, bdh = PBH * bsc;
                /* Scale positions relative to screen centre */
                float bx = 320 + (pbcx[i] - 320.0f) * psc - bdw * 0.5f;
                float by = 240 + (pbcy    - 240.0f) * psc - bdh * 0.5f;
                bool  bsel = (popSel == i);
                Ui::button(bx, by, bdw, bdh, i == 0 ? "Yes" : "No", (int)(19 * psc),
                           bsel ? Ui::pulse() : 0.0f);
            }
        }

        /* Fade in from black while opening */
        if (oc < 1.0f)
            GRRLIB_Rectangle(Ui::screenLeft(), 0, Ui::screenWidth(), 480, (u32)((1.0f - oc) * 255.0f), 1);

        /* IR cursor — always on top */
        if (ir.valid && cursorPointerTex)
            GRRLIB_DrawImg((int)irX - 10, (int)irY - 4,
                           cursorPointerTex, orient.roll, 1, 1, 0xFFFFFFFF);

        GRRLIB_Render();
    }
}
