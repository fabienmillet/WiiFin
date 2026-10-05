#!/bin/bash
# Boot WiiFin in Dolphin (Null video backend) and check it gets to its menu:
# the OSREPORT log must show "[WiiFin] ready" and Dolphin must still be
# running a few seconds later.  Exit status 0 when it does.
#
# usage: tools/test/smoke.sh [WiiFin.wad|WiiFin.dol] [timeout in s]
#        (no display, e.g. CI: xvfb-run -a tools/test/smoke.sh ...)
# env:   DOLPHIN  Dolphin command (default: dolphin-emu-nogui -p headless, else
#                 dolphin-emu -b, else the Flatpak org.DolphinEmu.dolphin-emu)
#        VIDEO    video backend (default Null: nothing drawn, fastest)
T=$(dirname "$(realpath "$0")")
. "$T/lib.sh"
FILE=$(realpath "${1:-WiiFin.wad}")
LIMIT=${2:-90}
[ -f "$FILE" ] || { echo "smoke: $FILE not found"; exit 2; }

# Dolphin's user folder, kept for a look after a failure (not under /tmp:
# the Flatpak build has its own /tmp)
U=$T/out/smoke
dolphin_setup "$U" < /dev/null
dolphin_cmd "$U"

$DOLPHIN -v "${VIDEO:-Null}" -u "$U" -e "$FILE" > "$U/out.txt" 2>&1 &
PID=$!

status=1
why="no \"[WiiFin] ready\" after ${LIMIT} s"
for _ in $(seq 1 "$LIMIT"); do
    sleep 1
    kill -0 $PID 2>/dev/null || { why="Dolphin exited during start-up"; break; }
    if dolphin_log "$U" | grep -q '^\[WiiFin\] ready'; then
        sleep 5
        if kill -0 $PID 2>/dev/null; then status=0; else why="Dolphin exited after start-up"; fi
        break
    fi
done

dolphin_stop $PID
echo "---- WiiFin log ----"
dolphin_log "$U" | head -60
[ $status -ne 0 ] && { echo "---- Dolphin output ----"; tail -30 "$U/out.txt"; }
if [ $status -eq 0 ]; then echo "smoke: OK, WiiFin reached its menu"; else echo "smoke: FAILED, $why"; fi
exit $status
