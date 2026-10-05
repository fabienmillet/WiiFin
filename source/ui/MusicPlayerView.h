#pragma once

#include <grrlib.h>
#include <wiiuse/wpad.h>
#include <stdint.h>
#include <vector>
#include <string>
#include "../jellyfin/JellyfinClient.h"

/* -----------------------------------------------------------------------
 * MusicOverlay — renders a music-player HUD using GRRLIB (~60 Hz from
 * bgThread via wii_player_set_audio_render_cb).  MPlayer runs with
 * -vo null for audio-only playback so GRRLIB remains in control of GX.
 *
 * Thread-safety note:
 *   bgTick() is called from WiiPlayer's background LWP thread.
 *   renderFrameGRRLIB() is also called from the bgThread.
 *   Both access only the `instance` singleton and its POD/volatile members.
 * ----------------------------------------------------------------------- */
class MusicOverlay {
public:
    static const int VIZ_BARS = 24;   /* visualizer bar count */

    struct Track {
        std::string id;
        std::string title;
        std::string artist;
        std::string album;
        long long   runtimeTicks = 0;
    };

    MusicOverlay(const std::vector<Track>& tracks, int currentIdx);

    /* ---- Called from the background LWP thread by WiiPlayer (s_music_tick_cb) */
    static void bgTick(int paused, uint32_t btnsDown, uint32_t btnsHeld);

    /* ---- Called ~60 Hz from bgThread for audio-only GRRLIB rendering.
     * Draws the full music HUD using GRRLIB and calls GRRLIB_Render(). */
    static void renderFrameGRRLIB();

    /* Font used by renderFrameGRRLIB — set by MusicPlayerView::run(). */
    static GRRLIB_ttfFont* renderFont;

    /* Cursor texture for IR pointer — set by MusicPlayerView::run(). */
    static GRRLIB_texImg* renderCursorTex;

    /* Album art texture for the current track — loaded by MusicPlayerView::run(). */
    static GRRLIB_texImg* renderArtTex;

    /* Singleton — set by constructor, accessed by static callbacks */
    static MusicOverlay* instance;

    /* ---- Results, inspected by MusicPlayerView::run() after playback ends */
    int  pendingNextIdx = -1;   /* -1 = no change requested; >=0 = jump to track */
    bool requestStop    = false;

    /* ---- Call when a seek is acknowledged so the seek-bar stays accurate */
    void notifySeek(float targetSecs);

    /* ---- Called just before wii_player_play_audio() to arm timing */
    void resetTiming(float startSecs = 0.0f);

    /* ---- Current estimated playback position (seconds, wall-clock based) */
    float getPosition() const;

    /* ---- Duration from MPlayer demuxer (reliable even for audio-only) */
    float getDuration() const;

private:
    const std::vector<Track>& tracks;
    int currentIdx;

    /* ---- Wall-clock position tracking (g_mplayer_time_pos is 0 for audio) */
    uint64_t startTick      = 0;   /* OGC tick at last seek/start              */
    float    seekBase       = 0.0f;/* playback position at last seek            */
    bool     wasPaused      = false;
    uint64_t pausedAtTick   = 0;   /* tick when pause began                    */
    float    totalPauseSecs = 0.0f;/* accumulated pause time since last seek   */
    bool     timingActive   = false;/* true once g_mplayer_duration>0 (audio started) */

    /* ---- Visualizer state: 24 animated bars                              */
    float vizHeight[VIZ_BARS];   /* current height [0..1]                    */
    float vizTarget[VIZ_BARS];   /* target height (drives smooth animation)  */
    float vizPhase          = 0.0f;
    uint32_t lastFrameTick  = 0;  /* for dt computation in renderFrameGRRLIB */

    void updateVisualizerFrame(float dt);
};

/* -----------------------------------------------------------------------
 * MusicPlayerView — orchestrates the full music-play session.
 *
 * Usage (called from App.cpp):
 *   MusicPlayerView mpv(font, client, auth, serverUrl);
 *   mpv.setTracks(tracks, startIdx);
 *   bool wiiMenu = mpv.run();  // blocks until user exits
 * ----------------------------------------------------------------------- */
class MusicPlayerView {
public:
    MusicPlayerView(GRRLIB_ttfFont* font,
                    JellyfinClient& client,
                    const JellyfinAuth& auth,
                    const std::string& serverUrl);

    void setTracks(const std::vector<MusicOverlay::Track>& tracks, int startIdx = 0);
    void setCursorTex(GRRLIB_texImg* tex);

    /* Run the full music session.
     * Returns true if the user pressed "Wii Menu" from the HOME overlay. */
    bool run();

private:
    GRRLIB_ttfFont*  font;
    JellyfinClient&  client;
    JellyfinAuth     auth;
    std::string      serverUrl;

    std::vector<MusicOverlay::Track> tracks;
    int  currentIdx = 0;

    bool playTrack(int idx);   /* returns false on fatal error */
};
