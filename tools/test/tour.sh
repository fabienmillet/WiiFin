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
#   script    { ms, buttons }, ...   ms since the first input read, e.g.
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
#      EXTRA_SED   shell command run in out/build before building
#      REPO        source tree to build (default: this repository)
#      DOLPHIN_ARGS, DOLPHIN_INI_EXTRA (lines for Dolphin.ini [Core]),
#      DOLPHIN (default dolphin-emu; needs a real video backend for the dump)
#
# Results: out/frames/*.png (and sheet.png side by side), out/log.txt
T=$(dirname "$(realpath "$0")"); REPO=$(realpath "${REPO:-$T/../..}")
[ -f "$T/out/session" ] || { echo "no out/session: start tools/test/server.sh first"; exit 1; }
{ read -r TOKEN; read -r USERID; } < "$T/out/session"
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
u32 tourButtons() {
    static u64 t0 = 0; if (!t0) t0 = ticks_to_millisecs(gettime());
    u64 t = ticks_to_millisecs(gettime()) - t0;
    static const struct { u64 at; u32 btn; } sc[] = { %s { 0xFFFFFFFFull, 0 } };
    static int i = 0;
    if (t >= sc[i].at) { SYS_Report("[TOUR] t=%%llu press %%x\\n", t, sc[i].btn); return sc[i++].btn; }
    return 0;
}
void Input::update() {''' % e['SCRIPT'], 1)
s += '''
/* no pointer */
extern "C" s32 __real_WPAD_IR(int chan, struct ir_t* ir);
extern "C" s32 __wrap_WPAD_IR(int chan, struct ir_t* ir) { s32 r = __real_WPAD_IR(chan, ir); ir->valid = 0; return r; }
'''
if e.get('MARKER'):
    s += '''
/* A/V marker: one white frame and the Start sound together, 12 s in */
#include <grrlib.h>
#include "../core/SoundFX.h"
extern "C" void __real_GRRLIB_Render(void);
extern "C" void __wrap_GRRLIB_Render(void) {
    static u64 t0 = 0; if (!t0) t0 = ticks_to_millisecs(gettime());
    static int done = 0;
    u64 t = ticks_to_millisecs(gettime()) - t0;
    if (!done && t >= 12000) {
        done = 1;
        GRRLIB_FillScreen(0xFFFFFFFF);
        SoundFX::play(SoundFX::FX::Start);
        SYS_Report("[MARK] t=%llu\\n", t);
    }
    __real_GRRLIB_Render();
}
'''
open(p, 'w').write(s)

p = 'Makefile'; s = open(p).read()
old = 'LDFLAGS     := -g $(MACHDEP)'
assert old in s, 'Makefile LDFLAGS changed: update tools/test/tour.sh'
s = s.replace(old, old + ' -Wl,--wrap=WPAD_IR' + (' -Wl,--wrap=GRRLIB_Render' if e.get('MARKER') else ''), 1)
open(p, 'w').write(s)

p = 'source/core/App.cpp'; s = open(p).read()
old = '    loadSettings();\n'
assert old in s, 'App::loadSettings call changed: update tools/test/tour.sh'
theme = {'light': 'Light', 'flix': 'Flix'}.get(e.get('THEME', ''), 'Dark')
s = s.replace(old, old + '''    profiles.clear();
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
