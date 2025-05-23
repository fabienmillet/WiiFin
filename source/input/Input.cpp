#include "Input.h"
#include <wiiuse/wpad.h>

static u32 buttonsDown = 0;

void Input::update() {
    WPAD_ScanPads();
    buttonsDown = WPAD_ButtonsDown(0);
}

bool Input::isHomePressed() {
    return buttonsDown & WPAD_BUTTON_HOME;
}

bool Input::isUpPressed() {
    return buttonsDown & WPAD_BUTTON_UP;
}

bool Input::isDownPressed() {
    return buttonsDown & WPAD_BUTTON_DOWN;
}

bool Input::isAJustPressed() {
    return buttonsDown & WPAD_BUTTON_A;
}

bool Input::isAPressed() {
    return WPAD_ButtonsHeld(0) & WPAD_BUTTON_A;
}