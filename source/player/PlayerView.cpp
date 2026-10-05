#include "PlayerView.h"
#include "../core/Text.h"
#include "../ui/Ui.h"
#include "WiiPlayer.h"
#include "VideoSurface.h"
#include "../input/Input.h"

#include <ogc/lwp_watchdog.h>
#include <stdio.h>

/* ---- Layout (640x480 virtual screen) ---------------------------------- */
static const int TOP_H      = 52;
static const int PANEL_Y    = 368;              /* bottom control panel   */
static const int BAR_X      = 32, BAR_W = 576;
static const int BAR_Y      = 390, BAR_H = 6;
static const int TIME_Y     = 402;
static const int BTN_Y      = 428, BTN_H = 38;
static const int BTN_W      = 84,  BTN_GAP = 12;
static const int BTN_COUNT  = 5;
static const int BTN_X0     = (640 - (BTN_COUNT * BTN_W + (BTN_COUNT - 1) * BTN_GAP)) / 2;
/* Zoom button (Fit / Fill), alone at the right end of the transport row */
static const int BTN_ZOOM   = BTN_COUNT, ZOOM_W = 48;
static float zoomX() { return Ui::screenRight() - 24 - ZOOM_W; }
static const int INTRO_X    = 466, INTRO_Y = 312, INTRO_W = 150, INTRO_H = 40;
static const int LIST_X     = 150, LIST_W  = 340;
static const int LIST_Y     = 112, ROW_H   = 34;
static const int LIST_ROWS  = 7;

static u64 nowMs() { return ticks_to_millisecs(gettime()); }

static std::string fmtTime(float secs)
{
    if (secs < 0.0f) secs = 0.0f;
    int s = (int)secs, h = s / 3600, m = (s % 3600) / 60;
    char buf[16];
    if (h > 0) snprintf(buf, sizeof(buf), "%d:%02d:%02d", h, m, s % 60);
    else       snprintf(buf, sizeof(buf), "%d:%02d", m, s % 60);
    return buf;
}

static bool inRect(const ir_t& ir, int x, int y, int w, int h)
{
    return ir.valid && ir.x >= x && ir.x < x + w && ir.y >= y && ir.y < y + h;
}

static std::string streamLabel(const MediaStream& s, int n)
{
    if (!s.displayTitle.empty()) return s.displayTitle;
    if (!s.language.empty() && !s.codec.empty()) return s.language + " (" + s.codec + ")";
    if (!s.language.empty()) return s.language;
    if (!s.codec.empty())    return s.codec;
    return "Track " + std::to_string(n);
}

/* ------------------------------------------------------------------------ */

PlayerView::PlayerView(GRRLIB_ttfFont* f, GRRLIB_texImg* cursor, GRRLIB_texImg* ring)
    : font(f), cursorTex(cursor), ringTex(ring)
{
    showControls();
}

void PlayerView::setContext(const PlayerViewContext& c)
{
    ctx          = c;
    pickAudio    = c.currentAudio;
    pickSub      = c.currentSub;
    introSkipped = false;
    seekPending  = false;
    seekInFlight = false;
    lastSeenPos  = -1.0f;
}

bool PlayerView::buffering() const
{
    return lastSeenPos > 0.0f && !g_mplayer_paused && !g_wiifin_loading_active &&
           nowMs() - lastPosChangeMs > 1000;
}

float PlayerView::position() const
{
    float t = g_mplayer_time_pos;
    return ctx.streamOrigin + (t > ctx.startSkip ? t : ctx.startSkip);
}

float PlayerView::duration() const
{
    if (ctx.runtime > 0.0f) return ctx.runtime;
    if (g_mplayer_duration > 0.0f) return ctx.streamOrigin + g_mplayer_duration;
    return 0.0f;
}

bool PlayerView::introVisible() const
{
    if (!ctx.intro.hasIntro || introSkipped || !busyMsg.empty()) return false;
    float showAt = ctx.intro.showPromptAt > 0 ? ctx.intro.showPromptAt : ctx.intro.introStart;
    float hideAt = ctx.intro.hidePromptAt > 0 ? ctx.intro.hidePromptAt : ctx.intro.introEnd + 2.0f;
    float pos = position();
    return pos >= showAt && pos < hideAt;
}

bool PlayerView::controlsVisible() const
{
    /* A pause held for rebuffering is not the user's: keep the picture clean */
    return panel != Panel::None || (g_mplayer_paused && rebufPercent < 0) ||
           nowMs() < controlsUntil;
}

void PlayerView::showControls(int ms)
{
    controlsUntil = nowMs() + ms;
}

/* MPlayer cannot seek in Jellyfin's live transcodes (the TS timestamps
 * don't start at 0 and the stream isn't seekable), so every seek restarts
 * the transcode at the target.  Repeated presses are merged into one. */
void PlayerView::nudgeSeek(float delta)
{
    float base = seekPending ? seekPendingTo : position();
    float target = base + delta;
    float dur = duration();
    if (target < 0.0f) target = 0.0f;
    if (dur > 0.0f && target > dur - 5.0f) target = dur - 5.0f;
    seekPending   = true;
    seekPendingTo = target;
    seekCommitAt  = nowMs() + 800;
    showControls();
}

float PlayerView::displayPosition() const
{
    if (seekPending)  return seekPendingTo;
    if (seekInFlight) return seekTo;
    return position();
}

void PlayerView::toast(const std::string& msg, int ms)
{
    toastMsg   = msg;
    toastUntil = nowMs() + ms;
}

void PlayerView::openPanel(Panel p)
{
    panel    = p;
    panelSel = 0;
    for (int i = 0; i < panelCount(); ++i)
        if (panelRowIsCurrent(i)) { panelSel = i; break; }
    panelTop = panelSel >= LIST_ROWS ? panelSel - LIST_ROWS + 1 : 0;
    prevHoverRow = -2;   /* first hover after opening counts as a move */
}

int PlayerView::panelCount() const
{
    if (panel == Panel::Audio) return (int)ctx.audioStreams.size();
    if (panel == Panel::Sub)   return (int)ctx.subStreams.size() + 1;   /* + "Off" */
    return 0;
}

std::string PlayerView::panelLabel(int row) const
{
    if (panel == Panel::Audio) return streamLabel(ctx.audioStreams[row], row + 1);
    if (row == 0) return "Off";
    return streamLabel(ctx.subStreams[row - 1], row);
}

bool PlayerView::panelRowIsCurrent(int row) const
{
    if (panel == Panel::Audio) return ctx.audioStreams[row].index == ctx.currentAudio;
    if (row == 0) return ctx.currentSub < 0;
    return ctx.subStreams[row - 1].index == ctx.currentSub;
}

PlayerView::Action PlayerView::activatePanelRow(int row)
{
    if (row < 0 || row >= panelCount()) return Action::None;
    bool current = panelRowIsCurrent(row);
    Panel p = panel;
    panel = Panel::None;
    showControls();
    if (current) return Action::None;
    if (p == Panel::Audio) {
        pickAudio = ctx.audioStreams[row].index;
        return Action::Audio;
    }
    pickSub = (row == 0) ? -1 : ctx.subStreams[row - 1].index;
    return Action::Sub;
}

void PlayerView::hitTest(const ir_t& ir)
{
    hoverButton = -1;
    hoverBar    = false;
    hoverRow    = -1;
    hoverIntro  = introVisible() && inRect(ir, INTRO_X, INTRO_Y, INTRO_W, INTRO_H);

    if (panel != Panel::None) {
        int rows = panelCount() - panelTop;
        if (rows > LIST_ROWS) rows = LIST_ROWS;
        for (int i = 0; i < rows; ++i)
            if (inRect(ir, LIST_X, LIST_Y + 44 + i * ROW_H, LIST_W, ROW_H))
                hoverRow = panelTop + i;
        return;
    }
    if (!controlsVisible()) return;
    hoverBar = inRect(ir, BAR_X - 8, BAR_Y - 12, BAR_W + 16, BAR_H + 24);
    for (int i = 0; i < BTN_COUNT; ++i)
        if (inRect(ir, BTN_X0 + i * (BTN_W + BTN_GAP), BTN_Y, BTN_W, BTN_H))
            hoverButton = i;
    if (inRect(ir, zoomX(), BTN_Y, ZOOM_W, BTN_H)) hoverButton = BTN_ZOOM;
}

void PlayerView::toggleZoom()
{
    bool fill = VideoSurface::zoom() != VideoSurface::Zoom::Fill;
    VideoSurface::setZoom(fill ? VideoSurface::Zoom::Fill : VideoSurface::Zoom::Fit);
    toast(fill ? "Zoom: fill the screen" : "Zoom: whole picture", 1500);
}

/* ---- Input ----------------------------------------------------------- */

PlayerView::Action PlayerView::update(u32 down, const ir_t& ir)
{
    {
        float t = g_mplayer_time_pos;
        if (t != lastSeenPos || g_mplayer_paused || g_wiifin_loading_active) {
            lastSeenPos     = t;
            lastPosChangeMs = nowMs();
        }
    }

    /* Pointer movement reveals the controls */
    if (ir.valid) {
        float dx = ir.x - lastIrX, dy = ir.y - lastIrY;
        if (lastIrX >= 0.0f && dx * dx + dy * dy > 16.0f) showControls();
        lastIrX = ir.x;
        lastIrY = ir.y;
    } else {
        lastIrX = lastIrY = -1.0f;
    }

    hitTest(ir);

    if (down & WPAD_BUTTON_HOME) return Action::Home;
    if (!busyMsg.empty()) return Action::None;   /* switching streams */

    const bool hasNext = ctx.episodeIdx + 1 < (int)ctx.episodes.size();
    const bool hasPrev = !ctx.episodes.empty() && ctx.episodeIdx > 0;

    if (seekPending && nowMs() >= seekCommitAt) {
        seekPending  = false;
        seekInFlight = true;
        seekTo = seekPendingTo;
        return Action::SeekTo;
    }

    /* ---- Track picker ---- */
    if (panel != Panel::None) {
        int n = panelCount();
        if (hoverRow >= 0 && hoverRow != prevHoverRow) panelSel = hoverRow;
        prevHoverRow = hoverRow;
        if (down & WPAD_BUTTON_B) { panel = Panel::None; showControls(); return Action::None; }
        if ((down & WPAD_BUTTON_UP)   && panelSel > 0)     --panelSel;
        if ((down & WPAD_BUTTON_DOWN) && panelSel + 1 < n) ++panelSel;
        if (panelSel < panelTop) panelTop = panelSel;
        if (panelSel >= panelTop + LIST_ROWS) panelTop = panelSel - LIST_ROWS + 1;
        if (down & WPAD_BUTTON_A) return activatePanelRow(panelSel);
        return Action::None;
    }

    const bool visible = controlsVisible();
    Action act = Action::None;

    if (down & WPAD_BUTTON_A) {
        if (hoverIntro || (introVisible() && hoverButton < 0 && !hoverBar)) {
            introSkipped = true;
            seekInFlight = true;
            seekTo = ctx.intro.introEnd + 0.5f;
            act = Action::SeekTo;
        } else if (visible && hoverBar && duration() > 0.0f) {
            float frac = (ir.x - BAR_X) / (float)BAR_W;
            if (frac < 0.0f) frac = 0.0f;
            if (frac > 1.0f) frac = 1.0f;
            seekInFlight = true;
            seekTo = frac * duration();
            act = Action::SeekTo;
        } else if (visible && hoverButton >= 0) {
            switch (hoverButton) {
            case 0: if (hasPrev) act = Action::Prev; else toast("No previous episode"); break;
            case 1: nudgeSeek(-10.0f); break;
            case 2: wii_player_pause_toggle(); break;
            case 3: nudgeSeek(+10.0f); break;
            case 4: if (hasNext) act = Action::Next; else toast("No next episode"); break;
            case BTN_ZOOM: toggleZoom(); break;
            }
        } else {
            wii_player_pause_toggle();
        }
        showControls();
    }

    if (down & WPAD_BUTTON_B) {
        if (visible) return Action::Back;
        showControls();
    }
    if (down & WPAD_BUTTON_RIGHT) nudgeSeek(+10.0f);
    if (down & WPAD_BUTTON_LEFT)  nudgeSeek(-10.0f);
    if (down & WPAD_BUTTON_UP)    { wii_player_vol_up();   toast("Volume +", 1000); }
    if (down & WPAD_BUTTON_DOWN)  { wii_player_vol_down(); toast("Volume -", 1000); }
    if (down & WPAD_BUTTON_PLUS) {
        if (hasNext) act = Action::Next; else toast("No next episode");
    }
    if (down & WPAD_BUTTON_MINUS) {
        if (hasPrev) act = Action::Prev; else toast("No previous episode");
    }
    if (down & Input::BTN_ZOOM) toggleZoom();   /* Classic ZL/ZR, GameCube Z */
    if (down & WPAD_BUTTON_1) {
        if (ctx.audioStreams.size() > 1) openPanel(Panel::Audio);
        else toast("No other audio track");
    }
    if (down & WPAD_BUTTON_2) {
        if (!ctx.subStreams.empty()) openPanel(Panel::Sub);
        else toast("No subtitles available");
    }
    return act;
}

/* ---- Drawing ----------------------------------------------------------- */

/* Trim s (adding "...") until it fits in maxW pixels. */
std::string PlayerView::fitText(const std::string& s, int size, int maxW)
{
    if ((int)Text::width(font, s.c_str(), size) <= maxW) return s;
    std::string t = s;
    while (!t.empty()) {
        t.pop_back();
        while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();  /* UTF-8 */
        if ((int)Text::width(font, (t + "...").c_str(), size) <= maxW) break;
    }
    return t + "...";
}

void PlayerView::drawSpinner(float cx, float cy)
{
    Ui::spinner(ringTex, cx, cy);
}

/* Centred pill with a message (busy state, toasts). */
static void messagePill(float cy, const std::string& msg, int size)
{
    const Ui::Palette& p = Ui::pal();
    int w = Ui::textWidth(msg.c_str(), size) + 40;
    int h = size + 18;
    Ui::shadow(320 - w / 2, cy - h / 2 + 2, w, h, h / 2, 6.0f, p.shadow);
    Ui::roundRect(320 - w / 2, cy - h / 2, w, h, h / 2, p.cardTop, p.cardBottom);
    Ui::roundBorder(320 - w / 2, cy - h / 2, w, h, h / 2, 1.5f, p.cardBorder);
    Ui::textCentered(320, cy - size / 2 - 1, msg.c_str(), size, p.text);
}

void PlayerView::render(const ir_t& ir)
{
    const Ui::Palette& p = Ui::pal();
    GRRLIB_FillScreen(0x000000FF);
    const bool hasVideo = VideoSurface::draw();

    /* Slide the control bars in/out */
    float target = controlsVisible() ? 1.0f : 0.0f;
    controlsAlpha += (target - controlsAlpha) * 0.25f;
    if (controlsAlpha < 0.01f) controlsAlpha = 0.0f;
    if (controlsAlpha > 0.99f) controlsAlpha = 1.0f;
    const float k = controlsAlpha;

    /* ---- Loading / busy ---- */
    const bool stalled = buffering() || rebufPercent >= 0;
    if (!busyMsg.empty() || g_wiifin_loading_active || !hasVideo || stalled) {
        if (hasVideo) GRRLIB_Rectangle(Ui::screenLeft(), 0, Ui::screenWidth(), 480, 0x00000099, 1);
        drawSpinner(320.0f, 220.0f);
        char msg[48];
        if (!busyMsg.empty())       snprintf(msg, sizeof(msg), "%s", busyMsg.c_str());
        else if (rebufPercent >= 0) snprintf(msg, sizeof(msg), "Buffering... %d%%", rebufPercent);
        else if (stalled)           snprintf(msg, sizeof(msg), "Buffering...");
        else                        snprintf(msg, sizeof(msg), "Loading...");
        messagePill(300, msg, 16);
    }

    /* ---- Controls ---- */
    if (k > 0.0f) {
        const float ease = 1.0f - (1.0f - k) * (1.0f - k);

        /* Top bar: title + button hints */
        Ui::pushOffset(0, -(1.0f - ease) * (TOP_H + 14));
        Ui::shadow(Ui::screenLeft() - 20, -30, Ui::screenWidth() + 40, TOP_H + 30, 22, 8.0f, p.shadow);
        Ui::roundRect(Ui::screenLeft() - 20, -30, Ui::screenWidth() + 40, TOP_H + 30, 22, p.barTop, p.barBottom);
        Ui::roundBorder(Ui::screenLeft() - 20, -30, Ui::screenWidth() + 40, TOP_H + 30, 22, 1.5f, p.barBorder);
        {
            /* zoom has a button on the Classic (ZR) and GameCube (Z) only */
            const Ui::ButtonStyle bs = Ui::buttonStyle();
            const Ui::Hint all[] = { { bs == Ui::ButtonStyle::GameCube ? "Z" : "ZR", "Zoom" },
                                     { "1", "Audio" }, { "2", "Subtitles" }, { "B", "Back" } };
            const Ui::Hint* hints = bs == Ui::ButtonStyle::WiiRemote ? all + 1 : all;
            const int nHints = bs == Ui::ButtonStyle::WiiRemote ? 3 : 4;
            float hw = 0;
            for (int i = 0; i < nHints; ++i) hw += Ui::hintWidth(hints[i]);
            float hx = 626 - hw;
            Ui::text(24, 15, fitText(ctx.title, 20, (int)hx - 40).c_str(), 20, p.text);
            for (int i = 0; i < nHints; ++i) hx += Ui::hint(hx, 16, hints[i]);
        }
        Ui::popOffset();

        /* Bottom panel */
        Ui::pushOffset(0, (1.0f - ease) * (480 - PANEL_Y + 14));
        Ui::shadow(Ui::screenLeft() - 20, PANEL_Y, Ui::screenWidth() + 40, 140, 22, 8.0f, p.shadow);
        Ui::roundRect(Ui::screenLeft() - 20, PANEL_Y, Ui::screenWidth() + 40, 140, 22, p.barTop, p.barBottom);
        Ui::roundBorder(Ui::screenLeft() - 20, PANEL_Y, Ui::screenWidth() + 40, 140, 22, 1.5f, p.barBorder);

        float dur = duration();
        float pos = displayPosition();
        float frac = dur > 0.0f ? pos / dur : 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        if (frac < 0.0f) frac = 0.0f;
        float barH = hoverBar ? BAR_H + 4 : BAR_H;
        float barY = BAR_Y - (barH - BAR_H) / 2;
        Ui::roundRect(BAR_X, barY, BAR_W, barH, barH / 2, Ui::alpha(p.cardBorder, 0.6f));
        if (frac > 0.0f)
            Ui::roundRect(BAR_X, barY, BAR_W * frac < barH ? barH : BAR_W * frac, barH, barH / 2,
                          Ui::mix(p.accent, 0xFFFFFFFF, 0.2f), p.accent);
        {
            float kx = BAR_X + BAR_W * frac, ky = BAR_Y + BAR_H / 2.0f, kr = hoverBar ? 9.0f : 7.0f;
            Ui::shadow(kx - kr, ky - kr + 1, kr * 2, kr * 2, kr, 4.0f, p.shadow);
            Ui::circle(kx, ky, kr, p.cardTop);
            Ui::roundBorder(kx - kr, ky - kr, kr * 2, kr * 2, kr, 2.0f, p.accent);
        }
        if (hoverBar && dur > 0.0f) {
            float f = (ir.x - BAR_X) / (float)BAR_W;
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            std::string t = fmtTime(f * dur);
            int w = Ui::textWidth(t.c_str(), 14) + 20;
            Ui::roundRect(ir.x - w / 2, BAR_Y - 34, w, 22, 11, p.accent, p.accentDark);
            Ui::triangle(ir.x - 5, BAR_Y - 12, ir.x + 5, BAR_Y - 12, ir.x, BAR_Y - 7, p.accentDark);
            Ui::textCentered(ir.x, BAR_Y - 31, t.c_str(), 14, p.textOnAccent);
        }
        Ui::text(BAR_X, TIME_Y, fmtTime(pos).c_str(), 15, p.text);
        Ui::textRight(BAR_X + BAR_W, TIME_Y, dur > 0.0f ? fmtTime(dur).c_str() : "--:--", 15, p.textDim);

        /* Transport buttons */
        const bool hasNext = ctx.episodeIdx + 1 < (int)ctx.episodes.size();
        const bool hasPrev = !ctx.episodes.empty() && ctx.episodeIdx > 0;
        const bool enabled[BTN_COUNT] = { hasPrev, true, true, true, hasNext };
        for (int i = 0; i < BTN_COUNT; ++i) {
            float x = BTN_X0 + i * (BTN_W + BTN_GAP);
            float f = (hoverButton == i && enabled[i]) ? Ui::pulse() : 0.0f;
            Ui::button(x, BTN_Y, BTN_W, BTN_H, "", 15, f);
            u32 c = enabled[i] ? Ui::mix(p.text, p.accentDark, f) : Ui::alpha(p.textDim, 0.45f);
            float cx = x + BTN_W * 0.5f, cy = BTN_Y + BTN_H * 0.5f;
            switch (i) {
            case 0:   /* previous episode |<< */
                Ui::roundRect(cx - 13, cy - 8, 3, 16, 1.5f, c);
                Ui::triangle(cx - 10, cy, cx, cy - 8, cx, cy + 8, c);
                Ui::triangle(cx, cy, cx + 10, cy - 8, cx + 10, cy + 8, c);
                break;
            case 4:   /* next episode >>| */
                Ui::triangle(cx - 10, cy - 8, cx, cy, cx - 10, cy + 8, c);
                Ui::triangle(cx, cy - 8, cx + 10, cy, cx, cy + 8, c);
                Ui::roundRect(cx + 10, cy - 8, 3, 16, 1.5f, c);
                break;
            case 2:
                if (g_mplayer_paused) {
                    Ui::triangle(cx - 6, cy - 10, cx + 10, cy, cx - 6, cy + 10, c);
                } else {
                    Ui::roundRect(cx - 8, cy - 9, 6, 18, 2, c);
                    Ui::roundRect(cx + 2, cy - 9, 6, 18, 2, c);
                }
                break;
            default:
                Ui::textCentered(cx, cy - 9, i == 1 ? "-10s" : "+10s", 16, c);
                break;
            }
        }
        {
            /* zoom: a small screen showing the current mode (picture with
             * bars, or picture filling it) */
            float x = zoomX();
            float f = hoverButton == BTN_ZOOM ? Ui::pulse() : 0.0f;
            Ui::button(x, BTN_Y, ZOOM_W, BTN_H, "", 15, f);
            u32 c = Ui::mix(p.text, p.accentDark, f);
            float cx = x + ZOOM_W * 0.5f, cy = BTN_Y + BTN_H * 0.5f;
            Ui::roundBorder(cx - 13, cy - 9, 26, 18, 3, 2.0f, c);
            if (VideoSurface::zoom() == VideoSurface::Zoom::Fill)
                Ui::roundRect(cx - 9.5f, cy - 5.5f, 19, 11, 1.5f, c);
            else
                Ui::roundRect(cx - 9.5f, cy - 3, 19, 6, 1, c);
        }
        Ui::popOffset();
    }

    /* ---- Skip intro ---- */
    if (introVisible()) {
        Ui::button(INTRO_X, INTRO_Y, INTRO_W, INTRO_H, "Skip intro", 16,
                   hoverIntro ? Ui::pulse() : 0.35f);
    }

    /* ---- Track picker ---- */
    if (panel != Panel::None) {
        int n = panelCount();
        int rows = n - panelTop < LIST_ROWS ? n - panelTop : LIST_ROWS;
        int h = 44 + rows * ROW_H + 12;
        Ui::card(LIST_X, LIST_Y, LIST_W, h, 16, 0.0f);
        Ui::text(LIST_X + 18, LIST_Y + 12, panel == Panel::Audio ? "Audio track" : "Subtitles",
                 18, p.accentDark);
        Ui::roundRect(LIST_X + 14, LIST_Y + 38, LIST_W - 28, 1.5f, 0.75f, Ui::alpha(p.cardBorder, 0.6f));
        for (int i = 0; i < rows; ++i) {
            int row = panelTop + i;
            int y = LIST_Y + 44 + i * ROW_H;
            bool sel = row == panelSel;
            if (sel)
                Ui::roundRect(LIST_X + 8, y, LIST_W - 16, ROW_H - 4, (ROW_H - 4) * 0.5f,
                              Ui::mix(p.accent, 0xFFFFFFFF, 0.2f), p.accentDark);
            bool current = panelRowIsCurrent(row);
            int  maxW = LIST_W - 48 - (current ? 70 : 0);
            Ui::text(LIST_X + 22, y + 7, fitText(panelLabel(row), 16, maxW).c_str(), 16,
                     sel ? p.textOnAccent : p.text);
            if (current)
                Ui::textRight(LIST_X + LIST_W - 22, y + 9, "\xe2\x9c\x93 Current", 13,
                              sel ? p.textOnAccent : p.ok);
        }
    }

    /* ---- Toast ---- */
    if (!toastMsg.empty() && nowMs() < toastUntil)
        messagePill((k > 0.0f) ? TOP_H + 31 : 41, toastMsg, 16);

    /* ---- Pointer ---- */
    if (ir.valid && cursorTex && (k > 0.5f || panel != Panel::None || introVisible()))
        GRRLIB_DrawImg((int)ir.x - 20, (int)ir.y - 4, cursorTex, ir.angle, 1.0f, 1.0f, 0xFFFFFFFF);
}
