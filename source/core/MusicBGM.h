#pragma once

/**
 * MusicBGM — background music playback manager.
 *
 * Plays data/music.mp3 in a loop via ASND/libmad everywhere in the app
 * except while MPlayer plays (ao_gekko needs the DSP).  Call pause() before
 * the player starts and resume() once it has stopped.
 */
namespace MusicBGM {
    void init(bool startPlaying = true);  // One-time init: ASND_Init + optionally start loop
    void stop();           // Full teardown: stop thread + MP3 + ASND_End (pair with init to restart)
    void pause();          // Stop music (call before starting MPlayer)
    void resume();         // Restart music + ASND (call after MPlayer stopped)
    void reinitAudio();    // Re-init ASND/MP3Player only, without starting the BGM thread
    void setEnabled(bool on); // Toggle music on/off (persisted setting)
    bool isRunning();           // True when the music thread is active
}
