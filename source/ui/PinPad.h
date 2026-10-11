#pragma once
#include <grrlib.h>
#include <wiiuse/wpad.h>
#include <string>

/* A profile's passcode: four digits typed on a keypad (D-pad or pointer,
 * A types, B removes the last digit or leaves).  The code is never kept:
 * hash() makes what wiifin.cfg holds. */
class PinPad {
public:
    /* title and note: N_ English, translated when drawn */
    void start(const char* title, const char* note = nullptr);
    /* 1: four digits typed (code()), -1: left with B, 0: still typing */
    int  update(ir_t& ir);
    void render(ir_t& ir);
    const std::string& code() const { return digits; }
    /* the code was wrong: shake, start again */
    void reject();

    static std::string newSalt();
    static std::string hash(const std::string& salt, const std::string& code);

private:
    const char* title = "";
    const char* note  = nullptr;
    std::string digits;
    int  sel = 0;          /* key under the selection, 0..11 */
    unsigned long long shakeSince = 0;
    int  keyAt(const ir_t& ir) const;
};
