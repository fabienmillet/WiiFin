#pragma once
#include <string>
#include <grrlib.h>
#include "Keyboard.h"
#include <wiiuse/wpad.h>
#include <ogc/lwp.h>
#include "../jellyfin/JellyfinClient.h"

// Result returned to caller after the view completes
enum class ConnectResult {
    None,       // still running
    Success,    // authenticated — check auth field
    Cancelled,  // user pressed B
};

class ConnectView {
public:
    ConnectView(GRRLIB_texImg* btn, GRRLIB_texImg* cursor,
                GRRLIB_ttfFont* font, JellyfinClient& client);
    ~ConnectView();

    // Pre-fill server URL and username (called before showing the view)
    void setFields(const std::string& serverUrl, const std::string& username);

    // Call every frame; returns None while running
    ConnectResult update(ir_t& ir);
    void render(ir_t& ir);

    // Filled on Success
    JellyfinAuth auth;
    std::string  serverUrl;
    std::string  username;   // username used to authenticate (empty for QuickConnect)

private:
    GRRLIB_texImg* btnTex;
    GRRLIB_texImg* cursorTex;
    GRRLIB_ttfFont* font;
    JellyfinClient& client;

    // --- Tabs ---
    enum class Tab { Credentials, QuickConnect, Discover };
    Tab activeTab = Tab::Credentials;

    // --- Fields (Credentials tab) ---
    enum class Field { Server, Username, Password, SubmitBtn };
    Field focusedField = Field::Server;
    std::string fields[3]; // Server, Username, Password
    bool showPassword = false;

    // --- Quick Connect tab ---
    enum class QCState { Idle, Waiting, Done, Error };
    QCState qcState = QCState::Idle;
    QuickConnectResult qcResult;
    std::string qcError;
    int qcPollTimer = 0; // frames between polls

    // --- Virtual keyboard ---
    bool kbActive = false;
    Keyboard kb;                 // on-screen keyboard (shared with search)
    // USB keyboard support
    bool usbKbInited = false;
    void initUsbKeyboard();

    // --- Status message ---
    std::string statusMsg;
    bool statusError = false;
    int  statusTimer = 0;

    // --- Networking ---
    bool netReady = false;
    /* Slow steps run off the main thread so the screen never freezes (the
     * Wii's network can take very long to come up, a sign-in waits on the
     * server): Network waits for JellyfinClient's start-up thread (B gives
     * up), Login waits for the sign-in thread. */
    enum class Busy { None, Network, Login };
    enum class AfterNet { Login, QuickConnect, Discover };
    Busy     busy     = Busy::None;
    AfterNet afterNet = AfterNet::Login;
    bool     autoQuickConnect = false, autoDiscover = false;   /* run once the network is up */
    lwp_t    loginThread = LWP_THREAD_NULL;
    struct LoginJob* loginJob = nullptr;
    /* true if the network is up; otherwise starts it and runs `then` later */
    bool needNetwork(AfterNet then);
    void startLogin();
    ConnectResult updateBusy();
    bool irMode   = false;  // true when last interaction was IR; gates d-pad-A

    void setStatus(const std::string& msg, bool isError = false);
    void handleVKBInput(ir_t& ir);   // keyboard input for the focused field
    void renderVKB(ir_t& ir);
    void renderCredentials(ir_t& ir);
    void renderQuickConnect(ir_t& ir);
    void renderDiscover(ir_t& ir);
    void renderBackground();
    void renderCursor(ir_t& ir);

    // --- Discover tab ---
    enum class DiscoverState { Idle, Scanning, Done };
    DiscoverState discoverState   = DiscoverState::Idle;
    std::vector<DiscoveredServer> discoveredServers;
    int discoverSelected          = 0;
    lwp_t discoverThread          = LWP_THREAD_NULL;
    struct DiscoverCtx* discoverCtx = nullptr;

};
