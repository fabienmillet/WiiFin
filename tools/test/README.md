# Test harness

Scripts to run WiiFin in Dolphin against a local Jellyfin, with scripted
input, frame captures and the log. Needs Docker, ffmpeg, Dolphin, devkitPPC
and rsync. Everything generated goes to `tools/test/media/` and `tools/test/out/`,
and both folders are ignored by git.

| Script | Purpose |
|---|---|
| `make_media.sh` | Generates the test library with ffmpeg (~700 MB, run once). |
| `server.sh [rate]` | Starts Jellyfin in Docker with that library (user `wii` / `wii`). It listens on 18096 directly and on 18080 through nginx, which throttles video to `rate` (default `180k`). `server.sh stop` stops it. `ONLY=sync` serves a single folder. |
| `tour.sh "<script>" "<seconds>" [duration]` | Builds a patched copy (scripted buttons, no pointer, a profile for the test server), runs it in Dolphin, then saves frames to `out/frames/` and the log to `out/log.txt`. |
| `measure_av.py` | Measures the picture/sound offset from a `tour.sh` run on Sync Test. |
| `smoke.sh [WiiFin.wad\|WiiFin.dol]` | Boots the build in Dolphin and checks that it reaches its menu. CI runs it on every build. |

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

tools/test/server.sh stop
```

Script times are in milliseconds since WiiFin first reads the buttons. The
seconds given for frames count from the start of Dolphin's dump, which begins
a little later, so take a few frames around the moment you want. The script's
button presses appear in the log as `[TOUR] t=... press ...`.

A PAL console: `DOLPHIN_ARGS="-C Dolphin.Core.FallbackRegion=2 -C SYSCONF.IPL.E60=False"`.
