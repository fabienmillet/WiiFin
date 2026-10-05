#pragma once
/* Drawing helpers shared by the LibraryView screens (Ui kit look). */
#include <grrlib.h>
#include <wiiuse/wpad.h>
#include <string>

namespace LibDraw {

// Filter a UTF-8 string to codepoints DejaVu Sans covers, then truncate at
// maxCp codepoints ("..." appended).  Use for any label drawn with the UI font.
std::string filterDejaVu(const std::string& s, int maxCp);

float focusOf(bool sel, bool hover);
// Shorten s (UTF-8) with "..." until it fits maxW pixels.
std::string fitText(GRRLIB_ttfFont* f, std::string s, int size, int maxW);
// Watched fraction of an item, -1 when not started.
template <typename T>
float progressOf(const T& it) {
    if (it.playbackPositionTicks <= 0 || it.runtimeTicks <= 0) return -1.0f;
    return (float)it.playbackPositionTicks / (float)it.runtimeTicks;
}
// Picture tile: shadow, focus halo, cover-cropped image (or a placeholder
// caption), border and optional progress bar.
void drawThumb(GRRLIB_texImg* tex, float x, float y, float w, float h,
               float focus, float prog = -1.0f, const char* caption = nullptr,
               bool grow = true);
// Row card for the vertical lists.
void drawRow(int x, int y, int w, int h, float focus);
void drawPageArrows(ir_t& ir, int upCy, int dnCy, bool canPrev, bool canNext,
                    int cx, int hitR);
// Small chevron at the ends of a horizontally scrolling row.
void drawRowChevron(float ax, float ay, bool left, bool active);
void sectionLabel(int x, int y, const char* s, bool active);
void headerLine(int y);
// Secondary header text colour.
u32  headerDim();
// Back chevron + title at the top left, shortened to maxW.
void drawBreadcrumb(const std::string& title, int size, int maxW);
void drawCount(int page, int perPage, int shown, int total);
// Tabbed library header: tabs centred, breadcrumb in the room on the left.
void drawLibHeader(const std::string& libName, const char* const* tabNames,
                   int nTabs, int sel);

extern const char* const kMovieTabs[4];
extern const char* const kTvTabs[3];
extern const char* const kMusicTabs[3];
extern const char* const kHomeTabs[3];

}  // namespace LibDraw
