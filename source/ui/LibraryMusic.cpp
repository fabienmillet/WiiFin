/* LibraryView: Music: an album's or a playlist's track list. */
#include "LibraryView.h"
#include "LibraryDraw.h"
#include "Ui.h"
#include "../input/Input.h"
#include <stdio.h>

using namespace LibDraw;

bool LibraryView::updateMusicTracks(ir_t& ir, bool aPressed) {
    int n = (int)musicTracks.size();
    if (Input::isBackPressed()) {
        if (tracksFromHome) {
            tracksFromHome = false;
            state = State::LibsReady;
        } else if (musicTab == 1)
            state = State::MusicSuggestionsReady;
        else
            state = State::ItemsReady;
        return false;
    }
    if (Input::isUpPressed())   {
        if (musicTrackSel > 0) { musicTrackSel--; clampMusicTrackScroll(); }
        irMode = false;
    }
    if (Input::isDownPressed()) {
        if (musicTrackSel < n - 1) { musicTrackSel++; clampMusicTrackScroll(); }
        irMode = false;
    }
    // IR hover
    if (irMode && ir.valid && !Input::isUpPressed() && !Input::isDownPressed()) {
        for (int i = 0; i < MUSIC_TRACKS_VISIBLE; i++) {
            int idx = musicTrackTop + i;
            if (idx >= n) break;
            int ry = LIST_Y + i * ROW_H;
            if (ir.x >= LIST_X && ir.x <= LIST_X + LIST_W &&
                ir.y >= ry && ir.y < ry + ROW_H) {
                musicTrackSel = idx;
                clampMusicTrackScroll();
                irMode = true;
            }
        }
    }
    // A: play this track (with full album context for prev/next)
    if (aPressed && n > 0 && musicTrackSel < n) {
        pendingMusicTracks.clear();
        for (const auto& at : musicTracks) {
            MusicTrack t;
            t.id           = at.id;
            t.title        = at.name;
            t.artist       = at.artist.empty() ? musicAlbumArtist : at.artist;
            t.album        = musicAlbumName;
            t.runtimeTicks = at.runtimeTicks;
            pendingMusicTracks.push_back(t);
        }
        pendingMusicTrackIdx = musicTrackSel;
        pendingPlayIsMusic   = true;
        return true;
    }
    return false;
}

// Music track list
void LibraryView::renderMusicTracks(ir_t& ir) {
    const Ui::Palette& p = Ui::pal();
    // Header: album name + artist
    std::string hdr = musicAlbumName;
    if (!musicAlbumArtist.empty()) hdr += "  \xe2\x80\x94  " + musicAlbumArtist;
    drawBreadcrumb(hdr, 18, 580);
    headerLine(46);

    int n = (int)musicTracks.size();
    for (int i = 0; i < MUSIC_TRACKS_VISIBLE; i++) {
        int idx = musicTrackTop + i;
        if (idx >= n) break;
        bool sel   = (idx == musicTrackSel);
        bool hover = ir.valid &&
                     ir.y >= LIST_Y + i * ROW_H &&
                     ir.y <  LIST_Y + (i + 1) * ROW_H &&
                     ir.x >= LIST_X && ir.x <= LIST_X + LIST_W;
        int ry = LIST_Y + i * ROW_H;
        float f = focusOf(sel, hover);
        drawRow(LIST_X, ry, LIST_W, ROW_H, f);
        const JellyfinAudioItem& at = musicTracks[idx];

        // Track number in a circle
        int tx = LIST_X + 14;
        if (at.trackNumber > 0) {
            char num[12];
            snprintf(num, sizeof(num), "%d", at.trackNumber);
            Ui::circle(tx + 11, ry + 22, 11, sel ? p.accent : Ui::alpha(p.cardBorder, 0.6f));
            Ui::textCentered(tx + 11, ry + 15, num, 12, sel ? p.textOnAccent : p.text);
            tx += 32;
        }
        // Duration on the right
        int dw = 0;
        if (at.runtimeTicks > 0) {
            int secs = (int)(at.runtimeTicks / 10000000LL);
            char dur[24];
            snprintf(dur, sizeof(dur), "%d:%02d", secs / 60, secs % 60);
            dw = Ui::textWidth(dur, 15) + 12;
            Ui::textRight(LIST_X + LIST_W - 14, ry + 13, dur, 15, p.textDim);
        }
        std::string labelStr = fitText(font, filterDejaVu(at.name, 60), 18,
                                       LIST_X + LIST_W - 16 - dw - tx);
        Ui::text(tx, ry + 11, labelStr.c_str(), 18, Ui::mix(p.text, p.accentDark, f));
    }
    Ui::scrollbar(614, LIST_Y + 2, MUSIC_TRACKS_VISIBLE * ROW_H - 6, musicTrackTop,
                  MUSIC_TRACKS_VISIBLE, n);
    const Ui::Hint l[] = { { "A", "Play" }, { "B", "Back" } };
    Ui::footer(l, 2);
}
