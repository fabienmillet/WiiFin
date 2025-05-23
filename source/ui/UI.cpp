#include "UI.h"
#include <stdio.h>
#include <string.h>

void UI::clear() {
    printf("\x1b[2J"); // Clear screen
}

void UI::printCenter(int row, const char* text) {
    int col = (80 - strlen(text)) / 2;
    printf("\x1b[%d;%dH%s", row, col, text);
}
