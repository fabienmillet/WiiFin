/* Drawing helpers shared by the LibraryView screens (LibraryView*.cpp, Library*.cpp). */
#include "LibraryDraw.h"
#include "Ui.h"
#include "../core/Text.h"
#include "../core/Utils.h"
#include <stdio.h>

namespace LibDraw {

// ---------------------------------------------------------------------------
// Utility: filter a UTF-8 string to codepoints DejaVu Sans covers, then
// truncate at maxCp codepoints. Use for any list label rendered with `font`.
// ---------------------------------------------------------------------------
std::string filterDejaVu(const std::string& s, int maxCp) {
    std::string out;
    const unsigned char* p = (const unsigned char*)s.c_str();
    int count = 0;
    while (*p && count < maxCp) {
        uint32_t cp; int seqLen;
        if      (*p < 0x80) { cp = *p;          seqLen = 1; }
        else if (*p < 0xE0) { cp = *p & 0x1F;   seqLen = 2; }
        else if (*p < 0xF0) { cp = *p & 0x0F;   seqLen = 3; }
        else                { cp = *p & 0x07;    seqLen = 4; }
        bool valid = true;
        for (int i = 1; i < seqLen; i++) {
            if ((p[i] & 0xC0) != 0x80) { valid = false; break; }
            cp = (cp << 6) | (p[i] & 0x3F);
        }
        if (!valid) { p++; continue; }
        bool ok = cp < 0x0500 || (cp >= 0x2000 && cp <= 0x26FF);
        if (ok) {
            for (int i = 0; i < seqLen; i++) out += (char)p[i];
            ++count;
        }
        p += seqLen;
    }
    if (*p) out += "...";
    return out;
}

float focusOf(bool sel, bool hover) {
    return sel ? Ui::pulse() : (hover ? 0.55f : 0.0f);
}

// Shorten s (UTF-8) with "..." until it fits maxW pixels.
std::string fitText(GRRLIB_ttfFont* f, std::string s, int size, int maxW) {
    if ((int)Text::width(f, s.c_str(), size) <= maxW) return s;
    while (!s.empty() && (int)Text::width(f, (s + "...").c_str(), size) > maxW) {
        while (!s.empty() && (s.back() & 0xC0) == 0x80) s.pop_back();
        if (!s.empty()) s.pop_back();
    }
    return s + "...";
}

// Picture tile: shadow, focus halo, cover-cropped image (or a placeholder
// caption), border and optional progress bar.  w is the logical width; the
// drawn width follows the widescreen pre-squish like every texture.
void drawThumb(GRRLIB_texImg* tex, float x, float y, float w, float h,
               float focus, float prog, const char* caption,
               bool grow) {
    const Ui::Palette& p = Ui::pal();
    const float r = 10;
    float g  = (grow && focus > 0.0f) ? 4.0f : 0.0f;   // pops out a little when focused
    float vw = w * WiiUtils::wsScaleX() + g;
    float aspect = (w + g) / (h + g);
    x -= g * 0.5f; y -= g * 0.5f; h += g;

    Ui::shadow(x + 1, y + 3, vw - 2, h - 2, r, 6.0f, p.shadow);
    if (focus > 0.0f) Ui::shadow(x - 2, y - 2, vw + 4, h + 4, r + 2, 10.0f, Ui::alpha(p.glow, focus));
    if (tex && tex->w > 0 && tex->h > 0) {
        Ui::texCover(tex, x, y, vw, h, r, aspect);
    } else {
        Ui::roundRect(x, y, vw, h, r, p.cardTop, p.cardBottom);
        if (caption && *caption) {
            std::string c = fitText(Ui::font(), caption, 13, (int)vw - 12);
            Ui::textCentered(x + vw * 0.5f, y + h * 0.5f - 8, c.c_str(), 13, p.textDim);
        }
    }
    Ui::roundBorder(x, y, vw, h, r, 1.5f + focus * 1.5f,
                    Ui::mix(Ui::alpha(p.cardBorder, 0.8f), p.accent, focus));
    if (prog >= 0.0f) Ui::progress(x + 8, y + h - 12, vw - 16, 5, prog);
}

// Row card for the vertical lists.
void drawRow(int x, int y, int w, int h, float focus) {
    Ui::card(x, y + 2, w, h - 6, 10, focus);
}

void drawPageArrows(ir_t& ir, int upCy, int dnCy, bool canPrev, bool canNext,
                    int cx, int hitR) {
    auto over = [&](int cy) {
        return ir.valid && (int)ir.x >= cx - hitR && (int)ir.x < cx + hitR &&
               (int)ir.y >= cy - hitR && (int)ir.y < cy + hitR;
    };
    Ui::arrowButton(cx, upCy, true,  canPrev, over(upCy));
    Ui::arrowButton(cx, dnCy, false, canNext, over(dnCy));
}

// Small chevron at the ends of a horizontally scrolling row.
void drawRowChevron(float ax, float ay, bool left, bool active) {
    const Ui::Palette& p = Ui::pal();
    u32 c = active ? p.accent : Ui::alpha(p.textDim, 0.3f);
    if (left) Ui::triangle(ax - 7, ay, ax + 5, ay - 9, ax + 5, ay + 9, c);
    else      Ui::triangle(ax + 7, ay, ax - 5, ay + 9, ax - 5, ay - 9, c);
}

void sectionLabel(int x, int y, const char* s, bool active) {
    const Ui::Palette& p = Ui::pal();
    Ui::roundRect(x, y + 1, 3, 12, 1.5f, active ? p.accent : Ui::alpha(p.textDim, 0.5f));
    Ui::text(x + 9, y, s, 12, active ? p.text : p.textDim);
}

void headerLine(int y) {
    if (Ui::headerBand() > 0) return;   // the band already separates
    Ui::roundRect(20, y, 600, 2, 1, Ui::alpha(Ui::pal().cardBorder, 0.7f));
}

// Secondary header text: dim on plain backgrounds, translucent white on a band.
u32 headerDim() {
    return Ui::headerBand() > 0 ? 0xFFFFFFC0 : Ui::pal().textDim;
}

// Back chevron + title at the top left, shortened to maxW.
void drawBreadcrumb(const std::string& title, int size, int maxW) {
    const Ui::Palette& p = Ui::pal();
    float cy = 14 + size * 0.55f;
    Ui::triangle(20, cy, 28, cy - 7, 28, cy + 7, Ui::headerBand() > 0 ? 0xFFFFFFFF : p.accent);
    std::string t = fitText(Ui::font(), filterDejaVu(title, 60), size, maxW - 14);
    Ui::text(34, 14, t.c_str(), size, p.text);
}

void drawCount(int page, int perPage, int shown, int total) {
    char countStr[48];
    snprintf(countStr, sizeof(countStr), "%d-%d / %d",
             page * perPage + 1, page * perPage + shown, total);
    Ui::textRight(620, 18, countStr, 15, headerDim());
}

// Tabbed library header: tabs centred, breadcrumb in the room on the left.
void drawLibHeader(const std::string& libName, const char* const* tabNames,
                   int nTabs, int sel) {
    float x0 = Ui::tabs(320, 10, tabNames, nTabs, sel, 13);
    drawBreadcrumb(libName, 16, (int)x0 - 34 - 10);
    headerLine(46);
}

const char* const kMovieTabs[4] = { "Movies", "Collections", "Favorites", "Suggestions" };
const char* const kTvTabs[3]    = { "Series", "Suggestions", "Coming Up" };
const char* const kMusicTabs[3] = { "Albums", "Suggestions", "Playlists" };
const char* const kHomeTabs[3]  = { "Libraries", "Activity", "Favorites" };

}  // namespace LibDraw
