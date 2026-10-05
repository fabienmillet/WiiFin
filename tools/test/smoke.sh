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
FILE=$(realpath "${1:-WiiFin.wad}")
LIMIT=${2:-90}
[ -f "$FILE" ] || { echo "smoke: $FILE not found"; exit 2; }

# Dolphin's user folder, kept for a look after a failure (not under /tmp:
# the Flatpak build has its own /tmp)
U=$(dirname "$(realpath "$0")")/out/smoke
rm -rf "$U"; mkdir -p "$U/Config"
printf '[Options]\nVerbosity = 3\nWriteToConsole = False\nWriteToFile = True\n[Logs]\nOSREPORT = True\n' \
    > "$U/Config/Logger.ini"
# no GameCube pad: an emulated one sends phantom START presses
printf '[Core]\nSIDevice0 = 0\nWiimoteContinuousScanning = False\n[Analytics]\nEnabled = False\nPermissionAsked = True\n' \
    > "$U/Config/Dolphin.ini"
LOG="$U/Logs/dolphin.log"

# dolphin-emu-nogui when there is one; else the Qt build, which needs a
# display (none: run this under xvfb-run).  The Flatpak has only the Qt build
# and sees the host read-only: let it write its user folder.
if [ -z "$DOLPHIN" ]; then
    if command -v dolphin-emu-nogui >/dev/null; then DOLPHIN="dolphin-emu-nogui -p headless"
    elif command -v dolphin-emu >/dev/null; then DOLPHIN="dolphin-emu -b"
    else DOLPHIN="flatpak run --filesystem=$U --command=dolphin-emu org.DolphinEmu.dolphin-emu -b"
    fi
fi

$DOLPHIN -v "${VIDEO:-Null}" -u "$U" -e "$FILE" > "$U/out.txt" 2>&1 &
PID=$!

status=1
why="no \"[WiiFin] ready\" after ${LIMIT} s"
for _ in $(seq 1 "$LIMIT"); do
    sleep 1
    kill -0 $PID 2>/dev/null || { why="Dolphin exited during start-up"; break; }
    if grep -q '\[WiiFin\] ready' "$LOG" 2>/dev/null; then
        sleep 5
        if kill -0 $PID 2>/dev/null; then status=0; else why="Dolphin exited after start-up"; fi
        break
    fi
done

kill $PID 2>/dev/null; sleep 2; kill -9 $PID 2>/dev/null; wait $PID 2>/dev/null
echo "---- WiiFin log ----"
sed -n 's/^.*OSREPORT[^:]*\]: //p' "$LOG" 2>/dev/null | head -60
[ $status -ne 0 ] && { echo "---- Dolphin output ----"; tail -30 "$U/out.txt"; }
if [ $status -eq 0 ]; then echo "smoke: OK, WiiFin reached its menu"; else echo "smoke: FAILED, $why"; fi
exit $status
