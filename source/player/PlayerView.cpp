#include "PlayerView.h"
#include "../core/Text.h"
#include "../ui/Ui.h"
#include "WiiPlayer.h"
#include "VideoSurface.h"
#include "Trickplay.h"
#include "Subtitles.h"
#include "../input/Input.h"

#include <ogc/lwp_watchdog.h>
#include <math.h>
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
    skippedSegments = 0;
    seekPending  = false;
    seekInFlight = false;
    seekInPlaceSince = false;
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

int PlayerView::activeSegment() const
{
    if (!busyMsg.empty()) return -1;
    float pos = position();
    const auto& segs = ctx.intro.segments;
    for (int i = 0; i < (int)segs.size() && i < 32; ++i) {
        if (skippedSegments & (1u << i)) continue;
        /* not in its last second: skipping would land about there anyway */
        if (pos >= segs[i].start && pos < segs[i].end - 1.0f) return i;
    }
    return -1;
}

/* Credits with an episode after them: straight to it, as Netflix does */
const char* PlayerView::segmentLabel(int i) const
{
    switch (ctx.intro.segments[i].kind) {
    case MediaSegment::Recap:      return "Skip recap";
    case MediaSegment::Preview:    return "Skip preview";
    case MediaSegment::Commercial: return "Skip ad";
    case MediaSegment::Outro:
        return ctx.episodeIdx + 1 < (int)ctx.episodes.size() ? "Next episode" : "Skip credits";
    default:                       return "Skip intro";
    }
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
    seekCommitAt  = nowMs() + 400;   /* presses closer than this add up */
    showControls();
}

void PlayerView::remoteSeek(float secs)
{
    nudgeSeek(secs - (seekPending ? seekPendingTo : position()));
}

void PlayerView::remoteTrack(bool audio, int index)
{
    if (audio) pickAudio = index;
    else       pickSub   = index;
    injected = audio ? Action::Audio : Action::Sub;
}

float PlayerView::displayPosition() const
{
    if (seekPending)  return seekPendingTo;
    if (seekInFlight) return seekTo;
    return position();
}

void PlayerView::seekInPlace()
{
    seekInPlaceSince = true;
    seekInPlaceFrom  = g_mplayer_time_pos;
    seekInPlaceAt    = nowMs();
}

/* The current cue, lines wrapped to the screen, white with a black outline
 * (readable on any picture), its last line ending at y = bottom. */
void PlayerView::drawSubtitle(float bottom)
{
    const Subtitles::Cue* cue = subtitles->at(position());
    if (!cue) return;
    const int SIZE = 24, LINE_H = 29;
    const float maxW = Ui::screenWidth() - 60;
    std::vector<std::string> lines;
    size_t p = 0;
    while (p <= cue->text.size()) {
        size_t e = cue->text.find('\n', p);
        std::string para = cue->text.substr(p, e == std::string::npos ? std::string::npos : e - p);
        p = e == std::string::npos ? cue->text.size() + 1 : e + 1;
        /* word wrap; a word too long for a line (Japanese and Chinese have
         * no spaces) is cut between its characters */
        std::string cur;
        size_t w = 0;
        while (w < para.size()) {
            size_t sp = para.find(' ', w);
            std::string word = para.substr(w, sp == std::string::npos ? std::string::npos : sp - w);
            w = sp == std::string::npos ? para.size() : sp + 1;
            std::string trial = cur.empty() ? word : cur + " " + word;
            if (Ui::textWidth(trial.c_str(), SIZE) <= maxW) { cur = trial; continue; }
            if (!cur.empty()) { lines.push_back(cur); cur.clear(); }
            while (Ui::textWidth(word.c_str(), SIZE) > maxW) {
                size_t n = Text::fitBytes(Ui::font(), word.c_str(), SIZE, maxW);
                lines.push_back(word.substr(0, n));
                word.erase(0, n);
            }
            cur = word;
        }
        if (!cur.empty()) lines.push_back(cur);
    }
    if (lines.size() > 4) lines.erase(lines.begin(), lines.end() - 4);
    float y = bottom - lines.size() * LINE_H;
    const float cx = (Ui::screenLeft() + Ui::screenRight()) * 0.5f;
    static const float OFF[8][2] = { {-2,0},{2,0},{0,-2},{0,2},{-1.5f,-1.5f},{1.5f,-1.5f},{-1.5f,1.5f},{1.5f,1.5f} };
    for (const std::string& l : lines) {
        for (const auto& o : OFF) Ui::textCentered(cx + o[0], y + o[1], l.c_str(), SIZE, 0x000000E0);
        Ui::textCentered(cx, y, l.c_str(), SIZE, 0xFFFFFFFF);
        y += LINE_H;
    }
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
    /* still on screen while they fade out: still clickable */
    if (!controlsVisible() && controlsAlpha < 0.3f) return;
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
    /* a seek in this stream is over once MPlayer shows a picture from
     * elsewhere: until then it may still show one or two from here */
    if (seekInPlaceSince && !g_wiifin_loading_active &&
        (fabsf(g_mplayer_time_pos - seekInPlaceFrom) > 1.0f || nowMs() - seekInPlaceAt > 4000)) {
        seekInPlaceSince = false;
        seekInFlight     = false;
    }
    {
        float t = g_mplayer_time_pos;
        if (t != lastSeenPos || g_mplayer_paused || g_wiifin_loading_active) {
            lastSeenPos     = t;
            lastPosChangeMs = nowMs();
        }
    }

    /* Pointer movement reveals the controls.  A hand holding the remote
     * shakes the pointer a few pixels all the time: while they are hidden,
     * only a real move brings them back (24 px from where the pointer was
     * half a second ago); once they show, any movement keeps them up. */
    if (ir.valid) {
        const u64 now = nowMs();
        if (lastIrX < 0.0f || now - irAnchorMs > 500) { lastIrX = ir.x; lastIrY = ir.y; irAnchorMs = now; }
        float dx = ir.x - lastIrX, dy = ir.y - lastIrY;
        float need = controlsVisible() ? 4.0f : 24.0f;
        if (dx * dx + dy * dy > need * need) {
            showControls();
            lastIrX = ir.x; lastIrY = ir.y; irAnchorMs = now;
        }
    } else {
        lastIrX = lastIrY = -1.0f;
    }

    hitTest(ir);

    if (down & WPAD_BUTTON_HOME) return Action::Home;
    if (!busyMsg.empty()) return Action::None;   /* switching streams */
    if (injected != Action::None) { Action a = injected; injected = Action::None; return a; }

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

    const bool visible = controlsVisible() || controlsAlpha >= 0.3f;   /* fading out: still there */
    Action act = Action::None;

    if (down & WPAD_BUTTON_A) {
        /* the pointer skips with the button only (a click on the bar meant
         * the bar, even as the controls faded); without one, A anywhere */
        if (hoverIntro || (!ir.valid && introVisible() && hoverButton < 0 && !hoverBar)) {
            int i = activeSegment();
            const MediaSegment& seg = ctx.intro.segments[i];
            skippedSegments |= 1u << i;
            if (seg.kind == MediaSegment::Outro && hasNext) {
                act = Action::Next;
            } else {
                float to = seg.end + 0.3f, dur = duration();
                if (dur > 0.0f && to > dur - 2.0f) to = dur - 2.0f;   /* credits at the very end */
                SYS_Report("[Segments] %s: to %.1f s\n", segmentLabel(i), (double)to);
                seekInFlight = true;
                seekTo = to;
                act = Action::SeekTo;
            }
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

    /* Subtitles: at the bottom, lifted over the control panel when it shows */
    if (subtitles && !subtitles->empty()) {
        const float e = 1 - (1 - k) * (1 - k);
        drawSubtitle(452.0f + (PANEL_Y - 12.0f - 452.0f) * e);
    }

    /* ---- Loading / busy ---- */
    const bool stalled = buffering() || rebufPercent >= 0;
    /* a short load (a seek in the file: a fraction of a second) shows no
     * spinner, it would only flash over the picture */
    if (!g_wiifin_loading_active) loadingSince = 0;
    else if (!loadingSince)       loadingSince = nowMs();
    const bool loadingLong = g_wiifin_loading_active && nowMs() - loadingSince > 250;
    if (!busyMsg.empty() || loadingLong || !hasVideo || stalled) {
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
        /* Where a seek would land: under the pointer on the bar, or the
         * target of the -10s / +10s presses still to be played.  Its time
         * in a bubble, with the trickplay thumbnail above when there is one. */
        float previewAt = -1.0f, px = 0.0f;
        if (hoverBar && dur > 0.0f) {
            float f = (ir.x - BAR_X) / (float)BAR_W;
            if (f < 0.0f) f = 0.0f;
            if (f > 1.0f) f = 1.0f;
            previewAt = f * dur;
            px = ir.x;
        } else if ((seekPending || seekInFlight) && dur > 0.0f) {
            previewAt = pos;
            px = BAR_X + BAR_W * frac;
        }
        /* asked for whenever the controls show, so the tile is there by the
         * time the user seeks */
        GRRLIB_texImg* thumb = trickplay ? trickplay->thumbnail(previewAt >= 0.0f ? previewAt : pos, previewAt >= 0.0f)
                                         : nullptr;
        if (previewAt >= 0.0f) {
            std::string t = fmtTime(previewAt);
            int w = Ui::textWidth(t.c_str(), 14) + 20;
            if (thumb) {
                const float TW = 160.0f, sc = TW / thumb->w, th = thumb->h * sc;
                float tx = px - TW / 2, ty = BAR_Y - 40 - th;
                if (tx < Ui::screenLeft() + 8)        tx = Ui::screenLeft() + 8;
                if (tx > Ui::screenRight() - 8 - TW)  tx = Ui::screenRight() - 8 - TW;
                Ui::shadow(tx - 3, ty - 3, TW + 6, th + 6, 6, 6.0f, p.shadow);
                Ui::roundRect(tx - 3, ty - 3, TW + 6, th + 6, 6, p.accentDark);
                GRRLIB_DrawImg(tx, ty, thumb, 0, sc, sc, 0xFFFFFFFF);
            }
            Ui::roundRect(px - w / 2, BAR_Y - 34, w, 22, 11, p.accent, p.accentDark);
            Ui::triangle(px - 5, BAR_Y - 12, px + 5, BAR_Y - 12, px, BAR_Y - 7, p.accentDark);
            Ui::textCentered(px, BAR_Y - 31, t.c_str(), 14, p.textOnAccent);
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
        Ui::button(INTRO_X, INTRO_Y, INTRO_W, INTRO_H, segmentLabel(activeSegment()), 16,
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
