#pragma once
#include <string>
#include <grrlib.h>
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
    int  kbRow = 0, kbCol = 0;
    bool kbShift = false;
    int  kbPage  = 0;
    // USB keyboard support
    bool usbKbInited = false;
    void initUsbKeyboard();

    // --- Status message ---
    std::string statusMsg;
    bool statusError = false;
    int  statusTimer = 0;

    // --- Networking ---
    bool netReady = false;
    bool irMode   = false;  // true when last interaction was IR; gates d-pad-A

    void setStatus(const std::string& msg, bool isError = false);
    void handleVKBInput(ir_t& ir);
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

    // VKB layout
    static const char* kbRows[7];
    static const int   KB_X     = 80;
    static const int   KB_Y     = 265;
    static const int   KB_CELLW = 38;
    static const int   KB_CELLH = 38;
};
