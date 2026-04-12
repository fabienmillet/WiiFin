#include "ConnectView.h"
#include "../input/Input.h"
#include "../core/SoundFX.h"
#include <wiikeyboard/keyboard.h>
#include <ogc/lwp.h>
#include <string.h>
#include <stdio.h>

// ---------------------------------------------------------------
// Keyboard layout  (2 pages)
// ---------------------------------------------------------------
const char* ConnectView::kbRows[7] = {
    // --- Page 0 : letters ---
    "1234567890-.",
    "qwertyuiop:/",
    "asdfghjkl@_ ",
    "\x01zxcvbnm,\x02\x7f",    // \x01=Shift  \x02=SYM  \x7f=Backspace
    // --- Page 1 : symbols ---
    "!?@#$%^&*()-",
    "_+=|\\[]{};:\"",
    "'<>./`~\x02\x7f",           // \x02=ABC (back to letters)
};

static const int KB_COLS_MAX = 12;

// USB keyboard callback — appends char to a shared buffer
static char usbChar = 0;
static void usbCallback(char c) { usbChar = c; }

// ---------------------------------------------------------------------------
// Background discovery thread
// ---------------------------------------------------------------------------
static u8                       s_discoverStack[32 * 1024];
static volatile bool            s_discoverDone;
static JellyfinClient*          s_discoverClient;
static std::vector<DiscoveredServer>* s_discoverOut;

static void* discoverWorker(void*) {
    s_discoverClient->discoverServers(*s_discoverOut);
    s_discoverDone = true;
    return nullptr;
}

// ---------------------------------------------------------------
ConnectView::ConnectView(GRRLIB_texImg* btn, GRRLIB_texImg* cursor,
                         GRRLIB_ttfFont* f, JellyfinClient& c)
    : btnTex(btn), cursorTex(cursor), font(f), client(c) {
    fields[0] = "";
    fields[1] = "";
    fields[2] = "";
    initUsbKeyboard();
}

void ConnectView::setFields(const std::string& url, const std::string& uname) {
    fields[0] = url;
    // Strip trailing slashes to prevent double-slash in API paths
    while (fields[0].size() > 1 && fields[0].back() == '/')
        fields[0].pop_back();
    fields[1] = uname;
    serverUrl  = fields[0];
}

void ConnectView::initUsbKeyboard() {
    if (KEYBOARD_Init(usbCallback) >= 0)
        usbKbInited = true;
}

void ConnectView::setStatus(const std::string& msg, bool isError) {
    statusMsg   = msg;
    statusError = isError;
    statusTimer = 180; // ~3 s at 60fps
}

// ---------------------------------------------------------------
// Main update loop
// ---------------------------------------------------------------
ConnectResult ConnectView::update(ir_t& ir) {
    // USB keyboard input
    if (usbKbInited) {
        keyboard_event ev;
        while (KEYBOARD_GetEvent(&ev) > 0) {
            if (ev.type == KEYBOARD_PRESSED && ev.symbol) {
                char c = (char)ev.symbol;
                int fi = (int)focusedField;
                if (fi <= 2) {
                    if (c == '\b' || c == 127) {
                        if (!fields[fi].empty()) fields[fi].pop_back();
                    } else if (c >= 32 && c < 127) {
                        fields[fi] += c;
                    }
                }
            }
        }
    }
    // USB callback char
    if (usbChar) {
        int fi = (int)focusedField;
        if (fi <= 2) {
            if (usbChar == '\b' || usbChar == 127) {
                if (!fields[fi].empty()) fields[fi].pop_back();
            } else if (usbChar >= 32 && usbChar < 127) {
                fields[fi] += usbChar;
            }
        }
        usbChar = 0;
    }

    // Status timer
    if (statusTimer > 0) statusTimer--;

    // B = cancel (or close VKB)
    if (Input::isBackPressed()) {
        if (kbActive) { kbActive = false; return ConnectResult::None; }
        return ConnectResult::Cancelled;
    }

    // --- Tab switching: IR click on tab headers OR L/R buttons ---
    // Tab zones (from renderBackground): Credentials ~x30-190 y50-74, QC ~x240-430 y50-74
    bool aPressed = Input::isAJustPressed();

    // IR hover: any valid IR position sets irMode so d-pad A is blocked while pointer is out
    if (ir.valid) irMode = true;

    bool irTabHandled = false;
    if (ir.valid && aPressed && ir.y >= 44 && ir.y <= 80) {
        if      (ir.x >= 10  && ir.x <= 195) { activeTab = Tab::Credentials;  kbActive = false; irTabHandled = true; }
        else if (ir.x >= 200 && ir.x <= 400) { activeTab = Tab::QuickConnect; kbActive = false; irTabHandled = true; }
        else if (ir.x >= 405 && ir.x <= 540) { activeTab = Tab::Discover;     kbActive = false; irTabHandled = true; }
    }
    if (!kbActive) {
        if (Input::isLPressed()) { activeTab = (Tab)(((int)activeTab - 1 + 3) % 3); kbActive = false; irMode = false; }
        if (Input::isRPressed()) { activeTab = (Tab)(((int)activeTab + 1) % 3);     kbActive = false; irMode = false; }
    }

    if (irTabHandled) return ConnectResult::None;

    if (activeTab == Tab::Credentials) {
        if (!kbActive) {
            // IR click: focus a field or submit
            if (ir.valid && aPressed) {
                bool irFieldHandled = false;
                // Field hit areas (from renderCredentials): y=110, y=180, y=250, button y=330
                if (ir.x >= 80 && ir.x <= 560) {
                    if      (ir.y >= 100 && ir.y <= 145) { focusedField = Field::Server;   kbActive = true; kbRow = 0; kbCol = 0; kbPage = 0; irFieldHandled = true; irMode = true; }
                    else if (ir.y >= 170 && ir.y <= 215) { focusedField = Field::Username;  kbActive = true; kbRow = 0; kbCol = 0; kbPage = 0; irFieldHandled = true; irMode = true; }
                    else if (ir.y >= 240 && ir.y <= 285) { focusedField = Field::Password;  kbActive = true; kbRow = 0; kbCol = 0; kbPage = 0; irFieldHandled = true; irMode = true; }
                }
                // Connect button: centered at x=320, y=330, 200x48
                if (!irFieldHandled &&
                    ir.x >= 220 && ir.x <= 420 &&
                    ir.y >= 330 && ir.y <= 378) {
                    focusedField = Field::SubmitBtn; irFieldHandled = true; irMode = true;
                    goto doSubmit;
                }
                if (irFieldHandled) return ConnectResult::None;
                return ConnectResult::None; // IR click missed all targets
            }

            // D-pad navigation
            if (Input::isDownPressed()) {
                int f = (int)focusedField;
                focusedField = (Field)((f + 1) % 4);
                irMode = false;
            }
            if (Input::isUpPressed()) {
                int f = (int)focusedField;
                focusedField = (Field)(((f - 1) + 4) % 4);
                irMode = false;
            }
            // A button: open VKB or submit (only when not in IR mode)
            if (aPressed && !irMode) {
                if (focusedField != Field::SubmitBtn) {
                    kbActive = true; kbRow = 0; kbCol = 0; kbPage = 0;
                } else {
                    doSubmit:
                    SoundFX::play(SoundFX::FX::Start);
                    if (!netReady) {
                        setStatus("Connecting to network...");
                        netReady = client.initNetwork();
                        if (!netReady) { setStatus(client.lastError(), true); return ConnectResult::None; }
                    }
                    setStatus("Authenticating...");
                    while (fields[0].size() > 1 && fields[0].back() == '/') fields[0].pop_back();
                    serverUrl = fields[0];
                    JellyfinAuth a;
                    if (client.authenticate(fields[0], fields[1], fields[2], a)) {
                        std::string sn;
                        if (client.getServerName(fields[0], a, sn)) a.serverName = sn;
                        auth = a;
                        username = fields[1];
                        return ConnectResult::Success;
                    } else {
                        setStatus(client.lastError(), true);
                    }
                }
            }
        } else {
            // VKB is active
            handleVKBInput(ir);
        }
    } else if (activeTab == Tab::QuickConnect) {
        // Quick Connect tab
        switch (qcState) {
            case QCState::Idle: {
                // QC button: cx=320, y=250, w=240, h=52 → x:200-440, y:250-302
                bool qcBtnHit = ir.valid &&
                                ir.x >= 200 && ir.x <= 440 &&
                                ir.y >= 250 && ir.y <= 302;
                if (qcBtnHit) irMode = true;
                if (aPressed && (qcBtnHit || (!ir.valid && !irMode))) {
                    if (fields[0].empty()) {
                        setStatus("Set the Server URL in the Credentials tab first.", true);
                        break;
                    }
                    if (!netReady) {
                        netReady = client.initNetwork();
                        if (!netReady) { setStatus(client.lastError(), true); break; }
                    }
                    while (fields[0].size() > 1 && fields[0].back() == '/') fields[0].pop_back();
                    serverUrl = fields[0];
                    if (client.quickConnectInitiate(serverUrl, qcResult)) {
                        qcState = QCState::Waiting;
                        qcPollTimer = 90;
                        setStatus("Enter code on another device:");
                    } else {
                        setStatus(client.lastError(), true);
                    }
                }
                break;
            }
            case QCState::Waiting:
                qcPollTimer--;
                if (qcPollTimer <= 0) {
                    qcPollTimer = 90;
                    if (client.quickConnectCheck(serverUrl, qcResult.secret, qcResult)) {
                        if (qcResult.authenticated) {
                            qcState = QCState::Done;
                            JellyfinAuth a;
                            if (client.quickConnectAuthenticate(serverUrl, qcResult.secret, a)) {
                                username = a.serverName; /* "Name" field = user display name, before overwrite */
                                std::string sn;
                                if (client.getServerName(serverUrl, a, sn)) a.serverName = sn;
                                auth = a;
                                return ConnectResult::Success;
                            } else {
                                setStatus(client.lastError(), true);
                                qcState = QCState::Error;
                            }
                        }
                    }
                }
                if (Input::isBPressed()) { qcState = QCState::Idle; }
                break;
            case QCState::Error:
                if (aPressed && (!irMode || ir.valid)) qcState = QCState::Idle;
                break;
            default: break;
        }
    } else {
        // Discover tab
        switch (discoverState) {
            case DiscoverState::Idle: {
                bool btnHit = ir.valid &&
                              ir.x >= 200 && ir.x <= 440 &&
                              ir.y >= 240 && ir.y <= 292;
                if (btnHit) irMode = true;
                if (aPressed && (btnHit || (!irMode && !ir.valid))) {
                    if (!netReady) {
                        netReady = client.initNetwork();
                        if (!netReady) { setStatus(client.lastError(), true); break; }
                    }
                    discoveredServers.clear();
                    discoverSelected = 0;
                    s_discoverClient = &client;
                    s_discoverOut    = &discoveredServers;
                    s_discoverDone   = false;
                    LWP_CreateThread(&discoverThread, discoverWorker, nullptr,
                                     s_discoverStack, sizeof(s_discoverStack), 64);
                    discoverState = DiscoverState::Scanning;
                }
                break;
            }
            case DiscoverState::Scanning:
                if (s_discoverDone) {
                    LWP_JoinThread(discoverThread, nullptr);
                    discoverThread = LWP_THREAD_NULL;
                    discoverState  = DiscoverState::Done;
                    if (discoveredServers.empty())
                        setStatus("No servers found on the local network.", false);
                }
                break;
            case DiscoverState::Done: {
                int n = (int)discoveredServers.size();
                if (n == 0) {
                    // Re-scan button (same hit zone as Scan)
                    bool btnHit = ir.valid &&
                                  ir.x >= 200 && ir.x <= 440 &&
                                  ir.y >= 240 && ir.y <= 292;
                    if (btnHit) irMode = true;
                    if (aPressed && (btnHit || (!irMode && !ir.valid)))
                        discoverState = DiscoverState::Idle;
                    break;
                }
                // Navigate list
                if (Input::isDownPressed()) { discoverSelected = (discoverSelected + 1) % n; irMode = false; }
                if (Input::isUpPressed())   { discoverSelected = (discoverSelected - 1 + n) % n; irMode = false; }

                bool doSelect = false;
                if (ir.valid) {
                    for (int i = 0; i < n && i < 6; i++) {
                        int rowY = 130 + i * 48;
                        if (ir.y >= rowY && ir.y <= rowY + 40 &&
                            ir.x >= 60  && ir.x <= 580) {
                            irMode = true;
                            if (aPressed) { discoverSelected = i; doSelect = true; }
                            break;
                        }
                    }
                }
                if (aPressed && !irMode) doSelect = true;

                if (doSelect && discoverSelected < n) {
                    fields[0] = discoveredServers[discoverSelected].address;
                    while (fields[0].size() > 1 && fields[0].back() == '/')
                        fields[0].pop_back();
                    serverUrl = fields[0];
                    activeTab = Tab::Credentials;
                    focusedField = Field::Username;
                    setStatus("Server URL set. Enter your credentials.", false);
                }
                break;
            }
        }
    }
    return ConnectResult::None;
}

// ---------------------------------------------------------------
// Virtual keyboard input
// ---------------------------------------------------------------
void ConnectView::handleVKBInput(ir_t& ir) {
    int pageStart = kbPage * 4;
    int pageRows  = (kbPage == 0) ? 4 : 3;
    int rowLen    = strlen(kbRows[pageStart + kbRow]);

    if (Input::isUpPressed()) {
        kbRow = (kbRow - 1 + pageRows) % pageRows;
        int nl = strlen(kbRows[pageStart + kbRow]);
        if (kbCol >= nl) kbCol = nl - 1;
    }
    if (Input::isDownPressed()) {
        kbRow = (kbRow + 1) % pageRows;
        int nl = strlen(kbRows[pageStart + kbRow]);
        if (kbCol >= nl) kbCol = nl - 1;
    }
    if (Input::isLeftPressed())  kbCol = (kbCol - 1 + rowLen) % rowLen;
    if (Input::isRightPressed()) kbCol = (kbCol + 1) % rowLen;

    // L button toggles caps lock (letters page only)
    if (Input::isLPressed() && kbPage == 0) kbShift = !kbShift;

    if (Input::isAJustPressed()) {
        int fi = (int)focusedField;
        if (fi > 2) { kbActive = false; return; }

        char key = 0;
        if (ir.valid) {
            for (int r = 0; r < pageRows && !key; r++) {
                int len = strlen(kbRows[pageStart + r]);
                for (int c = 0; c < len && !key; c++) {
                    int cx = KB_X + c * KB_CELLW;
                    int cy = KB_Y + r * KB_CELLH;
                    if (ir.x >= cx && ir.x <= cx + KB_CELLW - 2 &&
                        ir.y >= cy && ir.y <= cy + KB_CELLH - 2) {
                        key = kbRows[pageStart + r][c];
                        kbRow = r; kbCol = c;
                    }
                }
            }
        }
        if (!key) key = kbRows[pageStart + kbRow][kbCol];

        if (key == '\x01') {
            kbShift = !kbShift;
            SoundFX::play(SoundFX::FX::PressKey);
        } else if (key == '\x02') {
            kbPage = 1 - kbPage;
            kbRow = 0; kbCol = 0;
            kbShift = false;
            SoundFX::play(SoundFX::FX::PressKey);
        } else if (key == '\x7f') {
            if (!fields[fi].empty()) fields[fi].pop_back();
            SoundFX::play(SoundFX::FX::Backspace);
        } else if (key == ' ') {
            fields[fi] += ' ';
            SoundFX::play(SoundFX::FX::PressKey);
        } else {
            if (kbShift && key >= 'a' && key <= 'z')
                fields[fi] += (char)(key - 32);
            else
                fields[fi] += key;
            kbShift = false;
            SoundFX::play(SoundFX::FX::PressKey);
        }
    }

    if (Input::isBPressed()) { kbActive = false; return; }

    if (Input::isRPressed()) {
        kbActive = false;
        if (!fields[0].empty() && !fields[1].empty() && !fields[2].empty())
            focusedField = Field::SubmitBtn;
    }
}

// ---------------------------------------------------------------
// Render helpers
// ---------------------------------------------------------------
static void drawGradientBG(GRRLIB_ttfFont* font) {
    const int r1 = 0x1a, g1 = 0x1a, b1 = 0x2e;
    const int r2 = 0x16, g2 = 0x21, b2 = 0x3e;
    const int bands = 16, bh = 480 / bands;
    for (int i = 0; i < bands; i++) {
        float t  = i / (float)(bands - 1);
        int rc   = r1 + (int)((r2 - r1) * t);
        int gc   = g1 + (int)((g2 - g1) * t);
        int bc   = b1 + (int)((b2 - b1) * t);
        u32 col  = ((u32)rc << 24) | ((u32)gc << 16) | ((u32)bc << 8) | 0xFF;
        GRRLIB_Rectangle(0, i * bh, 640, bh, col, 1);
    }
    (void)font;
}

static void drawField(GRRLIB_ttfFont* font, const char* label, int x, int y,
                       int w, const std::string& value,
                       bool focused, bool masked) {
    // Label
    GRRLIB_PrintfTTF(x, y - 20, font, label, 16, 0xCCCCCCFF);
    // Field border
    u32 borderCol = focused ? 0x4499FFFF : 0x446688FF;
    GRRLIB_Rectangle(x - 2, y - 2, w + 4, 34, borderCol, 1);
    GRRLIB_Rectangle(x, y, w, 30, 0x0D1526FF, 1);
    // Value
    std::string display;
    if (masked) display = std::string(value.size(), '*');
    else        display = value;
    if (focused) display += "_"; // caret
    GRRLIB_PrintfTTF(x + 6, y + 6, font, display.c_str(), 18, 0xEEEEEEFF);
}

static void drawButton(GRRLIB_texImg* btnTex, GRRLIB_ttfFont* font,
                        int cx, int y, int w, int h,
                        const char* label, bool focused) {
    if (btnTex) {
        float zoom = focused ? 1.05f : 1.0f;
        int dw = (int)(w * zoom), dh = (int)(h * zoom);
        float dsx = (float)dw / btnTex->w;
        float dsy = (float)dh / btnTex->h;
        u32 tint = focused ? 0x4499FFFF : 0xFFFFFFFF;
        GRRLIB_DrawImg(cx - dw / 2, y + (h - dh) / 2, btnTex, 0, dsx, dsy, tint);
    }
    int tw = GRRLIB_WidthTTF(font, label, 20);
    u32 tc = focused ? 0x003A80FF : 0x1A3A5AFF;
    GRRLIB_PrintfTTF(cx - tw / 2, y + (h - 20) / 2, font, label, 20, tc);
}

void ConnectView::renderBackground() {
    drawGradientBG(font);
    // Title
    GRRLIB_PrintfTTF(0, 14, font, "Connect to Jellyfin", 24, 0xFFFFFFFF);
    // Tabs
    u32 credCol = (activeTab == Tab::Credentials)    ? 0xFFFFFFFF : 0x778899FF;
    u32 qcCol   = (activeTab == Tab::QuickConnect)   ? 0xFFFFFFFF : 0x778899FF;
    u32 discCol = (activeTab == Tab::Discover)       ? 0xFFFFFFFF : 0x778899FF;
    GRRLIB_PrintfTTF(20,  50, font, "[ Credentials ]",  18, credCol);
    GRRLIB_PrintfTTF(200, 50, font, "[ Quick Connect ]", 18, qcCol);
    GRRLIB_PrintfTTF(405, 50, font, "[ Discover ]",      18, discCol);

    // Status (deux lignes si le message est trop large)
    if (statusTimer > 0) {
        u32 sc = statusError ? 0xFF4444FF : 0x44EE88FF;
        const std::string& msg = statusMsg;
        int sw = GRRLIB_WidthTTF(font, msg.c_str(), 14);
        if (sw <= 620) {
            int x = (640 - sw) / 2;
            GRRLIB_PrintfTTF(x < 10 ? 10 : x, 432, font, msg.c_str(), 14, sc);
        } else {
            // Roughly split at midpoint on a space boundary
            size_t mid = msg.size() / 2;
            size_t sp  = msg.rfind(' ', mid);
            if (sp == std::string::npos) sp = mid;
            std::string l1 = msg.substr(0, sp);
            std::string l2 = msg.substr(sp + 1);
            int w1 = GRRLIB_WidthTTF(font, l1.c_str(), 13);
            int w2 = GRRLIB_WidthTTF(font, l2.c_str(), 13);
            GRRLIB_PrintfTTF((640 - w1) / 2, 424, font, l1.c_str(), 13, sc);
            GRRLIB_PrintfTTF((640 - w2) / 2, 439, font, l2.c_str(), 13, sc);
        }
    }
    // Footer
    const char* footer = "A Select  B Back/Close  L/R Tabs";
    int fw = GRRLIB_WidthTTF(font, footer, 14);
    GRRLIB_PrintfTTF((640 - fw) / 2, 458, font, footer, 14, 0x778899FF);
}

void ConnectView::renderCredentials(ir_t& ir) {
    int fx = 80, fw = 480;
    drawField(font, "Server URL", fx, 110, fw, fields[0],
              focusedField == Field::Server && !kbActive, false);
    drawField(font, "Username",   fx, 180, fw, fields[1],
              focusedField == Field::Username && !kbActive, false);
    drawField(font, "Password",   fx, 250, fw, fields[2],
              focusedField == Field::Password && !kbActive, true);

    // Submit button
    drawButton(btnTex, font, 320, 330, 200, 48, "Connect",
               focusedField == Field::SubmitBtn && !kbActive);

    if (kbActive) renderVKB(ir);
}

void ConnectView::renderVKB(ir_t& ir) {
    int pageStart = kbPage * 4;
    int pageRows  = (kbPage == 0) ? 4 : 3;

    // Semi-transparent background behind keyboard
    GRRLIB_Rectangle(KB_X - 8, KB_Y - 8,
                     KB_COLS_MAX * KB_CELLW + 16,
                     pageRows * KB_CELLH + 16, 0x00000099, 1);

    for (int r = 0; r < pageRows; r++) {
        int ri  = pageStart + r;
        int len = strlen(kbRows[ri]);
        for (int c = 0; c < len; c++) {
            int cx = KB_X + c * KB_CELLW;
            int cy = KB_Y + r * KB_CELLH;
            bool sel = (r == kbRow && c == kbCol);

            bool irHover = ir.valid &&
                           ir.x >= cx && ir.x <= cx + KB_CELLW - 2 &&
                           ir.y >= cy && ir.y <= cy + KB_CELLH - 2;

            char k = kbRows[ri][c];

            // Key label
            char label[5] = {0};
            if      (k == '\x7f') { label[0]='<'; label[1]='-'; }
            else if (k == ' ')    { label[0]='_'; }
            else if (k == '\x01') { label[0]='^'; label[1]='S'; label[2]='H'; }
            else if (k == '\x02') {
                if (kbPage == 0) { label[0]='S'; label[1]='Y'; label[2]='M'; }
                else             { label[0]='A'; label[1]='B'; label[2]='C'; }
            }
            else if (kbShift && k >= 'a' && k <= 'z') { label[0] = (char)(k - 32); }
            else { label[0] = k; }

            // Background colour
            u32 bgCol = (k == '\x01' && kbShift) ? 0xDD8800CC
                      : (k == '\x02')             ? 0x226633CC
                      : sel                       ? 0x4499FFDD
                      : irHover                   ? 0x6699BBDD
                      :                             0x1E2D44CC;
            GRRLIB_Rectangle(cx + 1, cy + 1, KB_CELLW - 2, KB_CELLH - 2, bgCol, 1);

            u32 tc = (k == '\x01' && kbShift) ? 0xFFDD44FF
                   : (k == '\x02')             ? 0x88FFAAFF
                   : sel                       ? 0xFFFFFFFF
                   :                             0xBBCCDDFF;
            // Approximate centering: avg ~10 px/char at size 16 avoids per-key WidthTTF
            int tw = (int)(strlen(label) * 10);
            GRRLIB_PrintfTTF(cx + (KB_CELLW - tw) / 2, cy + 10, font, label, 16, tc);
        }
    }

    // Hint
    int hintY = KB_Y + pageRows * KB_CELLH + 6;
    if (kbPage == 0 && kbShift) {
        GRRLIB_PrintfTTF(KB_X, hintY, font, "[SHIFT]  -: toggle  |  B: close  |  +: confirm", 14, 0xFFDD44FF);
    } else {
        GRRLIB_PrintfTTF(KB_X, hintY, font, "-: SHIFT  |  B: close  |  +: confirm", 14, 0x778899FF);
    }
}

void ConnectView::renderQuickConnect(ir_t& ir) {
    int cx = 320;
    switch (qcState) {
        case QCState::Idle: {
            GRRLIB_PrintfTTF(0, 110, font,
                "Quick Connect lets you sign in by approving", 18, 0xCCCCCCFF);
            GRRLIB_PrintfTTF(0, 135, font,
                "a code in the Jellyfin web interface.", 18, 0xCCCCCCFF);

            /* Show current server URL or a warning if not set */
            if (fields[0].empty()) {
                GRRLIB_PrintfTTF(0, 178, font,
                    "Go to Credentials tab and enter the Server URL first.", 14, 0xFF8844FF);
            } else {
                char sbuf[128];
                snprintf(sbuf, sizeof(sbuf), "Server: %s", fields[0].c_str());
                int sw = GRRLIB_WidthTTF(font, sbuf, 14);
                GRRLIB_PrintfTTF((640-sw)/2, 178, font, sbuf, 14, 0x778899FF);
            }

            bool focused = ir.valid &&
                           ir.x >= 200 && ir.x <= 440 &&
                           ir.y >= 250 && ir.y <= 302;
            drawButton(btnTex, font, cx, 250, 240, 52, "Start Quick Connect", focused);
            break;
        }

        case QCState::Waiting: {
            GRRLIB_PrintfTTF(0, 120, font, "Enter this code on your Jellyfin server:", 18, 0xCCCCCCFF);
            // Big code display
            int cw = GRRLIB_WidthTTF(font, qcResult.code.c_str(), 52);
            GRRLIB_PrintfTTF((640 - cw) / 2, 160, font, qcResult.code.c_str(), 52, 0x4499FFFF);
            GRRLIB_PrintfTTF(0, 240, font, "Waiting for approval...", 18, 0xAAAAAAFF);
            GRRLIB_PrintfTTF(0, 268, font, "B to cancel", 16, 0x778899FF);
            break;
        }
        case QCState::Done:
            GRRLIB_PrintfTTF(0, 200, font, "Approved! Signing in...", 22, 0x44EE88FF);
            break;
        case QCState::Error:
            GRRLIB_PrintfTTF(0, 200, font, "Error. Press A to retry.", 20, 0xFF4444FF);
            break;
    }
}

void ConnectView::renderCursor(ir_t& ir) {
    if (ir.valid && cursorTex) {
        orient_t orient;
        WPAD_Orientation(WPAD_CHAN_0, &orient);
        GRRLIB_DrawImg((int)ir.x - 20, (int)ir.y - 4,
                       cursorTex, orient.roll, 1, 1, 0xFFFFFFFF);
    }
}

void ConnectView::renderDiscover(ir_t& ir) {
    int cx = 320;
    switch (discoverState) {
        case DiscoverState::Idle:
            GRRLIB_PrintfTTF(0, 110, font,
                "Find Jellyfin servers on your local network.", 18, 0xCCCCCCFF);
            GRRLIB_PrintfTTF(0, 138, font,
                "The Wii must be on the same network as the server.", 16, 0x778899FF);
            {
                bool btnFocus = ir.valid &&
                                ir.x >= 200 && ir.x <= 440 &&
                                ir.y >= 240 && ir.y <= 292;
                drawButton(btnTex, font, cx, 240, 240, 52, "Scan for Servers", btnFocus);
            }
            break;

        case DiscoverState::Scanning: {
            int tw = GRRLIB_WidthTTF(font, "Scanning...", 22);
            GRRLIB_PrintfTTF((640 - tw) / 2, 220, font, "Scanning...", 22, 0x4499FFFF);
            break;
        }

        case DiscoverState::Done: {
            int n = (int)discoveredServers.size();
            if (n == 0) {
                GRRLIB_PrintfTTF(0, 170, font, "No servers found.", 20, 0xFF8844FF);
                GRRLIB_PrintfTTF(0, 200, font,
                    "Check your network and server settings, then try again.", 15, 0x778899FF);
                bool btnFocus = ir.valid &&
                                ir.x >= 200 && ir.x <= 440 &&
                                ir.y >= 240 && ir.y <= 292;
                drawButton(btnTex, font, cx, 240, 200, 48, "Scan Again", btnFocus);
                break;
            }
            GRRLIB_PrintfTTF(20, 100, font, "Found — select a server:", 16, 0xCCCCCCFF);
            for (int i = 0; i < n && i < 6; i++) {
                int rowY = 126 + i * 50;
                bool sel = (i == discoverSelected);
                bool irHov = ir.valid &&
                             ir.x >= 60 && ir.x <= 580 &&
                             ir.y >= rowY && ir.y <= rowY + 42;
                bool highlight = sel || irHov;
                u32 bg = highlight ? 0x1E3A5AE0 : 0x0D1526CC;
                GRRLIB_Rectangle(60, rowY, 520, 42, bg, 1);
                if (highlight)
                    GRRLIB_Rectangle(58, rowY - 2, 524, 46, 0x4499FFFF, 0);
                GRRLIB_PrintfTTF(70, rowY + 4, font,
                    discoveredServers[i].name.c_str(), 16,
                    highlight ? 0xFFFFFFFF : 0xBBCCDDFF);
                GRRLIB_PrintfTTF(70, rowY + 24, font,
                    discoveredServers[i].address.c_str(), 13,
                    highlight ? 0x88CCFFFF : 0x778899FF);
            }
            GRRLIB_PrintfTTF(20, 126 + 6 * 50 + 6, font,
                "A: Use this server   Up/Down: Navigate", 14, 0x778899FF);
            break;
        }
    }
}

void ConnectView::render(ir_t& ir) {
    renderBackground();
    if      (activeTab == Tab::Credentials)  renderCredentials(ir);
    else if (activeTab == Tab::QuickConnect) renderQuickConnect(ir);
    else                                     renderDiscover(ir);
    renderCursor(ir);
}
