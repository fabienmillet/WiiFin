#!/bin/bash
# End-to-end playback check, run by the CI: plays a scenario of
# tools/test/scenarios (format: see scenario.py) against the test server and
# checks WiiFin's log.
#
#   tools/test/make_media.sh playback
#   MOVIES=playback MIXED= tools/test/server.sh 300k
#   NOPROFILE=1 BUILD_ONLY=1 tools/test/tour.sh ""
#   tools/test/playback.sh resume [tools/test/out/build/WiiFin.dol] [timeout s]
#
# The profile, settings and presses go to Dolphin's SD card (wiifin.cfg,
# tour.txt), so one test build plays every scenario.  The log is kept in
# out/playback/<scenario>/log.txt.
# env: DOLPHIN as in smoke.sh; VIDEO (default Null)
#      DUMP=1: also dump the picture (default video backend) and look for
#      screens that flash for a frame or two (flicker.py, out/.../flicker)
#      FRAMES="5 10 20": dump too, and save those seconds of it to
#      out/playback/<scenario>/frames
#      PAL=1: a European Wii in PAL 50 Hz (576i) instead of NTSC 480i
#      AUDIO_DUMP=1: Dolphin writes what the console plays to
#      out/playback/<scenario>/dolphin/Dump/Audio (nothing on the speakers)
T=$(dirname "$(realpath "$0")")
[ -n "$FRAMES" ] && DUMP=1
. "$T/lib.sh"
NAME=${1:?usage: playback.sh <scenario> [dol] [timeout s]}
SC=$T/scenarios/$NAME.txt
FILE=$(realpath "${2:-$T/out/build/WiiFin.dol}")
LIMIT=${3:-300}
[ -f "$SC" ] || { echo "playback: no scenario $SC"; exit 2; }
[ -f "$FILE" ] || { echo "playback: $FILE not found"; exit 2; }
[ -f "$T/out/session" ] || { echo "playback: no out/session, start server.sh first"; exit 2; }

# a scenario of a 50 Hz European Wii (its "pal50" line): PAL=1
grep -q '^pal50$' "$SC" && PAL=1

P=$T/out/playback/$NAME
rm -rf "$P"
python3 "$T/scenario.py" sd "$SC" "$P/sd" "$T/out/session" || exit 2
python3 "$T/scenario.py" positions "$SC" http://127.0.0.1:18096 "$T/out/session" || exit 2

U=$P/dolphin
# the SD card is built from $P/sd at boot
dolphin_setup "$U" <<EOF
[Core]
WiiSDCard = True
WiiSDCardEnableFolderSync = True
[General]
WiiSDCardSyncFolder = $P/sd
${PAL:+FallbackRegion = 2}
${AUDIO_DUMP:+[DSP]
DumpAudio = True
DumpAudioSilent = True}
${DUMP:+[Movie]
DumpFrames = True
DumpFramesSilent = True}
EOF
dolphin_cmd "$P"
# PAL=1: the console set to 50 Hz in its SYSCONF (IPL.E60 0), as a European
# Wii's: libogc 3.2 reads it there to pick 576i over EuRGB60 (Dolphin's
# -C SYSCONF.IPL.E60=False below sets its own boot, not that file).  A
# SYSCONF Dolphin made, its E60 cleared (tools/test/SYSCONF-pal50)
if [ -n "$PAL" ]; then
    mkdir -p "$U/Wii/shared2/sys"
    cp "$T/SYSCONF-pal50" "$U/Wii/shared2/sys/SYSCONF"
fi

VARGS=(-v "${VIDEO:-Null}")
[ -n "$DUMP" ] && [ -z "$VIDEO" ] && VARGS=()   # Null draws nothing to dump
[ -n "$PAL" ] && VARGS+=(-C SYSCONF.IPL.E60=False)   # PAL 50 Hz, 576i
[ -n "$XFB" ] && VARGS+=(-C GFX.Hacks.XFBToTextureEnable=False -C GFX.Hacks.ImmediateXFBEnable=False)   # what the VI shows
$DOLPHIN "${VARGS[@]}" -u "$U" -e "$FILE" > "$U/out.txt" 2>&1 &
PID=$!
# the server's remote control, when the scenario has some (remote lines)
RPID=
if grep -q '^remote ' "$SC"; then
    python3 "$T/scenario.py" remote "$SC" "$U/Logs/dolphin.log" http://127.0.0.1:18096 "$T/out/session" \
        > "$P/remote.txt" 2>&1 &
    RPID=$!
fi
# the scenario's END press (button 0) marks the end
for _ in $(seq 1 "$LIMIT"); do
    sleep 1
    kill -0 $PID 2>/dev/null || break
    dolphin_log "$U" | grep -q '^\[TOUR\] t=[0-9]* press 0$' && break
done
dolphin_stop $PID
[ -n "$RPID" ] && kill $RPID 2>/dev/null
dolphin_log "$U" > "$P/log.txt"
# what the server answered the remote commands (expect log can check it)
[ -f "$P/remote.txt" ] && cat "$P/remote.txt" >> "$P/log.txt"
F=$(/bin/ls -S "$U"/Dump/Frames/*.avi 2>/dev/null | head -1)
if [ -n "$FRAMES" ]; then
    mkdir -p "$P/frames"
    for t in $FRAMES; do
        ffmpeg -loglevel error -y -ss "$t" -i "$F" -frames:v 1 -vf scale=480:-1 "$P/frames/t$t.png"
    done
fi

echo "== $NAME: $(sed -n '1s/^# //p' "$SC")"
python3 "$T/scenario.py" check "$SC" "$P/log.txt"
status=$?
if [ -n "$DUMP" ]; then
    python3 "$T/flicker.py" "$F" "$P/flicker" || status=1
fi
if [ $status -ne 0 ]; then
    echo "---- WiiFin log (no MPlayer status lines) ----"
    grep -v '^A: ' "$P/log.txt" | tail -80
    echo "---- Dolphin output ----"; tail -20 "$U/out.txt"
fi
exit $status
