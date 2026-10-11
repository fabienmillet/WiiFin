#pragma once
/* The music session (MusicPlayerView.cpp): a queue MPlayer plays on a thread
 * of its own from its first song to its last, whatever screen is up.  The
 * Now Playing page is a screen over it: leaving the page (B) leaves the
 * music playing; the footers say what plays.  While it plays the audio is
 * MPlayer's: no menu music, no interface sounds (ASND is off). */
namespace MusicSession {

bool active();
/* the Now Playing page over the session playing (Now Playing in the profile
 * menu); true when the user chose Wii Menu or Reset in its HOME menu */
bool open();
/* stopped and its thread over; restoreAudio: the menu music (when it was
 * on) or at least the interface sounds back.  True: the menu music was on
 * before the session, and belongs back after what comes next */
bool stop(bool restoreAudio);
/* each frame, on the main thread (RenderHook): the queue over by itself,
 * the session ended and the menu's audio back */
void pump();

} // namespace MusicSession
