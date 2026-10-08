#!/bin/bash
# Scripted run of WiiFin in Dolphin against the test server (server.sh).
#
# Builds a copy of the repo (out/build) where:
#   - the Wii Remote buttons come from the script below, not from Dolphin,
#   - the pointer is never valid (Dolphin's emulated one wanders over the UI),
#   - a profile for the test server is already saved, so A on
#     "Connect To Jellyfin" signs straight in,
# then runs it in Dolphin with frame dumping, extracts frames at the given
# seconds of the dump and writes the OSREPORT log.
#
# usage: tools/test/tour.sh "<script>" "<seconds>" [duration]
#   script    { ms, buttons }, ...   ms since the first input read (or ""
#             and the presses in sd:/apps/WiiFin/tour.txt, see playback.sh), e.g.
#             "{ 6000, WPAD_BUTTON_A }, { 9000, WPAD_BUTTON_DOWN },"
#             (WPAD_* bits, Input::BTN_ZOOM; a Classic Controller is reported)
#   seconds   "5 10 20": frames of the dump to save as out/frames/t<s>.png
#   duration  seconds of emulation (default 60)
#
# env: SERVER      profile URL (default http://127.0.0.1:18080, the throttled one)
#      THEME       dark (default), light or flix
#      FIRSTRUN=1  keep the first-launch questions (screen area)
#      ASPECT43=1  4:3 console
#      MARKER=1    white frame + Start sound 12 s in, audio dump: for measure_av.py
#      (the scenario's POINTER / NOPOINTER presses show a pointer: [POINTER] lines)
#      EXTRA_SED   shell command run in out/build before building
#      REPO        source tree to build (default: this repository)
#      NOPROFILE=1 no profile or settings built in: they come from wiifin.cfg
#      BUILD_ONLY=1 stop once out/build/WiiFin.dol is built (no Dolphin run)
#      DOLPHIN_ARGS, DOLPHIN_INI_EXTRA (lines for Dolphin.ini [Core]),
#      DOLPHIN (default dolphin-emu; needs a real video backend for the dump)
#
# Results: out/frames/*.png (and sheet.png side by side), out/log.txt
T=$(dirname "$(realpath "$0")"); REPO=$(realpath "${REPO:-$T/../..}")
if [ -z "$NOPROFILE" ]; then
    [ -f "$T/out/session" ] || { echo "no out/session: start tools/test/server.sh first"; exit 1; }
    { read -r TOKEN; read -r USERID; } < "$T/out/session"
fi
B=$T/out/build
rm -rf "$B"; mkdir -p "$B"
rsync -a --exclude /build --exclude .git --exclude /tools/test/out --exclude /tools/test/media \
    --exclude /tools/.venv --exclude '*.elf' --exclude '*.dol' --exclude '*.wad' --exclude '*.map' \
    "$REPO/" "$B/" && cd "$B" || exit 1

SCRIPT="$1" TOKEN="$TOKEN" USERID="$USERID" SERVER="${SERVER:-http://127.0.0.1:18080}" python3 - <<'PY' || { echo "patch failed"; exit 1; }
import os
e = os.environ
p = 'source/input/Input.cpp'; s = open(p).read()
old = 'u32 wDown = WPAD_ButtonsDown(0);'
assert old in s, 'Input::update changed: update tools/test/tour.sh'
s = s.replace(old, 'u32 wDown = tourButtons(); classic = true;')
s = s.replace('void Input::update() {', '''#include <ogc/system.h>
#include <stdio.h>
/* "POINTER" / "NOPOINTER" in a scenario: the pointer shows at a fixed spot
 * (no press meanwhile); each screen must draw it (see the [POINTER] lines) */
#define TOUR_POINTER_ON  0x04000000u
#define TOUR_POINTER_OFF 0x02000000u
void tourPointer(bool on);
/* Presses: the built-in script, or sd:/apps/WiiFin/tour.txt when present
 * ("<ms> <hex buttons>" per line, see playback.sh) */
u32 tourButtons() {
    static u64 t0 = 0; if (!t0) t0 = ticks_to_millisecs(gettime());
    u64 t = ticks_to_millisecs(gettime()) - t0;
    static struct { u64 at; u32 btn; } sc[256] = { %s { 0xFFFFFFFFull, 0 } };
    static int i = -1;
    if (i < 0) {
        i = 0;
        if (FILE* f = fopen("sd:/apps/WiiFin/tour.txt", "r")) {
            int n = 0; unsigned long long at; unsigned b;
            while (n < 255 && fscanf(f, "%%llu %%x", &at, &b) == 2) { sc[n].at = at; sc[n].btn = b; n++; }
            sc[n].at = 0xFFFFFFFFull; sc[n].btn = 0;
            fclose(f);
            SYS_Report("[TOUR] %%d presses from tour.txt\\n", n);
        }
    }
    if (t >= sc[i].at) {
        u32 b = sc[i++].btn;
        SYS_Report("[TOUR] t=%%llu press %%x\\n", t, b);
        if (b & TOUR_POINTER_ON)  tourPointer(true);
        if (b & TOUR_POINTER_OFF) tourPointer(false);
        return b & ~(TOUR_POINTER_ON | TOUR_POINTER_OFF);
    }
    return 0;
}
void Input::update() {''' % e['SCRIPT'], 1)
s += '''
/* The pointer: none, except when the scenario turns it on.  Frames drawn
 * while it is on are counted, with and without the pointer sprite. */
#include <grrlib.h>
#include "../core/SoundFX.h"
extern "C" { GRRLIB_texImg* g_tour_cursor = nullptr; }
static volatile bool s_pointer = false;
static int s_frames = 0, s_withCursor = 0;
static bool s_cursorDrawn = false;
void tourPointer(bool on) {
    if (!on && s_pointer)
        SYS_Report("[POINTER] %%d frames, pointer drawn on %%d\\n", s_frames, s_withCursor);
    s_pointer = on; s_frames = s_withCursor = 0;
}
extern "C" s32 __real_WPAD_IR(int chan, struct ir_t* ir);
extern "C" s32 __wrap_WPAD_IR(int chan, struct ir_t* ir) {
    s32 r = __real_WPAD_IR(chan, ir);
    ir->valid = s_pointer ? 1 : 0;
    if (s_pointer) { ir->x = 636; ir->y = 300; ir->sx = 636; ir->sy = 300; ir->angle = 0; }
    return r;
}
extern "C" void __real_GRRLIB_DrawImg(const f32 x, const f32 y, const GRRLIB_texImg* tex, const f32 deg,
                                      const f32 sx, const f32 sy, const u32 color);
extern "C" void __wrap_GRRLIB_DrawImg(const f32 x, const f32 y, const GRRLIB_texImg* tex, const f32 deg,
                                      const f32 sx, const f32 sy, const u32 color) {
    if (tex && tex == g_tour_cursor) s_cursorDrawn = true;
    __real_GRRLIB_DrawImg(x, y, tex, deg, sx, sy, color);
}
/* each frame, just before GRRLIB_Render (source/core/RenderHook.cpp) */
extern "C" void (*g_wiifin_before_render)(void);
static void tourBeforeRender(void) {
    if (s_pointer) { s_frames++; if (s_cursorDrawn) s_withCursor++; }
    s_cursorDrawn = false;
    /* frame pacing, every 5 s: the time between two frames (16.7 ms when
     * each one makes its vsync), and the frames that missed it */
    static u64 last = 0, from = 0, worst = 0;
    static int n = 0, late = 0;
    u64 now = gettime();
    if (last) {
        u64 d = now - last;
        n++; if (d > worst) worst = d;
        if (ticks_to_microsecs(d) > 20000) late++;
        if (ticks_to_millisecs(now - from) >= 5000) {
            SYS_Report("[FRAMES] %%d frames in %%llu ms, %%d late, worst %%llu ms\\n", n,
                       ticks_to_millisecs(now - from), late, ticks_to_millisecs(worst));
            n = late = 0; worst = 0; from = now;
        }
    } else from = now;
    last = now;
%s}
static struct TourHook { TourHook() { g_wiifin_before_render = tourBeforeRender; } } s_tourHook;
''' % ('''    /* A/V marker: one white frame and the Start sound together, 12 s in */
    static u64 t0 = 0; if (!t0) t0 = ticks_to_millisecs(gettime());
    static int done = 0;
    u64 t = ticks_to_millisecs(gettime()) - t0;
    if (!done && t >= 12000) {
        done = 1;
        GRRLIB_FillScreen(0xFFFFFFFF);
        SoundFX::play(SoundFX::FX::Start);
        SYS_Report("[MARK] t=%llu\\n", t);
    }
''' if e.get('MARKER') else '')
open(p, 'w').write(s)

p = 'Makefile'; s = open(p).read()
old = 'LDFLAGS     := -g $(MACHDEP)'
assert old in s, 'Makefile LDFLAGS changed: update tools/test/tour.sh'
s = s.replace(old, old + ' -Wl,--wrap=WPAD_IR -Wl,--wrap=GRRLIB_DrawImg', 1)   # GRRLIB_Render: g_wiifin_before_render
open(p, 'w').write(s)

p = 'source/core/App.cpp'; s = open(p).read()
cur = '    cursorPointerTex    = GRRLIB_LoadTexture(data_cursors_PointerP1_64_png);\n'
assert cur in s, 'cursor texture load changed: update tools/test/tour.sh'
s = s.replace(cur, cur + '    { extern GRRLIB_texImg* g_tour_cursor; g_tour_cursor = cursorPointerTex; }\n', 1)
old = '    loadSettings();\n'
if e.get('NOPROFILE'):
    old = None   # profile and settings come from wiifin.cfg
assert old is None or old in s, 'App::loadSettings call changed: update tools/test/tour.sh'
theme = {'light': 'Light', 'flix': 'Flix'}.get(e.get('THEME', ''), 'Dark')
if old: s = s.replace(old, old + '''    profiles.clear();
    { SavedProfile sp; sp.serverUrl = "%s"; sp.username = "wii"; sp.serverName = "Jellyfin Test";
      sp.userId = "%s"; sp.accessToken = "%s"; profiles.push_back(sp); }
    Ui::setTheme(Ui::Theme::%s);
%s%s''' % (e['SERVER'], e['USERID'], e['TOKEN'], theme,
           '' if e.get('FIRSTRUN') else '    screenAreaAsked = true;\n',
           '    musicEnabled = false;\n' if e.get('MARKER') else ''), 1)
open(p, 'w').write(s)
PY
[ -n "$EXTRA_SED" ] && eval "$EXTRA_SED"

export DEVKITPRO=${DEVKITPRO:-/opt/devkitpro} DEVKITPPC=${DEVKITPPC:-/opt/devkitpro/devkitPPC}
PATH=$DEVKITPRO/tools/bin:$PATH make -j"$(nproc)" 2>&1 | grep -E ' error|error:' | head -5
[ -f WiiFin.dol ] || { echo "build failed"; exit 1; }
[ -n "$BUILD_ONLY" ] && { echo "built $B/WiiFin.dol"; exit 0; }

U=$T/out/dolphin; rm -rf "$U"; mkdir -p "$U/Config"
printf '[Options]\nVerbosity = 3\nWriteToConsole = False\nWriteToFile = True\n[Logs]\nOSREPORT = True\n' \
    > "$U/Config/Logger.ini"
# SIDevice0 = 0: an emulated GameCube pad sends phantom START presses
printf '[Movie]\nDumpFrames = True\nDumpFramesSilent = True\n[Core]\nWiimoteContinuousScanning = False\nSIDevice0 = 0\n%b[Analytics]\nEnabled = False\nPermissionAsked = True\n%b' \
    "${DOLPHIN_INI_EXTRA:+$DOLPHIN_INI_EXTRA\n}" "${MARKER:+[DSP]\nDumpAudio = True\nDumpAudioSilent = True\n}" \
    > "$U/Config/Dolphin.ini"
ARGS="$DOLPHIN_ARGS"; [ -n "$ASPECT43" ] && ARGS="$ARGS -C SYSCONF.IPL.AR=False"
DUR=${3:-60}
# shellcheck disable=SC2086
timeout $((DUR + 20)) ${DOLPHIN:-dolphin-emu} $ARGS -u "$U" -b -e "$B/WiiFin.dol" > "$U/out.txt" 2>&1 &
PID=$!
sleep "$DUR"; kill $PID 2>/dev/null
# let Dolphin finish the dump file (a killed one has no index)
for _ in $(seq 1 20); do kill -0 $PID 2>/dev/null || break; sleep 1; done
pkill -9 -f -- "-u $U" 2>/dev/null; wait $PID 2>/dev/null

F=$(/bin/ls -S "$U"/Dump/Frames/*.avi 2>/dev/null | head -1)
rm -rf "$T/out/frames"; mkdir -p "$T/out/frames"
IN=()
for t in $2; do
    ffmpeg -loglevel error -y -ss "$t" -i "$F" -frames:v 1 -vf scale=480:-1 "$T/out/frames/t$t.png" &&
        IN+=(-i "$T/out/frames/t$t.png")
done
[ ${#IN[@]} -gt 2 ] && ffmpeg -loglevel error -y "${IN[@]}" -filter_complex hstack=$((${#IN[@]} / 2)) "$T/out/frames/sheet.png"
sed -n 's/^.*OSREPORT[^:]*\]: //p' "$U/Logs/dolphin.log" > "$T/out/log.txt"
echo "frames: $(ls "$T/out/frames" | tr '\n' ' ')"
echo "log: $T/out/log.txt ($(wc -l < "$T/out/log.txt") lines)"
