#!/bin/bash
# The small library of servers.sh, for Plex, Emby and Jellyfin alike; made
# with the encoders any ffmpeg has (openh264, xvid, lame: no libx264 needed)
set -e
mkdir -p "$(dirname "$0")/media" && cd "$(dirname "$0")/media"
FF="ffmpeg -loglevel error -n"
num() { echo "testsrc2=size=$2:rate=24,drawtext=text='$1 %{n}':fontsize=48:fontcolor=white:box=1:boxcolor=black:x=20:y=20"; }
mk() { local out="$1"; shift; mkdir -p "$(dirname "$out")"; [ -f "$out" ] || $FF "$@" "$out"; }
# 720p H.264: transcoded for the Wii
mk "Movies/Counter Film (2019)/Counter Film (2019).mkv" -f lavfi -i "$(num COUNTER 1280x720)" \
   -f lavfi -i "sine=f=330:sample_rate=48000" -t 120 -c:v libopenh264 -b:v 2500k -pix_fmt yuv420p -c:a aac
# SD Xvid + MP3 in AVI: played as it is
mk "Movies/Xvid Film (2015)/Xvid Film (2015).avi" -f lavfi -i "$(num XVID 640x360)" \
   -f lavfi -i "sine=f=440:sample_rate=48000" -t 90 -c:v libxvid -b:v 1000k -c:a libmp3lame -b:a 128k
# SD H.264 in MKV: played as it is
mk "Movies/Small H264 (2012)/Small H264 (2012).mkv" -f lavfi -i "$(num H264 640x360)" \
   -f lavfi -i "sine=f=550:sample_rate=48000" -t 60 -c:v libopenh264 -b:v 1000k -pix_fmt yuv420p -c:a libmp3lame
# A series: 2 seasons of 3 episodes
for s in 1 2; do for e in 1 2 3; do
  mk "TV/Shuffle Show/Season 0$s/Shuffle Show - s0${s}e0$e.avi" -f lavfi -i "$(num S${s}E$e 640x360)" \
     -f lavfi -i "sine=f=$((300+100*e)):sample_rate=48000" -t 20 -c:v libxvid -b:v 800k -c:a libmp3lame -b:a 128k
done; done
# Subtitles: an embedded French track (burned in by Plex) and an English
# sidecar SRT (drawn by WiiFin)
D="Movies/Subtitle Film (2017)"; mkdir -p "$D"
[ -f "$D/Subtitle Film (2017).en.srt" ] || python3 -c "
for i in range(1,30): print('%d\n00:00:%02d,000 --> 00:00:%02d,500\nSubtitle line %d\n' % (i, i*3, i*3+2, i))" > "$D/Subtitle Film (2017).en.srt"
mk "$D/Subtitle Film (2017).mkv" -f lavfi -i "$(num SUB 640x360)" -f lavfi -i "sine=f=440:sample_rate=48000" \
   -i "$D/Subtitle Film (2017).en.srt" -t 90 -map 0 -map 1 -map 2 -c:v libxvid -b:v 900k -c:a libmp3lame \
   -c:s srt -metadata:s:s:0 language=fra
# An album: 4 tagged MP3 tracks and a cover
mkdir -p "Music/Test Artist/Test Album"
[ -f "Music/Test Artist/Test Album/cover.jpg" ] || $FF -f lavfi -i "testsrc2=size=500x500" -frames:v 1 "Music/Test Artist/Test Album/cover.jpg"
for t in 1 2 3 4; do
  mk "Music/Test Artist/Test Album/0$t - Track $t.mp3" -f lavfi -i "sine=f=$((200*t)):sample_rate=44100" -t 15 \
     -c:a libmp3lame -b:a 192k -metadata title="Track $t" -metadata artist="Test Artist" \
     -metadata album_artist="Test Artist" -metadata album="Test Album" -metadata track=$t -metadata date=2020
done
echo media OK; du -sh .
