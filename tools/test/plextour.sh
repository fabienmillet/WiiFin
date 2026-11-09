#!/bin/bash
# tour.sh against a local test server (Plex by default; SERVER=, SESSION=), frames picked from
# Dolphin's PNG dump (Fedora's Dolphin has no AVI dump).
# usage: plextour.sh "<script>" "<seconds...>" <duration>
T=$(dirname "$(realpath "$0")")
mkdir -p "$T/out"
# SESSION="<token> <user id>" for Jellyfin / Emby; none for Plex (a trusted network)
if [ -n "$SESSION" ]; then printf '%s\n%s\n' $SESSION > "$T/out/session"; else printf '\nplex\n' > "$T/out/session"; fi
SERVER=${SERVER:-http://127.0.0.1:32400} xvfb-run -a "$T/tour.sh" "$1" "" "$3" >/dev/null 2>&1
D=$T/out/dolphin/Dump/Frames; O=$T/out/frames; rm -rf "$O"; mkdir -p "$O"
n=$(ls "$D" | wc -l)
for t in $2; do f=$D/framedump_$((t*60)).png; [ -f "$f" ] && cp "$f" "$O/t$t.png"; done
echo "frames: $n dumped; $(ls "$O" | tr '\n' ' ')"
echo "log: $T/out/log.txt ($(wc -l < "$T/out/log.txt") lines)"
