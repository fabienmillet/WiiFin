#pragma once

#include <grrlib.h>
#include <wiiuse/wpad.h>
#include <string>
#include <vector>
#include "../jellyfin/JellyfinClient.h"

/* -----------------------------------------------------------------------
 * PlayerViewContext — what is playing; refreshed by App for each stream.
 * ----------------------------------------------------------------------- */
struct PlayerViewContext {
    std::string title;                      /* shown in the top bar           */
    std::vector<JellyfinEpisode> episodes;  /* season episodes; empty = movie */
    int episodeIdx = 0;

    std::vector<MediaStream> audioStreams;
    std::vector<MediaStream> subStreams;
    int currentAudio = 0;    /* MediaStream::index of the playing audio track */
    int currentSub   = -1;   /* MediaStream::index of the subtitle, -1 = off  */

    IntroInfo intro;

    /* The transcode starts at streamOrigin seconds into the item and MPlayer
     * skips its first startSkip seconds, so the absolute position is
     * streamOrigin + max(g_mplayer_time_pos, startSkip). */
    float streamOrigin = 0.0f;
    float startSkip    = 0.0f;  /* seconds MPlayer discards at start (-ss) */
    float runtime      = 0.0f;  /* item length in seconds, 0 = unknown */
};

/* -----------------------------------------------------------------------
 * PlayerView — the in-player UI, drawn with GRRLIB over the video.
 *
 * Main loop, once per frame:
 *     Input::update(); WPAD_IR(...);
 *     Action a = view.update(Input::rawDown(), ir);
 *     view.render(ir);
 *     GRRLIB_Render();
 *     VideoSurface::endFrame();
 * ----------------------------------------------------------------------- */
class PlayerView {
public:
    enum class Action {
        None,
        Back,     /* B: leave the player                           */
        Next,     /* next episode                                  */
        Prev,     /* previous episode                              */
        Audio,    /* switch to chosenAudio()                       */
        Sub,      /* switch to chosenSub()                         */
        SeekTo,   /* restart the stream at seekTarget() (absolute s) */
        Home,     /* HOME button                                   */
    };

    PlayerView(GRRLIB_ttfFont* font, GRRLIB_texImg* cursor, GRRLIB_texImg* ring);

    void setContext(const PlayerViewContext& c);

    /* Busy message shown with the spinner over the last frame (stream
     * switch, stopping...).  Empty string = normal playback UI. */
    void setBusy(const std::string& msg) { busyMsg = msg; }

    /* Rebuffering progress (0-100) while App holds playback paused to refill
     * the cache, -1 otherwise. */
    void setRebuffering(int percent) { rebufPercent = percent; }

    Action update(u32 btnsDown, const ir_t& ir);
    void   render(const ir_t& ir);

    int   chosenAudio() const { return pickAudio; }
    int   chosenSub()   const { return pickSub; }
    float seekTarget()  const { return seekTo; }

    /* Absolute playback position in seconds. */
    float position() const;

    /* True while playback is stuck (not paused, position not moving). */
    bool  buffering() const;

    void toast(const std::string& msg, int ms = 2000);

private:
    enum class Panel { None, Audio, Sub };

    GRRLIB_ttfFont* font;
    GRRLIB_texImg*  cursorTex;
    GRRLIB_texImg*  ringTex;

    PlayerViewContext ctx;
    std::string busyMsg;
    int         rebufPercent = -1;

    Panel panel        = Panel::None;
    int   panelSel     = 0;
    int   panelTop     = 0;
    u64   controlsUntil = 0;   /* ms timestamp; controls visible until then */
    float controlsAlpha = 0.0f;
    std::string toastMsg;
    u64   toastUntil   = 0;
    float lastIrX = -1.0f, lastIrY = -1.0f;
    bool  introSkipped = false;

    /* Stall detection: the position stops moving while not paused when
     * MPlayer's cache runs dry (slow Wi-Fi or server). */
    float lastSeenPos     = -1.0f;
    u64   lastPosChangeMs = 0;

    int   pickAudio = 0, pickSub = -1;
    float seekTo    = 0.0f;

    /* Left/Right presses accumulate into one seek, committed after a pause */
    bool  seekInFlight  = false;   /* SeekTo returned, new stream not started */
    bool  seekPending   = false;
    float seekPendingTo = 0.0f;
    u64   seekCommitAt  = 0;

    /* Hit-testing results for the current frame */
    int   hoverButton = -1;    /* 0..4 = transport buttons, 5 = zoom */
    bool  hoverBar    = false;
    int   hoverRow    = -1;    /* panel row */
    int   prevHoverRow = -1;   /* hover only moves the selection when it changes */
    bool  hoverIntro  = false;

    float duration() const;
    bool  introVisible() const;
    bool  controlsVisible() const;
    void  nudgeSeek(float delta);
    float displayPosition() const;
    void  showControls(int ms = 5000);
    void  openPanel(Panel p);
    int   panelCount() const;
    std::string panelLabel(int row) const;
    bool  panelRowIsCurrent(int row) const;
    void  hitTest(const ir_t& ir);
    void  toggleZoom();          /* Fit <-> Fill, kept for the next videos */
    Action activatePanelRow(int row);

    void drawSpinner(float cx, float cy);
    std::string fitText(const std::string& s, int size, int maxW);
};
