#pragma once
#include <grrlib.h>
#include <wiiuse/wpad.h>
#include <vector>
#include "../jellyfin/JellyfinClient.h"
#include "PinPad.h"

struct SavedProfile;

class SettingsView {
public:
    /* profile: the one signed in (its own look, how it signs in); null
     * from WiiFin's menu (WiiFin's own look, the system) */
    SettingsView(GRRLIB_texImg* btn, GRRLIB_ttfFont* font, JellyfinClient& client, bool& musicEnabled,
                 SavedProfile* profile = nullptr);

    // Returns true when the user exits (B or back)
    bool update(ir_t& ir);
    void render(ir_t& ir);

    // Opens the screen-area calibration directly (first-launch offer).
    void startCalibration() { calibrating = true; calCorner = 0; }
    bool isCalibrating() const { return calibrating; }

private:
    GRRLIB_texImg*  btnTex;
    GRRLIB_ttfFont* font;
    JellyfinClient& client;
    bool&           musicEnabled;
    SavedProfile*   profile;

    /* the sections (View, System, Sign-In), then one's rows */
    int section = -1;             /* -1: the sections to pick from */
    std::vector<int> rows;        /* the open section's rows (Row in the .cpp) */
    int   menuSel = 0;
    float menuFocus[3] = {};
    int  sectionCount() const;
    void openSection(int s);
    bool updateMenu(ir_t& ir);
    void renderMenu(ir_t& ir);

    int   selectedIndex = 0;      /* place in rows */
    float pageAnim      = 0.0f;   /* page slide                   */
    bool  irMode        = false;
    float focusAnim[16] = {};     /* one per place in rows */
    int   pages() const;

    /* the Sign-In row's choice, applied on leaving its section (a new
     * passcode asked then) */
    int    signInWant = -1;
    /* images the Image Cache row removed this visit, -1 not yet */
    int    cacheCleared = -1;
    void   clearCache();
    bool   leaveSignIn();
    /* a new passcode, typed twice */
    PinPad pin;
    int    pinStep = 0;           /* 0 none, 1 first time, 2 again */
    std::string pinFirst;

    /* Screen-area (overscan) calibration */
    bool  calibrating   = false;
    int   calCorner     = 0;      /* 0 top-left, 1 bottom-right */
    u64   calHeldSince  = 0;      /* d-pad auto-repeat */
    u64   calLastStep   = 0;

    void drawRow(int id, const char* label, const char* desc,
                 const char* value, u32 pillCol);
    void activate(int id);
    void updateCalibration();
    void renderCalibration();
};
