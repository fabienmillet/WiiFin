#include "App.h"
#include "Input.h"

#include <grrlib.h>
#include <wiiuse/wpad.h>
#include <gccore.h>

#include <string>

extern const u8 bg_main_png[];
extern const u8 logo_wiifin_png[];
extern const u8 button_start_png[];
extern const u8 cursor_png[];
extern const u8 wii_font_ttf[];
extern const u32 wii_font_ttf_len;
extern const unsigned char cursor_png[];
extern const unsigned int cursor_png_len;

void App::DrawIRCursor(int x, int y) {
    if (cursorTex) {
        GRRLIB_DrawImg(x - cursorTex->w / 2, y - cursorTex->h / 2, cursorTex, 0, 1, 1, 0xFFFFFFFF);
    }
}

void App::init() {
    VIDEO_Init();
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);

    // GRRLIB handles video initialization, no need for xfb / rmode
    GRRLIB_Init();
    GRRLIB_SetBackgroundColour(255, 255, 255, 255);  // fond blanc

    // Load textures
    bgTex = GRRLIB_LoadTexture(bg_main_png);
    logoTex = GRRLIB_LoadTexture(logo_wiifin_png);
    btnTex = GRRLIB_LoadTexture(button_start_png);
    cursorTex = GRRLIB_LoadTexture(cursor_png);

    // Load font
    font = GRRLIB_LoadTTF(wii_font_ttf, wii_font_ttf_len);
}

ir_t ir;

void App::loop() {
    int selectedIndex = 0;
    const std::string menuItems[] = {
        "Connect To Jellyfin",
        "Browse Library",
        "Settings",
        "Exit"
    };

    while (running) {
        WPAD_ScanPads();
        Input::update();
        WPAD_IR(WPAD_CHAN_0, &ir);

        if (Input::isHomePressed()) running = false;
        if (Input::isUpPressed()) selectedIndex = (selectedIndex - 1 + 4) % 4;
        if (Input::isDownPressed()) selectedIndex = (selectedIndex + 1) % 4;
        if (Input::isAPressed()) {
        switch (selectedIndex) {
            case 0:
                // Connect to Jellyfin
                break;
            case 1:
                // Browse Library
                break;
            case 2:
                // Settings
                break;
            case 3:
                running = false;
                break;
        }
    }

        GRRLIB_DrawImg(0, 0, bgTex, 0, 1, 1, 0xFFFFFFFF);
        GRRLIB_DrawImg(120, 20, logoTex, 0, 1, 1, 0xFFFFFFFF);

        for (int i = 0; i < 4; ++i) {
            int x = 70;
            int y = 140 + i * 80;
            u32 color = (i == selectedIndex) ? 0x0073E5FF : 0x555555FF;
            GRRLIB_DrawImg(x, y, btnTex, 0, 1, 1, 0xFFFFFFFF);
            GRRLIB_PrintfTTF(x + 50, y + 50, font, menuItems[i].c_str(), 20, color);
        }

        // IR Cursor
        if (ir.valid) {
            DrawIRCursor(ir.x, ir.y);
        }

        // Hover + click with A
        if (ir.valid && Input::isAPressed()) {
            for (int i = 0; i < 4; ++i) {
                int x = 70;
                int y = 140 + i * 80;
                int w = btnTex->w;
                int h = btnTex->h;
                if (ir.x >= x && ir.x <= x + w && ir.y >= y && ir.y <= y + h) {
                    selectedIndex = i;
                    switch (i) {
                        case 0: /* Se connecter */ break;
                        case 1: /* Parcourir */ break;
                        case 2: /* Paramètres */ break;
                        case 3: running = false; break;
                    }
                }
            }
        }

        GRRLIB_Render();
    }

    // Clean up
    GRRLIB_FreeTexture(bgTex);
    GRRLIB_FreeTexture(logoTex);
    GRRLIB_FreeTexture(btnTex);
    GRRLIB_FreeTexture(cursorTex);
    GRRLIB_FreeTTF(font);
    GRRLIB_Exit();
}

void App::run() {
    loop();
}
