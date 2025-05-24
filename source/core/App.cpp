#include "App.h"
#include "Input.h"

#include <grrlib.h>
#include <wiiuse/wpad.h>
#include <gccore.h>

#include <string>

extern unsigned char data_bg_main_png[];
extern unsigned int data_bg_main_png_len;
extern unsigned char data_logo_wiifin_png[];
extern unsigned int data_logo_wiifin_png_len;
extern unsigned char data_button_start_png[];
extern unsigned int data_button_start_png_len;
extern unsigned char data_cursor_png[];
extern unsigned int data_cursor_png_len;
extern unsigned char data_wii_font_ttf[];
extern unsigned int data_wii_font_ttf_len;

#define bg_main_png data_bg_main_png
#define bg_main_png_len data_bg_main_png_len
#define logo_wiifin_png data_logo_wiifin_png
#define logo_wiifin_png_len data_logo_wiifin_png_len
#define button_start_png data_button_start_png
#define button_start_png_len data_button_start_png_len
#define cursor_png data_cursor_png
#define cursor_png_len data_cursor_png_len
#define wii_font_ttf data_wii_font_ttf
#define wii_font_ttf_len data_wii_font_ttf_len

void App::DrawIRCursor(int x, int y) {
    if (!cursorTex) {
        GRRLIB_PrintfTTF(20, 20, font, "Error: cursorTex NULL", 18, 0xFF0000FF);
        return;
    }
    GRRLIB_DrawImg(x - cursorTex->w / 2, y - cursorTex->h / 2, cursorTex, 0, 1, 1, 0xFFFFFFFF);
}

void App::init() {
    VIDEO_Init();
    WPAD_Init();
    WPAD_SetDataFormat(WPAD_CHAN_0, WPAD_FMT_BTNS_ACC_IR);

    // GRRLIB handles video initialization, no need for xfb / rmode
    GRRLIB_Init();
    GRRLIB_SetBackgroundColour(255, 255, 255, 255);  // white background

    // Load textures
    bgTex = GRRLIB_LoadTexture(bg_main_png);
    logoTex = GRRLIB_LoadTexture(logo_wiifin_png);
    btnTex = GRRLIB_LoadTexture(button_start_png);
    cursorTex = GRRLIB_LoadTexture(cursor_png);
    if (!cursorTex) {
        printf("[ERROR] cursor.png could not be loaded!\n");
    }

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

        // Dynamic IR Cursor with rotation
        if (cursorTex && ir.valid) {
            // Uses the Wiimote's angle for rotation
            float angle = 0.0f;
            orient_t orient;
            WPAD_Orientation(WPAD_CHAN_0, &orient);
            angle = orient.roll; // in degrees
            // Displays the cursor with rotation
            GRRLIB_DrawImg(ir.x - cursorTex->w / 2, ir.y - cursorTex->h / 2, cursorTex, angle, 1, 1, 0xFFFFFFFF);
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
                        case 0: /* Connect */ break;
                        case 1: /* Browse */ break;
                        case 2: /* Settings */ break;
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
    exit(0);
}

void App::run() {
    loop();
}
