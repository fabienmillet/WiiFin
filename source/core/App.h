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
    void logSettings();   // one line, on leaving Settings (tests, bug reports)

    GRRLIB_texImg* logoTex = nullptr;
    GRRLIB_texImg* btnTex = nullptr;
    GRRLIB_texImg* cursorPointerTex   = nullptr;
    GRRLIB_texImg* ringTex            = nullptr;
    GRRLIB_ttfFont* font = nullptr;
    GRRLIB_ttfFont* jpFont = nullptr;
    /* the SD card's fonts/ (Chinese, Korean...): fallbacks after jpFont */
    std::vector<GRRLIB_ttfFont*> sdFonts;
    std::vector<void*>           sdFontData;   /* FreeType reads them in place */
    void loadSdFonts(const std::string& dir);
    ir_t ir;
    JellyfinClient jellyfinClient;
    bool musicEnabled = true;
    /* WiiFin's own look (the menus, and a profile without its own) */
    ViewPrefs startView;
    ViewPrefs currentView() const;                 /* what Ui shows now */
    void applyView(const ViewPrefs& v);
    ViewPrefs viewOf(const SavedProfile& p) const; /* its own, or startView's */
    void keepView(SavedProfile& p);                /* Ui's look, into p */
    /* Settings, from WiiFin's menu (profile null) or a profile's menu */
    void runSettings(SavedProfile* profile);
    /* a new profile's questions: sign-in, home screen, library view */
    void runOnboarding(SavedProfile& p);
    /* the passcode of a profile asked; false: B (or power) */
    bool askPasscode(const SavedProfile& p);
    bool screenAreaAsked = false;      // first-launch offer to fit the TV made
    bool testOnboarding  = false;      // tests: the first profile opened is asked as a new one
    void offerScreenCalibration(ir_t& ir);
};
