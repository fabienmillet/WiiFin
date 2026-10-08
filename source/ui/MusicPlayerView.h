#pragma once

#include <grrlib.h>
#include <string>
#include <vector>
#include "../jellyfin/JellyfinClient.h"

/* A track of the music queue */
struct MusicTrack {
    std::string id;
    std::string title;
    std::string artist;
    std::string album;
    long long   runtimeTicks = 0;
};

/* -----------------------------------------------------------------------
 * MusicPlayerView — the "Now Playing" screen and the music session.
 *
 * MPlayer plays one track at a time with -vo null, so GRRLIB stays WiiFin's.
 * While a track plays, WiiPlayer's background thread calls this view ~60
 * times a second: tick() reads the buttons, render() draws the screen.
 * Between tracks that thread is joined and the main thread fetches the
 * next stream, details and cover while the screen stays up.
 *
 * The screen: cover (or a coloured placeholder) pulsing with the bass,
 * title / artist / album, a spectrum of
 * the sound actually playing (AudioSpectrum), a seek bar, and the controls:
 * favourite, shuffle, previous, play/pause, next, repeat, up next.  The "Up
 * Next" panel lists the queue in play order.  When the queue runs out,
 * similar tracks follow (Jellyfin's instant mix).  After a while without
 * input the controls fade away and the cover takes the screen.
 *
 * Usage (App.cpp):
 *   MusicPlayerView mpv(font, client, auth, serverUrl);
 *   mpv.setTracks(tracks, startIdx);
 *   mpv.run();   // blocks until the user leaves
 * ----------------------------------------------------------------------- */
class MusicPlayerView {
public:
    MusicPlayerView(GRRLIB_ttfFont* font,
                    JellyfinClient& client,
                    const JellyfinAuth& auth,
                    const std::string& serverUrl);

    void setTracks(const std::vector<MusicTrack>& tracks, int startIdx = 0);
    void setCursorTex(GRRLIB_texImg* tex);

    /* Runs the session; returns true if the user chose "Wii Menu". */
    bool run();

private:
    GRRLIB_ttfFont*  font;
    JellyfinClient&  client;
    JellyfinAuth     auth;
    std::string      serverUrl;
    std::vector<MusicTrack> tracks;
    int              startIdx = 0;
    GRRLIB_texImg*   cursorTex = nullptr;
};
