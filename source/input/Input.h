#pragma once
#include <wiiuse/wpad.h>
#include <string>

/* -----------------------------------------------------------------------
 * Input — every controller, read as a Wii Remote.
 *
 * The devices libwiiinput (github.com/quatric/libwiiinput) reads, its
 * logic taken over here: the four Wii Remotes (Nunchuk, Classic
 * Controller), the four GameCube ports, the Wii U Pro Controller, the Wii U
 * GamePad, a DualShock 3 on USB, the GameCube controller adapter for Wii U,
 * a USB keyboard and mouse, USB pen tablets and the uDraw GameTablet.  The
 * controllers are translated to Wii Remote buttons, so screens only test
 * WPAD_BUTTON_* bits:
 *
 *   Wii Remote   Classic / Pro / GamePad   GameCube   DualShock 3
 *   A / B        A / B                     A / B      Cross / Circle
 *   D-pad        D-pad, left stick         D-pad, stick  D-pad, left stick
 *   - / +        - / +, L / R              L / R      Select / Start, L1 / R1
 *   1 / 2        Y / X                     Y / X      Square / Triangle
 *   HOME         HOME                      START      PS
 *   (zoom)       ZL / ZR                   Z          L2 / R2  -> BTN_ZOOM
 *
 * Keyboard: arrows = D-pad, Enter = A, Backspace = B, X = 1, Y = 2,
 * Delete / Page Up = -, Page Down = +, Esc / Home = HOME (UsbKeyboard.h).
 * Mouse: the pointer, left = A, right = B, middle = zoom, the wheel = up /
 * down.  Pen tablet / uDraw: the pointer while the pen is near, its tip =
 * A, its (lower) side switch = B.
 *
 * The is*Pressed() directions repeat while held (menus, lists, keyboard);
 * rawDown() does not (the player: a held D-pad must not seek again and
 * again).  No pointer without a Wii Remote, mouse or pen: screens fall back
 * to the D-pad when ir.valid is false.
 * ----------------------------------------------------------------------- */
class Input {
public:
    /* Video zoom (Fit / Fill), no Wii Remote button for it */
    static const u32 BTN_ZOOM = 1u << 30;

    /* Every device, once (after WPAD_Init would have been called), and
     * before the app leaves (IOS shuts USB down). */
    static void init();
    static void shutdown();

    static void update();
    /* The pointer in drawing space (see Ui::pointerToScreen): a Wii
     * Remote's, else a pen's, else the mouse's. */
    static void readIR(ir_t& ir);
    /* How the pointer sprite is turned: the Wii Remote's roll, 0 for the
     * mouse and pens. */
    static float pointerRoll();

    /* Text typed on the USB keyboard since the last frame, into text (at
     * most maxLen bytes).  While a screen calls this every frame, the keys
     * that type leave the controller layout (Esc is then B). */
    enum class Typed { None, Enter };
    static Typed typeInto(std::string& text, size_t maxLen);

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
     * the next page: the + buttons and Z (ZL / ZR), not the R triggers,
     * which only turn pages (hints: "+!") */
    static bool isActionPressed();
    static bool is1Pressed();
    static bool is2Pressed();
};
