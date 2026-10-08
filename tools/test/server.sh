#!/bin/bash
# Test Jellyfin server in Docker, on loopback only:
#   http://127.0.0.1:18096   Jellyfin itself
#   http://127.0.0.1:18080   through nginx, /videos/ and /Playback/ throttled
#                            (a weak Wi-Fi link; /Playback/BitrateTest included)
#   https://127.0.0.1:18443  through nginx with a self-signed certificate
#   https://127.0.0.1:18444  the same with an ECDSA P-384 key signed in SHA-384
#                            (like Let's Encrypt's ECDSA certificates)
#   http://127.0.0.1:18081   through nginx, every item with the same media
#                            segments (intro 2-9 s, credits 12-20 s: Shuffle
#                            Show's episodes, keyframes at 0 and 10.4 s); the
#                            server has no way to add some of its own
# The server is set up from scratch (user wii / wii) with libraries made of
# folders of tools/test/media (make_media.sh):
#   MOVIES  folder of the Movies library (default movies)
#   SHOWS   folder of the Shows library (default shows; empty: none)
#   MIXED   folder of the Mixed library, no type (default mixed; empty: none)
#   media/music (make_media.sh music: Test Album), when there, is the
#           "Singles" library, right after "Shows" (the music scenarios)
#   MUSIC   any other folder of audio files (your own music), as the "Songs"
#           library, after "Singles" (default: none)
#   ONLY=<folder>  that folder alone as the Movies library (e.g. ONLY=sync)
#   KEEP=<folder>  Jellyfin's configuration and cache kept there: accounts,
#           tokens and scans survive a restart, and the containers come back
#           by themselves after a reboot (until server.sh stop).  Without
#           it, each start is a new server.
# Every port lets the WebSocket of the remote control through (Upgrade), as
# a reverse proxy in front of Jellyfin must.
# The session (token, user id) goes to out/session for tour.sh and
# playback.sh.
#
# The images are pinned so a new release cannot change the tests unseen;
# JELLYFIN_IMAGE / NGINX_IMAGE override them (the CI's weekly run uses
# jellyfin/jellyfin:latest).
#
# usage: tools/test/server.sh [rate]    nginx limit_rate, default 180k
#        tools/test/server.sh stop
JELLYFIN_IMAGE=${JELLYFIN_IMAGE:-jellyfin/jellyfin:12.1.20260915-010956}
NGINX_IMAGE=${NGINX_IMAGE:-nginx:1.30.5-alpine}
[ -n "$MUSIC" ] && MUSIC=$(realpath "$MUSIC")   # from where they were given
[ -n "$KEEP" ] && { mkdir -p "$KEEP/config" "$KEEP/cache" && KEEP=$(realpath "$KEEP"); }
cd "$(dirname "$0")" || exit 1
T=$PWD; M=$T/media
if [ "$1" = stop ]; then
    docker stop wiifin-jf-test wiifin-nginx-test >/dev/null 2>&1
    docker network rm wiifin-test-net >/dev/null 2>&1
    exit 0
fi
RATE=${1:-180k}
if [ -n "$ONLY" ]; then MOVIES=$ONLY; SHOWS=; MIXED=
else MOVIES=${MOVIES-movies}; SHOWS=${SHOWS-shows}; MIXED=${MIXED-mixed}
fi
for d in $MOVIES $SHOWS $MIXED; do
    [ -d "$M/$d" ] || { echo "$M/$d missing: run tools/test/make_media.sh"; exit 1; }
done
mkdir -p out/tls
[ -f out/tls/cert.pem ] || openssl req -x509 -newkey rsa:2048 -nodes -days 3650 -subj /CN=localhost \
    -keyout out/tls/key.pem -out out/tls/cert.pem 2>/dev/null || { echo "openssl failed"; exit 1; }
[ -f out/tls/ec384-cert.pem ] || openssl req -x509 -newkey ec -pkeyopt ec_paramgen_curve:secp384r1 -sha384 \
    -nodes -days 3650 -subj /CN=localhost -keyout out/tls/ec384-key.pem -out out/tls/ec384-cert.pem 2>/dev/null \
    || { echo "openssl failed"; exit 1; }
chmod 644 out/tls/*.pem

cat > out/nginx.conf <<EOF
events {}
http {
  server {
    listen 8080;
    keepalive_timeout 75s;
    location /videos/   { proxy_pass http://wiifin-jf-test:8096; proxy_http_version 1.1; proxy_set_header Host \$host; proxy_buffering on; limit_rate $RATE; }
    location /Playback/ { proxy_pass http://wiifin-jf-test:8096; proxy_http_version 1.1; proxy_set_header Host \$host; proxy_buffering on; limit_rate $RATE; }
    location /          { proxy_pass http://wiifin-jf-test:8096; proxy_http_version 1.1; proxy_set_header Host \$host; proxy_buffering off;
                          proxy_set_header Upgrade \$http_upgrade; proxy_set_header Connection \$http_connection; }
  }
  server {
    listen 8081;
    keepalive_timeout 75s;
    location /MediaSegments/ {
      default_type application/json;
      return 200 '{"Items":[{"Type":"Intro","StartTicks":20000000,"EndTicks":90000000},{"Type":"Outro","StartTicks":120000000,"EndTicks":200000000}],"TotalRecordCount":2,"StartIndex":0}';
    }
    location / { proxy_pass http://wiifin-jf-test:8096; proxy_http_version 1.1; proxy_set_header Host \$host; proxy_buffering off;
                 proxy_set_header Upgrade \$http_upgrade; proxy_set_header Connection \$http_connection; }
  }
  server {
    listen 8443 ssl;
    ssl_certificate     /etc/nginx/tls/cert.pem;
    ssl_certificate_key /etc/nginx/tls/key.pem;
    ssl_protocols TLSv1.2 TLSv1.3;
    keepalive_timeout 75s;
    location / { proxy_pass http://wiifin-jf-test:8096; proxy_http_version 1.1; proxy_set_header Host \$host; proxy_buffering off;
                 proxy_set_header Upgrade \$http_upgrade; proxy_set_header Connection \$http_connection; }
  }
  server {
    listen 8444 ssl;
    ssl_certificate     /etc/nginx/tls/ec384-cert.pem;
    ssl_certificate_key /etc/nginx/tls/ec384-key.pem;
    ssl_protocols TLSv1.2 TLSv1.3;
    keepalive_timeout 75s;
    location / { proxy_pass http://wiifin-jf-test:8096; proxy_http_version 1.1; proxy_set_header Host \$host; proxy_buffering off;
                 proxy_set_header Upgrade \$http_upgrade; proxy_set_header Connection \$http_connection; }
  }
}
EOF

VOLS=(-v "$M/$MOVIES:/movies:ro")
[ -n "$SHOWS" ] && VOLS+=(-v "$M/$SHOWS:/tv:ro")
[ -n "$MIXED" ] && VOLS+=(-v "$M/$MIXED:/mixed:ro")
if [ -n "$MUSIC" ]; then
    [ -d "$MUSIC" ] || { echo "$MUSIC missing"; exit 1; }
    VOLS+=(-v "$MUSIC:/music:ro")
fi
[ -d "$M/music" ] && VOLS+=(-v "$M/music:/singles:ro")
RUN=(--rm); JF_USER=()
if [ -n "$KEEP" ]; then
    VOLS+=(-v "$KEEP/config:/config" -v "$KEEP/cache:/cache")
    RUN=(--restart unless-stopped)
    JF_USER=(--user "$(id -u):$(id -g)")   # the kept files stay the user's
fi
docker rm -f wiifin-jf-test wiifin-nginx-test >/dev/null 2>&1
docker network create wiifin-test-net >/dev/null 2>&1
docker run -d "${RUN[@]}" "${JF_USER[@]}" --name wiifin-jf-test --network wiifin-test-net -p 127.0.0.1:18096:8096 \
    "${VOLS[@]}" "$JELLYFIN_IMAGE" >/dev/null || exit 1
docker run -d "${RUN[@]}" --name wiifin-nginx-test --network wiifin-test-net \
    -p 127.0.0.1:18080:8080 -p 127.0.0.1:18081:8081 -p 127.0.0.1:18443:8443 -p 127.0.0.1:18444:8444 \
    -v "$T/out/nginx.conf:/etc/nginx/nginx.conf:ro" -v "$T/out/tls:/etc/nginx/tls:ro" \
    "$NGINX_IMAGE" >/dev/null || exit 1

J=http://127.0.0.1:18096
H='Content-Type: application/json'
C='MediaBrowser Client="WiiFinTest", Device="test", DeviceId="wiifin-test", Version="1"'
for _ in $(seq 1 90); do
    [ "$(curl -s -o /dev/null -w '%{http_code}' -m 2 $J/System/Info/Public)" = 200 ] && break; sleep 2
done
# the first start only (a KEEP folder has it done).  /System/Info/Public
# answers while the rest still says 503: the wizard's own page is awaited.
if ! curl -s -m 5 $J/System/Info/Public | grep -q '"StartupWizardCompleted":true'; then
    for _ in $(seq 1 90); do
        [ "$(curl -s -o /dev/null -w '%{http_code}' -m 2 $J/Startup/Configuration)" = 200 ] && break; sleep 2
    done
    curl -s -o /dev/null -X POST $J/Startup/Configuration -H "$H" \
        -d '{"UICulture":"en-US","MetadataCountryCode":"US","PreferredMetadataLanguage":"en"}'
    curl -s -o /dev/null $J/Startup/User
    curl -s -o /dev/null -X POST $J/Startup/User -H "$H" -d '{"Name":"wii","Password":"wii"}'
    curl -s -o /dev/null -X POST $J/Startup/Complete
fi
for _ in $(seq 1 30); do   # until the server is up (503 while it starts)
    R=$(curl -s -X POST $J/Users/AuthenticateByName -H "$H" -H "Authorization: $C" -d '{"Username":"wii","Pw":"wii"}')
    TOKEN=$(echo "$R" | grep -o '"AccessToken":"[^"]*"' | cut -d'"' -f4)
    [ -n "$TOKEN" ] && break
    sleep 2
done
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
        # the first scan sometimes misses files: refresh until it sees them
        # all (adding the library again would make a second one, "Movies2")
        curl -s "$J/Library/VirtualFolders" -H "$A" | grep -q "\"Name\":\"$1\"" ||
            curl -s -o /dev/null -X POST "$J/Library/VirtualFolders?name=$1&paths=%2F$2&refreshLibrary=true&collectionType=$3" \
                -H "$A" -H "$H" -d '{"LibraryOptions":{"EnableTrickplayImageExtraction":true}}'
        sleep 5
        for _ in $(seq 1 30); do n=$(count "$4"); [ "${n:-0}" -ge "$5" ] && break; sleep 3; done
        [ "${n:-0}" -ge "$5" ] && break
        curl -s -o /dev/null -X POST "$J/Library/Refresh" -H "$A"
    done
    echo "$1: ${n:-0} $4"
}
library Movies movies movies Movie "$(ls "$M/$MOVIES" | wc -l)"
[ -n "$SHOWS" ] && library Shows tv tvshows Episode "$(find "$M/$SHOWS" -name '*.mkv' | wc -l)"
# Test Album: every track; a big music folder takes minutes to scan, ready
# once a first track is in
if [ -d "$M/music" ]; then
    n=$(count Audio)
    library Singles singles music Audio $(( ${n:-0} + $(find "$M/music" -type f ! -name '*.jpg' ! -name '*.png' | wc -l) ))
fi
[ -n "$MUSIC" ] && library Songs music music Audio 1
# the libraries in the order the scenarios count them (RIGHT on the home
# tiles), whatever the order they were added in
J="$J" A="$A" USERID="$USERID" python3 - <<'PYEOF1'
import json, os, urllib.request
J, auth, uid = os.environ['J'], os.environ['A'].split(': ', 1)[1], os.environ['USERID']
def call(path, body=None):
    q = urllib.request.Request(J + path, data=json.dumps(body).encode() if body is not None else None,
                               headers={'Authorization': auth, 'Content-Type': 'application/json'},
                               method='POST' if body is not None else 'GET')
    d = urllib.request.urlopen(q).read()
    return json.loads(d) if d else None
views = {v['Name']: v['Id'] for v in call('/UserViews?userId=' + uid)['Items']}
order = [views[n] for n in ('Movies', 'Shows', 'Singles', 'Songs', 'Mixed') if n in views]
conf = call('/Users/' + uid)['Configuration']
conf['OrderedViews'] = order + [i for i in views.values() if i not in order]
call('/Users/%s/Configuration' % uid, conf)
print('libraries:', ', '.join(n for n in ('Movies', 'Shows', 'Singles', 'Songs', 'Mixed') if n in views))
PYEOF1
if [ -n "$MIXED" ]; then
    # a library without a type: nothing simple to count, give it time
    curl -s -o /dev/null -X POST "$J/Library/VirtualFolders?name=Mixed&paths=%2Fmixed&refreshLibrary=true" \
        -H "$A" -H "$H" -d '{"LibraryOptions":{}}'
    sleep 8
fi

# trickplay thumbnails (shown while seeking): Jellyfin's scheduled task,
# waited for
J="$J" A="$A" python3 - <<'PYEOF2'
import json, os, time, urllib.request
J, auth = os.environ['J'], os.environ['A'].split(': ', 1)[1]
def call(path, method='GET'):
    q = urllib.request.Request(J + path, method=method, headers={'Authorization': auth})
    d = urllib.request.urlopen(q).read()
    return json.loads(d) if d else None
task = next(t for t in call('/ScheduledTasks') if t['Key'] == 'RefreshTrickplayImages')
call('/ScheduledTasks/Running/' + task['Id'], 'POST')
for _ in range(200):
    time.sleep(1)
    t = next(t for t in call('/ScheduledTasks') if t['Id'] == task['Id'])
    if t['State'] == 'Idle' and t.get('LastExecutionResult'):
        break
print('trickplay:', t.get('LastExecutionResult', {}).get('Status', 'not finished'))
PYEOF2

printf '%s\n%s\n' "$TOKEN" "$USERID" > out/session
echo "ready: $J (direct), http://127.0.0.1:18080 (limited to $RATE/s), http://127.0.0.1:18081 (media segments), https://127.0.0.1:18443, user wii / wii"
