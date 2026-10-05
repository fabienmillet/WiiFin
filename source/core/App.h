#pragma once
#include <grrlib.h>
#include <wiiuse/wpad.h>
#include <vector>
#include "../jellyfin/JellyfinClient.h"
#include "SavedProfile.h"

class App {
public:
    void init(const char* argv0 = nullptr);
    void run();

private:
    bool running      = true;
    std::string settingsPath;           // resolved in init()
    std::string argvPath;               // full path to wiifin.cfg derived from HBC argv[0]
    // Saved profiles (persisted to sd:/apps/WiiFin/wiifin.cfg — tokens only, NO passwords)
    std::vector<SavedProfile> profiles;
    void loop();
    void loadSettings();
    void saveSettings();

    GRRLIB_texImg* logoTex = nullptr;
    GRRLIB_texImg* btnTex = nullptr;
    GRRLIB_texImg* cursorPointerTex   = nullptr;
    GRRLIB_texImg* ringTex            = nullptr;
    GRRLIB_ttfFont* font = nullptr;
    GRRLIB_ttfFont* jpFont = nullptr;
    ir_t ir;
    JellyfinClient jellyfinClient;
    bool musicEnabled = true;
};
