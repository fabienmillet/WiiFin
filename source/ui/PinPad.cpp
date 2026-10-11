#include "PinPad.h"
#include "Ui.h"
#include "../input/Input.h"
#include "../core/SoundFX.h"
#include "../core/Lang.h"
#include <mbedtls/sha256.h>
#include <ogc/lwp_watchdog.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>

static const int KW = 70, KH = 40, KG = 8, KX0 = 320 - (3 * KW + 2 * KG) / 2, KY0 = 190;
static const int CX = 140, CY = 56, CW = 360, CH = 342;   /* the card */
/* keys 1-9, then delete, 0: a phone's keypad, 0 in the middle of the last
 * row and delete on its right */
static const char* const KEYS[11] = { "1", "2", "3", "4", "5", "6", "7", "8", "9", "\xe2\x86\x90", "0" };
static int keyCol(int k) { return k < 9 ? k % 3 : k == 10 ? 1 : 2; }
static int keyRow(int k) { return k < 9 ? k / 3 : 3; }
static int keyAtCell(int c, int r) {
    if (r < 3) return r * 3 + c;
    return c == 1 ? 10 : c == 2 ? 9 : -1;
}

void PinPad::start(const char* t, const char* n) {
    title = t; note = n; digits.clear(); sel = 0; shakeSince = 0;
}

void PinPad::reject() {
    digits.clear();
    shakeSince = ticks_to_millisecs(gettime());
    SoundFX::play(SoundFX::FX::Back);
}

int PinPad::keyAt(const ir_t& ir) const {
    if (!ir.valid) return -1;
    for (int k = 0; k < 11; ++k) {
        int x = KX0 + keyCol(k) * (KW + KG), y = KY0 + keyRow(k) * (KH + KG);
        if (ir.x >= x && ir.x < x + KW && ir.y >= y && ir.y < y + KH) return k;
    }
    return -1;
}

int PinPad::update(ir_t& ir) {
    if (Input::isBackPressed()) {
        if (digits.empty()) { SoundFX::play(SoundFX::FX::Back); return -1; }
        digits.pop_back();
        SoundFX::play(SoundFX::FX::Backspace);
        return 0;
    }
    const int was = sel;
    int c = keyCol(sel), r = keyRow(sel), dc = 0, dr = 0;
    if (Input::isUpPressed())    dr = -1;
    if (Input::isDownPressed())  dr = +1;
    if (Input::isLeftPressed())  dc = -1;
    if (Input::isRightPressed()) dc = +1;
    if ((dc || dr) && c + dc >= 0 && c + dc < 3 && r + dr >= 0 && r + dr < 4) {
        int k = keyAtCell(c + dc, r + dr);
        if (k < 0 && dr > 0) k = 10;   /* down from 7: the 0 */
        if (k >= 0) sel = k;
    }
    const int hover = keyAt(ir);
    if (hover >= 0) sel = hover;
    if (sel != was) SoundFX::play(SoundFX::FX::Move);
    if (!Input::isAJustPressed() || (ir.valid && hover < 0)) return 0;
    if (sel == 9) {
        if (!digits.empty()) digits.pop_back();
        SoundFX::play(SoundFX::FX::Backspace);
        return 0;
    }
    digits += KEYS[sel];
    SoundFX::play(SoundFX::FX::PressKey);
    return digits.size() == 4 ? 1 : 0;
}

void PinPad::render(ir_t& ir) {
    const Ui::Palette& p = Ui::pal();
    Ui::card(CX, CY, CW, CH, 18, 0.0f);
    Ui::textCentered(320, CY + 16, tr(title), 20, p.text);
    if (note) {   /* within the card, however long the language makes it */
        std::string n = tr(note);
        while (Ui::textWidth(n.c_str(), 13) > CW - 32 && n.size() > 4) {
            n.pop_back();
            while (!n.empty() && ((unsigned char)n.back() & 0xC0) == 0x80) n.pop_back();
        }
        if (n != tr(note)) n += "...";
        Ui::textCentered(320, CY + 44, n.c_str(), 13, p.textDim);
    }
    /* the four digits, as dots; a wrong code shakes them */
    float dx = 0;
    const unsigned long long el = shakeSince ? ticks_to_millisecs(gettime()) - shakeSince : 1000;
    if (el < 400) dx = sinf(el * 0.06f) * 10.0f * (1.0f - el / 400.0f);
    for (int i = 0; i < 4; ++i) {
        float x = 320 - 2 * 46 + i * 46 + 4 + dx;
        Ui::roundRect(x, CY + 72, 38, 44, 10, p.field);
        Ui::roundBorder(x, CY + 72, 38, 44, 10, 1.5f, i == (int)digits.size() ? p.accent : p.fieldBorder);
        if (i < (int)digits.size()) Ui::circle(x + 19, CY + 94, 7, p.text);
    }
    for (int k = 0; k < 11; ++k) {
        int x = KX0 + keyCol(k) * (KW + KG), y = KY0 + keyRow(k) * (KH + KG);
        Ui::key(x, y, KW, KH, KEYS[k], k == sel, false, k == 9, false);
    }
    const Ui::Hint l[] = { { "A", N_("Type") } };
    const Ui::Hint r[] = { { "B", digits.empty() ? N_("Back") : N_("Erase") } };
    Ui::footer(l, 1, r, 1);
}

std::string PinPad::newSalt() {
    char s[24];
    snprintf(s, sizeof(s), "%08lx%08x", (unsigned long)gettime(), (unsigned)rand());
    return s;
}

std::string PinPad::hash(const std::string& salt, const std::string& code) {
    const std::string in = salt + ":" + code;
    unsigned char out[32];
    mbedtls_sha256((const unsigned char*)in.data(), in.size(), out, 0);
    char hex[65];
    for (int i = 0; i < 32; ++i) snprintf(hex + i * 2, 3, "%02x", out[i]);
    return hex;
}
