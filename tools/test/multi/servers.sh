#!/bin/bash
# Plex, Emby and Jellyfin test servers (podman, or docker), on the library of
# make_media.sh, each set up from scratch (user wii / wii):
#   Plex      http://127.0.0.1:32400  no account: the local networks are let in
#   Emby      http://127.0.0.1:8096
#   Jellyfin  http://127.0.0.1:18097
# The sessions (token and user id) go to out/emby-session, out/jf-session for
# ../plextour.sh:  SERVER=http://127.0.0.1:8096 SESSION="$(cat out/emby-session)"
# usage: servers.sh [stop]
cd "$(dirname "$0")" || exit 1
D=$PWD; O=$D/out; mkdir -p "$O"
C=$(command -v podman || command -v docker)
if [ "$1" = stop ]; then $C rm -f wf-plex wf-emby wf-jellyfin >/dev/null 2>&1; exit 0; fi
[ -d media/Movies ] || ./make_media.sh || exit 1
# a label every container may read (SELinux)
command -v chcon >/dev/null && chcon -R -l s0 -t container_file_t media 2>/dev/null
mkdir -p "$O/plex" "$O/emby" "$O/jf/config" "$O/jf/cache"
$C rm -f wf-plex wf-emby wf-jellyfin >/dev/null 2>&1
$C run -d --name wf-plex --network host -e ALLOWED_NETWORKS=127.0.0.0/8,192.168.0.0/16,10.0.0.0/8,172.16.0.0/12 \
    -v "$O/plex:/config:Z" -v "$D/media:/data:ro" docker.io/plexinc/pms-docker:latest >/dev/null
$C run -d --name wf-emby --network host -v "$O/emby:/config:Z" -v "$D/media:/data:ro" \
    docker.io/emby/embyserver:latest >/dev/null
$C run -d --name wf-jellyfin -p 18097:8096 -v "$O/jf/config:/config:Z" -v "$O/jf/cache:/cache:Z" \
    -v "$D/media:/data:ro" docker.io/jellyfin/jellyfin:12.1.20260915-010956 >/dev/null
wait_for() { for _ in $(seq 120); do curl -s "$1" | grep -q "$2" && return 0; sleep 2; done; echo "$1: no answer"; return 1; }

# Plex
P=http://127.0.0.1:32400
wait_for $P/identity machineIdentifier || exit 1
sleep 5
plex_add() { curl -s -o /dev/null -X POST -G "$P/library/sections" --data-urlencode "name=$1" --data-urlencode "type=$2" \
    --data-urlencode "agent=$3" --data-urlencode "scanner=$4" --data-urlencode "language=en-US" --data-urlencode "location=$5"; }
plex_add Films movie tv.plex.agents.movie "Plex Movie" /data/Movies
plex_add Series show tv.plex.agents.series "Plex TV Series" /data/TV
plex_add Music artist tv.plex.agents.music "Plex Music" /data/Music
echo "Plex:     $P"

# Emby and Jellyfin: the same start-up wizard
mb_setup() {   # <url> <header name> <session file>
    local U=$1 A="$2: MediaBrowser Client=\"setup\", Device=\"pc\", DeviceId=\"setup1\", Version=\"1\""
    wait_for "$U/System/Info/Public" Version || return 1
    curl -s -o /dev/null -X POST -H "$A" -H 'Content-Type: application/json' "$U/Startup/Configuration" \
        -d '{"UICulture":"en-US","MetadataCountryCode":"US","PreferredMetadataLanguage":"en"}'
    curl -s -o /dev/null -H "$A" "$U/Startup/User"
    curl -s -o /dev/null -X POST -H "$A" -H 'Content-Type: application/json' "$U/Startup/User" -d '{"Name":"wii","Password":"wii"}'
    curl -s -o /dev/null -X POST -H "$A" "$U/Startup/Complete"
    curl -s -X POST -H "$A" -H 'Content-Type: application/json' "$U/Users/AuthenticateByName" -d '{"Username":"wii","Pw":"wii"}' |
        python3 -c 'import json,sys; d=json.load(sys.stdin); print(d["AccessToken"], d["User"]["Id"])' > "$3"
    local T; read -r T _ < "$3"
    for l in "Films movies /data/Movies" "Series tvshows /data/TV" "Music music /data/Music"; do
        set -- $l
        curl -s -o /dev/null -X POST -H "$A, Token=\"$T\"" -H 'Content-Type: application/json' -G "$U/Library/VirtualFolders" \
            --data-urlencode "name=$1" --data-urlencode "collectionType=$2" --data-urlencode "paths=$3" \
            --data-urlencode "refreshLibrary=true" -d '{"LibraryOptions":{}}'
    done
}
mb_setup http://127.0.0.1:8096 X-Emby-Authorization "$O/emby-session" && echo "Emby:     http://127.0.0.1:8096 ($(cat "$O/emby-session"))"
mb_setup http://127.0.0.1:18097 Authorization "$O/jf-session" && echo "Jellyfin: http://127.0.0.1:18097 ($(cat "$O/jf-session"))"
echo "the libraries are being scanned (a minute)"
