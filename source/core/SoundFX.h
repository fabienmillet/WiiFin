#pragma once

/**
 * SoundFX — short-sound effect playback via ASND.
 *
 * Each FX MP3 is decoded to PCM once at init() and stored in
 * memalign(32) buffers.  Playback uses ASND voice 4 (one-shot) and
 * voice 5 (Loading loop).
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
        Back,       // "No" in HOME-menu confirmation popup
        COUNT_      // internal sentinel — not a playable sound
    };

    void init();          // decode all MP3 FX to PCM; call once after ASND_Init
    void play(FX fx);     // play FX; for Loading, arms the loop (call tickLoading each frame)
    void waitDone(FX fx); // block until the voice for 'fx' finishes (use before hard exits)
    void tickLoading();   // call every loading frame — restarts voice only when it finishes
    void stopLoading();   // stop the Loading loop voice
}
