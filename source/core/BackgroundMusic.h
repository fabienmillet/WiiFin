#pragma once
/* The music that goes on while browsing: B in the music player while a song
 * plays hands its queue over to this.  The songs play through MP3Player
 * (libmad, as the menu music), each one an MP3 that Jellyfin streams
 * (transcoded when it is not one), with MPlayer off.  A video, or the music
 * player opened again, stops it; the footers show what plays. */
#include <string>
#include <vector>
#include "../jellyfin/JellyfinClient.h"
#include "../ui/MusicPlayerView.h"

namespace BackgroundMusic {

/* the queue (tracks in order, the place in it), from secs into the song;
 * menuMusic: the menu music comes back once it is over */
void start(const JellyfinClient& client, const std::string& serverUrl, const JellyfinAuth& auth,
           const std::vector<MusicTrack>& tracks, const std::vector<int>& order, int pos,
           float secs, int repeat, bool menuMusic);
/* stopped (the menu music back when resumeMenuMusic); true: it was playing
 * in place of the menu music, which belongs back after what comes next */
bool stop(bool resumeMenuMusic);
bool playing();
/* each frame, on the main thread (RenderHook): the queue over, the menu
 * music back */
void pump();
/* where it is, for the music player opened again */
void state(std::vector<MusicTrack>& tracks, std::vector<int>& order, int& pos, float& secs, int& repeat);

} // namespace BackgroundMusic
