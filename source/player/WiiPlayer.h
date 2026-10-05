#pragma once

#ifdef __cplusplus
extern "C" {
#endif

#include <stdint.h>

/* ------------------------------------------------------------------
 * Video playback — runs MPlayer CE on its own thread.
 *
 * WiiFin keeps GX / GRRLIB for the whole session: decoded frames are
 * handed over by vo_wiifin.c and drawn by VideoSurface from the main
 * loop, together with the regular UI (PlayerView).
 *
 *   wii_player_start(url);
 *   while (wii_player_is_running()) { input, draw, GRRLIB_Render(); }
 *   int reason = wii_player_wait();
 * ------------------------------------------------------------------ */

/* Start playing url.  Returns 0 if a session is already running. */
int  wii_player_start(const char* url);

/* 1 while MPlayer is still running (including while it shuts down). */
int  wii_player_is_running(void);

/* Join the player thread; returns the PLAYER_STOP_* reason. */
int  wii_player_wait(void);

/* Ask MPlayer to quit with the given PLAYER_STOP_* reason.
 * Safe from any thread; the session ends shortly after. */
void wii_player_request_stop(int reason);

/* Close every IOS socket.  MPlayer's cache thread can sit in a blocking
 * net_read() (e.g. on a keep-alive connection after the last chunk), and
 * MPlayer waits for that thread before quitting; closing its socket makes
 * the read fail so shutdown can finish.  Only call while a stop is pending
 * and WiiFin itself has no request in flight. */
void wii_player_abort_io(void);

/* ------------------------------------------------------------------
 * Return / stop-reason codes
 * ------------------------------------------------------------------ */
#define PLAYER_STOP_EOF     0   /* End of file, or user went back            */
#define PLAYER_STOP_ERROR   1   /* Stream failed / ended prematurely         */
#define PLAYER_STOP_NEXT    2   /* Play next episode                         */
#define PLAYER_STOP_PREV    3   /* Play previous episode                     */
#define PLAYER_STOP_AUDIO   4   /* Re-transcode with another audio track     */
#define PLAYER_STOP_SUB     5   /* Re-transcode with another subtitle track  */
#define PLAYER_STOP_WIIMENU 6   /* User chose "Wii Menu" from the HOME menu  */
#define PLAYER_STOP_RESET   7   /* User chose "Reset" from the HOME menu     */
#define PLAYER_STOP_SEEK    8   /* Re-transcode from another position        */

/* Read after the session ended to know why. */
extern volatile int g_player_stop_reason;

/* Known duration (seconds) from Jellyfin, set before starting; used by the
 * patched mplayer.c when the MPEG-TS demuxer cannot determine it. */
extern volatile float g_wiifin_known_duration;

/* 1 while MPlayer is opening / buffering the stream: set when a session or
 * a seek starts, cleared by the patched mplayer.c once playback progressed
 * more than 1 s.  The UI shows a loading indicator while it is set. */
extern volatile int g_wiifin_loading_active;

/* PTS of the first decoded frame, used by mplayer.c with the flag above. */
extern float g_wiifin_loading_start_pos;

/* ------------------------------------------------------------------
 * MPlayer state — written by the patched mplayer.c main loop.
 * ------------------------------------------------------------------ */
extern volatile float g_mplayer_time_pos;  /* current PTS (seconds, from stream start) */
extern volatile float g_mplayer_duration;  /* stream duration (seconds)              */
extern volatile int   g_mplayer_paused;    /* 1 while MPlayer is paused              */

/* MPlayer cache2.c: % of the cache holding data not yet read, -1 once the
 * whole stream has been received. */
extern float cache_fill_status;

/* ------------------------------------------------------------------
 * Controls — safe from any thread while a session is running.
 * ------------------------------------------------------------------ */
void wii_player_pause_toggle(void);
void wii_player_seek_abs(float seconds);   /* stream-relative seconds */
void wii_player_seek_rel(float delta);
void wii_player_vol_up(void);
void wii_player_vol_down(void);

/* When > 0, adds "-ss <secs>" so MPlayer discards that much stream at start.
 * Set to 3.0f with JellyfinClient's 3 s RESUME_PAD (startTimeTicks > 3 s). */
extern volatile float g_wiifin_ss_secs;

/* 1 when the server burns subtitles into the picture.  Its stream then
 * starts with seconds of audio before the first video frame, more than the
 * quick stream probe reads, and MPlayer started without video. */
extern volatile int g_wiifin_burned_subs;

/* One-shot callback fired once the stream is opened (on the player thread). */
extern void (*g_stream_opened_cb)(void);

#ifdef __cplusplus
}

/* -----------------------------------------------------------------------
 * Music (audio-only) playback path — blocking.
 *
 * wii_player_play_audio() runs MPlayer with -vo null on the calling thread.
 * A background thread polls the Wiimote, calls the music tick callback and
 * the GRRLIB render callback at ~60 Hz (used by MusicPlayerView).
 * ----------------------------------------------------------------------- */

/* Play audio-only URL; returns a PLAYER_STOP_* code. */
int wii_player_play_audio(const char* url);

/* Ask the running audio session to stop. */
void wii_player_stop(void);

/* Register a tick callback for the music overlay (nullptr to clear). */
void wii_player_set_music_tick(
    void (*cb)(int paused, uint32_t btnsDown, uint32_t btnsHeld));

/* Register a GRRLIB render callback for audio-only playback.
 * Called ~60 Hz from the background thread; it must call GRRLIB_Render()
 * itself.  Pass nullptr to clear. */
void wii_player_set_audio_render_cb(void (*cb)());

#endif
