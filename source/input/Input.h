#pragma once
#include <wiiuse/wpad.h>

/* -----------------------------------------------------------------------
 * Input — every controller, read as a Wii Remote.
 *
 * The Classic Controller (also what the Wii U GamePad is in a Virtual
 * Console inject) and the GameCube controller in port 1 are translated to
 * Wii Remote buttons, so screens only test WPAD_BUTTON_* bits:
 *
 *   Wii Remote   Classic / GamePad          GameCube
 *   A / B        A / B                      A / B
 *   D-pad        D-pad, left stick          D-pad, stick
 *   - / +        - / +, L / R               L / R
 *   1 / 2        Y / X                      Y / X
 *   HOME         HOME                       START
 *   (zoom)       ZL / ZR                    Z         -> BTN_ZOOM
 *
 * The is*Pressed() directions repeat while held (menus, lists, keyboard);
 * rawDown() does not (the player: a held D-pad must not seek again and
 * again).  No pointer without a Wii Remote: screens fall back to the
 * D-pad when ir.valid is false.
 * ----------------------------------------------------------------------- */
class Input {
public:
    /* Video zoom (Fit / Fill), no Wii Remote button for it */
    static const u32 BTN_ZOOM = 1u << 30;

    static void update();
    /* Wii Remote pointer in drawing space (see Ui::pointerToScreen). */
    static void readIR(ir_t& ir);

    /* Buttons pressed this frame, Wii Remote bits, no repeat. */
    static u32 rawDown();
    /* Buttons held, Wii Remote bits. */
    static u32 held();

    static bool isHomePressed();
    static bool isUpPressed();
    static bool isDownPressed();
    static bool isLeftPressed();
    static bool isRightPressed();
    static bool isAJustPressed();
    static bool isBPressed();
    static bool isBackPressed();  // alias for B
    static bool isLPressed();     // -
    static bool isRPressed();     // +
    /* + as an action of its own (Watched, Browse, the keyboard's Enter), not
     * the next page: Z too, a button where the GameCube's R is a trigger
     * (hints: "+!") */
    static bool isActionPressed();
    static bool is1Pressed();
    static bool is2Pressed();
};
