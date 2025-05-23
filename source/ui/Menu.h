#pragma once
#include <string>
#include <vector>

class Menu {
public:
    Menu();
    void render();
    void handleInput(int direction); // -1 = up, 1 = down
    int getSelected() const;
    const std::string& getSelectedLabel() const;

private:
    std::vector<std::string> items;
    size_t selectedIndex;
};
