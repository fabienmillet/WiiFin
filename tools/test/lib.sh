# shellcheck shell=bash
# Sourced by smoke.sh and playback.sh.

# dolphin_setup <user folder>: a fresh Dolphin user folder logging OSREPORT
# (WiiFin's log) to <user folder>/Logs/dolphin.log, with no GameCube pad (an
# emulated one sends phantom START presses), and silent: no sound on the
# speakers of whoever runs the tests (SOUND=1 keeps it).  Dolphin.ini lines
# can follow on stdin.
dolphin_setup() {
    rm -rf "$1"; mkdir -p "$1/Config"
    printf '[Options]\nVerbosity = 3\nWriteToConsole = False\nWriteToFile = True\n[Logs]\nOSREPORT = True\n' \
        > "$1/Config/Logger.ini"
    { printf '[Core]\nSIDevice0 = 0\nWiimoteContinuousScanning = False\n'
      printf '[Analytics]\nEnabled = False\nPermissionAsked = True\n'
      [ "$SOUND" = 1 ] || printf '[DSP]\nBackend = No Audio Output\n'
      [ -t 0 ] || cat; } > "$1/Config/Dolphin.ini"
}

# dolphin_cmd <writable folder>: sets DOLPHIN, unless already set, to
# dolphin-emu-nogui when there is one, else the Qt build (it needs a display:
# without one run under xvfb-run), else the Flatpak, which has only the Qt
# build and sees the host read-only (it may write <writable folder>).
dolphin_cmd() {
    [ -n "$DOLPHIN" ] && return
    if command -v dolphin-emu-nogui >/dev/null; then DOLPHIN="dolphin-emu-nogui -p headless"
    elif command -v dolphin-emu >/dev/null; then DOLPHIN="dolphin-emu -b"
    else DOLPHIN="flatpak run --filesystem=$1 --command=dolphin-emu org.DolphinEmu.dolphin-emu -b"
    fi
}

# dolphin_log <user folder>: WiiFin's lines from the Dolphin log
dolphin_log() {
    sed -n 's/^.*OSREPORT[^:]*\]: //p' "$1/Logs/dolphin.log" 2>/dev/null | tr -d '\r'
}

# dolphin_stop <pid>: ask Dolphin to stop (it finishes its files), then force
dolphin_stop() {
    kill "$1" 2>/dev/null
    for _ in $(seq 1 20); do kill -0 "$1" 2>/dev/null || break; sleep 1; done
    kill -9 "$1" 2>/dev/null; wait "$1" 2>/dev/null
}
