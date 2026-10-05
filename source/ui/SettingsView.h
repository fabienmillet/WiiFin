#pragma once
#include <grrlib.h>
#include <wiiuse/wpad.h>
#include "../jellyfin/JellyfinClient.h"

class SettingsView {
public:
    SettingsView(GRRLIB_texImg* btn, GRRLIB_ttfFont* font, JellyfinClient& client, bool& musicEnabled);

    // Returns true when the user exits (B or back)
    bool update(ir_t& ir);
    void render(ir_t& ir);

private:
    GRRLIB_texImg*  btnTex;
    GRRLIB_ttfFont* font;
    JellyfinClient& client;
    bool&           musicEnabled;

    int   selectedIndex = 0;
    float pageAnim      = 0.0f;   /* page slide                   */
    bool  irMode        = false;
    float focusAnim[8]  = {};

    /* Screen-area (overscan) calibration */
    bool  calibrating   = false;
    int   calCorner     = 0;      /* 0 top-left, 1 bottom-right */
    u64   calHeldSince  = 0;      /* d-pad auto-repeat */
    u64   calLastStep   = 0;

    void drawRow(int index, const char* label, const char* desc,
                 const char* value, u32 pillCol);
    void activate(int index);
    void updateCalibration();
    void renderCalibration();
};
