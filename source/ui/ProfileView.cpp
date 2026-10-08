#include "ProfileView.h"
#include "../core/Text.h"
#include "Ui.h"
#include "../input/Input.h"
#include "../core/SoundFX.h"
#include <stdio.h>
#include <string.h>

ProfileView::ProfileView(GRRLIB_ttfFont* f, GRRLIB_texImg* cursor,
                         const std::vector<SavedProfile>& p)
    : font(f), cursorTex(cursor), profiles(p)
{
    focusedRow = 0;
}

/* -----------------------------------------------------------------------
 * update()
 * ----------------------------------------------------------------------- */
ProfileResult ProfileView::update(ir_t& ir) {
    irMode = ir.valid;   /* no pointer on the screen: the D-pad drives, A acts on the highlighted item */

    int totalRows = (int)profiles.size() + 1; /* profiles + "Add New" row */
    const int rowBefore = focusedRow;

    /* D-pad navigation */
    if (Input::isUpPressed()) {
        focusedRow = (focusedRow - 1 + totalRows) % totalRows;
        irMode = false;
        confirmDelete = false;
    }
    if (Input::isDownPressed()) {
        focusedRow = (focusedRow + 1) % totalRows;
        irMode = false;
        confirmDelete = false;
    }

    /* IR hover */
    bool irHoveredRow = false;
    if (ir.valid) {
        int vis = (totalRows < MAX_VIS) ? totalRows : MAX_VIS;
        for (int i = 0; i < vis; i++) {
            int ry = ROW_Y0 + i * ROW_H;
            if (ir.x >= ROW_X && ir.x <= ROW_X + ROW_W &&
                ir.y >= ry    && ir.y <= ry + ROW_H - 4) {
                if (focusedRow != i) confirmDelete = false;
                focusedRow = i;
                irMode = true;
                irHoveredRow = true;
            }
        }
    }

    if (focusedRow != rowBefore) SoundFX::play(SoundFX::FX::Move);

    /* B = back / cancel confirm */
    if (Input::isBackPressed()) {
        SoundFX::play(SoundFX::FX::Back);
        if (confirmDelete) { confirmDelete = false; return ProfileResult::None; }
        return ProfileResult::Back;
    }

    /* MINUS (isLPressed) = toggle delete confirm for a profile row */
    if (Input::isLPressed() && focusedRow < (int)profiles.size()) {
        confirmDelete = !confirmDelete;
        SoundFX::play(confirmDelete ? SoundFX::FX::Select : SoundFX::FX::Back);
        return ProfileResult::None;
    }

    /* A = confirm */
    if (Input::isAJustPressed() && (irHoveredRow || (!ir.valid && !irMode))) {
        SoundFX::play(SoundFX::FX::Open);
        if (confirmDelete) {
            confirmDelete = false;
            selectedIdx = focusedRow;
            return ProfileResult::DeleteOne;
        }
        if (focusedRow == (int)profiles.size()) {
            return ProfileResult::AddNew;
        }
        selectedIdx = focusedRow;
        return ProfileResult::Selected;
    }

    return ProfileResult::None;
}

/* -----------------------------------------------------------------------
 * render()
 * ----------------------------------------------------------------------- */
void ProfileView::render(ir_t& ir) {
    const Ui::Palette& pal = Ui::pal();
    Ui::background();
    Ui::header(profiles.empty() ? "Add a Profile" : "Who's watching?");

    int totalRows = (int)profiles.size() + 1;
    int vis = (totalRows < MAX_VIS) ? totalRows : MAX_VIS;

    for (int i = 0; i < vis; i++) {
        focusAnim[i] = Ui::approach(focusAnim[i], i == focusedRow ? 1.0f : 0.0f);
        float f   = focusAnim[i];
        float ry  = ROW_Y0 + i * ROW_H;
        float g   = 6.0f * f;                       /* grow when focused */
        float x = ROW_X - g, w = ROW_W + g * 2, y = ry - g * 0.3f, h = ROW_H - 8 + g * 0.6f;
        bool isAdd = (i == (int)profiles.size());

        Ui::card(x, y, w, h, 16, f);
        float cx = x + 34, cy = y + h * 0.5f;
        if (isAdd) {
            Ui::circle(cx, cy, 20, Ui::mix(pal.cardBorder, pal.accent, f));
            Ui::circle(cx, cy, 17, pal.cardTop);
            Ui::textCentered(cx, cy - 15, "+", 26, Ui::mix(pal.textDim, pal.accentDark, f));
            Ui::text(x + 68, cy - 10, "Add New Profile", 19, Ui::mix(pal.textDim, pal.accentDark, f));
            continue;
        }
        const SavedProfile& p = profiles[i];

        /* Avatar: first letter of the user name in a Wii-blue disc */
        Ui::avatar(cx, cy, 20, p.username.c_str(), 0.0f);

        const char* name = !p.username.empty() ? p.username.c_str()
                         : !p.serverName.empty() ? p.serverName.c_str() : p.serverUrl.c_str();
        char server[96];
        if (!p.serverName.empty()) snprintf(server, sizeof(server), "%s  \xe2\x80\xa2  %s", p.serverName.c_str(), p.serverUrl.c_str());
        else                       snprintf(server, sizeof(server), "%s", p.serverUrl.c_str());
        Ui::text(x + 68, y + 9,  name,   20, pal.text);
        Ui::text(x + 68, y + 34, server, 13, pal.textDim);
    }

    /* Delete confirmation */
    if (confirmDelete) {
        GRRLIB_Rectangle(Ui::screenLeft(), 0, Ui::screenWidth(), 480, pal.dim, 1);
        Ui::card(140, 170, 360, 130, 20, 0.0f);
        Ui::textCentered(320, 192, "Delete this profile?", 21, pal.danger);
        Ui::textCentered(320, 222, "Its saved sign-in will be forgotten.", 14, pal.textDim);
        Ui::button(170, 248, 140, 38, "Delete", 17, 1.0f);
        Ui::button(330, 248, 140, 38, "Cancel", 17, 0.0f);
    }

    if (confirmDelete) {
        static const Ui::Hint left[] = { { "A", "Delete" }, { "B", "Cancel" } };
        Ui::bottomBar(left, 2);
    } else {
        static const Ui::Hint left[]  = { { "A", "Open" }, { "-", "Delete" } };
        static const Ui::Hint right[] = { { "B", "Back" } };
        Ui::bottomBar(left, 2, right, 1);
    }

    /* IR cursor */
    if (ir.valid && cursorTex) {
        orient_t orient;
        WPAD_Orientation(WPAD_CHAN_0, &orient);
        GRRLIB_DrawImg((int)ir.x - 20, (int)ir.y - 4,
                       cursorTex, orient.roll, 1, 1, 0xFFFFFFFF);
    }
}
