#include "Keyboard.h"
#include "Ui.h"
#include "../input/Input.h"
#include "../core/SoundFX.h"

#include <string.h>
#include <ogc/lwp_watchdog.h>

static const char* const LETTERS[4] = {
    "1234567890-",
    "qwertyuiop/",
    "asdfghjkl:@",
    "zxcvbnm,._'",
};
static const char* const SYMBOLS[4] = {
    "!?#$%&*()=+",
    "\"<>[]{}|\\;~",
    "^`_-/:@,.'*",
    "1234567890.",
};

/* Bottom row: wide keys, as (first column, span) */
static const struct { int col, span; int sp; } BOTTOM[5] = {
    { 0, 2, 1 /* K_SHIFT */ }, { 2, 2, 2 /* K_SYM */ }, { 4, 3, 3 /* K_SPACE */ },
    { 7, 2, 4 /* K_DEL */ },   { 9, 2, 5 /* K_ENTER */ },
};

static unsigned long long nowMs() { return ticks_to_millisecs(gettime()); }

void Keyboard::reset()
{
    page = 0; shift = 0;
    selRow = 1; selCol = 0;
    flashMs = 0;
}

Keyboard::Key Keyboard::keyAt(int row, int col) const
{
    Key k{ row, col, 1, 0, K_NONE };
    if (row < 4) {
        const char* r = (page == 0 ? LETTERS : SYMBOLS)[row];
        k.ch = r[col];
        return k;
    }
    for (const auto& b : BOTTOM)
        if (col >= b.col && col < b.col + b.span) {
            k.col = b.col; k.span = b.span; k.sp = (Special)b.sp;
            return k;
        }
    return k;
}

bool Keyboard::keyUnder(const ir_t& ir, int& row, int& col) const
{
    if (!ir.valid) return false;
    float fx = (ir.x - x0) / CELL_W, fy = (ir.y - y0) / CELL_H;
    if (fx < 0 || fy < 0 || fx >= COLS || fy >= ROWS) return false;
    row = (int)fy; col = (int)fx;
    /* the gaps between keys count as nothing */
    float cx = fx - col, cy = fy - row;
    if (row < 4 && (cx < 0.04f || cx > 0.96f)) return false;
    if (cy < 0.05f || cy > 0.95f) return false;
    return true;
}

Keyboard::Result Keyboard::press(const Key& k, std::string& text, size_t maxLen)
{
    flashRow = k.row; flashCol = k.col; flashMs = nowMs();
    switch (k.sp) {
    case K_SHIFT: {
        unsigned long long t = nowMs();
        if (shift == 1 && t - lastShiftMs < 450) shift = 2;   /* double press: lock */
        else shift = shift ? 0 : 1;
        lastShiftMs = t;
        SoundFX::play(SoundFX::FX::PressKey);
        return Result::None;
    }
    case K_SYM:
        page ^= 1;
        SoundFX::play(SoundFX::FX::PressKey);
        return Result::None;
    case K_SPACE:
        if (text.size() < maxLen) text += ' ';
        SoundFX::play(SoundFX::FX::PressKey);
        return Result::None;
    case K_DEL:
        if (!text.empty()) text.pop_back();
        SoundFX::play(SoundFX::FX::Backspace);
        return Result::None;
    case K_ENTER:
        return Result::Enter;
    default: break;
    }
    if (!k.ch || text.size() >= maxLen) return Result::None;
    char c = k.ch;
    if (shift && c >= 'a' && c <= 'z') c = (char)(c - 32);
    text += c;
    if (shift == 1 && page == 0 && k.ch >= 'a' && k.ch <= 'z') shift = 0;   /* one-shot */
    SoundFX::play(SoundFX::FX::PressKey);
    return Result::None;
}

Keyboard::Result Keyboard::update(const ir_t& ir, std::string& text, size_t maxLen)
{
    /* Pointer: the highlight follows it */
    int pr, pc;
    bool overKey = keyUnder(ir, pr, pc);
    if (overKey) { Key k = keyAt(pr, pc); selRow = k.row; selCol = k.col; }

    /* D-pad */
    if (Input::isUpPressed())   selRow = (selRow + ROWS - 1) % ROWS;
    if (Input::isDownPressed()) selRow = (selRow + 1) % ROWS;
    if (Input::isLeftPressed()) {
        Key k = keyAt(selRow, selCol);
        selCol = (k.col + COLS - 1) % COLS;
    }
    if (Input::isRightPressed()) {
        Key k = keyAt(selRow, selCol);
        selCol = (k.col + k.span) % COLS;
    }

    if (Input::isAJustPressed()) {
        if (ir.valid) {
            /* with the pointer on screen, only the key under it counts */
            if (overKey) return press(keyAt(pr, pc), text, maxLen);
        } else {
            return press(keyAt(selRow, selCol), text, maxLen);
        }
    }
    if (Input::isLPressed()) press(Key{ 4, 0, 2, 0, K_SHIFT }, text, maxLen);   /* - */
    if (Input::isActionPressed()) return Result::Enter;                       /* + (GameCube Z) */
    if (Input::isBackPressed()) {
        if (text.empty()) return Result::Cancel;
        text.pop_back();
        SoundFX::play(SoundFX::FX::Backspace);
    }
    return Result::None;
}

const char* Keyboard::label(const Key& k, char* buf) const
{
    switch (k.sp) {
    case K_SHIFT: return shift == 2 ? "\xe2\x87\xaa Caps" : "\xe2\x87\xa7 Shift";
    case K_SYM:   return page == 0 ? "#+=" : "ABC";
    case K_SPACE: return "Space";
    case K_DEL:   return "\xe2\x86\x90 Delete";
    case K_ENTER: return enterLabel;
    default: break;
    }
    buf[0] = (shift && k.ch >= 'a' && k.ch <= 'z') ? (char)(k.ch - 32) : k.ch;
    buf[1] = 0;
    return buf;
}

void Keyboard::render(const ir_t& ir) const
{
    const Ui::Palette& p = Ui::pal();
    int hr = -1, hc = -1;
    bool hover = keyUnder(ir, hr, hc);
    Key hk = hover ? keyAt(hr, hc) : Key{ -1, -1, 0, 0, K_NONE };
    Key sk = keyAt(selRow, selCol);
    unsigned long long t = nowMs();

    for (int row = 0; row < ROWS; ++row) {
        for (int col = 0; col < COLS; ) {
            Key k = keyAt(row, col);
            col = k.col + k.span;
            float x = x0 + k.col * CELL_W + 2, y = y0 + row * CELL_H + 2;
            float w = k.span * CELL_W - 4, h = CELL_H - 4;
            bool sel = (k.row == sk.row && k.col == sk.col);
            bool hov = hover && k.row == hk.row && k.col == hk.col;
            /* typed: the key dips for a moment */
            float flash = (k.row == flashRow && k.col == flashCol && t - flashMs < 140)
                        ? 1.0f - (t - flashMs) / 140.0f : 0.0f;
            if (flash > 0) { x += 1.5f * flash; y += 2.0f * flash; w -= 3 * flash; h -= 3 * flash; }

            char buf[4];
            const char* lab = label(k, buf);
            bool special = k.sp != K_NONE;
            if (k.sp == K_ENTER) {
                /* action key: always in the accent colour */
                if (sel) Ui::shadow(x - 2, y - 2, w + 4, h + 4, 9, 8.0f, p.glow);
                Ui::roundRect(x, y, w, h, 7, Ui::mix(p.accent, 0xFFFFFFFF, sel ? 0.35f : 0.15f), p.accentDark);
                if (sel) Ui::roundBorder(x, y, w, h, 7, 2.0f, 0xFFFFFFC0);
                Ui::textCentered(x + w * 0.5f, y + (h - 15) * 0.5f - 1, lab, 15, p.textOnAccent);
                continue;
            }
            bool latched = (k.sp == K_SHIFT && shift) || (k.sp == K_SYM && page == 1);
            Ui::key(x, y, w, h, lab, sel, hov && !sel, special, latched);
        }
    }
}
