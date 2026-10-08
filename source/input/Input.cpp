#include "Input.h"
#include "../ui/Ui.h"
#include <wiiuse/wpad.h>
#include <ogc/pad.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/system.h>

static u32 rawDownBits = 0;   /* pressed this frame, no repeat */
static u32 heldBits    = 0;
static u32 buttonsDown = 0;   /* rawDownBits + repeated directions */

static const u32 DIRS = WPAD_BUTTON_UP | WPAD_BUTTON_DOWN | WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT;

/* Classic Controller bits (upper half of the WPAD word) -> Wii Remote bits */
static u32 fromClassic(u32 c)
{
    static const struct { u32 from, to; } MAP[] = {
        { WPAD_CLASSIC_BUTTON_UP,    WPAD_BUTTON_UP },
        { WPAD_CLASSIC_BUTTON_DOWN,  WPAD_BUTTON_DOWN },
        { WPAD_CLASSIC_BUTTON_LEFT,  WPAD_BUTTON_LEFT },
        { WPAD_CLASSIC_BUTTON_RIGHT, WPAD_BUTTON_RIGHT },
        { WPAD_CLASSIC_BUTTON_A,     WPAD_BUTTON_A },
        { WPAD_CLASSIC_BUTTON_B,     WPAD_BUTTON_B },
        { WPAD_CLASSIC_BUTTON_Y,     WPAD_BUTTON_1 },
        { WPAD_CLASSIC_BUTTON_X,     WPAD_BUTTON_2 },
        { WPAD_CLASSIC_BUTTON_MINUS, WPAD_BUTTON_MINUS },
        { WPAD_CLASSIC_BUTTON_PLUS,  WPAD_BUTTON_PLUS },
        { WPAD_CLASSIC_BUTTON_FULL_L, WPAD_BUTTON_MINUS },
        { WPAD_CLASSIC_BUTTON_FULL_R, WPAD_BUTTON_PLUS },
        { WPAD_CLASSIC_BUTTON_HOME,  WPAD_BUTTON_HOME },
        { WPAD_CLASSIC_BUTTON_ZL,    Input::BTN_ZOOM },
        { WPAD_CLASSIC_BUTTON_ZR,    Input::BTN_ZOOM },
    };
    u32 out = 0;
    for (const auto& m : MAP) if (c & m.from) out |= m.to;
    return out;
}

/* The upper half holds the expansion's buttons: only the Classic
 * Controller's are wanted (a Nunchuk's C/Z use the same bits). */
static u32 fromWpad(u32 w, bool classic)
{
    return (w & 0xFFFF) | (classic ? fromClassic(w & 0xFFFF0000u) : 0);
}

static u32 fromGameCube(u16 p)
{
    static const struct { u16 from; u32 to; } MAP[] = {
        { PAD_BUTTON_UP,    WPAD_BUTTON_UP },
        { PAD_BUTTON_DOWN,  WPAD_BUTTON_DOWN },
        { PAD_BUTTON_LEFT,  WPAD_BUTTON_LEFT },
        { PAD_BUTTON_RIGHT, WPAD_BUTTON_RIGHT },
        { PAD_BUTTON_A,     WPAD_BUTTON_A },
        { PAD_BUTTON_B,     WPAD_BUTTON_B },
        { PAD_BUTTON_Y,     WPAD_BUTTON_1 },
        { PAD_BUTTON_X,     WPAD_BUTTON_2 },
        { PAD_TRIGGER_L,    WPAD_BUTTON_MINUS },
        { PAD_TRIGGER_R,    WPAD_BUTTON_PLUS },
        { PAD_BUTTON_START, WPAD_BUTTON_HOME },
        { PAD_TRIGGER_Z,    Input::BTN_ZOOM },
    };
    u32 out = 0;
    for (const auto& m : MAP) if (p & m.from) out |= m.to;
    return out;
}

/* GameCube controller state, with our own press detection: when a read
 * gets no answer, libogc clears the pad's state, and a button still held
 * came back as a new press at the next answer (about every 130 ms in
 * Dolphin): one A on "Connect To Jellyfin" also picked the first profile.
 * A read without an answer now keeps the last state. */
static u16  s_padHeld = 0;
static bool s_padUp   = false;            /* last read answered */

/* Sticks pushed past half way count as the D-pad. */
static u32 stickDirs()
{
    u32 d = 0;
    WPADData* wd = WPAD_Data(WPAD_CHAN_0);
    if (wd && wd->exp.type == WPAD_EXP_CLASSIC) {
        const joystick_t& js = wd->exp.classic.ljs;
        if (js.mag > 0.5f) {
            float a = js.ang;                     /* 0 up, 90 right */
            if      (a >= 315 || a < 45) d |= WPAD_BUTTON_UP;
            else if (a < 135)            d |= WPAD_BUTTON_RIGHT;
            else if (a < 225)            d |= WPAD_BUTTON_DOWN;
            else                         d |= WPAD_BUTTON_LEFT;
        }
    }
    int x = s_padUp ? PAD_StickX(0) : 0, y = s_padUp ? PAD_StickY(0) : 0;
    const int T = 48;                             /* full tilt is about 100 */
    if (x * x + y * y > T * T) {
        if (y > (x < 0 ? -x : x))       d |= WPAD_BUTTON_UP;
        else if (-y > (x < 0 ? -x : x)) d |= WPAD_BUTTON_DOWN;
        else if (x > 0)                 d |= WPAD_BUTTON_RIGHT;
        else                            d |= WPAD_BUTTON_LEFT;
    }
    return d;
}

void Input::update() {
    static bool padInit = false;
    if (!padInit) { PAD_Init(); padInit = true; }
    WPAD_ScanPads();
    bool padUp = (PAD_ScanPads() & 1) != 0;
    u64 nowMs = ticks_to_millisecs(gettime());
    static u64 lastAnswer = 0, lastDropReport = 0;
    static u32 dropouts = 0;
    if (padUp) lastAnswer = nowMs;
    else if (s_padUp) ++dropouts;
    if (dropouts && nowMs - lastDropReport > 5000) {
        SYS_Report("[Input] GameCube controller: %u read(s) without answer\n", (unsigned)dropouts);
        dropouts = 0;
        lastDropReport = nowMs;
    }
    s_padUp = padUp;
    /* no answer for 0.5 s: unplugged, nothing is held any more */
    u16 padHeld = padUp ? PAD_ButtonsHeld(0) : (nowMs - lastAnswer < 500 ? s_padHeld : 0);
    u16 pDown   = padHeld & ~s_padHeld;
    s_padHeld   = padHeld;

    static u32 prevStick = 0;
    u32 stick = stickDirs();
    WPADData* wd = WPAD_Data(WPAD_CHAN_0);
    bool classic = wd && wd->exp.type == WPAD_EXP_CLASSIC;
    u32 wDown = WPAD_ButtonsDown(0);
    u32 down = fromWpad(wDown, classic) | fromGameCube(pDown) | (stick & ~prevStick);
    /* the hints follow the controller used last */
    if (pDown || (s_padUp && (stick & ~prevStick) && !classic))
        Ui::setButtonStyle(Ui::ButtonStyle::GameCube);
    else if (classic && ((wDown & 0xFFFF0000u) || (stick & ~prevStick)))
        Ui::setButtonStyle(Ui::ButtonStyle::Classic);
    else if (wDown & 0xFFFF)
        Ui::setButtonStyle(Ui::ButtonStyle::WiiRemote);
    /* which controller sent what: for controller bug reports */
    if (down & ~DIRS)
        SYS_Report("[Input] pressed 0x%x (remote 0x%x, gamecube 0x%x, stick 0x%x)\n",
                   (unsigned)down, (unsigned)wDown, (unsigned)pDown, (unsigned)(stick & ~prevStick));
    u32 held = fromWpad(WPAD_ButtonsHeld(0), classic) | fromGameCube(padHeld) | stick;
    prevStick = stick;

    /* A held direction repeats: after 400 ms, then every 90 ms */
    static u64 heldSince[4], lastRepeat[4];
    static const u32 BIT[4] = { WPAD_BUTTON_UP, WPAD_BUTTON_DOWN, WPAD_BUTTON_LEFT, WPAD_BUTTON_RIGHT };
    u64 now = ticks_to_millisecs(gettime());
    u32 repeat = 0;
    for (int i = 0; i < 4; ++i) {
        if (!(held & BIT[i]))      { heldSince[i] = 0; continue; }
        if (down & BIT[i] || !heldSince[i]) { heldSince[i] = lastRepeat[i] = now; continue; }
        if (now - heldSince[i] >= 400 && now - lastRepeat[i] >= 90) {
            lastRepeat[i] = now;
            repeat |= BIT[i];
        }
    }

    rawDownBits = down;
    heldBits    = held;
    buttonsDown = down | (repeat & DIRS);
}

u32  Input::rawDown()        { return rawDownBits; }
u32  Input::held()           { return heldBits; }
bool Input::isHomePressed()  { return buttonsDown & WPAD_BUTTON_HOME; }
bool Input::isUpPressed()    { return buttonsDown & WPAD_BUTTON_UP; }
bool Input::isDownPressed()  { return buttonsDown & WPAD_BUTTON_DOWN; }
bool Input::isLeftPressed()  { return buttonsDown & WPAD_BUTTON_LEFT; }
bool Input::isRightPressed() { return buttonsDown & WPAD_BUTTON_RIGHT; }
bool Input::isAJustPressed() { return buttonsDown & WPAD_BUTTON_A; }
bool Input::isBPressed()     { return buttonsDown & WPAD_BUTTON_B; }
bool Input::isBackPressed()  { return buttonsDown & WPAD_BUTTON_B; }
bool Input::isLPressed()     { return buttonsDown & WPAD_BUTTON_MINUS; }
bool Input::isRPressed()     { return buttonsDown & WPAD_BUTTON_PLUS; }
bool Input::isActionPressed() { return buttonsDown & (WPAD_BUTTON_PLUS | BTN_ZOOM); }
bool Input::is1Pressed()     { return buttonsDown & WPAD_BUTTON_1; }
bool Input::is2Pressed()     { return buttonsDown & WPAD_BUTTON_2; }

void Input::readIR(ir_t& ir) {
    WPAD_IR(WPAD_CHAN_0, &ir);
    Ui::pointerToScreen(ir);
}
