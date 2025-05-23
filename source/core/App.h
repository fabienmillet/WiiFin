#pragma once
#include <grrlib.h>
#include <wiiuse/wpad.h>

class App {
public:
    void init();
    void run();

private:
    bool running = true;
    void loop();
    void DrawIRCursor(int x, int y);

    GRRLIB_texImg* bgTex = nullptr;
    GRRLIB_texImg* logoTex = nullptr;
    GRRLIB_texImg* cursorTex = nullptr;
    GRRLIB_texImg* btnTex = nullptr;
    GRRLIB_ttfFont* font = nullptr;
    ir_t ir;
};
