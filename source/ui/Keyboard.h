#pragma once
#include <grrlib.h>
#include <wiiuse/wpad.h>
#include <string>

/* -----------------------------------------------------------------------
 * Keyboard — on-screen keyboard shared by the search and sign-in screens.
 *
 *   letters page    1 2 3 4 5 6 7 8 9 0 -
 *                   q w e r t y u i o p /
 *                   a s d f g h j k l : @
 *                   z x c v b n m , . _ '
 *                   [ Shift ][ #+= ][  Space  ][ Delete ][ Search ]
 *
 * Pointer: the highlight follows it and A types the key under it (A over
 * empty space does nothing).  D-pad: moves the highlight, A types it.
 * B deletes a character (leaves when the text is empty), - is Shift (press
 * twice for caps lock), + presses the action key.
 * ----------------------------------------------------------------------- */
class Keyboard {
public:
    enum class Result { None, Enter, Cancel };

    /* Top-left corner of the keys and the action key's label. */
    void setOrigin(float x, float y)    { x0 = x; y0 = y; }
    void setEnterLabel(const char* s)   { enterLabel = s; }
    void reset();

    /* Edits text (at most maxLen bytes). */
    Result update(const ir_t& ir, std::string& text, size_t maxLen = 128);
    void   render(const ir_t& ir) const;

    static const int COLS = 11, ROWS = 5;
    static const int CELL_W = 46, CELL_H = 40;
    float width()  const { return COLS * CELL_W; }
    float height() const { return ROWS * CELL_H; }

private:
    enum Special { K_NONE = 0, K_SHIFT, K_SYM, K_SPACE, K_DEL, K_ENTER };
    struct Key { int row, col, span; char ch; Special sp; };

    float       x0 = 67, y0 = 140;
    const char* enterLabel = "OK";
    int   page  = 0;          /* 0 letters, 1 symbols         */
    int   shift = 0;          /* 0 off, 1 next letter, 2 lock */
    int   selRow = 1, selCol = 0;
    unsigned long long lastShiftMs = 0;
    mutable unsigned long long flashMs = 0;   /* key press feedback */
    int   flashRow = -1, flashCol = -1;

    Key  keyAt(int row, int col) const;               /* key covering a cell */
    bool keyUnder(const ir_t& ir, int& row, int& col) const;
    Result press(const Key& k, std::string& text, size_t maxLen);
    const char* label(const Key& k, char* buf) const;
};
