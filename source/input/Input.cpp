#include "Input.h"
#include "GCAdapter.h"
#include "InputDrivers.h"
#include "Tablet.h"
#include "UsbKeyboard.h"
#include "../ui/Ui.h"
#include <wiiuse/wpad.h>
#include <ogc/ios.h>
#include <ogc/pad.h>
#include <ogc/usb.h>
#include <ogc/usbmouse.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/system.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#ifdef HAVE_WUPC
#include <wupc/wupc.h>
#endif
#ifdef HAVE_WIIDRC
#include <wiidrc/wiidrc.h>
#endif
#ifdef HAVE_SICKSAXIS
extern "C" {
#include <sicksaxis.h>
}
#endif

static u32 rawDownBits = 0;   /* pressed this frame, no repeat */
static u32 heldBits    = 0;
static u32 buttonsDown = 0;   /* rawDownBits + repeated directions */
static bool actionDown = false;   /* isActionPressed */

static const u32 DIRS = WPAD_BUTTON_UP | WPAD_BUTTON_DOWN | WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT;

/* Classic Controller bits (upper half of the WPAD word, and the Wii U Pro
 * Controller's) -> Wii Remote bits */
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

#ifdef HAVE_WIIDRC
static u32 fromGamePad(u32 b)
{
    static const struct { u32 from, to; } MAP[] = {
        { WIIDRC_BUTTON_UP,    WPAD_BUTTON_UP },
        { WIIDRC_BUTTON_DOWN,  WPAD_BUTTON_DOWN },
        { WIIDRC_BUTTON_LEFT,  WPAD_BUTTON_LEFT },
        { WIIDRC_BUTTON_RIGHT, WPAD_BUTTON_RIGHT },
        { WIIDRC_BUTTON_A,     WPAD_BUTTON_A },
        { WIIDRC_BUTTON_B,     WPAD_BUTTON_B },
        { WIIDRC_BUTTON_Y,     WPAD_BUTTON_1 },
        { WIIDRC_BUTTON_X,     WPAD_BUTTON_2 },
        { WIIDRC_BUTTON_MINUS, WPAD_BUTTON_MINUS },
        { WIIDRC_BUTTON_PLUS,  WPAD_BUTTON_PLUS },
        { WIIDRC_BUTTON_L,     WPAD_BUTTON_MINUS },
        { WIIDRC_BUTTON_R,     WPAD_BUTTON_PLUS },
        { WIIDRC_BUTTON_HOME,  WPAD_BUTTON_HOME },
        { WIIDRC_BUTTON_ZL,    Input::BTN_ZOOM },
        { WIIDRC_BUTTON_ZR,    Input::BTN_ZOOM },
    };
    u32 out = 0;
    for (const auto& m : MAP) if (b & m.from) out |= m.to;
    return out;
}
#endif

/* GameCube controller state, with our own press detection: when a read
 * gets no answer, libogc clears the pad's state, and a button still held
 * came back as a new press at the next answer (about every 130 ms in
 * Dolphin): one A on "Connect To Jellyfin" also picked the first profile.
 * A read without an answer now keeps the last state.  The same for each
 * of the four ports. */
static u16  s_padHeld[PAD_CHANMAX] = {};
static bool s_padUp[PAD_CHANMAX]   = {};   /* last read answered */

/* The strongest left stick over every device, -1..1, y up (libwiiinput's
 * ranges: GameCube 70, Wii U Pro and GamePad 900, DualShock 3 110). */
static float s_stickX, s_stickY, s_stickMag;
enum class Src { None, Remote, Classic, GameCube };
static Src s_stickSrc;

static void stickFeed(Src src, float x, float y)
{
    float m = sqrtf(x * x + y * y);
    if (m > s_stickMag) { s_stickX = x; s_stickY = y; s_stickMag = m; s_stickSrc = src; }
}

static void joyFeed(Src src, const joystick_t& js)
{
    if (js.mag < 0.01f) return;
    float a = js.ang * 0.017453292f;               /* 0 up, 90 right */
    stickFeed(src, sinf(a) * js.mag, cosf(a) * js.mag);
}

/* Sticks pushed past half way count as the D-pad. */
static u32 stickDirs()
{
    if (s_stickMag <= 0.5f) return 0;
    float ax = fabsf(s_stickX);
    if (s_stickY > ax)       return WPAD_BUTTON_UP;
    if (-s_stickY > ax)      return WPAD_BUTTON_DOWN;
    return s_stickX > 0 ? WPAD_BUTTON_RIGHT : WPAD_BUTTON_LEFT;
}

/* ---- the pointer: Wii Remote, pen, mouse ------------------------------- */

static int   s_pointerChan = 0;        /* the Wii Remote pointing last */
static bool  s_mouseOn = false;        /* moved or clicked, nothing else used since */
static int   s_mouseX = 320, s_mouseY = 240;   /* raw Wii Remote space, 640x480 */
static bool  s_penOn = false;          /* a pen near a tablet or over a uDraw */
static int   s_penX = 320, s_penY = 240;
static bool  s_usbTablet = false;      /* a USB tablet was opened: IOS reload on exit */

static int clampi(int v, int lo, int hi) { return v < lo ? lo : (v > hi ? hi : v); }

/* SYS_Report for the drivers' logs */
static int report(const char* fmt, ...)
{
    char line[160];
    va_list ap;
    va_start(ap, fmt);
    int n = vsnprintf(line, sizeof(line), fmt, ap);
    va_end(ap);
    SYS_Report("[Input] %s", line);
    return n;
}

#ifdef HAVE_SICKSAXIS
static struct ss_device s_ds3;
static bool s_ds3Init = false;
static u32  s_ds3Prev = 0;
/* Opening the DS3 walks the USB device list through IOS: retry about every
 * two seconds, not every frame. */
static int  s_ds3Retry = 0;
#endif

/* The typing screen asked last frame (typeInto) */
static bool s_typing = false, s_typingAsked = false;

static bool s_inited = false;

void Input::init() {
    if (s_inited) return;
    s_inited = true;
    USB_Initialize();
    PAD_Init();
#ifdef HAVE_WUPC
    WUPC_Init();
#endif
    indrv_wpadqueue_link();
    WPAD_Init();
    /* the four Wii Remotes: the Balance Board's channel is never read */
    for (int ch = WPAD_CHAN_0; ch <= WPAD_CHAN_3; ++ch) {
        WPAD_SetDataFormat(ch, WPAD_FMT_BTNS_ACC_IR);
        WPAD_SetVRes(ch, 640, 480);
    }
#ifdef HAVE_WIIDRC
    WiiDRC_Init();
#endif
#ifdef HAVE_SICKSAXIS
    if (ss_init() >= 0 && ss_initialize(&s_ds3) >= 0) {
        s_ds3Init = true;
        if (ss_open(&s_ds3) > 0) ss_start_reading(&s_ds3);
    }
#endif
    GCA_SetLog(report);
    indrv_gcadapter_init(true);
    /* no keyboard: not fatal, nor is no mouse (both are picked up when
     * plugged in later) */
    if (KB_Init() < 0) SYS_Report("[Input] USB keyboard driver did not start\n");
    MOUSE_Init();
    TB_SetLog(report);
    TB_Init();
}

void Input::shutdown() {
    if (!s_inited) return;
    s_inited = false;
    TB_Shutdown();
    MOUSE_Deinit();
    KB_Shutdown();
    indrv_gcadapter_shutdown();
#ifdef HAVE_SICKSAXIS
    if (s_ds3Init) {
        if (ss_is_connected(&s_ds3)) ss_stop_reading(&s_ds3);
        ss_close(&s_ds3);
        s_ds3Init = false;
    }
#endif
#ifdef HAVE_WUPC
    WUPC_Shutdown();
#endif
    WPAD_Shutdown();
    /* a tablet's read stays queued in IOS while the pen is away: what
     * starts next must not get its answer */
    if (s_usbTablet) IOS_ReloadIOS(IOS_GetVersion());
}

void Input::update() {
    static bool padInit = false;
    if (!padInit) { init(); padInit = true; }

    /* USB keyboard: the keys that type are text while a screen takes it */
    s_typing = s_typingAsked;
    s_typingAsked = false;
    KB_SetTyping(s_typing);
    KB_Update();
    if (!s_typing) { kb_key k; while (KB_Pop(&k)) {} }   /* nobody to type to */
    const u32 kDown = KB_PadDown(), kHeld = KB_PadHeld();

    /* USB mouse: every movement since the last frame */
    int mdx = 0, mdy = 0, wheel = 0;
    static u8 mHeld = 0;
    u8 mPrev = mHeld;
    if (MOUSE_IsConnected()) {
        mouse_event ev;
        for (int guard = 0; guard < 64 && MOUSE_GetEvent(&ev) > 0; ++guard) {
            mdx += ev.rx; mdy += ev.ry; wheel += ev.rz;
            mHeld = ev.button & 7;
        }
    } else {
        mHeld = 0;
        s_mouseOn = false;
    }
    const u8 mDown = mHeld & ~mPrev;
    s_mouseX = clampi(s_mouseX + mdx, 0, 639);
    s_mouseY = clampi(s_mouseY + mdy, 0, 479);
    if (mdx || mdy || mDown || wheel) s_mouseOn = true;
    auto fromMouse = [](u8 b) {
        return (b & 1 ? WPAD_BUTTON_A : 0) | (b & 2 ? WPAD_BUTTON_B : 0) | (b & 4 ? BTN_ZOOM : 0);
    };
    /* the wheel: one step up or down per frame it turns */
    const u32 mouseDown = fromMouse(mDown) | (wheel < 0 ? WPAD_BUTTON_UP : wheel > 0 ? WPAD_BUTTON_DOWN : 0);
    const u32 mouseHeld = fromMouse(mHeld);

    s_stickX = s_stickY = s_stickMag = 0;
    s_stickSrc = Src::None;

    /* every device's presses (padDown) and held buttons (padHeld), + as an
     * action of its own (plusDown), and which kind of controller sent them */
    u32 padDown = 0, padHeld = 0, plusDown = 0;
    bool usedGc = false, usedClassic = false, usedRemote = false;

#ifdef HAVE_WUPC
    WUPC_UpdateButtonStats();
    for (int ch = 0; ch < 4; ++ch) {
        if (!WUPC_Data(ch)) continue;
        const u32 d = WUPC_ButtonsDown(ch);
        padDown |= fromClassic(d);
        padHeld |= fromClassic(WUPC_ButtonsHeld(ch));
        if (d & WPAD_CLASSIC_BUTTON_PLUS) plusDown |= WPAD_BUTTON_PLUS;
        if (d) usedClassic = true;
        stickFeed(Src::Classic, WUPC_lStickX(ch) / 900.0f, WUPC_lStickY(ch) / 900.0f);
    }
#endif

    WPAD_ScanPads();
    /* a Wii Remote connecting (later than WiiFin, or back from sleep) is in
     * buttons-only mode: libogc sets the pointer's mode only on those there
     * when asked; so ask again for each one that comes */
    static bool remoteThere[WPAD_BALANCE_BOARD] = {};
    for (int ch = WPAD_CHAN_0; ch < WPAD_BALANCE_BOARD; ++ch) {
        u32 type;
        const bool there = WPAD_Probe(ch, &type) == WPAD_ERR_NONE;
        if (there && !remoteThere[ch]) {
            WPAD_SetDataFormat(ch, WPAD_FMT_BTNS_ACC_IR);
            WPAD_SetVRes(ch, 640, 480);
            SYS_Report("[Input] Wii Remote %d connected: pointer on\n", ch + 1);
        }
        remoteThere[ch] = there;
    }

    /* GameCube ports: a port with no answer for 0.5 s is unplugged, nothing
     * is held any more */
    const u32 padsUp = PAD_ScanPads();
    u64 nowMs = ticks_to_millisecs(gettime());
    static u64 lastAnswer[PAD_CHANMAX] = {}, lastDropReport = 0;
    static u32 dropouts = 0;
    u16 pDownAll = 0;
    for (int port = 0; port < PAD_CHANMAX; ++port) {
        const bool up = (padsUp & (1u << port)) != 0;
        if (up) lastAnswer[port] = nowMs;
        else if (s_padUp[port]) ++dropouts;
        s_padUp[port] = up;
        const u16 h = up ? PAD_ButtonsHeld(port) : (nowMs - lastAnswer[port] < 500 ? s_padHeld[port] : 0);
        const u16 d = h & ~s_padHeld[port];
        s_padHeld[port] = h;
        pDownAll |= d;
        padDown |= fromGameCube(d);
        padHeld |= fromGameCube(h);
        if (up) stickFeed(Src::GameCube, PAD_StickX(port) / 70.0f, PAD_StickY(port) / 70.0f);
    }
    if (pDownAll) usedGc = true;
    if (dropouts && nowMs - lastDropReport > 5000) {
        SYS_Report("[Input] GameCube controller: %u read(s) without answer\n", (unsigned)dropouts);
        dropouts = 0;
        lastDropReport = nowMs;
    }

    /* The GameCube adapter for Wii U: its ports like the console's own */
    indrv_gcadapter_update();
    static u16 gcaPrev[4] = {};
    for (int port = 0; port < 4; ++port) {
        gca_pad p;
        if (!GCA_Pad(port, &p)) { gcaPrev[port] = 0; continue; }
        const u16 d = p.buttons & ~gcaPrev[port];
        gcaPrev[port] = p.buttons;
        pDownAll |= d;
        padDown |= fromGameCube(d);
        padHeld |= fromGameCube(p.buttons);
        if (d) usedGc = true;
        stickFeed(Src::GameCube, p.stick_x / 70.0f, p.stick_y / 70.0f);
    }

#ifdef HAVE_WIIDRC
    if (WiiDRC_Inited() && WiiDRC_Connected() && WiiDRC_ScanPads()) {
        const u32 d = WiiDRC_ButtonsDown();
        padDown |= fromGamePad(d);
        padHeld |= fromGamePad(WiiDRC_ButtonsHeld());
        if (d & WIIDRC_BUTTON_PLUS) plusDown |= WPAD_BUTTON_PLUS;
        if (d) usedClassic = true;
        stickFeed(Src::Classic, WiiDRC_lStickX() / 900.0f, WiiDRC_lStickY() / 900.0f);
    }
#endif

#ifdef HAVE_SICKSAXIS
    if (s_ds3Init && !ss_is_connected(&s_ds3)) {
        s_ds3Prev = 0;
        if (++s_ds3Retry >= 120) {
            s_ds3Retry = 0;
            if (ss_open(&s_ds3) > 0) ss_start_reading(&s_ds3);
        }
    }
    if (s_ds3Init && ss_is_connected(&s_ds3)) {
        const struct SS_BUTTONS& b = s_ds3.pad.buttons;
        u32 now = 0;
        if (b.start)            now |= WPAD_BUTTON_PLUS;
        if (b.R1)               now |= WPAD_BUTTON_PLUS;
        if (b.select || b.L1)   now |= WPAD_BUTTON_MINUS;
        if (b.L2 || b.R2)       now |= BTN_ZOOM;
        if (b.cross)    now |= WPAD_BUTTON_A;
        if (b.circle)   now |= WPAD_BUTTON_B;
        if (b.square)   now |= WPAD_BUTTON_1;
        if (b.triangle) now |= WPAD_BUTTON_2;
        if (b.PS)       now |= WPAD_BUTTON_HOME;
        if (b.up)       now |= WPAD_BUTTON_UP;
        if (b.down)     now |= WPAD_BUTTON_DOWN;
        if (b.left)     now |= WPAD_BUTTON_LEFT;
        if (b.right)    now |= WPAD_BUTTON_RIGHT;
        const u32 d = now & ~s_ds3Prev;
        s_ds3Prev = now;
        padDown |= d;
        padHeld |= now;
        if (b.start && (d & WPAD_BUTTON_PLUS)) plusDown |= WPAD_BUTTON_PLUS;
        if (d) usedClassic = true;
        /* 0..255, 128 at rest, y down */
        stickFeed(Src::Classic, (s_ds3.pad.left_analog.x - 128) / 110.0f,
                  -(s_ds3.pad.left_analog.y - 128) / 110.0f);
    }
#endif

    /* Wii Remotes 2 to 4 */
    for (int ch = WPAD_CHAN_1; ch <= WPAD_CHAN_3; ++ch) {
        if (!remoteThere[ch]) continue;
        WPADData* cd = WPAD_Data(ch);
        if (!cd || cd->err != WPAD_ERR_NONE) continue;
        const bool cc = cd->exp.type == WPAD_EXP_CLASSIC;
        const u32 w = WPAD_ButtonsDown(ch);
        padDown |= fromWpad(w, cc);
        padHeld |= fromWpad(WPAD_ButtonsHeld(ch), cc);
        if ((w & WPAD_BUTTON_PLUS) || (cc && (w & WPAD_CLASSIC_BUTTON_PLUS))) plusDown |= WPAD_BUTTON_PLUS;
        if (cc && ((w & 0xFFFF0000u))) usedClassic = true;
        else if (w & 0xFFFF)           usedRemote = true;
        if (cc)                                     joyFeed(Src::Classic, cd->exp.classic.ljs);
        else if (cd->exp.type == WPAD_EXP_NUNCHUK) joyFeed(Src::Remote, cd->exp.nunchuk.js);
    }

    /* Wii Remote 1 */
    WPADData* wd = WPAD_Data(WPAD_CHAN_0);
    bool classic = wd && wd->exp.type == WPAD_EXP_CLASSIC;
    if (wd && classic)                              joyFeed(Src::Classic, wd->exp.classic.ljs);
    else if (wd && wd->exp.type == WPAD_EXP_NUNCHUK) joyFeed(Src::Remote, wd->exp.nunchuk.js);
    u32 wDown = WPAD_ButtonsDown(0);

    /* Pen tablets and the uDraw: the pointer while the pen is near */
    static u8 penPrev = 0;
    static int penRemote = -1;         /* the uDraw's Wii Remote, -1 a USB tablet */
    u8 pen = penPrev;
    tb_sample s;
    while (TB_Pop(&s)) {
        s_penOn = s.in_range;
        TB_ToScreen(&s, 640, 480, &s_penX, &s_penY);
        pen = s.in_range ? (s.buttons & (TB_BUTTON_TIP | TB_BUTTON_BARREL)) : 0;
        penRemote = s.wiimote;
        if (s.wiimote < 0) s_usbTablet = true;
    }
    /* a uDraw taken off its Wii Remote sends nothing more */
    if (s_penOn && penRemote >= 0 && !TB_UDrawConnected(penRemote)) { s_penOn = false; pen = 0; }
    const u8 penDown = pen & ~penPrev;
    penPrev = pen;
    auto fromPen = [](u8 b) {
        return (b & TB_BUTTON_TIP ? WPAD_BUTTON_A : 0) | (b & TB_BUTTON_BARREL ? WPAD_BUTTON_B : 0);
    };

    static u32 prevStick = 0;
    u32 stick = stickDirs();
    const u32 stickDown = stick & ~prevStick;
    if (stickDown) {
        if (s_stickSrc == Src::GameCube)     usedGc = true;
        else if (s_stickSrc == Src::Classic) usedClassic = true;
    }
    u32 down = fromWpad(wDown, classic) | padDown | stickDown | kDown | mouseDown | fromPen(penDown);
    /* the hints follow the controller used last */
    if (usedGc)
        Ui::setButtonStyle(Ui::ButtonStyle::GameCube);
    else if (usedClassic || (classic && (wDown & 0xFFFF0000u)))
        Ui::setButtonStyle(Ui::ButtonStyle::Classic);
    else if (usedRemote || (wDown & 0xFFFF) || (stickDown && s_stickSrc == Src::Remote))
        Ui::setButtonStyle(Ui::ButtonStyle::WiiRemote);
    /* which controller sent what: for controller bug reports */
    if (down & ~DIRS)
        SYS_Report("[Input] pressed 0x%x (remote 0x%x, gamecube 0x%x, stick 0x%x, keyboard 0x%x, mouse 0x%x)\n",
                   (unsigned)down, (unsigned)wDown, (unsigned)pDownAll, (unsigned)stickDown,
                   (unsigned)kDown, (unsigned)mouseDown);
    u32 held = fromWpad(WPAD_ButtonsHeld(0), classic) | padHeld | stick | kHeld | mouseHeld | fromPen(pen);
    prevStick = stick;

    /* the mouse pointer goes away once something else is used */
    if ((down & ~(mouseDown | fromPen(penDown))) && !(mdx || mdy || mDown)) s_mouseOn = false;

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

    /* + as an action: a Wii Remote's, Classic's, Pro's or GamePad's +
     * button, the DualShock 3's Start, or a zoom button (the GameCube's
     * Z), not the R triggers: they turn pages */
    actionDown = (wDown & WPAD_BUTTON_PLUS) || (classic && (wDown & WPAD_CLASSIC_BUTTON_PLUS)) ||
                 plusDown || (down & BTN_ZOOM);
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
bool Input::isActionPressed() { return actionDown; }
bool Input::is1Pressed()     { return buttonsDown & WPAD_BUTTON_1; }
bool Input::is2Pressed()     { return buttonsDown & WPAD_BUTTON_2; }

Input::Typed Input::typeInto(std::string& text, size_t maxLen) {
    s_typingAsked = true;
    if (!s_typing) return Typed::None;
    Typed r = Typed::None;
    kb_key k;
    while (KB_Pop(&k)) {
        if (k.code == 0x28 || k.code == 0x58) { r = Typed::Enter; break; }   /* Enter */
        if (k.code == 0x2A) {                                                 /* Backspace */
            if (!text.empty()) text.pop_back();
        } else if (k.ch && text.size() < maxLen) {
            text += k.ch;
        }
    }
    return r;
}

void Input::readIR(ir_t& ir) {
    /* the Wii Remote pointing at the screen, the last one first */
    for (int i = 0; i < 4; ++i) {
        const int ch = (s_pointerChan + i) % 4;
        WPAD_IR(ch, &ir);
        if (ir.valid) {
            s_pointerChan = ch;
            /* the mouse goes on from where the Wii Remote points */
            s_mouseX = clampi((int)ir.x, 0, 639);
            s_mouseY = clampi((int)ir.y, 0, 479);
            Ui::pointerToScreen(ir);
            return;
        }
    }
    if (s_penOn || s_mouseOn) {
        ir.valid = 1;
        ir.x = s_penOn ? s_penX : s_mouseX;
        ir.y = s_penOn ? s_penY : s_mouseY;
        ir.angle = 0;
        Ui::pointerToScreen(ir);
    }
}

float Input::pointerRoll() {
    ir_t ir;
    WPAD_IR(s_pointerChan, &ir);
    if (!ir.valid) return 0;
    orient_t o;
    WPAD_Orientation(s_pointerChan, &o);
    return o.roll;
}
