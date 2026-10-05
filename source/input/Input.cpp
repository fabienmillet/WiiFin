#include "Input.h"
#include "../ui/Ui.h"
#include <wiiuse/wpad.h>

static u32 buttonsDown = 0;

void Input::update() {
    WPAD_ScanPads();
    buttonsDown = WPAD_ButtonsDown(0);
}

bool Input::isHomePressed()  { return buttonsDown & WPAD_BUTTON_HOME; }
bool Input::isUpPressed()    { return buttonsDown & WPAD_BUTTON_UP; }
bool Input::isDownPressed()  { return buttonsDown & WPAD_BUTTON_DOWN; }
bool Input::isLeftPressed()  { return buttonsDown & WPAD_BUTTON_LEFT; }
bool Input::isRightPressed() { return buttonsDown & WPAD_BUTTON_RIGHT; }
bool Input::isAJustPressed() { return buttonsDown & WPAD_BUTTON_A; }
bool Input::isBPressed()     { return buttonsDown & WPAD_BUTTON_B; }
bool Input::isBackPressed()  { return buttonsDown & WPAD_BUTTON_B; }
bool Input::isLPressed()     { return buttonsDown & WPAD_BUTTON_MINUS; }
bool Input::isRPressed()     { return buttonsDown & WPAD_BUTTON_PLUS; }

void Input::readIR(ir_t& ir) {
    WPAD_IR(WPAD_CHAN_0, &ir);
    Ui::pointerToScreen(ir);
}
