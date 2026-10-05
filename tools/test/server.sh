#!/bin/bash
# Test Jellyfin server in Docker, on loopback only:
#   http://127.0.0.1:18096  Jellyfin itself
#   http://127.0.0.1:18080  through nginx, /videos/ and /Playback/ throttled
#                           (a weak Wi-Fi link; /Playback/BitrateTest included)
# The server is set up from scratch (user wii / wii) with the libraries of
# tools/test/media (make_media.sh): Movies, Shows and Mixed; or, with
# ONLY=<folder>, that folder alone as the Movies library (e.g. ONLY=sync).
# The session (token, user id) goes to out/session for tour.sh.
#
# usage: tools/test/server.sh [rate]    nginx limit_rate, default 180k
#        tools/test/server.sh stop
cd "$(dirname "$0")" || exit 1
T=$PWD; M=$T/media
if [ "$1" = stop ]; then
    docker stop wiifin-jf-test wiifin-nginx-test >/dev/null 2>&1
    docker network rm wiifin-test-net >/dev/null 2>&1
    exit 0
fi
RATE=${1:-180k}
MOVIES=$M/${ONLY:-movies}
[ -d "$MOVIES" ] || { echo "$MOVIES missing: run tools/test/make_media.sh"; exit 1; }
mkdir -p out

cat > out/nginx.conf <<EOF
events {}
http {
  server {
    listen 8080;
    keepalive_timeout 75s;
    location /videos/   { proxy_pass http://wiifin-jf-test:8096; proxy_http_version 1.1; proxy_set_header Host \$host; proxy_buffering on; limit_rate $RATE; }
    location /Playback/ { proxy_pass http://wiifin-jf-test:8096; proxy_http_version 1.1; proxy_set_header Host \$host; proxy_buffering on; limit_rate $RATE; }
    location /          { proxy_pass http://wiifin-jf-test:8096; proxy_http_version 1.1; proxy_set_header Host \$host; proxy_buffering off; }
  }
}
EOF

VOLS=(-v "$MOVIES:/movies:ro")
[ -z "$ONLY" ] && VOLS+=(-v "$M/shows:/tv:ro" -v "$M/mixed:/mixed:ro")
docker rm -f wiifin-jf-test wiifin-nginx-test >/dev/null 2>&1
docker network create wiifin-test-net >/dev/null 2>&1
docker run -d --rm --name wiifin-jf-test --network wiifin-test-net -p 127.0.0.1:18096:8096 \
    "${VOLS[@]}" jellyfin/jellyfin:latest >/dev/null || exit 1
docker run -d --rm --name wiifin-nginx-test --network wiifin-test-net -p 127.0.0.1:18080:8080 \
    -v "$T/out/nginx.conf:/etc/nginx/nginx.conf:ro" nginx:alpine >/dev/null || exit 1

J=http://127.0.0.1:18096
H='Content-Type: application/json'
C='MediaBrowser Client="WiiFinTest", Device="test", DeviceId="wiifin-test", Version="1"'
for _ in $(seq 1 90); do
    [ "$(curl -s -o /dev/null -w '%{http_code}' -m 2 $J/Startup/Configuration)" = 200 ] && break; sleep 2
done
curl -s -o /dev/null -X POST $J/Startup/Configuration -H "$H" \
    -d '{"UICulture":"en-US","MetadataCountryCode":"US","PreferredMetadataLanguage":"en"}'
curl -s -o /dev/null $J/Startup/User
curl -s -o /dev/null -X POST $J/Startup/User -H "$H" -d '{"Name":"wii","Password":"wii"}'
curl -s -o /dev/null -X POST $J/Startup/Complete
R=$(curl -s -X POST $J/Users/AuthenticateByName -H "$H" -H "Authorization: $C" -d '{"Username":"wii","Pw":"wii"}')
TOKEN=$(echo "$R" | grep -o '"AccessToken":"[^"]*"' | cut -d'"' -f4)
USERID=$(echo "$R" | grep -oE '"User":\{"Name":"wii","ServerId":"[^"]*","Id":"[^"]*"' | grep -oE '"Id":"[^"]*"' | cut -d'"' -f4)
[ -n "$TOKEN" ] || { echo "Jellyfin setup failed"; exit 1; }
A="Authorization: $C, Token=\"$TOKEN\""

count() {  # count <ItemType>
    curl -s "$J/Items?Recursive=true&IncludeItemTypes=$1&userId=$USERID" -H "$A" \
        | grep -o '"TotalRecordCount":[0-9]*' | cut -d: -f2
}
library() {  # library <name> <path> <collection type> <ItemType> <expected count>
    local n=0
    for _ in 1 2 3; do
        # the first scan sometimes misses files: add again / refresh until it sees them all
        curl -s -o /dev/null -X POST "$J/Library/VirtualFolders?name=$1&paths=%2F$2&refreshLibrary=true&collectionType=$3" \
            -H "$A" -H "$H" -d '{"LibraryOptions":{}}'
        sleep 5
        for _ in $(seq 1 30); do n=$(count "$4"); [ "${n:-0}" -ge "$5" ] && break; sleep 3; done
        [ "${n:-0}" -ge "$5" ] && break
        curl -s -o /dev/null -X POST "$J/Library/Refresh" -H "$A"
    done
    echo "$1: ${n:-0} $4"
}
library Movies movies movies Movie "$(ls "$MOVIES" | wc -l)"
if [ -z "$ONLY" ]; then
    library Shows tv tvshows Episode 10
    # a library without a type: nothing simple to count, give it time
    curl -s -o /dev/null -X POST "$J/Library/VirtualFolders?name=Mixed&paths=%2Fmixed&refreshLibrary=true" \
        -H "$A" -H "$H" -d '{"LibraryOptions":{}}'
    sleep 8
fi

printf '%s\n%s\n' "$TOKEN" "$USERID" > out/session
echo "ready: $J (direct), http://127.0.0.1:18080 (limited to $RATE/s), user wii / wii"
