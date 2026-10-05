#!/bin/bash
# End-to-end playback check, run by the CI:
#   - Counter Film (h264, re-encoded by the server) from the start, then a
#     +10 s seek, which restarts the stream
#   - Xvid Film (copied by the server when played from 0) resumed at 0:45
# Each of the three streams must start and its video advance by at least
# 5 s, without a decoding error ("Error at MB": the picture is broken or
# black) and without an error screen.
#
# Needs the test server with those two movies, and a WiiFin built with the
# button presses of playback.script:
#   tools/test/make_media.sh playback && ONLY=playback tools/test/server.sh
#   NOPROFILE=1 BUILD_ONLY=1 tools/test/tour.sh "$(cat tools/test/playback.script)"
#   tools/test/playback.sh tools/test/out/build/WiiFin.dol
#
# The profile (server, token) and settings go to wiifin.cfg on Dolphin's SD
# card.  env: DOLPHIN as in smoke.sh; VIDEO (default Null)
T=$(dirname "$(realpath "$0")")
. "$T/lib.sh"
FILE=$(realpath "${1:-$T/out/build/WiiFin.dol}")
LIMIT=${2:-300}
[ -f "$FILE" ] || { echo "playback: $FILE not found"; exit 2; }
[ -f "$T/out/session" ] || { echo "playback: no out/session, start server.sh first"; exit 2; }
{ read -r TOKEN; read -r USERID; } < "$T/out/session"
J=http://127.0.0.1:18096
A="Authorization: MediaBrowser Client=\"WiiFinTest\", Device=\"test\", DeviceId=\"wiifin-test\", Version=\"1\", Token=\"$TOKEN\""

# Saved positions: Counter Film from the start, Xvid Film at 0:45
itemId() {
    curl -s "$J/Items?Recursive=true&IncludeItemTypes=Movie&userId=$USERID" -H "$A" |
        python3 -c "import json,sys; print(next(i['Id'] for i in json.load(sys.stdin)['Items'] if i['Name'].startswith('$1')))"
}
position() {
    curl -s -f -o /dev/null -X POST "$J/UserItems/$1/UserData?userId=$USERID" -H "$A" \
        -H 'Content-Type: application/json' -d "{\"PlaybackPositionTicks\":$2,\"Played\":false}"
}
COUNTER=$(itemId Counter) && XVID=$(itemId Xvid) || { echo "playback: Counter Film / Xvid Film not on the server"; exit 2; }
position "$COUNTER" 0 && position "$XVID" 450000000 || { echo "playback: cannot set the positions"; exit 2; }

P=$T/out/playback
rm -rf "$P"; mkdir -p "$P/sd/apps/WiiFin"
cat > "$P/sd/apps/WiiFin/wiifin.cfg" <<EOF
music_enabled=0
home_layout=0
library_view=2
screen_area_asked=1
profile_count=1
profile.0.server_url=$J
profile.0.username=wii
profile.0.server_name=Jellyfin Test
profile.0.user_id=$USERID
profile.0.access_token=$TOKEN
EOF
U=$P/dolphin
# the SD card is built from $P/sd at boot
dolphin_setup "$U" <<EOF
[Core]
WiiSDCard = True
WiiSDCardEnableFolderSync = True
[General]
WiiSDCardSyncFolder = $P/sd
EOF
dolphin_cmd "$P"

$DOLPHIN -v "${VIDEO:-Null}" -u "$U" -e "$FILE" > "$U/out.txt" 2>&1 &
PID=$!
# the script's last entry (button 0) marks the end
for _ in $(seq 1 "$LIMIT"); do
    sleep 1
    kill -0 $PID 2>/dev/null || break
    dolphin_log "$U" | grep -q '^\[TOUR\] t=[0-9]* press 0$' && break
done
dolphin_stop $PID
dolphin_log "$U" > "$P/log.txt"

python3 - "$P/log.txt" <<'PY'
import re, sys
log = open(sys.argv[1], errors='replace').read().split('\n')
fails = []
if not any(re.match(r'\[TOUR\] t=\d+ press 0$', l) for l in log):
    fails.append('the scenario did not reach its end')
starts = [i for i, l in enumerate(log) if l.startswith('[WiiPlayer] play:')]
names = ['Counter Film from the start', 'Counter Film after a +10 s seek', 'Xvid Film resumed at 0:45']
if len(starts) != 3:
    fails.append('%d streams opened, expected 3' % len(starts))
for n, s in enumerate(starts[:3]):
    seg = log[s:starts[n + 1] if n + 1 < len(starts) else len(log)]
    v = [float(m.group(1)) for l in seg for m in [re.search(r'\bV:\s*(-?[\d.]+)', l)] if m]
    errs = sum('Error at MB' in l for l in seg)
    shown = (max(v) - min(v)) if v else 0.0
    ok = any('Starting playback' in l for l in seg) and shown >= 5.0 and errs == 0
    print('%-34s video %5.1f s, %d decoding errors  %s' % (names[n], shown, errs, 'ok' if ok else 'FAILED'))
    if not ok:
        fails.append(names[n])
if starts[2:3] and 'AllowVideoStreamCopy=false' not in log[starts[2]]:
    fails.append('the resumed Xvid stream is not re-encoded')
for l in log:
    if l.startswith('[Library] error screen') or 'Can\'t play this video' in l:
        fails.append(l)
if fails:
    print('playback: FAILED: ' + '; '.join(fails))
    sys.exit(1)
print('playback: OK')
PY
status=$?
if [ $status -ne 0 ]; then
    echo "---- WiiFin log (no MPlayer status lines) ----"
    grep -v '^A: ' "$P/log.txt" | tail -80
    echo "---- Dolphin output ----"; tail -20 "$U/out.txt"
fi
exit $status
