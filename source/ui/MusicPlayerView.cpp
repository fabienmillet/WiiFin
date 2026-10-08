/*
 * MusicPlayerView.cpp — the music session and its "Now Playing" screen
 * (see MusicPlayerView.h).
 *
 * Threads:
 *   main        run(): per track, fetches the stream URL, the details and
 *               the cover, then blocks in wii_player_play_audio()
 *   background  WiiPlayer's thread while a track plays: tick() (buttons)
 *               and render() (~60 Hz, GRRLIB_Render waits for the vsync)
 *   jobs        reports to Jellyfin, favourite changes and the autoplay
 *               fetch, so the screen never waits on the network
 * The queue and the screen state belong to the background thread while a
 * track plays and to the main thread between tracks.
 *
 * Controls (any controller, as Wii Remote buttons):
 *   A          the focused / pointed control (play/pause on the seek bar)
 *   Left/Right move along the controls, or seek -10/+10 s on the seek bar
 *   Up/Down    seek bar / controls
 *   - / +      previous / next track
 *   1          Up Next panel          2   shuffle
 *   B / HOME   back to the library
 */

#include "MusicPlayerView.h"
#include "../jellyfin/RemoteControl.h"
#include "../core/ExitZone.h"
#include "../core/MusicBGM.h"
#include "../input/Input.h"
#include "../player/AudioSpectrum.h"
#include "../player/WiiPlayer.h"
#include "JpegTexture.h"
#include "LibraryDraw.h"
#include "Ui.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <ogcsys.h>
#include <ogc/lwp.h>
#include <ogc/lwp_watchdog.h>
#include <ogc/video.h>
#include <wiiuse/wpad.h>

extern volatile bool g_app_powerOff;
extern volatile bool g_app_reset;
extern unsigned char data_ring_png[];

namespace {

const int  BARS       = 32;       /* spectrum bands                         */
const u64  IDLE_MS    = 20000;    /* controls fade away after this          */
const int  PANEL_ROWS = 8;
const int  AUTOPLAY   = 40;       /* tracks fetched when the queue runs out */

u64 nowMs() { return ticks_to_millisecs(gettime()); }

/* ---- Session ------------------------------------------------------------ */

enum class Req { None, Next, Prev, Jump, Ended, Leave };

enum Ctl { C_FAV, C_SHUFFLE, C_PREV, C_PLAY, C_NEXT, C_REPEAT, C_QUEUE, C_COUNT };

struct Session {
    JellyfinClient* client = nullptr;
    std::string     serverUrl;
    JellyfinAuth    auth;
    GRRLIB_texImg*  cursor = nullptr;

    std::vector<MusicTrack> tracks;
    std::vector<int>        order;     /* play order: indexes into tracks */
    int  pos     = 0;                  /* current place in order          */
    bool shuffle = false;
    int  repeat  = 0;                  /* 0 off, 1 all, 2 one             */

    /* the playing track */
    MusicTrack     cur;
    std::string    title, artist, detail;
    AudioTrackInfo info;
    bool           favorite = false;
    GRRLIB_texImg* art      = nullptr;
    u32            artA = 0, artB = 0;          /* placeholder gradient */

    /* position: MPlayer gives none for audio streams, so wall clock */
    u64   startTick = 0;
    float seekBase  = 0.0f;
    bool  wasPaused = false;
    u64   pausedAt  = 0;
    float pausedSecs = 0.0f;
    bool  timing    = false;

    /* background thread -> main thread */
    volatile Req req    = Req::None;
    volatile int jumpTo = -1;
    bool  autoplayAsked = false;

    /* screen */
    volatile bool renderOn = false;    /* MPlayer is past its start-up     */
    bool  pointer  = false;            /* the Wii Remote points at the TV  */
    int   focus    = C_PLAY;
    bool  focusBar = false;
    int   hover    = -1;
    bool  hoverBar = false;
    float hoverFrac = 0.0f;
    int   hoverRow = -1;
    int   hoverArrow = 0;              /* Up Next arrows: -1 up, +1 down  */
    u64   arrowSince = 0, arrowLast = 0;
    bool  panel    = false;
    float panelK   = 0.0f;
    int   panelSel = 0, panelTop = 0;
    float idleK    = 0.0f;
    u64   lastInput = 0;
    float lastIrX = -1.0f, lastIrY = -1.0f;
    float viz[BARS] = {}, peak[BARS] = {};
    u64   peakAt[BARS] = {};
    float bassAvg = 0.0f, beat = 0.0f;
    u64   titleSince = 0;
    std::string toastMsg;
    u64   toastUntil = 0;
    u64   lastFrame  = 0;
};
Session* S = nullptr;

/* ---- Jobs thread: Jellyfin requests ---------------------------------------
 * One per track, joined before the next: it reads the track's ids. */
volatile bool s_streamOpened = false;   /* set from MPlayer's thread */
volatile bool s_jobStart     = false;
volatile bool s_startDone    = false;
volatile bool s_jobProgress  = false;
volatile long long s_progressTicks = 0;
volatile bool s_progressPaused = false;
volatile int  s_jobFavorite  = 0;       /* 1 mark, 2 unmark */
volatile bool s_jobAutoplay  = false;
volatile bool s_autoplayReady = false;
std::vector<JellyfinAudioItem> s_autoplay;
/* "Play next" / "Add to queue" from the server: fetched by the jobs thread,
 * then put in the queue by the player (appendQueued) */
volatile bool s_jobQueue   = false;
volatile bool s_queueReady = false;
Remote::PlayRequest            s_queueReq;
std::vector<JellyfinAudioItem> s_queued;
volatile bool s_jobsStop     = false;
u64           s_lastProgress = 0;
std::string   s_sessionId;
std::string   s_playMethod = "Transcode";   /* as Jellyfin serves this track */
int           s_audioIndex = 0;
lwp_t         s_jobsThread   = LWP_THREAD_NULL;
u8            s_jobsStack[32 * 1024] DEAD_AT_EXIT __attribute__((aligned(32)));

void* jobsMain(void*)
{
    Session& s = *S;
    while (!s_jobsStop) {
        if (s_jobStart) {
            s.client->reportPlaybackStart(s.serverUrl, s.auth, s.cur.id, s.cur.id, s_sessionId,
                                          s_playMethod.c_str(), s_audioIndex);
            s_jobStart = false;
            s_startDone = true;
        }
        if (s_jobProgress) {
            long long t = s_progressTicks;
            bool p = s_progressPaused;
            s_jobProgress = false;
            s.client->reportPlaybackProgress(s.serverUrl, s.auth, s.cur.id, s.cur.id,
                                             s_sessionId, t, p, s_playMethod.c_str(), s_audioIndex);
        }
        if (s_jobFavorite) {
            bool on = s_jobFavorite == 1;
            s_jobFavorite = 0;
            s.client->setFavorite(s.serverUrl, s.auth, s.cur.id, on);
        }
        if (s_jobAutoplay && !s_autoplayReady) {
            s.client->getAutoplayTracks(s.serverUrl, s.auth, s.cur.id, s.info.parentId,
                                        AUTOPLAY, s_autoplay);
            s_jobAutoplay = false;
            s_autoplayReady = true;
        }
        if (s_jobQueue && !s_queueReady) {
            std::vector<JellyfinItem> items;
            std::vector<JellyfinAudioItem> audio;
            s_queued.clear();
            if (s.client->getItemsByIds(s.serverUrl, s.auth, s_queueReq.ids, items, audio))
                for (size_t i = 0; i < items.size(); ++i)
                    if (items[i].type == "Audio") s_queued.push_back(audio[i]);
            s_jobQueue = false;
            s_queueReady = true;
        }
        usleep(16000);
    }
    return nullptr;
}

void onStreamOpened() { s_streamOpened = true; }

/* ---- Queue ---------------------------------------------------------------- */

void appendAutoplay()
{
    Session& s = *S;
    for (const JellyfinAudioItem& a : s_autoplay) {
        MusicTrack t;
        t.id = a.id; t.title = a.name; t.artist = a.artist; t.album = a.album;
        t.runtimeTicks = a.runtimeTicks;
        s.order.push_back((int)s.tracks.size());
        s.tracks.push_back(t);
    }
    s_autoplay.clear();
    s_autoplayReady = false;
}

/* the server's tracks: after this one (Play next, in their order) or at the
 * end (Add to queue) */
void appendQueued()
{
    Session& s = *S;
    int at = s.pos + 1;
    for (const JellyfinAudioItem& a : s_queued) {
        MusicTrack t;
        t.id = a.id; t.title = a.name; t.artist = a.artist; t.album = a.album;
        t.runtimeTicks = a.runtimeTicks;
        int idx = (int)s.tracks.size();
        s.tracks.push_back(t);
        if (s_queueReq.next) s.order.insert(s.order.begin() + at++, idx);
        else                 s.order.push_back(idx);
    }
    SYS_Report("[Music] %u track(s) %s\n", (unsigned)s_queued.size(),
               s_queueReq.next ? "to play next" : "added to the queue");
    if (!s_queued.empty()) {
        S->toastMsg = s_queueReq.next ? "Playing next" : "Added to the queue";
        S->toastUntil = nowMs() + 1600;
    }
    s.autoplayAsked = false;   /* the queue's end moved */
    s_queued.clear();
    s_queueReady = false;
}

void setShuffle(bool on)
{
    Session& s = *S;
    int current = s.order.empty() ? 0 : s.order[s.pos];
    s.shuffle = on;
    s.order.clear();
    if (on) {
        /* the playing track first, the others in random order */
        s.order.push_back(current);
        for (int i = 0; i < (int)s.tracks.size(); ++i) if (i != current) s.order.push_back(i);
        for (int i = (int)s.order.size() - 1; i > 1; --i) {
            int j = 1 + rand() % i;
            int t = s.order[i]; s.order[i] = s.order[j]; s.order[j] = t;
        }
        s.pos = 0;
    } else {
        for (int i = 0; i < (int)s.tracks.size(); ++i) s.order.push_back(i);
        s.pos = current;
    }
    s.panelSel = s.pos;
}

/* ---- Position ------------------------------------------------------------- */

float duration()
{
    Session& s = *S;
    if (s.cur.runtimeTicks > 0) return (float)(s.cur.runtimeTicks / 10000000LL);
    return g_mplayer_duration > 0.0f ? (float)g_mplayer_duration : 0.0f;
}

float position()
{
    Session& s = *S;
    /* counting starts with MPlayer's main loop, not its seconds of start-up */
    if (!s.timing) {
        if (g_mplayer_duration <= 0.0f) return s.seekBase;
        s.startTick = gettime();
        s.timing = true;
    }
    u64 now = gettime();
    float paused = s.pausedSecs;
    if (s.wasPaused && s.pausedAt >= s.startTick)
        paused += ticks_to_millisecs(now - s.pausedAt) / 1000.0f;
    float p = s.seekBase + ticks_to_millisecs(now - s.startTick) / 1000.0f - paused;
    float d = duration();
    if (p < 0.0f) p = 0.0f;
    if (d > 0.0f && p > d) p = d;
    return p;
}

void seekTo(float t)
{
    Session& s = *S;
    float d = duration();
    if (t < 0.0f) t = 0.0f;
    if (d > 0.0f && t > d - 1.0f) t = d - 1.0f;
    wii_player_seek_abs(t);
    s.startTick = gettime();
    s.seekBase = t;
    s.pausedSecs = 0.0f;
    s.timing = true;
    if (s.wasPaused) s.pausedAt = s.startTick;
}

/* ---- Colours -------------------------------------------------------------- */

u32 hsv(float h, float sat, float v)
{
    h = fmodf(h, 360.0f) / 60.0f;
    if (h < 0) h += 6.0f;
    int i = (int)h;
    float f = h - i, p = v * (1 - sat), q = v * (1 - sat * f), t = v * (1 - sat * (1 - f));
    float r, g, b;
    switch (i % 6) {
    case 0: r = v; g = t; b = p; break;
    case 1: r = q; g = v; b = p; break;
    case 2: r = p; g = v; b = t; break;
    case 3: r = p; g = q; b = v; break;
    case 4: r = t; g = p; b = v; break;
    default: r = v; g = p; b = q; break;
    }
    return ((u32)(r * 255) << 24) | ((u32)(g * 255) << 16) | ((u32)(b * 255) << 8) | 0xFF;
}

u32 hashOf(const std::string& s)
{
    u32 h = 2166136261u;
    for (unsigned char c : s) h = (h ^ c) * 16777619u;
    return h;
}

/* ---- Text ------------------------------------------------------------------ */

std::string fmtTime(float secs)
{
    if (secs < 0) secs = 0;
    int t = (int)secs;
    char b[16];
    if (t >= 3600) snprintf(b, sizeof(b), "%d:%02d:%02d", t / 3600, t / 60 % 60, t % 60);
    else           snprintf(b, sizeof(b), "%d:%02d", t / 60, t % 60);
    return b;
}

/* Title and artist of a track; untagged files are named "Artist - Title". */
void splitName(const MusicTrack& t, const std::string& artists, std::string& title, std::string& artist)
{
    title  = LibDraw::filterDejaVu(t.title, 200);
    artist = LibDraw::filterDejaVu(!artists.empty() ? artists : t.artist, 120);
    size_t p = title.find(" - ");
    if (artist.empty() && p != std::string::npos && p > 0 && p + 3 < title.size()) {
        artist = title.substr(0, p);
        title  = title.substr(p + 3);
    }
}

/* One line in w pixels; a longer one scrolls back and forth slowly. */
void marquee(const std::string& s, float x, float y, float w, int size, u32 color, u64 since)
{
    int tw = Ui::textWidth(s.c_str(), size);
    if (tw <= w) { Ui::text(x, y, s.c_str(), size, color); return; }
    const float SPEED = 28.0f, PAUSE = 2.0f;
    float travel = tw - w, run = travel / SPEED, cycle = 2 * (PAUSE + run);
    float t = fmodf((nowMs() - since) / 1000.0f, cycle), off;
    if      (t < PAUSE)               off = 0;
    else if (t < PAUSE + run)         off = (t - PAUSE) * SPEED;
    else if (t < 2 * PAUSE + run)     off = travel;
    else                              off = travel - (t - 2 * PAUSE - run) * SPEED;
    Ui::clip(x, y - 4, w, size + 12);
    Ui::text(x - off, y, s.c_str(), size, color);
    Ui::clipReset();
}

/* ---- Glyphs ---------------------------------------------------------------- */

void line(float x0, float y0, float x1, float y1, float w, u32 c)
{
    float dx = x1 - x0, dy = y1 - y0, l = sqrtf(dx * dx + dy * dy);
    if (l < 0.01f) return;
    float nx = -dy / l * w * 0.5f, ny = dx / l * w * 0.5f;
    Ui::triangle(x0 + nx, y0 + ny, x1 + nx, y1 + ny, x1 - nx, y1 - ny, c);
    Ui::triangle(x0 + nx, y0 + ny, x1 - nx, y1 - ny, x0 - nx, y0 - ny, c);
    Ui::circle(x0, y0, w * 0.5f, c);
    Ui::circle(x1, y1, w * 0.5f, c);
}

void glyphHeart(float cx, float cy, float k, u32 c)
{
    Ui::circle(cx - 4.2f * k, cy - 2.5f * k, 4.6f * k, c);
    Ui::circle(cx + 4.2f * k, cy - 2.5f * k, 4.6f * k, c);
    Ui::triangle(cx - 8.6f * k, cy - 1.0f * k, cx + 8.6f * k, cy - 1.0f * k, cx, cy + 8.0f * k, c);
}

void glyphShuffle(float cx, float cy, u32 c)
{
    line(cx - 9, cy - 5, cx + 4, cy + 5, 2.2f, c);
    line(cx - 9, cy + 5, cx + 4, cy - 5, 2.2f, c);
    Ui::triangle(cx + 3, cy - 9, cx + 3, cy - 1, cx + 10, cy - 5, c);
    Ui::triangle(cx + 3, cy + 1, cx + 3, cy + 9, cx + 10, cy + 5, c);
}

void glyphRepeat(float cx, float cy, bool one, u32 c)
{
    Ui::roundBorder(cx - 9, cy - 6, 18, 12, 5, 2.2f, c);
    Ui::triangle(cx + 1, cy - 10.5f, cx + 1, cy - 1.5f, cx + 7, cy - 6, c);
    if (one) Ui::textCentered(cx, cy - 6, "1", 10, c);
}

void glyphQueue(float cx, float cy, u32 c)
{
    Ui::roundRect(cx - 10, cy - 7, 13, 2.6f, 1.3f, c);
    Ui::roundRect(cx - 10, cy - 1.3f, 13, 2.6f, 1.3f, c);
    Ui::roundRect(cx - 10, cy + 4.4f, 9, 2.6f, 1.3f, c);
    Ui::triangle(cx + 5, cy + 0.5f, cx + 5, cy + 8.5f, cx + 11, cy + 4.5f, c);
}

void glyphPrevNext(float cx, float cy, bool next, u32 c)
{
    float s = next ? 1.0f : -1.0f;
    Ui::triangle(cx - 8 * s, cy - 9, cx + 5 * s, cy, cx - 8 * s, cy + 9, c);
    Ui::roundRect(next ? cx + 6 : cx - 10, cy - 9, 4, 18, 1.5f, c);
}

void glyphPlayPause(float cx, float cy, bool playing, u32 c)
{
    if (playing) {
        Ui::roundRect(cx - 8, cy - 10, 6, 20, 2, c);
        Ui::roundRect(cx + 2, cy - 10, 6, 20, 2, c);
    } else {
        Ui::triangle(cx - 6, cy - 11, cx + 11, cy, cx - 6, cy + 11, c);
    }
}

/* ---- Layout ---------------------------------------------------------------- */

const float ART_X = 48, ART_Y = 66, ART = 216;
const float COL_X = 290, COL_W = 302;
const float VIZ_BASE = 262, VIZ_H = 104;
const float BAR_X = 48, BAR_W = 544, BAR_Y = 304;
const float CTL_Y = 372;
const float CTL_X[C_COUNT] = { 70, 184, 248, 320, 392, 456, 570 };   /* 18 px or more between two */
const float CTL_R[C_COUNT] = { 20, 20, 24, 30, 24, 20, 20 };
const float PANEL_W = 306, PANEL_Y = 18, PANEL_H = 420, ROW_H = 38, ROWS_Y = 86;
/* the Up Next panel hugs the right edge of the screen, 4:3 or 16:9 */
float panelX() { return Ui::screenRight() - PANEL_W - 16; }
/* its scroll arrows, for the pointer, under the rows */
const float ARROW_Y = ROWS_Y + PANEL_ROWS * ROW_H + 14, ARROW_DX = 30;

/* ---- Drawing ---------------------------------------------------------------- */

void drawCover(float x, float y, float size, float a)
{
    Session& s = *S;
    const Ui::Palette& p = Ui::pal();
    u32 tint = 0xFFFFFF00 | (u32)(255 * a);
    Ui::shadow(x + 2, y + 6, size - 4, size - 4, 16, 14.0f, Ui::alpha(0x000000FF, 0.45f * a));
    if (s.art) {
        Ui::texCover(s.art, x, y, size, size, 16, 1.0f, tint);
    } else {
        Ui::roundRect(x, y, size, size, 16, Ui::alpha(s.artA, a), Ui::alpha(s.artB, a));
        Ui::textCentered(x + size / 2, y + size / 2 - size * 0.24f, "\xe2\x99\xab", (int)(size * 0.36f),
                         Ui::alpha(0xFFFFFFFF, 0.55f * a));
    }
    Ui::roundBorder(x, y, size, size, 16, 1.5f, Ui::alpha(p.cardBorder, 0.6f * a));
}

void drawSpectrum(float x, float base, float w, float h, float a, bool reflect)
{
    Session& s = *S;
    const Ui::Palette& p = Ui::pal();
    const float gap = 3, bw = (w - gap * (BARS - 1)) / BARS;
    u32 top = Ui::mix(p.accent, 0xFFFFFFFF, 0.25f), bottom = Ui::mix(p.accent, 0x000000FF, 0.35f);
    for (int i = 0; i < BARS; ++i) {
        float v = s.viz[i], bh = 4 + v * h, bx = x + i * (bw + gap);
        Ui::roundRect(bx, base - bh, bw, bh, bw * 0.4f, Ui::alpha(Ui::mix(top, 0xFFFFFFFF, 0.3f * v), a),
                      Ui::alpha(bottom, a * 0.85f));
        float py = base - 4 - s.peak[i] * h - 5;
        Ui::roundRect(bx, py, bw, 3, 1.5f, Ui::alpha(top, a * 0.9f));
        if (reflect)
            Ui::roundRect(bx, base + 3, bw, bh * 0.32f, bw * 0.4f, Ui::alpha(bottom, a * 0.22f),
                          Ui::alpha(bottom, 0.0f));
    }
}

/* Round control: filled when focused, accent when on (shuffle, repeat). */
void drawControl(int i, bool focused, bool playing)
{
    Session& s = *S;
    const Ui::Palette& p = Ui::pal();
    float cx = CTL_X[i], cy = CTL_Y, r = CTL_R[i];
    bool primary = i == C_PLAY;
    bool on = (i == C_SHUFFLE && s.shuffle) || (i == C_REPEAT && s.repeat) || (i == C_QUEUE && s.panel);
    if (focused) Ui::roundBorder(cx - r - 4, cy - r - 4, 2 * r + 8, 2 * r + 8, r + 4, 2.0f, p.accent);
    if (primary || focused) {
        Ui::circle(cx, cy, r, focused ? Ui::mix(p.accent, 0xFFFFFFFF, 0.15f) : p.accent);
    } else {
        Ui::circle(cx, cy, r, Ui::alpha(p.cardTop, 0.55f));
        Ui::roundBorder(cx - r, cy - r, 2 * r, 2 * r, r, 1.2f, Ui::alpha(p.cardBorder, 0.7f));
    }
    u32 c = (primary || focused) ? p.textOnAccent : (on ? p.accent : p.text);
    switch (i) {
    case C_FAV:     glyphHeart(cx, cy + 1, 1.0f, s.favorite ? 0xE8475EFF : Ui::alpha(c, 0.8f)); break;
    case C_SHUFFLE: glyphShuffle(cx, cy, c); break;
    case C_PREV:    glyphPrevNext(cx, cy, false, c); break;
    case C_PLAY:    glyphPlayPause(cx, cy, playing, c); break;
    case C_NEXT:    glyphPrevNext(cx, cy, true, c); break;
    case C_REPEAT:  glyphRepeat(cx, cy, s.repeat == 2, c); break;
    case C_QUEUE:   glyphQueue(cx, cy, c); break;
    }
    if (on && !focused) Ui::circle(cx, cy + r + 6, 2.2f, p.accent);
}

void drawSeekBar(float pos, float dur, bool focused)
{
    Session& s = *S;
    const Ui::Palette& p = Ui::pal();
    float frac = dur > 0 ? pos / dur : 0;
    if (frac > 1) frac = 1;
    float h = (s.hoverBar || focused) ? 8 : 6, y = BAR_Y - h / 2;
    Ui::roundRect(BAR_X, y, BAR_W, h, h / 2, Ui::alpha(p.cardBorder, 0.55f));
    if (frac > 0)
        Ui::roundRect(BAR_X, y, fmaxf(BAR_W * frac, h), h, h / 2, Ui::mix(p.accent, 0xFFFFFFFF, 0.25f), p.accent);
    float kx = BAR_X + BAR_W * frac, kr = (s.hoverBar || focused) ? 9 : 7;
    if (focused) Ui::roundBorder(kx - kr - 4, BAR_Y - kr - 4, 2 * kr + 8, 2 * kr + 8, kr + 4, 2.0f, p.accent);
    Ui::shadow(kx - kr, BAR_Y - kr + 1, 2 * kr, 2 * kr, kr, 4.0f, p.shadow);
    Ui::circle(kx, BAR_Y, kr, 0xFFFFFFFF);
    Ui::circle(kx, BAR_Y, kr - 3, p.accent);
    Ui::text(BAR_X, BAR_Y + 12, fmtTime(pos).c_str(), 14, p.text);
    std::string rest = dur > 0 ? "-" + fmtTime(dur - pos) : "--:--";
    Ui::textRight(BAR_X + BAR_W, BAR_Y + 12, rest.c_str(), 14, p.textDim);
    if (s.hoverBar && dur > 0) {
        float hx = BAR_X + BAR_W * s.hoverFrac;
        std::string t = fmtTime(s.hoverFrac * dur);
        int w = Ui::textWidth(t.c_str(), 14) + 20;
        Ui::roundRect(hx - w / 2, BAR_Y - 38, w, 22, 11, p.accent, p.accentDark);
        Ui::triangle(hx - 5, BAR_Y - 16, hx + 5, BAR_Y - 16, hx, BAR_Y - 11, p.accentDark);
        Ui::textCentered(hx, BAR_Y - 35, t.c_str(), 14, p.textOnAccent);
    }
}

void drawPanel()
{
    Session& s = *S;
    const Ui::Palette& p = Ui::pal();
    float e = 1 - (1 - s.panelK) * (1 - s.panelK);
    Ui::pushOffset((1 - e) * (Ui::screenRight() - panelX() + 20), 0);
    Ui::shadow(panelX(), PANEL_Y + 4, PANEL_W, PANEL_H, 18, 14.0f, Ui::alpha(0x000000FF, 0.5f));
    Ui::roundRect(panelX(), PANEL_Y, PANEL_W, PANEL_H, 18, p.cardTop, p.cardBottom);
    Ui::roundBorder(panelX(), PANEL_Y, PANEL_W, PANEL_H, 18, 1.5f, Ui::alpha(p.cardBorder, 0.8f));
    Ui::text(panelX() + 20, PANEL_Y + 14, "Up Next", 20, p.text);
    char mode[64];
    snprintf(mode, sizeof(mode), "Shuffle %s  \xc2\xb7  Repeat %s", s.shuffle ? "on" : "off",
             s.repeat == 0 ? "off" : (s.repeat == 1 ? "all" : "one"));
    Ui::text(panelX() + 20, PANEL_Y + 42, mode, 12, p.textDim);
    char cnt[24];
    snprintf(cnt, sizeof(cnt), "%d / %d", s.pos + 1, (int)s.order.size());
    Ui::textRight(panelX() + PANEL_W - 20, PANEL_Y + 20, cnt, 13, p.textDim);
    Ui::roundRect(panelX() + 14, ROWS_Y - 8, PANEL_W - 28, 1.5f, 0.75f, Ui::alpha(p.cardBorder, 0.6f));

    int n = (int)s.order.size();
    for (int r = 0; r < PANEL_ROWS && s.panelTop + r < n; ++r) {
        int k = s.panelTop + r;
        const MusicTrack& t = s.tracks[s.order[k]];
        float y = ROWS_Y + r * ROW_H;
        bool sel = k == s.panelSel, now = k == s.pos;
        if (sel)
            Ui::roundRect(panelX() + 8, y, PANEL_W - 16, ROW_H - 4, (ROW_H - 4) / 2,
                          Ui::mix(p.accent, 0xFFFFFFFF, 0.2f), p.accentDark);
        u32 c = sel ? p.textOnAccent : (now ? p.accent : p.text);
        u32 dim = sel ? Ui::alpha(p.textOnAccent, 0.8f) : p.textDim;
        float tx = panelX() + 24;
        std::string title, artist;
        splitName(t, "", title, artist);
        float ty = artist.empty() ? y + 9 : y + 2;   /* one line: centred in the row */
        if (now) {   /* three little bars dancing with the music */
            for (int b = 0; b < 3; ++b) {
                float bh = 4 + s.viz[3 + b * 7] * 12;
                Ui::roundRect(tx + b * 4.5f, ty + 16 - bh, 3, bh, 1.5f, c);
            }
        } else {
            char num[8];
            snprintf(num, sizeof(num), "%d", k - s.pos);
            Ui::textCentered(tx + 6, ty + 2, num, 12, dim);
        }
        int maxW = (int)(PANEL_W - 64);
        Ui::text(tx + 22, ty, LibDraw::fitText(Ui::font(), title, 15, maxW).c_str(), 15, c);
        if (!artist.empty())
            Ui::text(tx + 22, y + 19, LibDraw::fitText(Ui::font(), artist, 11, maxW).c_str(), 11, dim);
    }
    Ui::scrollbar(panelX() + PANEL_W - 10, ROWS_Y, PANEL_ROWS * ROW_H - 6, s.panelTop, PANEL_ROWS, n);
    float acx = panelX() + PANEL_W / 2;
    Ui::arrowButton(acx - ARROW_DX, ARROW_Y, true,  s.panelTop > 0, s.hoverArrow < 0);
    Ui::arrowButton(acx + ARROW_DX, ARROW_Y, false, s.panelTop + PANEL_ROWS < n, s.hoverArrow > 0);
    Ui::popOffset();
}

/* The whole screen.  live: drawn by the background thread while a track
 * plays (pointer, hover); else a still frame between tracks. */
void drawScreen(bool live, bool loading)
{
    Session& s = *S;
    const Ui::Palette& p = Ui::pal();
    float pos = live ? position() : s.seekBase, dur = duration();
    bool playing = live && !g_mplayer_paused;
    float idle = s.idleK, idleE = idle * idle * (3 - 2 * idle);

    Ui::background(false);

    /* ---- normal layout (fades out when idle) ---- */
    if (idleE < 0.99f) {
        float a = 1 - idleE;
        float pulse = 1 + fminf(s.beat, 0.3f) * 0.10f, sz = ART * pulse, off = (sz - ART) / 2;
        drawCover(ART_X - off, ART_Y - off, sz, a);

        Ui::text(48, 22, "NOW PLAYING", 13, Ui::alpha(p.textDim, a));
        if (s.order.size() > 1) {
            char b[40];
            snprintf(b, sizeof(b), "%d of %d%s", s.pos + 1, (int)s.order.size(), s.shuffle ? "  \xc2\xb7  Shuffle" : "");
            Ui::textRight(592, 22, b, 13, Ui::alpha(p.textDim, a));
        }
        marquee(s.title, COL_X, ART_Y - 2, COL_W, 26, Ui::alpha(p.text, a), s.titleSince);
        if (!s.artist.empty())
            marquee(s.artist, COL_X, ART_Y + 34, COL_W, 18, Ui::alpha(p.accent, a), s.titleSince);
        if (!s.detail.empty())
            Ui::text(COL_X, ART_Y + 60, LibDraw::fitText(Ui::font(), s.detail, 14, (int)COL_W).c_str(), 14,
                     Ui::alpha(p.textDim, a));
        if (s.favorite) glyphHeart(COL_X + COL_W - 8, ART_Y + 70, 0.75f, Ui::alpha(0xE8475EFF, a));
        drawSpectrum(COL_X, VIZ_BASE, COL_W, VIZ_H, a, true);

        if (idleE < 0.5f) {
            drawSeekBar(pos, dur, live && !s.pointer && s.focusBar && !s.panel);
            for (int i = 0; i < C_COUNT; ++i) {
                bool f = live && !s.panel && (s.pointer ? s.hover == i : (!s.focusBar && s.focus == i));
                drawControl(i, f, live ? playing : true);
            }
            if (loading)
                Ui::textCentered(320, BAR_Y + 12, "Loading...", 14, p.textDim);
            /* what comes next */
            int nextPos = s.repeat == 2 ? s.pos : (s.pos + 1 < (int)s.order.size() ? s.pos + 1
                                                   : (s.repeat == 1 ? 0 : -1));
            if (nextPos >= 0 && !s.panel) {
                std::string t, ar;
                splitName(s.tracks[s.order[nextPos]], "", t, ar);
                std::string line = "Next:  " + t + (ar.empty() ? "" : "  \xe2\x80\x94  " + ar);
                Ui::textCentered(320, 414, LibDraw::fitText(Ui::font(), line, 13, 400).c_str(), 13,
                                 Ui::alpha(p.textDim, 0.9f));
            }
            if (s.panel) {
                const Ui::Hint l[] = { { "A", "Play" }, { "2", "Shuffle" } };
                const Ui::Hint r[] = { { "B", "Close" } };
                Ui::footer(l, 2, r, 1);
            } else {
                const Ui::Hint l[] = { { "A", "Select" }, { "-/+", "Track" } };
                const Ui::Hint r[] = { { "1", "Up Next" }, { "B", "Back" } };
                Ui::footer(l, 2, r, 2);
            }
        }
    }

    /* ---- idle layout: the cover alone, centred ---- */
    if (idleE > 0.01f) {
        float a = idleE;
        float sz = 236 * (1 + fminf(s.beat, 0.3f) * 0.10f);
        drawCover(320 - sz / 2, 172 - sz / 2 - 40 + 30 * (1 - a), sz, a);
        Ui::textCentered(320, 318, LibDraw::fitText(Ui::font(), s.title, 24, 560).c_str(), 24, Ui::alpha(p.text, a));
        if (!s.artist.empty())
            Ui::textCentered(320, 350, LibDraw::fitText(Ui::font(), s.artist, 17, 560).c_str(), 17,
                             Ui::alpha(p.accent, a));
        drawSpectrum(Ui::screenLeft() + 20, 470, Ui::screenWidth() - 40, 60, a * 0.6f, false);
        float frac = dur > 0 ? pos / dur : 0;
        Ui::roundRect(220, 384, 200, 3, 1.5f, Ui::alpha(p.cardBorder, 0.5f * a));
        Ui::roundRect(220, 384, 200 * fminf(frac, 1.0f), 3, 1.5f, Ui::alpha(p.accent, a));
    }

    if (s.panelK > 0.01f) drawPanel();

    if (!s.toastMsg.empty() && nowMs() < s.toastUntil) {
        int w = Ui::textWidth(s.toastMsg.c_str(), 15) + 28;
        Ui::shadow(320 - w / 2, 40, w, 30, 15, 8.0f, p.shadow);
        Ui::roundRect(320 - w / 2, 40, w, 30, 15, p.accent, p.accentDark);
        Ui::textCentered(320, 46, s.toastMsg.c_str(), 15, p.textOnAccent);
    }
}

void toast(const char* msg)
{
    S->toastMsg = msg;
    S->toastUntil = nowMs() + 1600;
}

/* ---- Background thread: buttons ----------------------------------------- */

void requestTrack(Req r, int jump = -1)
{
    Session& s = *S;
    if (s.req != Req::None && r != Req::Leave) return;   /* leaving wins */
    s.jumpTo = jump;
    s.req = r;
    wii_player_stop();
}

/* Up Next: move the rows by d, the selection kept in sight */
void scrollPanel(int d)
{
    Session& s = *S;
    int n = (int)s.order.size(), maxTop = n > PANEL_ROWS ? n - PANEL_ROWS : 0;
    s.panelTop += d;
    if (s.panelTop < 0) s.panelTop = 0;
    if (s.panelTop > maxTop) s.panelTop = maxTop;
    if (s.panelSel < s.panelTop) s.panelSel = s.panelTop;
    if (s.panelSel > s.panelTop + PANEL_ROWS - 1) s.panelSel = s.panelTop + PANEL_ROWS - 1;
}

void activate(int c)
{
    Session& s = *S;
    switch (c) {
    case C_FAV:
        s.favorite = !s.favorite;
        s_jobFavorite = s.favorite ? 1 : 2;
        toast(s.favorite ? "Added to favourites" : "Removed from favourites");
        break;
    case C_SHUFFLE:
        setShuffle(!s.shuffle);
        toast(s.shuffle ? "Shuffle on" : "Shuffle off");
        break;
    case C_PREV:   requestTrack(Req::Prev); break;
    case C_PLAY:   wii_player_pause_toggle(); break;
    case C_NEXT:   requestTrack(Req::Next); break;
    case C_REPEAT:
        s.repeat = (s.repeat + 1) % 3;
        toast(s.repeat == 0 ? "Repeat off" : (s.repeat == 1 ? "Repeat all" : "Repeat this track"));
        break;
    case C_QUEUE:
        s.panel = !s.panel;
        s.panelSel = s.pos + 1 < (int)s.order.size() ? s.pos + 1 : s.pos;
        s.panelTop = s.pos;
        break;
    }
}

void tick(int paused, u32 down, u32 held)
{
    (void)held;
    if (!S) return;
    Session& s = *S;
    u64 now = nowMs();

    if (g_app_powerOff || g_app_reset) requestTrack(Req::Leave);

    /* reports, on the jobs thread */
    if (s_streamOpened) { s_streamOpened = false; s.renderOn = true; s_jobStart = true; }
    if (s_startDone && !s_jobProgress && now - s_lastProgress >= 10000) {
        s_lastProgress   = now;
        s_progressTicks  = (long long)(position() * 10000000.0f);
        s_progressPaused = paused != 0;
        s_jobProgress    = true;
    }

    /* time spent paused does not count */
    if (paused && !s.wasPaused) { s.pausedAt = gettime(); s.wasPaused = true; }
    else if (!paused && s.wasPaused) {
        s.pausedSecs += ticks_to_millisecs(gettime() - s.pausedAt) / 1000.0f;
        s.wasPaused = false;
    }

    /* end of the track: MPlayer may wait a long time for more of a stream
     * that is over, so stop it at the known length */
    float dur = duration();
    if (!paused && s.req == Req::None && dur > 0 && g_mplayer_time_pos > 2.0f &&
        g_mplayer_time_pos >= dur - 1.0f)
        requestTrack(Req::Ended);

    /* last track of the queue: fetch what follows while it plays */
    if (!s.autoplayAsked && s.repeat == 0 && s.pos == (int)s.order.size() - 1 && position() > 5) {
        s.autoplayAsked = true;
        s_jobAutoplay = true;
    }
    if (s_autoplayReady) appendAutoplay();
    if (s_queueReady) appendQueued();

    /* the server's remote control: as the buttons; something else to play
     * ("Play on..."): leave, the library starts it; tracks for the queue:
     * fetched by the jobs thread */
    if (Remote::hasPlay()) requestTrack(Req::Leave);
    if (!s_jobQueue && !s_queueReady && Remote::takeQueue(s_queueReq)) s_jobQueue = true;
    for (Remote::Command rc; Remote::poll(rc); ) {
        switch (rc.cmd) {
        case Remote::Cmd::Pause:       if (!paused) wii_player_pause_toggle(); break;
        case Remote::Cmd::Unpause:     if (paused)  wii_player_pause_toggle(); break;
        case Remote::Cmd::PlayPause:   wii_player_pause_toggle(); break;
        case Remote::Cmd::Stop:        requestTrack(Req::Leave); break;
        case Remote::Cmd::Next:        requestTrack(Req::Next); break;
        case Remote::Cmd::Prev:        requestTrack(Req::Prev); break;
        case Remote::Cmd::Seek:        seekTo((float)(rc.ticks / 10000000.0)); break;
        case Remote::Cmd::FastForward: seekTo(position() + 30.0f); break;
        case Remote::Cmd::Rewind:      seekTo(position() - 10.0f); break;
        case Remote::Cmd::VolumeUp:    wii_player_vol_up();   toast("Volume +"); break;
        case Remote::Cmd::VolumeDown:  wii_player_vol_down(); toast("Volume -"); break;
        case Remote::Cmd::SetVolume:   wii_player_set_volume(rc.value); toast("Volume"); break;
        case Remote::Cmd::Mute:        wii_player_set_mute(1); toast("Sound off"); break;
        case Remote::Cmd::Unmute:      wii_player_set_mute(0); toast("Sound on"); break;
        case Remote::Cmd::ToggleMute:
            wii_player_set_mute(!wii_player_muted());
            toast(wii_player_muted() ? "Sound off" : "Sound on");
            break;
        default: break;
        }
        s.lastInput = now;   /* the screen wakes up, as for a press */
    }

    if (!down) return;
    s.lastInput = now;
    if (s.idleK > 0.3f) return;          /* the first press wakes the screen */

    if (s.panel) {
        int n = (int)s.order.size();
        if (down & (WPAD_BUTTON_B | WPAD_BUTTON_1 | WPAD_BUTTON_HOME)) { s.panel = false; return; }
        if ((down & WPAD_BUTTON_UP) && s.panelSel > 0)       s.panelSel--;
        if ((down & WPAD_BUTTON_DOWN) && s.panelSel < n - 1) s.panelSel++;
        if (s.panelSel < s.panelTop) s.panelTop = s.panelSel;
        if (s.panelSel >= s.panelTop + PANEL_ROWS) s.panelTop = s.panelSel - PANEL_ROWS + 1;
        if ((down & WPAD_BUTTON_A) && s.pointer && s.hoverArrow) {
            scrollPanel(s.hoverArrow * (PANEL_ROWS - 1));
            s.arrowLast = nowMs();
        } else if (down & WPAD_BUTTON_A) {
            int k = s.pointer ? s.hoverRow : s.panelSel;
            if (k >= 0 && k != s.pos) requestTrack(Req::Jump, k);
            else if (s.pointer && k < 0) s.panel = false;   /* a click beside the panel */
        }
        if (down & WPAD_BUTTON_2) activate(C_SHUFFLE);
        if (down & WPAD_BUTTON_PLUS)  requestTrack(Req::Next);
        if (down & WPAD_BUTTON_MINUS) requestTrack(Req::Prev);
        return;
    }

    if (down & (WPAD_BUTTON_LEFT | WPAD_BUTTON_RIGHT)) {
        int d = (down & WPAD_BUTTON_RIGHT) ? 1 : -1;
        if (s.focusBar) seekTo(position() + 10.0f * d);
        else            s.focus = (s.focus + d + C_COUNT) % C_COUNT;
    }
    if (down & WPAD_BUTTON_UP)   s.focusBar = true;
    if (down & WPAD_BUTTON_DOWN) s.focusBar = false;
    if (down & WPAD_BUTTON_A) {
        if (s.pointer) {
            if (s.hoverBar)       seekTo(s.hoverFrac * duration());
            else if (s.hover >= 0) activate(s.hover);
            else                  wii_player_pause_toggle();
        } else if (s.focusBar) {
            wii_player_pause_toggle();
        } else {
            activate(s.focus);
        }
    }
    if (down & WPAD_BUTTON_PLUS)  requestTrack(Req::Next);
    if (down & WPAD_BUTTON_MINUS) requestTrack(Req::Prev);
    if (down & WPAD_BUTTON_1)     activate(C_QUEUE);
    if (down & WPAD_BUTTON_2)     activate(C_SHUFFLE);
    if (down & (WPAD_BUTTON_B | WPAD_BUTTON_HOME)) requestTrack(Req::Leave);
}

/* ---- Background thread: one frame --------------------------------------- */

void render()
{
    if (!S) return;
    Session& s = *S;
    /* nothing on GX until MPlayer is past its start-up: the still frame
     * drawn by the main thread stays on screen meanwhile */
    if (!s.renderOn) { VIDEO_WaitVSync(); return; }

    u64 now = nowMs();
    float dt = s.lastFrame ? (now - s.lastFrame) / 1000.0f : 0.016f;
    if (dt > 0.1f) dt = 0.033f;
    s.lastFrame = now;

    /* spectrum: quick rise, slower fall, peaks that hold then drop */
    float bands[BARS], bass = 0;
    bool sound = !g_mplayer_paused && AudioSpectrum::analyse(bands, BARS, &bass);
    for (int i = 0; i < BARS; ++i) {
        float t = sound ? bands[i] : 0.0f;
        if (t > s.viz[i]) s.viz[i] += (t - s.viz[i]) * 0.6f;
        else              s.viz[i] = fmaxf(t, s.viz[i] - dt * 1.4f);
        if (s.viz[i] >= s.peak[i]) { s.peak[i] = s.viz[i]; s.peakAt[i] = now; }
        else if (now - s.peakAt[i] > 400) s.peak[i] = fmaxf(s.viz[i], s.peak[i] - dt * 0.8f);
    }
    /* beat: the bass jumping above its average */
    s.bassAvg += (bass - s.bassAvg) * 0.05f;
    float kick = bass - s.bassAvg - 0.06f;
    s.beat = kick > s.beat ? kick : s.beat * 0.88f;

    /* pointer, hover, idle */
    ir_t ir;
    Input::readIR(ir);
    s.pointer = ir.valid;
    if (ir.valid) {
        float dx = ir.x - s.lastIrX, dy = ir.y - s.lastIrY;
        if (s.lastIrX < 0 || dx * dx + dy * dy > 36) s.lastInput = now;
        s.lastIrX = ir.x; s.lastIrY = ir.y;
    } else {
        s.lastIrX = -1;
    }
    bool idle = !s.panel && now - s.lastInput > IDLE_MS;
    s.idleK = Ui::approach(s.idleK, idle ? 1.0f : 0.0f, idle ? 0.02f : 0.18f);
    s.panelK = Ui::approach(s.panelK, s.panel ? 1.0f : 0.0f, 0.22f);

    s.hover = -1; s.hoverBar = false; s.hoverRow = -1;
    int arrow = 0;
    if (ir.valid && s.idleK < 0.3f) {
        if (s.panel) {
            for (int d = -1; d <= 1; d += 2) {
                float dx = ir.x - (panelX() + PANEL_W / 2 + d * ARROW_DX), dy = ir.y - ARROW_Y;
                if (dx * dx + dy * dy <= 20 * 20) arrow = d;
            }
            if (arrow) {
                s.hoverRow = -2;
            } else if (ir.x >= panelX() && ir.x < panelX() + PANEL_W) {
                int r = (int)((ir.y - ROWS_Y) / ROW_H);
                if (ir.y >= ROWS_Y && r < PANEL_ROWS && s.panelTop + r < (int)s.order.size()) {
                    s.hoverRow = s.panelTop + r;
                    s.panelSel = s.hoverRow;
                } else {
                    s.hoverRow = -2;   /* inside the panel, on no row */
                }
            }
        } else {
            for (int i = 0; i < C_COUNT; ++i) {
                float dx = ir.x - CTL_X[i], dy = ir.y - CTL_Y, r = CTL_R[i] + 4;
                if (dx * dx + dy * dy <= r * r) s.hover = i;
            }
            if (ir.x >= BAR_X - 8 && ir.x <= BAR_X + BAR_W + 8 && fabsf(ir.y - BAR_Y) < 14) {
                s.hoverBar = true;
                s.hoverFrac = fminf(fmaxf((ir.x - BAR_X) / BAR_W, 0.0f), 1.0f);
            }
        }
    }

    /* resting on an Up Next arrow scrolls, one row at a time */
    if (arrow != s.hoverArrow) { s.hoverArrow = arrow; s.arrowSince = now; }
    if (arrow && now - s.arrowSince > 400 && now - s.arrowLast > 130) {
        s.arrowLast = now;
        scrollPanel(arrow);
    }

    drawScreen(true, false);
    if (ir.valid && s.cursor) {
        orient_t o;
        WPAD_Orientation(WPAD_CHAN_0, &o);
        GRRLIB_DrawImg((int)ir.x - 8, (int)ir.y - 4, s.cursor, o.roll, 1, 1, 0xFFFFFFFF);
    }
    GRRLIB_Render();
}

/* ---- Main thread: between tracks ----------------------------------------- */

void stillFrames(bool loading)
{
    for (int i = 0; i < 2; ++i) {   /* both framebuffers */
        drawScreen(false, loading);
        GRRLIB_Render();
    }
}

/* Details and cover of the track at order position pos. */
void loadTrack()
{
    Session& s = *S;
    s.cur = s.tracks[s.order[s.pos]];
    s.info = AudioTrackInfo();
    s.favorite = false;
    splitName(s.cur, "", s.title, s.artist);
    s.detail.clear();
    s.titleSince = nowMs();
    s.seekBase = 0;
    if (s.art) { GRRLIB_FreeTexture(s.art); s.art = nullptr; }
    u32 h = hashOf(s.title);
    float hue = (float)(h % 360);
    s.artA = hsv(hue, 0.55f, 0.62f);
    s.artB = hsv(hue + 40, 0.70f, 0.32f);
    stillFrames(true);

    if (s.client->getAudioTrackInfo(s.serverUrl, s.auth, s.cur.id, s.info)) {
        splitName(s.cur, s.info.artists, s.title, s.artist);
        s.favorite = s.info.isFavorite;
        std::string album = LibDraw::filterDejaVu(!s.info.album.empty() ? s.info.album : s.cur.album, 80);
        s.detail = album;
        if (s.info.year > 0) s.detail += (album.empty() ? "" : "  \xc2\xb7  ") + std::to_string(s.info.year);
    }
    std::string id = s.info.hasImage ? s.cur.id : (s.info.albumHasImage ? s.info.albumId : "");
    std::string bytes;
    if (!id.empty() && s.client->getItemImageBytes(s.serverUrl, s.auth, id, 320, 320, bytes) && !bytes.empty())
        s.art = loadJPEGTexture((const u8*)bytes.data(), (u32)bytes.size());
}

} // namespace

/* ======================================================================== */

MusicPlayerView::MusicPlayerView(GRRLIB_ttfFont* f, JellyfinClient& c,
                                 const JellyfinAuth& a, const std::string& srv)
    : font(f), client(c), auth(a), serverUrl(srv)
{}

void MusicPlayerView::setCursorTex(GRRLIB_texImg* tex)
{
    cursorTex = tex;
}

void MusicPlayerView::setTracks(const std::vector<MusicTrack>& t, int idx)
{
    tracks   = t;
    startIdx = (idx >= 0 && idx < (int)t.size()) ? idx : 0;
}

bool MusicPlayerView::run()
{
    if (tracks.empty()) return false;
    (void)font;
    S = new Session();
    s_jobQueue = false;
    s_queueReady = false;
    s_queued.clear();
    Session& s = *S;
    s.client = &client;
    s.serverUrl = serverUrl;
    s.auth = auth;
    s.cursor = cursorTex;
    s.tracks = tracks;
    for (int i = 0; i < (int)tracks.size(); ++i) s.order.push_back(i);
    s.pos = startIdx;
    s.lastInput = nowMs();
    srand((unsigned)gettime());

    const bool musicWasRunning = MusicBGM::isRunning();
    MusicBGM::pause();
    int failures = 0;
    Remote::setPlayerActive(true);
    struct RemoteIdle { ~RemoteIdle() { Remote::setPlayerActive(false); } } remoteIdle;

    for (;;) {
        loadTrack();
        std::string url;
        s_sessionId.clear();
        if (!client.getAudioStreamUrl(serverUrl, auth, s.cur.id, 0, url, s_sessionId,
                                      &s_playMethod, &s_audioIndex)) {
            SYS_Report("[Music] no stream for this track: %s\n", client.lastError().c_str());
            if (++failures >= 3 || s.pos + 1 >= (int)s.order.size()) break;
            s.pos++;
            continue;
        }
        stillFrames(true);

        /* this track's state */
        s.req = Req::None;
        s.jumpTo = -1;
        s.renderOn = false;
        s.timing = false;
        s.wasPaused = false;
        s.pausedSecs = 0;
        s.lastFrame = 0;
        s.autoplayAsked = false;
        for (int i = 0; i < BARS; ++i) s.viz[i] = s.peak[i] = 0;
        AudioSpectrum::reset();

        s_streamOpened = false; s_jobStart = false; s_startDone = false; s_jobProgress = false;
        s_jobFavorite = 0;
        s_jobAutoplay = false;
        s_lastProgress = nowMs();
        s_jobsStop = false;
        LWP_CreateThread(&s_jobsThread, jobsMain, nullptr, s_jobsStack, sizeof(s_jobsStack), 30);

        g_stream_opened_cb = onStreamOpened;
        wii_player_set_music_tick(tick);
        wii_player_set_audio_render_cb(render);
        g_wiifin_known_duration = duration();
        u64 t0 = nowMs();
        wii_player_play_audio(url.c_str());
        g_wiifin_known_duration = 0.0f;
        g_stream_opened_cb = nullptr;

        s_jobsStop = true;
        LWP_JoinThread(s_jobsThread, nullptr);
        s_jobsThread = LWP_THREAD_NULL;

        /* what the jobs thread had left to do */
        if (!s_startDone) client.reportPlaybackStart(serverUrl, auth, s.cur.id, s.cur.id, s_sessionId,
                                                     s_playMethod.c_str(), s_audioIndex);
        if (s_jobFavorite) client.setFavorite(serverUrl, auth, s.cur.id, s_jobFavorite == 1);
        client.reportPlaybackStopped(serverUrl, auth, s.cur.id, s.cur.id, s_sessionId,
                                     (long long)(position() * 10000000.0f));
        if (!s_sessionId.empty()) client.deleteActiveEncoding(serverUrl, auth, s_sessionId);
        if (s_autoplayReady) appendAutoplay();
        if (s_queueReady) appendQueued();

        /* a track that fails at once, again and again: give up */
        failures = nowMs() - t0 < 2000 && s.req == Req::None ? failures + 1 : 0;
        if (failures >= 3 || g_app_powerOff || g_app_reset) break;

        Req r = s.req;
        if (r == Req::Leave) break;
        int n = (int)s.order.size(), next;
        if (r == Req::Jump) {
            next = s.jumpTo;
        } else if (r == Req::Prev) {
            /* a few seconds in, "previous" starts the track again */
            next = position() > 3.0f ? s.pos : (s.pos > 0 ? s.pos - 1 : (s.repeat == 1 ? n - 1 : 0));
        } else if (r != Req::Next && s.repeat == 2) {
            next = s.pos;                       /* repeat this track */
        } else if (s.pos + 1 < n) {
            next = s.pos + 1;
        } else if (s.repeat == 1) {
            next = 0;
        } else {
            /* the queue ran out: similar tracks follow */
            stillFrames(true);
            if (s_autoplay.empty())
                client.getAutoplayTracks(serverUrl, auth, s.cur.id, s.info.parentId, AUTOPLAY, s_autoplay);
            appendAutoplay();
            if (s.pos + 1 >= (int)s.order.size()) break;
            next = s.pos + 1;
        }
        s.pos = next;
        if (s.panelSel < s.pos) s.panelSel = s.pos;
    }

    wii_player_set_music_tick(nullptr);
    wii_player_set_audio_render_cb(nullptr);

    /* back to the library through the usual spinner */
    GRRLIB_texImg* ring = GRRLIB_LoadTexture(data_ring_png);
    for (int i = 0; i < 2; ++i) {
        Ui::background(false);
        Ui::spinner(ring, 320, 240);
        GRRLIB_Render();
    }
    GRRLIB_FreeTexture(ring);

    if (s.art) GRRLIB_FreeTexture(s.art);
    delete S;
    S = nullptr;
    /* the menu music only when it is on; the sound effects either way */
    if (musicWasRunning) MusicBGM::resume();
    else                 MusicBGM::reinitAudio();
    return false;
}
