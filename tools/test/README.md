# Test harness

Scripts to run WiiFin in Dolphin against a local Jellyfin, with scripted
input, frame captures and the log. Needs Docker, ffmpeg, Dolphin, devkitPPC
and rsync. Everything generated goes to `tools/test/media/` and `tools/test/out/`,
and both folders are ignored by git.

Dolphin's sound is off in these runs (`SOUND=1` keeps it). Its window opens on
the desktop: to keep it out of the way, run the scripts under `xvfb-run -a`
(as the CI does) or on any other X display (`DISPLAY=:99 QT_QPA_PLATFORM=xcb`,
without `WAYLAND_DISPLAY`).

| Script | Purpose |
|---|---|
| `make_media.sh [name...]` | Generates the test library with ffmpeg (~700 MB, run once), or only the named parts (`counter`, `xvid`, `formats`, `shows`, `music`, `sync`, `playback`…). Files already there are kept; `playback/` (hard links) gets what it lacks. |
| `server.sh [rate]` | Starts Jellyfin in Docker with that library (user `wii` / `wii`). It listens on 18096 directly, on 18080 through nginx, which throttles video to `rate` (default `180k`), on 18443 in HTTPS with a self-signed certificate, and on 18444 the same with an ECDSA P-384 certificate signed in SHA-384. `server.sh stop` stops it. `MOVIES`, `SHOWS` and `MIXED` choose the folders of the libraries (empty: no such library), `ONLY=sync` serves a single folder, `media/music` (`make_media.sh music`: Test Album) becomes the "Singles" library whenever it is there, and `MUSIC=<folder>` adds a folder of your own audio files as the "Songs" library; the test user sees Movies, Shows, Singles, Songs in that order (the scenarios count them). Port 18081 gives every item the same media segments (an intro and credits). Every port lets the remote control's WebSocket through. Jellyfin makes the trickplay thumbnails of the videos. Each start is a new server; with `KEEP=<folder>` (e.g. `tools/test/out/keep`) its configuration stays in that folder instead: accounts and tokens survive a restart, and the containers come back by themselves after a reboot, until `server.sh stop`. The images are pinned; `JELLYFIN_IMAGE` and `NGINX_IMAGE` override them. |
| `tour.sh "<script>" "<seconds>" [duration]` | Builds a patched copy (scripted buttons, no pointer, a profile for the test server), runs it in Dolphin, then saves frames to `out/frames/` and the log to `out/log.txt`. |
| `measure_av.py` | Measures the picture/sound offset from a `tour.sh` run on Sync Test. |
| `smoke.sh [WiiFin.wad\|WiiFin.dol]` | Boots the build in Dolphin and checks that it reaches its menu. CI runs it on every build. |
| `playback.sh <scenario> [dol]` | Plays a scenario of `scenarios/` and checks the log: each stream must start, its video must advance and decode without errors, plus the scenario's own expectations. The profile, settings and button presses go to Dolphin's SD card, so one test build plays them all. `FRAMES="5 10"` also saves pictures. `DUMP=1` dumps the picture and fails on screens that flash (`flicker.py`). CI runs the scenarios listed below on every build. |
| `flicker.py <dump.avi> [folder]` | Finds screens shown for less than 100 ms in a Dolphin frame dump, and saves each one with the screens before and after. |
| `scenario.py` | Reads the scenario files (format in its header) for `playback.sh`. |
| `latency.py [log]` | Times pause, resume and ±10 s seeks, stage by stage, from a run of the `latency` or `latency-slow` scenario (direct link, or 2.4 Mb/s with `server.sh 300k`). |

## Examples

```sh
tools/test/make_media.sh
tools/test/server.sh

# Sign in, open the first library, open the first movie and play it.
# Frames at 8, 20 and 40 s; the run lasts 50 s.
tools/test/tour.sh "{ 6000, WPAD_BUTTON_A }, { 9000, WPAD_BUTTON_A }, \
  { 13000, WPAD_BUTTON_A }, { 16000, WPAD_BUTTON_A }," "8 20 40" 50

# Direct connection instead of the throttled one, first launch, light theme
SERVER=http://127.0.0.1:18096 FIRSTRUN=1 THEME=light tools/test/tour.sh "" "5 10" 15

# A/V offset
ONLY=sync tools/test/server.sh
MARKER=1 tools/test/tour.sh "<script that plays Sync Test>" "" 60
tools/test/measure_av.py

# The CI playback checks
tools/test/make_media.sh playback music
MOVIES=playback MIXED= tools/test/server.sh 300k
NOPROFILE=1 BUILD_ONLY=1 tools/test/tour.sh ""
for s in resume resume-transcode xvid formats audio-track https https-ecdsa slow subtitles subtitles-switch subtitles-big next next-from-home trickplay music-formats sounds sounds-off remote remote-play remote-queue segments stuck japanese music-favorite sort-favorite music-tabs specials cjk watched settings home-sounds music-quiet season-posters overview favorite-home versions; do tools/test/playback.sh $s; done

# A server that keeps its accounts, with your own music as the Songs library
KEEP=tools/test/out/keep MOVIES=playback MIXED= MUSIC=~/Music tools/test/server.sh 300k

tools/test/server.sh stop
```

Script times are in milliseconds since WiiFin first reads the buttons. The
seconds given for frames count from the start of Dolphin's dump, which begins
a little later, so take a few frames around the moment you want. The script's
button presses appear in the log as `[TOUR] t=... press ...`.

Scenarios (`scenarios/*.txt`, their format in the header of `scenario.py`:
presses, settings, saved positions, custom sounds, the server's remote
commands, and what the log must or must not show):

| Scenario | Checks |
|---|---|
| `resume` | Counter Film (720p: transcoded) from the start, a +10 s seek, then Xvid Film resumed at 0:45, played as it is: MPlayer seeks in the file, back to the keyframe before 0:45 |
| `resume-transcode` | The same with direct play off: the resumed Xvid is re-encoded (no decoding errors) |
| `formats` | Direct play of each container (`make_media.sh formats`): AVI Xvid + AC3, MKV H.264 + AC3 5.1, MP4 index first and last, MPEG-2 PS and TS; a +10 s seek in each file (Z7, two audio tracks, is for `audio-track`) |
| `xvid` | Xvid Film from the start: the AVI's video copied as is, B-frames included |
| `https` | Counter Film over HTTPS (mbedTLS) |
| `https-ecdsa` | The same with an ECDSA P-384 certificate signed in SHA-384 (like Let's Encrypt's ECDSA certificates, Caddy's default): the handshake needs SHA-384 in mbedTLS |
| `slow` | Through the throttled proxy (300k): the quality is capped to Low and plays without rebuffering |
| `subtitles` | Subtitle Film with its SRT track, on from the start as the server picks it for the user (its subtitle mode, the file's flags): drawn by WiiFin over the picture, the film played as it is |
| `subtitles-switch` | Subtitles turned off on the page, then the SRT track turned on and off in the player: no restart of the stream |
| `subtitles-big` | Z8's 730 KB SRT track (past the 256 KB response buffer): fetched whole and drawn |
| `music-formats` | Test Album (`make_media.sh music`, the Singles library) in the music player: MP3, FLAC and WAV as they are, a 96 kHz FLAC and an AAC transcoded, each for its reason |
| `audio-track` | Z7's second audio track picked in the detail page, then the first in the player: both played as they are (`-aid`) |
| `next` | An episode played to its end goes on with the next one |
| `next-from-home` | The last episode of season 1 resumed from Continue Watching, then + and -: the next episode is the first of season 2, the previous one brings it back (WiiFin fetches the series itself) |
| `trickplay` | +10 s presses show the target's trickplay thumbnail over the seek bar (`server.sh` has Jellyfin make them) |
| `remote` | The server's remote control: Counter Film paused, played again, a message shown over it, then stopped from the server (`scenario.py` sends them through its API); in the menus a message shows too, and a pause has nothing to pause |
| `remote-play` | "Play on... Nintendo Wii" from the server: a film from the home screen, its volume set from the server and shown in the server's session, another film replacing it, then a track of Test Album in the music player |
| `remote-queue` | "Add to queue" with nothing playing plays at once; then a track added to the queue and another to play next: they play in that order (`expect logorder`) |
| `music-favorite` | The music player's heart: a track marked as a favourite on the server, then unmarked (`[Favorite] on/off: HTTP 200`) |
| `sort-favorite` | A film marked as a favourite on its page (1), then the Movies list sorted by year and filtered to the favourites (2): that film alone; unmarked again |
| `music-tabs` | The music library's tabs on Singles: Albums, Suggestions, Artists, Playlists, Songs, each its own list (1 album, 1 artist, no playlist, 5 tracks) |
| `season-posters` | Library View: Posters: Shuffle Show's seasons in a grid of posters (their own, or the series' one), Right to the next season, A opens its episodes |
| `overview` | A synopsis too long for the page (`overview` sets it on the server): "Read more" under its five lines, - opens all of it, paragraphs kept, Up / Down scroll it, B closes |
| `favorite-home` | The home rows follow a favourite: Counter Film marked on its page (from Continue Watching), back on the rows it is in My Favorites; unmarked, gone again (no reconnecting) |
| `versions` | ZB, a film in two versions (Fullscreen with two audio tracks, Widescreen with one): a Version row above Audio, Right switches to Widescreen and its track, Play plays that file |
| `watched` | Watched / not watched with +: on Counter Film's page (on, then off), then on the first row of Shuffle Show's episode list; its page shows it watched, the zoom button (a GameCube's Z, here a Classic's ZL) unmarks it, and the list follows |
| `specials` | Special features: on Counter Film's page, 2 lists its trailer and its featurette; the featurette's page, then it plays; B back to the list, then to the film |
| `japanese` | Z9, a Japanese title with Japanese subtitles: not one character missing from the fonts (`[Text] no glyph`) |
| `cjk` | ZA, a Korean and Chinese title with subtitles in both, drawn with the SD card's font (`data/fonts/cjk`, put there with `sdfile`): not one character missing, and that font drew some |
| `stuck` | A picture stuck while the data comes in (`test_freeze_at`): one more try at a lower quality, then a message that says so instead of blaming the connection |
| `segments` | Media segments (`server.sh`'s port 18081 gives every item an intro and credits): Skip intro jumps to the intro's end, then Next episode during the credits |
| `sounds` | The interface sounds (each one logged with `log_sounds=1`): moving, opening, going back, pages, playback; a `move.wav` and a `bgm.mp3` in `sounds/` on the SD card replace the built-in ones |
| `sounds-off` | Settings > Interface Sounds off: not a sound in the menus |
| `settings` | Every setting changed once from the Settings screen: the `[Settings]` line logged on leaving shows each one changed; Background Music off stops the music at once and the interface sounds go on without it; Interface Sounds off, then not a sound |
| `home-sounds` | The HOME menu with the background music off: its sounds play, and the menus' sounds still do once it is closed |
| `music-quiet` | Leaving the music player with the background music off: the menu music stays off, the interface sounds come back |
| `screens` | Not in CI. Every screen with the D-pad alone; each one must draw the pointer when there is one. With `DUMP=1`, no screen may flash. |
| `latency`, `latency-slow` | Not in CI. Pause, resume and seeks, timed by `latency.py`. |
| `music` | Not in CI, needs `MUSIC=<a big folder>` of loose files (the Songs library opens on its tracks). A track of the list starts a queue; seek bar, Up Next panel, shuffle, idle screen. |
| `music-wav` | Not in CI, needs a WAV at that place of the list. Jellyfin transcodes it and the session says so. |
| `letters` | Not in CI, needs `MUSIC=<folder>`. Left/Right jump between the letters of a long list. |
| `pal` | Not in CI, run with `PAL=1` (a European Wii in PAL 50 Hz, 576i) and `XFB=1 FRAMES="5 11"` (the picture as the console sends it): the menus fill the screen. |
| `solid` | Not in CI, a look with `FRAMES`. Settings > Backgrounds: Solid, the third settings page and the home screen without gradients. |
| `bgm-pace` | Not in CI. Browsing with the background music on; the test build logs the frame pacing (`[FRAMES]`, every 5 s): compare with `music_enabled=0`. Dolphin underrates MP3 decoding, so only a real Wii tells for sure. |

A PAL console: `PAL=1` for `playback.sh`, `DOLPHIN_ARGS="-C Dolphin.Core.FallbackRegion=2 -C SYSCONF.IPL.E60=False"` for `tour.sh`.
