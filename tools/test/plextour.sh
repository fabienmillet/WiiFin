#!/bin/bash
# WiiPlex: tour.sh against the local Plex test server, frames picked from
# Dolphin's PNG dump (Fedora's Dolphin has no AVI dump).
# usage: plextour.sh "<script>" "<seconds...>" <duration>
T=$(dirname "$(realpath "$0")")
mkdir -p "$T/out"; printf '\nplex\n' > "$T/out/session"
SERVER=${SERVER:-http://127.0.0.1:32400} xvfb-run -a "$T/tour.sh" "$1" "" "$3" >/dev/null 2>&1
D=$T/out/dolphin/Dump/Frames; O=$T/out/frames; rm -rf "$O"; mkdir -p "$O"
n=$(ls "$D" | wc -l)
for t in $2; do f=$D/framedump_$((t*60)).png; [ -f "$f" ] && cp "$f" "$O/t$t.png"; done
echo "frames: $n dumped; $(ls "$O" | tr '\n' ' ')"
echo "log: $T/out/log.txt ($(wc -l < "$T/out/log.txt") lines)"
