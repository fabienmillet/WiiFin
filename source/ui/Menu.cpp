#include "Menu.h"
#include <stdio.h>

Menu::Menu() {
    items = {
        "Connect To Jellyfin",
        "Browse Library",
        "Settings",
        "Exit"
    };
    selectedIndex = 0;
}

void Menu::render() {
    printf("\x1b[7;0H"); // Display from 7th line
    for (size_t i = 0; i < items.size(); ++i) {
        if (i == selectedIndex) {
            printf("> %s\n", items[i].c_str());
        } else {
            printf("  %s\n", items[i].c_str());
        }
    }
}

void Menu::handleInput(int direction) {
    selectedIndex += direction;
    if (selectedIndex < 0) selectedIndex = 0;
    if (selectedIndex >= items.size()) selectedIndex = items.empty() ? 0 : items.size() - 1;
}

int Menu::getSelected() const {
    return selectedIndex;
}

const std::string& Menu::getSelectedLabel() const {
    return items[selectedIndex];
}
