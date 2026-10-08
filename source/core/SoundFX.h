#pragma once

/**
 * SoundFX — short-sound effect playback via ASND.
 *
 * Each FX MP3 is decoded to PCM once at init() and stored in
 * memalign(32) buffers.  Playback uses ASND voices 4 and 7-9 (one-shots),
 * 5 (Loading loop) and 6 (HOME menu).
 *
 * A file in <app folder>/sounds/ replaces the built-in sound of the same
 * name (name.mp3 or name.wav, see NAMES in SoundFX.cpp and the README).
 * No sound while MPlayer plays: ASND is shut down then (MusicBGM).
 *
 * Call SoundFX::init() after MusicBGM::init() (which calls ASND_Init).
 * MusicBGM occupies voices 0–1 via MP3Player; SoundFX uses 4–5 to avoid
 * conflicts.
 */
namespace SoundFX {
    enum class FX {
        Start,      // button click  (button_start.png texture)
        Select,     // cursor hover over button_start button
        PressKey,   // VKB key press in ConnectView
        MenuExit,   // "Yes" in HOME-menu confirmation popup
        MenuEnter,  // HOME button pressed → overlay opens
        Loading,    // loading spinner (looped; call stopLoading() to end)
        Backspace,  // backspace key on VKB
        Back,       // "No" in HOME-menu confirmation popup, B in the menus
        Move,       // cursor moved in the menus (lists, rows, grids, settings)
        Open,       // a show, film, library... opened
        Play,       // playback asked for
        Page,       // page / tab turned
        COUNT_      // internal sentinel — not a playable sound
    };

    /* decode every FX to PCM; call once after ASND_Init.  dir: the app
     * folder (with its '/'), whose sounds/ may replace the built-in ones */
    void init(const char* dir);
    void setEnabled(bool on); // Settings > Interface Sounds
    bool enabled();
    void setLogging(bool on); // tests (wiifin.cfg log_sounds=1): each sound played logged
    void fadeOut();       // the one-shots faded to silence (before ASND_End)
    void play(FX fx);     // play FX; for Loading, arms the loop (call tickLoading each frame)
    void waitDone(FX fx); // block until the voice for 'fx' finishes (use before hard exits)
    void tickLoading();   // call every loading frame — restarts voice only when it finishes
    void stopLoading();   // stop the Loading loop voice
}
