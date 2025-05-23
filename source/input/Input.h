#pragma once
#include <wiiuse/wpad.h>

class Input {
public:
    static void update();
    static bool isHomePressed();
    static bool isUpPressed();
    static bool isDownPressed();
    static bool isAPressed();
    static bool isAJustPressed();
};
