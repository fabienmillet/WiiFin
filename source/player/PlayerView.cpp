#include "PlayerView.h"
#include "../core/Text.h"
#include "../core/Lang.h"
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
static const int LIST_X     = 120, LIST_W  = 400;
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
    char buf[48];
    snprintf(buf, sizeof(buf), tr("Track %d"), n);
    return buf;
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
    dragging = false;
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
    float t    = wii_player_time_pos();
    float skip = wii_player_ss_missed() ? 0.0f : ctx.startSkip;
    return ctx.streamOrigin + (t > skip ? t : skip);
}

float PlayerView::duration() const
{
    if (ctx.runtime > 0.0f) return ctx.runtime;
    if (g_mplayer_duration > 0.0f) return ctx.streamOrigin + g_mplayer_duration;
    return 0.0f;
}

int g_wiifin_skip_prompts = SKIP_PROMPTS_ALL;

int PlayerView::activeSegment() const
{
    if (!busyMsg.empty() || g_wiifin_skip_prompts == SKIP_PROMPTS_OFF) return -1;
    float pos = position();
    const auto& segs = ctx.intro.segments;
    for (int i = 0; i < (int)segs.size() && i < 32; ++i) {
        if (skippedSegments & (1u << i)) continue;
        if (g_wiifin_skip_prompts == SKIP_PROMPTS_RECAPS &&
            (segs[i].kind == MediaSegment::Intro || segs[i].kind == MediaSegment::Outro)) continue;
        /* not in its last second: skipping would land about there anyway */
        if (pos >= segs[i].start && pos < segs[i].end - 1.0f) return i;
    }
    return -1;
}

/* Credits with an episode after them: straight to it, as Netflix does */
const char* PlayerView::segmentLabel(int i) const
{
    switch (ctx.intro.segments[i].kind) {
    case MediaSegment::Recap:      return N_("Skip recap");
    case MediaSegment::Preview:    return N_("Skip preview");
    case MediaSegment::Commercial: return N_("Skip ad");
    case MediaSegment::Outro:
        return ctx.episodeIdx + 1 < (int)ctx.episodes.size() ? N_("Next episode") : N_("Skip credits");
    default:                       return N_("Skip intro");
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

/* |< and >| with chapters: the next one's start, or this one's (the one
 * before when this one has just started); false past the last or before
 * the first: then they go to the next or previous episode */
bool PlayerView::chapterJump(int dir)
{
    if (ctx.chapters.size() < 2) return false;
    const float pos = seekPending ? seekPendingTo : position();
    float to = -1.0f;
    if (dir > 0) {
        for (float c : ctx.chapters) if (c > pos + 1.0f) { to = c; break; }
    } else {
        int cur = -1;
        for (int i = 0; i < (int)ctx.chapters.size(); ++i) if (ctx.chapters[i] <= pos + 0.5f) cur = i;
        if (cur < 0) return false;
        if (pos - ctx.chapters[cur] > 3.0f) to = ctx.chapters[cur];
        else if (cur > 0)                   to = ctx.chapters[cur - 1];
        else if (pos > 1.0f)                to = 0.0f;
    }
    if (to < 0.0f) return false;
    SYS_Report("[Player] chapter %s: to %.1f s\n", dir > 0 ? "next" : "back", (double)to);
    seekPending  = false;
    seekInFlight = true;
    seekTo       = to;
    showControls();
    return true;
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
    seekInPlaceFrom  = wii_player_time_pos();
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
    panelTop = panelSel >= listRows() ? panelSel - listRows() + 1 : 0;
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
    if (row == 0) return tr("Off");
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

/* ---- Subtitle delay (text subtitles, drawn by WiiFin) ---- */
static const int DELAY_H = 74, NOTE_H = 34;
static const int DBTN_W  = 34, DBTN_H = 28, DVAL_W = 64;
/* Reset: as wide as its word needs */
static int resetW() { int w = Ui::textWidth(tr("Reset"), 14) + 24; return w < 64 ? 64 : w; }
/* the buttons for it: -/+ then 1 / 2, on two lines when one is too narrow */
static const Ui::Hint DELAY_HINTS[] = { { "-/+", N_("0.1 s") }, { "1", N_("Previous line") },
                                        { "2", N_("Next line") } };
static bool delayHintsSplit()
{
    float w = 0;
    for (const Ui::Hint& h : DELAY_HINTS) w += Ui::hintWidth(h);
    return w > LIST_W - 24;
}

bool PlayerView::delayShown() const { return panel == Panel::Sub && subtitles && !subtitles->empty(); }
bool PlayerView::delayNote() const  { return panel == Panel::Sub && !delayShown() && ctx.currentSub >= 0; }
/* the rows shown at once: fewer with the delay under them (on screen still) */
int  PlayerView::listRows() const { return LIST_ROWS - (delayShown() ? 2 : delayNote() ? 1 : 0); }
int  PlayerView::panelRowsShown() const
{
    int n = panelCount() - panelTop;
    return n < listRows() ? n : listRows();
}
int  PlayerView::delayFooterH() const
{
    return delayShown() ? DELAY_H + (delayHintsSplit() ? 22 : 0) : delayNote() ? NOTE_H : 0;
}

void PlayerView::stepDelay(float d)
{
    if (!subtitles) return;
    subtitles->setDelay(subtitles->delay() + d);
    SYS_Report("[Subtitles] delay %+.1f s\n", (double)subtitles->delay());
}

void PlayerView::lineDelay(int dir)
{
    float d;
    if (!subtitles) return;
    if (!subtitles->lineDelay(position(), dir, d)) {
        toast(dir > 0 ? tr("No next line") : tr("No previous line"), 1500);
        return;
    }
    /* rounded down to 0.1 s: the line shows at once rather than just after */
    subtitles->setDelay(floorf(d * 10.0f + 0.001f) / 10.0f);
    SYS_Report("[Subtitles] %s line: delay %+.1f s\n", dir > 0 ? "next" : "previous",
               (double)subtitles->delay());
}

/* where the picker's parts are: its rows, then the delay below them */
static int delayY(int rows) { return LIST_Y + 44 + rows * ROW_H + 4; }
static int delayPlusX()  { return LIST_X + LIST_W - 22 - resetW() - 12 - DBTN_W; }
static int delayMinusX() { return delayPlusX() - DVAL_W - DBTN_W; }
static int delayResetX() { return LIST_X + LIST_W - 22 - resetW(); }

void PlayerView::hitTest(const ir_t& ir)
{
    hoverButton = -1;
    hoverBar    = false;
    hoverRow    = -1;
    hoverDelay  = -1;
    hoverIntro  = introVisible() && inRect(ir, INTRO_X, INTRO_Y, INTRO_W, INTRO_H);

    if (panel != Panel::None) {
        int rows = panelRowsShown();
        for (int i = 0; i < rows; ++i)
            if (inRect(ir, LIST_X, LIST_Y + 44 + i * ROW_H, LIST_W, ROW_H))
                hoverRow = panelTop + i;
        if (delayShown()) {
            int y = delayY(rows) + 8;
            if      (inRect(ir, delayMinusX(), y, DBTN_W, DBTN_H)) hoverDelay = DELAY_MINUS;
            else if (inRect(ir, delayPlusX(),  y, DBTN_W, DBTN_H)) hoverDelay = DELAY_PLUS;
            else if (inRect(ir, delayResetX(), y, resetW(), DBTN_H)) hoverDelay = DELAY_RESET;
            else if (inRect(ir, LIST_X, y - 4, LIST_W, DBTN_H + 8)) hoverDelay = DELAY_ROW;
        }
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
    toast(fill ? tr("Zoom: fill the screen") : tr("Zoom: whole picture"), 1500);
}

/* ---- Input ----------------------------------------------------------- */

PlayerView::Action PlayerView::update(u32 down, const ir_t& ir)
{
    /* a seek in this stream is over once MPlayer shows a picture from
     * elsewhere: until then it may still show one or two from here */
    if (seekInPlaceSince && !g_wiifin_loading_active &&
        (fabsf(wii_player_time_pos() - seekInPlaceFrom) > 1.0f || nowMs() - seekInPlaceAt > 4000)) {
        seekInPlaceSince = false;
        seekInFlight     = false;
    }
    {
        float t = wii_player_time_pos();
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

    if (dragging) {   /* the handle in hand: on the bar wherever the pointer goes */
        if (ir.valid) {
            dragFrac = (ir.x - BAR_X) / (float)BAR_W;
            if (dragFrac < 0.0f) dragFrac = 0.0f;
            if (dragFrac > 1.0f) dragFrac = 1.0f;
        }
        showControls();
        if (Input::held() & WPAD_BUTTON_A) return Action::None;
        dragging = false;
        if (duration() <= 0.0f) return Action::None;
        seekInFlight = true;
        seekTo = dragFrac * duration();
        return Action::SeekTo;
    }

    /* the video's first picture: the controls, up since the loading
     * started, go a second after it (not 5 s after the start) */
    if (!firstPicture && !g_wiifin_loading_active && wii_player_time_pos() > 0.0f) {
        firstPicture = true;
        if (controlsUntil > nowMs() + 1000) controlsUntil = nowMs() + 1000;
    }

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
        const bool delay = delayShown();
        /* the delay row: one past the tracks */
        const int hovered = hoverDelay >= 0 ? n : hoverRow;
        if (hovered >= 0 && hovered != prevHoverRow) panelSel = hovered;
        prevHoverRow = hovered;
        if (!delay && panelSel >= n) panelSel = n - 1;
        if (down & WPAD_BUTTON_B) { panel = Panel::None; showControls(); return Action::None; }
        if ((down & WPAD_BUTTON_UP)   && panelSel > 0)     --panelSel;
        if ((down & WPAD_BUTTON_DOWN) && panelSel + 1 < n + (delay ? 1 : 0)) ++panelSel;
        if (panelSel < n) {
            if (panelSel < panelTop) panelTop = panelSel;
            if (panelSel >= panelTop + listRows()) panelTop = panelSel - listRows() + 1;
        }
        if (delay) {
            /* -/+ (and Left/Right on the delay row): 0.1 s, on stepping when
             * held; 1/2: the previous / next line at once */
            const bool onRow = panelSel == n;
            const u32 stepBtns = WPAD_BUTTON_MINUS | WPAD_BUTTON_PLUS |
                                 (onRow ? WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT : 0);
            const u32 hd = Input::held() & stepBtns;
            const u64 now = nowMs();
            if (down & stepBtns) {
                stepDelay(down & (WPAD_BUTTON_PLUS | WPAD_BUTTON_RIGHT) ? 0.1f : -0.1f);
                delayHoldSince = delayHoldLast = now;
            } else if (!hd) {
                delayHoldSince = 0;
            } else if (delayHoldSince && now - delayHoldSince > 400 && now - delayHoldLast >= 100) {
                delayHoldLast = now;
                stepDelay(hd & (WPAD_BUTTON_PLUS | WPAD_BUTTON_RIGHT) ? 0.1f : -0.1f);
            }
            if (down & WPAD_BUTTON_1) lineDelay(-1);
            if (down & WPAD_BUTTON_2) lineDelay(+1);
            if ((down & WPAD_BUTTON_A) && (onRow || hoverDelay >= 0)) {
                if      (hoverDelay == DELAY_MINUS) stepDelay(-0.1f);
                else if (hoverDelay == DELAY_PLUS)  stepDelay(+0.1f);
                else if (subtitles->delay() != 0.0f) {
                    subtitles->setDelay(0.0f);
                    SYS_Report("[Subtitles] delay reset\n");
                }
                return Action::None;
            }
        }
        if ((down & WPAD_BUTTON_A) && panelSel < n) return activatePanelRow(panelSel);
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
            /* the handle taken: it jumps when A is let go (a click too) */
            dragFrac = (ir.x - BAR_X) / (float)BAR_W;
            if (dragFrac < 0.0f) dragFrac = 0.0f;
            if (dragFrac > 1.0f) dragFrac = 1.0f;
            dragging = true;
        } else if (visible && hoverButton >= 0) {
            switch (hoverButton) {
            case 0:
                if (chapterJump(-1)) { act = Action::SeekTo; break; }
                if (hasPrev) act = Action::Prev; else toast(tr("No previous episode"));
                break;
            case 1: nudgeSeek(-10.0f); break;
            case 2: wii_player_pause_toggle(); break;
            case 3: nudgeSeek(+10.0f); break;
            case 4:
                if (chapterJump(+1)) { act = Action::SeekTo; break; }
                if (hasNext) act = Action::Next; else toast(tr("No next episode"));
                break;
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
    /* Left / Right held: on seeking, 10 s steps, 30 s after 3 s (the seek
     * is made once let go, as the presses add up) */
    {
        const u32 hd = Input::held() & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT);
        const u64 now = nowMs();
        if (!hd)                               holdSince = 0;
        else if (down & hd)                    holdSince = holdLast = now;
        else if (holdSince && now - holdSince > 500 && now - holdLast >= 200) {
            holdLast = now;
            const float step = now - holdSince > 3000 ? 30.0f : 10.0f;
            nudgeSeek(hd & WPAD_BUTTON_RIGHT ? step : -step);
        }
    }
    if (down & WPAD_BUTTON_UP)    { wii_player_vol_up();   toast(tr("Volume +"), 1000); }
    if (down & WPAD_BUTTON_DOWN)  { wii_player_vol_down(); toast(tr("Volume -"), 1000); }
    if (down & WPAD_BUTTON_PLUS) {
        if (hasNext) act = Action::Next; else toast(tr("No next episode"));
    }
    if (down & WPAD_BUTTON_MINUS) {
        if (hasPrev) act = Action::Prev; else toast(tr("No previous episode"));
    }
    if (down & Input::BTN_ZOOM) toggleZoom();   /* Classic ZL/ZR, GameCube Z */
    if (down & WPAD_BUTTON_1) {
        if (ctx.audioStreams.size() > 1) openPanel(Panel::Audio);
        else toast(tr("No other audio track"));
    }
    if (down & WPAD_BUTTON_2) {
        if (!ctx.subStreams.empty()) openPanel(Panel::Sub);
        else toast(tr("No subtitles available"));
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
        /* a whole UTF-8 character: its continuation bytes, then its first */
        while (!t.empty() && ((unsigned char)t.back() & 0xC0) == 0x80) t.pop_back();
        if (!t.empty()) t.pop_back();
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
        char msg[128];
        if (!busyMsg.empty())       snprintf(msg, sizeof(msg), "%s", tr(busyMsg.c_str()));
        else if (rebufPercent >= 0) snprintf(msg, sizeof(msg), tr("Buffering... %d%%"), rebufPercent);
        else if (stalled)           snprintf(msg, sizeof(msg), "%s", tr("Buffering..."));
        else                        snprintf(msg, sizeof(msg), "%s", tr("Loading..."));
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
            const Ui::Hint all[] = { { bs == Ui::ButtonStyle::GameCube ? "Z" : "ZR", N_("Zoom") },
                                     { "1", N_("Audio") }, { "2", N_("Subtitles") }, { "B", N_("Back") } };
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
        float frac = dragging ? dragFrac : dur > 0.0f ? pos / dur : 0.0f;
        if (frac > 1.0f) frac = 1.0f;
        if (frac < 0.0f) frac = 0.0f;
        float barH = hoverBar || dragging ? BAR_H + 4 : BAR_H;
        float barY = BAR_Y - (barH - BAR_H) / 2;
        Ui::roundRect(BAR_X, barY, BAR_W, barH, barH / 2, Ui::alpha(p.cardBorder, 0.6f));
        if (frac > 0.0f)
            Ui::roundRect(BAR_X, barY, BAR_W * frac < barH ? barH : BAR_W * frac, barH, barH / 2,
                          Ui::mix(p.accent, 0xFFFFFFFF, 0.2f), p.accent);
        {
            float kx = BAR_X + BAR_W * frac, ky = BAR_Y + BAR_H / 2.0f, kr = hoverBar || dragging ? 9.0f : 7.0f;
            Ui::shadow(kx - kr, ky - kr + 1, kr * 2, kr * 2, kr, 4.0f, p.shadow);
            Ui::circle(kx, ky, kr, p.cardTop);
            Ui::roundBorder(kx - kr, ky - kr, kr * 2, kr * 2, kr, 2.0f, p.accent);
        }
        /* Where a seek would land: under the pointer on the bar, or the
         * target of the -10s / +10s presses still to be played.  Its time
         * in a bubble, with the trickplay thumbnail above when there is one. */
        float previewAt = -1.0f, px = 0.0f;
        if (dragging && dur > 0.0f) {
            previewAt = dragFrac * dur;
            px = BAR_X + BAR_W * dragFrac;
        } else if (hoverBar && dur > 0.0f) {
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
        /* |< and >| go from chapter to chapter first */
        const bool chapters = ctx.chapters.size() >= 2;
        const bool enabled[BTN_COUNT] = { hasPrev || chapters, true, true, true, hasNext || chapters };
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
        int rows = panelRowsShown();
        int h = 44 + rows * ROW_H + 12 + delayFooterH();
        Ui::card(LIST_X, LIST_Y, LIST_W, h, 16, 0.0f);
        Ui::text(LIST_X + 18, LIST_Y + 12, panel == Panel::Audio ? tr("Audio track") : tr("Subtitles"),
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
                Ui::textRight(LIST_X + LIST_W - 22, y + 9, tr("\xe2\x9c\x93 Current"), 13,
                              sel ? p.textOnAccent : p.ok);
        }
        if (delayShown() || delayNote()) {
            int y = delayY(rows);
            Ui::roundRect(LIST_X + 14, y, LIST_W - 28, 1.5f, 0.75f, Ui::alpha(p.cardBorder, 0.6f));
            if (delayNote()) {
                Ui::textCentered(320, y + 9, tr("Picture subtitles: no delay to set"), 13, p.textDim);
            } else {
                const bool sel = panelSel == n;
                const float d = subtitles->delay();
                int by = y + 8;
                if (sel)
                    Ui::roundRect(LIST_X + 8, by - 3, LIST_W - 16, DBTN_H + 6, (DBTN_H + 6) * 0.5f,
                                  Ui::alpha(p.accent, 0.25f));
                {
                    /* the label in what is left of the row, smaller if need be */
                    const int room = delayMinusX() - 8 - (LIST_X + 22);
                    int size = 16;
                    while (size > 12 && Ui::textWidth(tr("Delay"), size) > room) --size;
                    Ui::text(LIST_X + 22, by + 5 + (16 - size) / 2, fitText(tr("Delay"), size, room).c_str(),
                             size, p.text);
                }
                Ui::button(delayMinusX(), by, DBTN_W, DBTN_H, "", 15,
                           hoverDelay == DELAY_MINUS ? Ui::pulse() : 0.0f);
                Ui::roundRect(delayMinusX() + DBTN_W * 0.5f - 5, by + DBTN_H * 0.5f - 1.25f, 10, 2.5f, 1.25f, p.text);
                char val[16];
                snprintf(val, sizeof(val), d == 0.0f ? "0.0 s" : "%+.1f s", (double)d);
                Ui::textCentered(delayMinusX() + DBTN_W + DVAL_W * 0.5f, by + 6, val, 16,
                                 d == 0.0f ? p.textDim : p.accentDark);
                Ui::button(delayPlusX(), by, DBTN_W, DBTN_H, "", 15,
                           hoverDelay == DELAY_PLUS ? Ui::pulse() : 0.0f);
                {
                    float cx = delayPlusX() + DBTN_W * 0.5f, cy = by + DBTN_H * 0.5f;
                    Ui::roundRect(cx - 5, cy - 1.25f, 10, 2.5f, 1.25f, p.text);
                    Ui::roundRect(cx - 1.25f, cy - 5, 2.5f, 10, 1.25f, p.text);
                }
                Ui::button(delayResetX(), by, resetW(), DBTN_H, tr("Reset"), 14,
                           hoverDelay == DELAY_RESET || (sel && hoverDelay < 0) ? Ui::pulse() : 0.0f);
                /* the buttons for it, centred: one line, or -/+ then 1 / 2 */
                const bool split = delayHintsSplit();
                for (int line = 0; line < (split ? 2 : 1); ++line) {
                    int from = split && line ? 1 : 0, to = split && !line ? 1 : 3;
                    float w = 0;
                    for (int i = from; i < to; ++i) w += Ui::hintWidth(DELAY_HINTS[i]);
                    float hx = 320 - w * 0.5f;
                    for (int i = from; i < to; ++i) hx += Ui::hint(hx, by + DBTN_H + 10 + line * 22, DELAY_HINTS[i]);
                }
            }
        }
    }

    /* ---- Toast ---- */
    if (!toastMsg.empty() && nowMs() < toastUntil)
        messagePill((k > 0.0f) ? TOP_H + 31 : 41, toastMsg, 16);

    /* ---- Pointer ---- */
    if (ir.valid && cursorTex && (k > 0.5f || panel != Panel::None || introVisible()))
        GRRLIB_DrawImg((int)ir.x - 20, (int)ir.y - 4, cursorTex, ir.angle, 1.0f, 1.0f, 0xFFFFFFFF);
}
