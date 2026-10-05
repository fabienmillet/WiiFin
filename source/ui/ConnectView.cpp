#include "ConnectView.h"
#include "../core/ExitZone.h"
#include "../core/Text.h"
#include "Ui.h"
#include "../input/Input.h"
#include "../core/SoundFX.h"
#include <wiikeyboard/keyboard.h>
#include <ogc/lwp.h>
#include <string.h>
#include <stdio.h>
#include <ogc/lwp_watchdog.h>


// Tab bar (drawn by renderBackground, hit-tested by update): three equal
// segments, centred
static const float TAB_W = 180, TAB_X0 = (640 - 3 * TAB_W) / 2, TAB_Y = 52, TAB_H = 30;

// USB keyboard callback — appends char to a shared buffer
static char usbChar = 0;
static void usbCallback(char c) { usbChar = c; }

// ---------------------------------------------------------------------------
// Background discovery thread
// ---------------------------------------------------------------------------
static u8  s_discoverStack[32 * 1024] DEAD_AT_EXIT;

struct DiscoverCtx {
    JellyfinClient*              client;
    std::vector<DiscoveredServer>* out;
    volatile bool                done;
};

static void* discoverWorker(void* arg) {
    DiscoverCtx* ctx = static_cast<DiscoverCtx*>(arg);
    ctx->client->discoverServers(*ctx->out);
    ctx->done = true;
    return nullptr;
}

// ---------------------------------------------------------------------------
// Background sign-in thread
// ---------------------------------------------------------------------------
/* Not DEAD_AT_EXIT: HOME can quit while a sign-in is still running. */
static u8 s_loginStack[64 * 1024] __attribute__((aligned(32)));   /* mbedTLS handshake */

struct LoginJob {
    JellyfinClient* client;
    std::string     url, user, pass;
    volatile bool   running = true;
    bool            ok = false;
    JellyfinAuth    auth;
    std::string     err;
};

static void* loginWorker(void* arg) {
    LoginJob* j = static_cast<LoginJob*>(arg);
    j->ok = j->client->authenticate(j->url, j->user, j->pass, j->auth);
    if (j->ok) {
        std::string sn;
        if (j->client->getServerName(j->url, j->auth, sn)) j->auth.serverName = sn;
    } else {
        j->err = j->client->lastError();
    }
    j->running = false;
    return nullptr;
}

// ---------------------------------------------------------------
ConnectView::~ConnectView() {
    /* the threads use this view's members: let them finish first */
    if (loginThread != LWP_THREAD_NULL) LWP_JoinThread(loginThread, nullptr);
    delete loginJob;
    if (discoverThread != LWP_THREAD_NULL) LWP_JoinThread(discoverThread, nullptr);
    delete discoverCtx;
}

bool ConnectView::needNetwork(AfterNet then) {
    if (netReady || (netReady = client.takeNetworkResult())) return true;
    client.startNetwork();
    afterNet = then;
    busy     = Busy::Network;
    return false;
}

void ConnectView::startLogin() {
    while (fields[0].size() > 1 && fields[0].back() == '/') fields[0].pop_back();
    serverUrl = fields[0];
    loginJob = new LoginJob;
    loginJob->client = &client;
    loginJob->url  = fields[0];
    loginJob->user = fields[1];
    loginJob->pass = fields[2];
    if (LWP_CreateThread(&loginThread, loginWorker, loginJob,
                         s_loginStack, sizeof(s_loginStack), 50) < 0) {
        loginThread = LWP_THREAD_NULL;
        loginWorker(loginJob);              /* no thread: sign in here */
    }
    busy = Busy::Login;
}

/* While a slow step runs: keep the screen alive, collect its result. */
ConnectResult ConnectView::updateBusy() {
    if (busy == Busy::Network) {
        if (Input::isBackPressed()) {            /* start-up goes on in the background */
            busy = Busy::None;
            setStatus("Cancelled.", true);
            return ConnectResult::None;
        }
        if (client.networkBusy()) return ConnectResult::None;
        busy = Busy::None;
        if (!client.takeNetworkResult()) { setStatus(client.lastError(), true); return ConnectResult::None; }
        netReady = true;
        if      (afterNet == AfterNet::Login)        startLogin();
        else if (afterNet == AfterNet::QuickConnect) autoQuickConnect = true;
        else                                         autoDiscover = true;
        return ConnectResult::None;
    }
    /* Login: every request times out on its own, B waits for it */
    if (loginJob->running) return ConnectResult::None;
    if (loginThread != LWP_THREAD_NULL) LWP_JoinThread(loginThread, nullptr);
    loginThread = LWP_THREAD_NULL;
    busy = Busy::None;
    bool ok = loginJob->ok;
    if (ok) { auth = loginJob->auth; username = loginJob->user; }
    else    setStatus(loginJob->err, true);
    delete loginJob;
    loginJob = nullptr;
    return ok ? ConnectResult::Success : ConnectResult::None;
}

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

/* USB keyboard: started once for the whole run, stopped before leaving.
 * KEYBOARD_Init queues USB requests again on every call, and its thread
 * keeps polling USB: started at each visit of this screen and never
 * stopped, a request still in flight when IOS shut down for power-off or
 * exit completed into freed memory (DSI in __usbv0_messageCB). */
static int s_usbKb = 0;            /* 0 not tried, 1 running, -1 failed */

void ConnectView::initUsbKeyboard() {
    if (s_usbKb == 0) s_usbKb = KEYBOARD_Init(usbCallback) >= 0 ? 1 : -1;
    usbKbInited = s_usbKb > 0;
}

void ConnectView::shutdownUsbKeyboard() {
    if (s_usbKb > 0) KEYBOARD_Deinit();
    s_usbKb = 0;
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

    if (busy != Busy::None) return updateBusy();

    // B = cancel (or close VKB)
    if (Input::isBackPressed() && !kbActive) {
        // Join any in-flight discovery thread before leaving
        if (discoverThread != LWP_THREAD_NULL) {
            LWP_JoinThread(discoverThread, nullptr);
            discoverThread = LWP_THREAD_NULL;
            delete discoverCtx;
            discoverCtx = nullptr;
        }
        return ConnectResult::Cancelled;
    }

    // --- Tab switching: IR click on tab headers OR L/R buttons ---
    bool aPressed = Input::isAJustPressed();

    // IR hover: any valid IR position sets irMode so d-pad A is blocked while pointer is out
    if (ir.valid) irMode = true;

    bool irTabHandled = false;
    if (ir.valid && aPressed && ir.y >= TAB_Y && ir.y <= TAB_Y + TAB_H &&
        ir.x >= TAB_X0 && ir.x < TAB_X0 + 3 * TAB_W) {
        activeTab = (Tab)(int)((ir.x - TAB_X0) / TAB_W);
        kbActive = false;
        irTabHandled = true;
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
                    if      (ir.y >= 100 && ir.y <= 145) { focusedField = Field::Server;   kbActive = true; kb.reset(); irFieldHandled = true; irMode = true; }
                    else if (ir.y >= 170 && ir.y <= 215) { focusedField = Field::Username;  kbActive = true; kb.reset(); irFieldHandled = true; irMode = true; }
                    else if (ir.y >= 240 && ir.y <= 285) { focusedField = Field::Password;  kbActive = true; kb.reset(); irFieldHandled = true; irMode = true; }
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
                    kbActive = true; kb.reset();
                } else {
                    doSubmit:
                    SoundFX::play(SoundFX::FX::Start);
                    if (needNetwork(AfterNet::Login)) startLogin();
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
                if ((aPressed && (qcBtnHit || (!ir.valid && !irMode))) || autoQuickConnect) {
                    autoQuickConnect = false;
                    if (fields[0].empty()) {
                        setStatus("Set the Server URL in the Credentials tab first.", true);
                        break;
                    }
                    if (!needNetwork(AfterNet::QuickConnect)) break;
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
                if ((aPressed && (btnHit || (!irMode && !ir.valid))) || autoDiscover) {
                    autoDiscover = false;
                    if (!needNetwork(AfterNet::Discover)) break;
                    discoveredServers.clear();
                    discoverSelected = 0;
                    discoverCtx = new DiscoverCtx{&client, &discoveredServers, false};
                    if (LWP_CreateThread(&discoverThread, discoverWorker, discoverCtx,
                                         s_discoverStack, sizeof(s_discoverStack), 64) < 0) {
                        delete discoverCtx;
                        discoverCtx = nullptr;
                        discoverThread = LWP_THREAD_NULL;
                        setStatus("Failed to start scan thread.", true);
                        break;
                    }
                    discoverState = DiscoverState::Scanning;
                }
                break;
            }
            case DiscoverState::Scanning:
                if (discoverCtx && discoverCtx->done) {
                    LWP_JoinThread(discoverThread, nullptr);
                    discoverThread = LWP_THREAD_NULL;
                    delete discoverCtx;
                    discoverCtx = nullptr;
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
                // Navigate list — clamp to the 6 visible rows
                int visible = (n < 6) ? n : 6;
                if (Input::isDownPressed()) { discoverSelected = (discoverSelected + 1) % visible; irMode = false; }
                if (Input::isUpPressed())   { discoverSelected = (discoverSelected - 1 + visible) % visible; irMode = false; }

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
    int fi = (int)focusedField;
    if (fi > 2) { kbActive = false; return; }
    kb.setOrigin((640 - kb.width()) * 0.5f, 238);
    kb.setEnterLabel(fi < 2 ? "Next" : "Done");
    switch (kb.update(ir, fields[fi], 255)) {
    case Keyboard::Result::Enter:
        /* OK walks through the fields, then lands on Connect */
        if (fi < 2) { focusedField = (Field)(fi + 1); kb.reset(); }
        else        { kbActive = false; focusedField = Field::SubmitBtn; }
        break;
    case Keyboard::Result::Cancel:
        kbActive = false;
        break;
    default: break;
    }
}

// ---------------------------------------------------------------
// Render helpers
// ---------------------------------------------------------------
static void drawField(const char* label, int x, int y, int w, const std::string& value,
                      bool focused, bool masked) {
    const Ui::Palette& p = Ui::pal();
    Ui::text(x + 4, y - 21, label, 15, focused ? p.accentDark : p.textDim);
    std::string display = masked ? std::string(value.size(), '*') : value;
    if (focused) display += "_"; // caret
    // keep the end (and the caret) visible
    while (display.size() > 1 && Ui::textWidth(display.c_str(), 18) > w - 20)
        display.erase(display.begin());
    Ui::field(x - 6, y - 3, w + 12, 36, display.c_str(), 18, focused);
}

static void drawButton(int cx, int y, int w, int h, const char* label, bool focused) {
    Ui::button(cx - w / 2, y, w, h, label, 20, focused ? Ui::pulse() : 0.0f);
}

// Centred paragraph line.
static void line(int y, const char* s, int size, u32 col) {
    Ui::textCentered(320, y, s, size, col);
}

void ConnectView::renderBackground() {
    const Ui::Palette& p = Ui::pal();
    Ui::background();
    Ui::text(28, 12, "Connect to Jellyfin", 22, p.text);

    // Tabs: one pill split into the segments update() hit-tests
    {
        const float X0 = TAB_X0, X3 = TAB_X0 + 3 * TAB_W, Y = TAB_Y, H = TAB_H;
        const float seg[4] = { X0, X0 + TAB_W, X0 + 2 * TAB_W, X3 };
        const char* names[3] = { "Credentials", "Quick Connect", "Discover" };
        Ui::shadow(X0, Y + 1, X3 - X0, H, H * 0.5f, 4.0f, p.shadow);
        Ui::roundRect(X0, Y, X3 - X0, H, H * 0.5f, p.field, Ui::mix(p.field, p.cardBottom, 0.5f));
        Ui::roundBorder(X0, Y, X3 - X0, H, H * 0.5f, 1.5f, p.fieldBorder);
        for (int i = 0; i < 3; ++i) {
            bool on = (int)activeTab == i;
            float sx = seg[i], sw = seg[i + 1] - seg[i];
            if (on) Ui::roundRect(sx + 2, Y + 2, sw - 4, H - 4, (H - 4) * 0.5f,
                                  Ui::mix(p.accent, 0xFFFFFFFF, 0.25f), p.accentDark);
            Ui::textCentered(sx + sw * 0.5f, Y + 7, names[i], 15, on ? p.textOnAccent : p.textDim);
        }
    }

    // Slow step in progress
    if (busy != Busy::None) {
        static const char* const dots[4] = { "", ".", "..", "..." };
        char msg[64];
        snprintf(msg, sizeof(msg), "%s%s",
                 busy == Busy::Network ? "Connecting to the network" : "Signing in",
                 dots[(ticks_to_millisecs(gettime()) / 400) % 4]);
        int w = Ui::textWidth("Connecting to the network...", 14);
        Ui::roundRect(320 - w / 2 - 16, 420, w + 32, 26, 13, p.cardTop, p.cardBottom);
        Ui::roundBorder(320 - w / 2 - 16, 420, w + 32, 26, 13, 1.5f, p.accent);
        Ui::text(320 - w / 2, 425, msg, 14, p.accentDark);
    } else
    // Status (deux lignes si le message est trop large)
    if (statusTimer > 0) {
        u32 sc = statusError ? p.danger : p.ok;
        const std::string& msg = statusMsg;
        int sw = Ui::textWidth(msg.c_str(), 14);
        std::string l1 = msg, l2;
        if (sw > 580) {
            // Roughly split at midpoint on a space boundary
            size_t mid = msg.size() / 2;
            size_t sp  = msg.rfind(' ', mid);
            if (sp == std::string::npos) sp = mid;
            l1 = msg.substr(0, sp);
            l2 = msg.substr(sp + 1);
        }
        int w  = Ui::textWidth(l1.c_str(), 14);
        if (!l2.empty()) { int w2 = Ui::textWidth(l2.c_str(), 14); if (w2 > w) w = w2; }
        int bh = l2.empty() ? 26 : 42;
        int by = 446 - bh;
        Ui::roundRect(320 - w / 2 - 16, by, w + 32, bh, 13, p.cardTop, p.cardBottom);
        Ui::roundBorder(320 - w / 2 - 16, by, w + 32, bh, 13, 1.5f, sc);
        Ui::textCentered(320, by + 5, l1.c_str(), 14, sc);
        if (!l2.empty()) Ui::textCentered(320, by + 21, l2.c_str(), 14, sc);
    }
}

void ConnectView::renderCredentials(ir_t& ir) {
    int fx = 80, fw = 480;
    drawField("Server URL", fx, 110, fw, fields[0],
              focusedField == Field::Server && !kbActive, false);
    drawField("Username",   fx, 180, fw, fields[1],
              focusedField == Field::Username && !kbActive, false);
    drawField("Password",   fx, 250, fw, fields[2],
              focusedField == Field::Password && !kbActive, true);

    // Submit button
    drawButton(320, 330, 200, 48, "Connect",
               focusedField == Field::SubmitBtn && !kbActive);

    if (kbActive) {
        renderVKB(ir);
        const Ui::Hint l[] = { { "A", "Type" }, { "B", "Delete" } };
        const Ui::Hint r[] = { { "-", "Shift" }, { "+", focusedField == Field::Password ? "Done" : "Next" } };
        Ui::footer(l, 2, r, 2);
    } else {
        const Ui::Hint l[] = { { "A", "Select" }, { "UD", "Move" } };
        const Ui::Hint r[] = { { "-/+", "Tabs" }, { "B", "Back" } };
        Ui::footer(l, 2, r, 2);
    }
}

void ConnectView::renderVKB(ir_t& ir) {
    // Keyboard panel (covers the lower fields while typing; the edited
    // field is shown in the panel's title row)
    kb.setOrigin((640 - kb.width()) * 0.5f, 238);
    const int PX = (int)((640 - kb.width()) * 0.5f) - 12, PW = (int)kb.width() + 24;
    const int PY = 238 - 50, PH = (int)kb.height() + 62;
    GRRLIB_Rectangle(Ui::screenLeft(), 0, Ui::screenWidth(), 480, Ui::alpha(Ui::pal().dim, 0.5f), 1);
    Ui::card(PX, PY, PW, PH, 16, 0.0f);
    {
        int fi = (int)focusedField;
        const char* names[3] = { "Server URL", "Username", "Password" };
        if (fi <= 2) {
            std::string v = fi == 2 ? std::string(fields[fi].size(), '*') : fields[fi];
            v += "_";
            while (v.size() > 1 && Ui::textWidth(v.c_str(), 16) > PW - 150)
                v.erase(v.begin());
            Ui::text(PX + 16, PY + 13, names[fi], 14, Ui::pal().textDim);
            Ui::field(PX + 110, PY + 8, PW - 122, 28, v.c_str(), 16, true);
        }
    }

    kb.render(ir);
}

void ConnectView::renderQuickConnect(ir_t& ir) {
    const Ui::Palette& p = Ui::pal();
    int cx = 320;
    switch (qcState) {
        case QCState::Idle: {
            Ui::card(70, 96, 500, 108, 16, 0.0f);
            line(112, "Quick Connect lets you sign in by approving", 17, p.text);
            line(136, "a code in the Jellyfin web interface.", 17, p.text);

            /* Show current server URL or a warning if not set */
            if (fields[0].empty()) {
                line(174, "Go to Credentials tab and enter the Server URL first.", 14, p.danger);
            } else {
                char sbuf[128];
                snprintf(sbuf, sizeof(sbuf), "Server: %s", fields[0].c_str());
                line(174, sbuf, 14, p.textDim);
            }

            bool focused = ir.valid &&
                           ir.x >= 200 && ir.x <= 440 &&
                           ir.y >= 250 && ir.y <= 302;
            drawButton(cx, 250, 240, 52, "Start Quick Connect", focused || !ir.valid);
            break;
        }

        case QCState::Waiting: {
            line(112, "Enter this code on your Jellyfin server:", 17, p.text);
            // Big code display on a card
            int cw = Ui::textWidth(qcResult.code.c_str(), 52);
            Ui::card(320 - cw / 2 - 30, 148, cw + 60, 80, 18, Ui::pulse());
            Ui::textCentered(320, 160, qcResult.code.c_str(), 52, p.accentDark);
            line(250, "Waiting for approval...", 17, p.textDim);
            break;
        }
        case QCState::Done:
            line(200, "Approved! Signing in...", 22, p.ok);
            break;
        case QCState::Error:
            line(200, "Error. Press A to retry.", 20, p.danger);
            break;
    }
    const Ui::Hint l[] = { { "A", "Select" } };
    const Ui::Hint r[] = { { "-/+", "Tabs" }, { "B", qcState == QCState::Waiting ? "Cancel" : "Back" } };
    Ui::footer(l, 1, r, 2);
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
    const Ui::Palette& p = Ui::pal();
    int cx = 320;
    switch (discoverState) {
        case DiscoverState::Idle:
            Ui::card(70, 96, 500, 84, 16, 0.0f);
            line(114, "Find Jellyfin servers on your local network.", 17, p.text);
            line(142, "The Wii must be on the same network as the server.", 14, p.textDim);
            {
                bool btnFocus = ir.valid &&
                                ir.x >= 200 && ir.x <= 440 &&
                                ir.y >= 240 && ir.y <= 292;
                drawButton(cx, 240, 240, 52, "Scan for Servers", btnFocus || !ir.valid);
            }
            break;

        case DiscoverState::Scanning:
            line(220, "Scanning...", 22, p.accentDark);
            break;

        case DiscoverState::Done: {
            int n = (int)discoveredServers.size();
            if (n == 0) {
                line(170, "No servers found.", 20, p.danger);
                line(200, "Check your network and server settings, then try again.", 14, p.textDim);
                bool btnFocus = ir.valid &&
                                ir.x >= 200 && ir.x <= 440 &&
                                ir.y >= 240 && ir.y <= 292;
                drawButton(cx, 240, 200, 48, "Scan Again", btnFocus || !ir.valid);
                break;
            }
            Ui::text(28, 98, "Found \xe2\x80\x94 select a server:", 15, p.textDim);
            for (int i = 0; i < n && i < 6; i++) {
                int rowY = 126 + i * 50;
                bool sel = (i == discoverSelected);
                bool irHov = ir.valid &&
                             ir.x >= 60 && ir.x <= 580 &&
                             ir.y >= rowY && ir.y <= rowY + 42;
                float f = sel ? Ui::pulse() : (irHov ? 0.55f : 0.0f);
                Ui::card(60, rowY, 520, 44, 12, f);
                Ui::text(76, rowY + 5, discoveredServers[i].name.c_str(), 16,
                         Ui::mix(p.text, p.accentDark, f));
                Ui::text(76, rowY + 25, discoveredServers[i].address.c_str(), 13, p.textDim);
            }
            break;
        }
    }
    const Ui::Hint l[] = { { "A", "Select" } };
    const Ui::Hint r[] = { { "-/+", "Tabs" }, { "B", "Back" } };
    Ui::footer(l, 1, r, 2);
}

void ConnectView::render(ir_t& ir) {
    renderBackground();
    if      (activeTab == Tab::Credentials)  renderCredentials(ir);
    else if (activeTab == Tab::QuickConnect) renderQuickConnect(ir);
    else                                     renderDiscover(ir);
    renderCursor(ir);
}
