/* LibraryView: Item detail page and the resume prompt. */
#include "LibraryView.h"
#include "LibraryDraw.h"
#include "Ui.h"
#include "../input/Input.h"
#include "../core/Text.h"
#include "../core/Utils.h"
#include <stdio.h>

using namespace LibDraw;

bool LibraryView::updateDetail(ir_t& ir, bool aPressed) {
    // A: play
    if (aPressed) {
        if (detail.playbackPositionTicks > 0) {
            // There is a saved position — ask the user Continue / Start Over
            resumeSel = 0;
            state = State::ResumePrompt;
            return false;
        }
        // No saved position: play from the beginning immediately
        preparePlay(0LL);
        return true;
    }
    if (Input::isBackPressed()) {
        freeDetail();
        state = detailReturnState;
    } else if (Input::isUpPressed()) {
        detailFocusRow = 0;
    } else if (Input::isDownPressed()) {
        detailFocusRow = 1;
    } else if (Input::isLeftPressed()) {
        if (detailFocusRow == 0 && !detail.audioStreams.empty()) {
            if (--detailAudioSel < 0) detailAudioSel = (int)detail.audioStreams.size() - 1;
        } else if (detailFocusRow == 1 && !detail.subtitleStreams.empty()) {
            if (--detailSubSel < -1) detailSubSel = (int)detail.subtitleStreams.size() - 1;
        }
    } else if (Input::isRightPressed()) {
        if (detailFocusRow == 0 && !detail.audioStreams.empty()) {
            if (++detailAudioSel >= (int)detail.audioStreams.size()) detailAudioSel = 0;
        } else if (detailFocusRow == 1) {
            if (++detailSubSel >= (int)detail.subtitleStreams.size()) detailSubSel = -1;
        }
    }
    return false;
}

bool LibraryView::updateResumePrompt(ir_t& ir, bool aPressed) {
    // IR hover: set resumeSel to whichever button the cursor is over
    if (ir.valid) {
        const int DW2 = 340, DH2 = 120;
        const int DX2 = (640 - DW2) / 2;
        const int DY2 = (480 - DH2) / 2;
        const int BW2 = 130, BH2 = 30, BGAP2 = 16;
        const int bTotalW2 = BW2 * 2 + BGAP2;
        const int bStartX2 = DX2 + (DW2 - bTotalW2) / 2;
        const int bY2      = DY2 + DH2 - BH2 - 14;
        for (int i = 0; i < 2; ++i) {
            int bx = bStartX2 + i * (BW2 + BGAP2);
            if (ir.x >= bx && ir.x < bx + BW2 &&
                ir.y >= bY2 && ir.y < bY2 + BH2) {
                resumeSel = i;
                break;
            }
        }
    }
    // Left/Right toggles between Continue (0) and Start Over (1)
    if (Input::isLeftPressed() || Input::isRightPressed())
        resumeSel ^= 1;
    if (Input::isBackPressed()) {
        state = State::DetailReady;
        return false;
    }
    if (aPressed) {
        preparePlay(resumeSel == 0 ? detail.playbackPositionTicks : 0LL);
        return true;
    }
    return false;
}

// Resume prompt overlay
void LibraryView::renderResumePrompt() {
    const Ui::Palette& p = Ui::pal();
    GRRLIB_Rectangle(Ui::screenLeft(), 0, Ui::screenWidth(), 480, p.dim, 1);

    // Dialog box (DW/DH/button geometry shared with update())
    const int DW = 340, DH = 120;
    const int DX = (640 - DW) / 2;
    const int DY = (480 - DH) / 2;
    Ui::card(DX, DY, DW, DH + 34, 18, 0.0f);

    Ui::textCentered(320, DY + 14, "Resume playback?", 17, p.text);
    {
        int secs = (int)(detail.playbackPositionTicks / 10000000LL);
        int rh = secs / 3600, rm = (secs % 3600) / 60, rs = secs % 60;
        char hint[48];
        if (rh > 0) snprintf(hint, sizeof(hint), "at %d:%02d:%02d", rh, rm, rs);
        else        snprintf(hint, sizeof(hint), "at %d:%02d", rm, rs);
        Ui::textCentered(320, DY + 38, hint, 13, p.accentDark);
    }

    // Two buttons: Continue | From Start
    const char* btnLabels[2] = { "Continue", "From Start" };
    const int BW = 130, BH = 30, BGAP = 16;
    int bTotalW = BW * 2 + BGAP;
    int bStartX = DX + (DW - bTotalW) / 2;
    int bY      = DY + DH - BH - 14;
    for (int i = 0; i < 2; ++i)
        Ui::button(bStartX + i * (BW + BGAP), bY, BW, BH, btnLabels[i], 15,
                   resumeSel == i ? Ui::pulse() : 0.0f);

    // Hints inside the dialog
    const Ui::Hint h[] = { { "LR", "Select" }, { "A", "Confirm" }, { "B", "Cancel" } };
    float hw = 0;
    for (const auto& hi : h) hw += Ui::hintWidth(hi);
    float hx = 320 - (hw - 14) * 0.5f;
    Ui::roundRect(DX + 16, DY + DH - 2, DW - 32, 1.5f, 0.75f, Ui::alpha(p.cardBorder, 0.6f));
    for (const auto& hi : h) hx += Ui::hint(hx, DY + DH + 6, hi);
}

// ---------------------------------------------------------------
// drawDetailView()
// ---------------------------------------------------------------
void LibraryView::drawDetailView(ir_t& ir) {
    const Ui::Palette& p = Ui::pal();
    const int POSTER_X  = 20;
    const int POSTER_Y  = 30;
    const int INFO_X    = 240;
    const int INFO_W    = 390;

    // Returns true if s contains any codepoint >= U+3000 (CJK/Japanese range)
    auto hasJapanese = [](const std::string& s) -> bool {
        const unsigned char* p = (const unsigned char*)s.c_str();
        while (*p) {
            uint32_t cp;
            if      (*p < 0x80)  { cp = *p++; }
            else if (*p < 0xE0)  { cp = (*p++ & 0x1F) << 6;  cp |= (*p++ & 0x3F); }
            else if (*p < 0xF0)  { cp = (*p++ & 0x0F) << 12; cp |= (*p++ & 0x3F) << 6; cp |= (*p++ & 0x3F); }
            else                 { cp = (*p++ & 0x07) << 18; cp |= (*p++ & 0x3F) << 12; cp |= (*p++ & 0x3F) << 6; cp |= (*p++ & 0x3F); }
            if (cp >= 0x3000) return true;
        }
        return false;
    };

    // Episode thumbnails are 16:9; movie/show posters are portrait
    // both end before the info card (INFO_X - 14 = 226): 20 + 196 / 20 + 200
    const int POSTER_W2 = detailIsEpisode ? 196 : 200;
    const int POSTER_H2 = detailIsEpisode ? 110 : 285;

    float ws   = WiiUtils::wsScaleX();
    int   visW = (int)(POSTER_W2 * ws + 0.5f);

    // ---- Info panel ----
    Ui::card(INFO_X - 14, POSTER_Y - 12, 640 - (INFO_X - 14) - 6, 438 - (POSTER_Y - 12), 16, 0.0f);

    // ---- Thumbnail / Poster ----
    bool posterHover = ir.valid
        && ir.x >= POSTER_X && ir.x < POSTER_X + visW
        && ir.y >= POSTER_Y && ir.y < POSTER_Y + POSTER_H2;
    drawThumb(detailTex, POSTER_X, POSTER_Y, POSTER_W2, POSTER_H2,
              posterHover ? 0.8f : 0.0f, progressOf(detail), detail.name.c_str(), false);

    // ---- Play button overlay (shown when cursor hovers the poster) ----
    if (posterHover) {
        Ui::roundRect(POSTER_X, POSTER_Y, visW, POSTER_H2, 10, 0x00000070);
        float cx = POSTER_X + visW * 0.5f;
        float cy = POSTER_Y + POSTER_H2 * 0.5f - 8;
        Ui::shadow(cx - 26, cy - 24, 52, 52, 26, 6.0f, 0x00000060);
        Ui::circle(cx, cy, 26, p.accent);
        Ui::roundBorder(cx - 26, cy - 26, 52, 52, 26, 2.0f, 0xFFFFFFE0);
        Ui::triangle(cx - 8, cy - 12, cx + 13, cy, cx - 8, cy + 12, 0xFFFFFFFF);
        Ui::textCentered(cx, cy + 32, "Lire", 15, 0xFFFFFFFF);
    }

    // ---- Title ----
    int y = POSTER_Y;
    {
        bool jp = hasJapanese(detail.name);
        std::string title = jp ? detail.name : fitText(font, filterDejaVu(detail.name, 60), 22, INFO_W - 12);
        Text::print(INFO_X, y, jp ? jpFont : font, title.c_str(), 22, p.text);
    }
    y += 32;

    // ---- Year  Runtime  Rating (chips) ----
    {
        char chips[3][24];
        int nChips = 0;
        if (detail.year)
            snprintf(chips[nChips++], sizeof(chips[0]), "%d", detail.year);
        if (detail.runtimeTicks > 0) {
            int secs = (int)(detail.runtimeTicks / 10000000LL);
            int h = secs / 3600, m = (secs % 3600) / 60;
            if (h > 0) snprintf(chips[nChips++], sizeof(chips[0]), "%dh %02dmin", h, m);
            else       snprintf(chips[nChips++], sizeof(chips[0]), "%dmin", m);
        }
        if (!detail.officialRating.empty())
            snprintf(chips[nChips++], sizeof(chips[0]), "%s", detail.officialRating.c_str());
        int cx = INFO_X;
        for (int i = 0; i < nChips; ++i) {
            int cw = Ui::textWidth(chips[i], 13) + 18;
            Ui::roundRect(cx, y, cw, 20, 10, p.field);
            Ui::roundBorder(cx, y, cw, 20, 10, 1.0f, p.fieldBorder);
            Ui::textCentered(cx + cw / 2, y + 3, chips[i], 13, p.textDim);
            cx += cw + 6;
        }
        if (nChips) y += 28;
    }

    // ---- Resume hint (shown when playback position is saved) ----
    if (detail.playbackPositionTicks > 0 && detail.runtimeTicks > 0) {
        int secs = (int)(detail.playbackPositionTicks / 10000000LL);
        int h    = secs / 3600, m = (secs % 3600) / 60;
        int pct  = (int)(detail.playbackPositionTicks * 100LL / detail.runtimeTicks);
        char buf[48];
        if (h > 0) snprintf(buf, sizeof(buf), "Resume at %dh%02d (%d%%)", h, m, pct);
        else       snprintf(buf, sizeof(buf), "Resume at %dmin (%d%%)", m, pct);
        Ui::triangle(INFO_X, y + 3, INFO_X + 9, y + 8, INFO_X, y + 13, p.accentDark);
        Ui::text(INFO_X + 14, y, buf, 14, p.accentDark);
        y += 20;
    }

    // ---- Genres ----
    if (!detail.genres.empty()) {
        std::string g;
        for (size_t i = 0; i < detail.genres.size(); i++) {
            if (i) g += "  \xc2\xb7  ";
            g += detail.genres[i];
        }
        g = fitText(font, g, 14, INFO_W - 12);
        Ui::text(INFO_X, y, g.c_str(), 14, p.textDim);
        y += 18;
    }
    y += 6;

    // ---- Overview (pre-computed lines, no per-frame width measuring) ----
    if (!detailLines.empty()) {
        for (const auto& line : detailLines) {
            Ui::text(INFO_X, y, line.c_str(), 13, Ui::mix(p.text, p.textDim, 0.25f));
            y += 17;
        }
        y += 6;
    }

    // ---- Cast & crew (max 6) ----
    if (!detail.people.empty()) {
        Ui::text(INFO_X, y, "Cast & crew", 14, p.accentDark);
        y += 18;
        int shown = 0;
        for (const auto& pp : detail.people) {
            if (shown >= 6) break;
            if (pp.name.empty() && pp.character.empty()) continue;
            int rx = INFO_X + 8;
            if (!pp.name.empty() && hasJapanese(pp.name)) {
                // Japanese VA name: render with jpFont, then Latin suffix with font
                Text::print(rx, y, jpFont, pp.name.c_str(), 13, p.text);
                rx += Text::width(jpFont, pp.name.c_str(), 13);
                std::string suffix;
                if (pp.role == "Director")          suffix = " (director)";
                else if (!pp.character.empty())     suffix = " - " + pp.character;
                if (!suffix.empty())
                    Ui::text(rx, y, suffix.c_str(), 13, p.textDim);
            } else {
                // All-Latin line: name in text colour, role dimmed
                const std::string& nm = !pp.name.empty() ? pp.name : pp.character;
                Ui::text(rx, y, nm.c_str(), 13, p.text);
                std::string suffix;
                if (!pp.name.empty()) {
                    if (pp.role == "Director")          suffix = " (director)";
                    else if (!pp.character.empty())     suffix = " - " + pp.character;
                }
                if (!suffix.empty()) {
                    int nw = Ui::textWidth(nm.c_str(), 13);
                    suffix = fitText(font, suffix, 13, INFO_W - 16 - nw);
                    Ui::text(rx + nw, y, suffix.c_str(), 13, p.textDim);
                }
            }
            y += 16;
            shown++;
        }
        y += 4;
    }

    // ---- Audio / Subtitle stream selectors ----
    // Drawn at a fixed bottom-anchor position so they're always visible.
    const int STREAM_Y0 = 390;
    const int STREAM_ROW_H = 24;
    const bool hasAudio = !detail.audioStreams.empty();
    const bool hasSub   = !detail.subtitleStreams.empty();
    if (hasAudio || hasSub) {
        Ui::roundRect(INFO_X, STREAM_Y0 - 8, INFO_W - 14, 1.5f, 0.75f, Ui::alpha(p.cardBorder, 0.6f));

        // Validate UTF-8 and truncate at a safe codepoint boundary so the
        // renderer never receives a broken multi-byte sequence.
        auto safeTitle = [](const char* s, int maxCodepoints) -> std::string {
            if (!s) return "-";
            std::string out;
            const unsigned char* p = (const unsigned char*)s;
            int count = 0;
            while (*p && count < maxCodepoints) {
                int seqLen;
                if      (*p < 0x80) seqLen = 1;
                else if (*p < 0xE0) seqLen = 2;
                else if (*p < 0xF0) seqLen = 3;
                else                seqLen = 4;
                bool valid = true;
                for (int i = 1; i < seqLen; i++) {
                    if ((p[i] & 0xC0) != 0x80) { valid = false; break; }
                }
                if (!valid) { p++; continue; }
                for (int i = 0; i < seqLen; i++) out += (char)p[i];
                p += seqLen;
                ++count;
            }
            if (*p) out += "...";
            if (out.empty()) out = "?";
            return out;
        };

        for (int row = 0; row < 2; row++) {
            if (row == 0 && !hasAudio) continue;
            if (row == 1 && !hasSub)   continue;

            int ry = STREAM_Y0 + row * STREAM_ROW_H;
            bool focused = (detailFocusRow == row);
            if (focused)
                Ui::roundRect(INFO_X - 6, ry - 3, INFO_W - 8, STREAM_ROW_H - 3, (STREAM_ROW_H - 3) * 0.5f,
                              Ui::mix(p.accent, 0xFFFFFFFF, 0.2f), p.accentDark);

            u32 labelCol = focused ? p.textOnAccent : p.textDim;
            u32 valueCol = focused ? p.textOnAccent : p.text;

            const char* label = row == 0 ? "Audio" : "Subtitles";
            const char* rawTitle;
            if (row == 0)
                rawTitle = detailAudioSel < (int)detail.audioStreams.size()
                    ? detail.audioStreams[detailAudioSel].displayTitle.c_str() : "-";
            else
                rawTitle = (detailSubSel == -1)
                    ? "Off"
                    : (detailSubSel < (int)detail.subtitleStreams.size()
                        ? detail.subtitleStreams[detailSubSel].displayTitle.c_str() : "-");
            Ui::text(INFO_X + 4, ry, label, 13, labelCol);
            std::string t = safeTitle(rawTitle, 36);
            char buf[64];
            snprintf(buf, sizeof(buf), "\xe2\x80\xb9 %s \xe2\x80\xba", t.c_str());
            GRRLIB_ttfFont* tf = hasJapanese(t) ? jpFont : font;
            Text::print(INFO_X + 74, ry, tf, buf, 13, valueCol);
        }
    }

    // ---- Footer ----
    const Ui::Hint l[] = { { "A", "Play" }, { "B", "Back" } };
    const Ui::Hint r[] = { { "UD", "Focus" }, { "LR", "Change" } };
    Ui::footer(l, 2, r, (hasAudio || hasSub) ? 2 : 0);
}
