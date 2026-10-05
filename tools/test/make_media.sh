#!/bin/bash
# Generate the test library (ffmpeg only, ~700 MB) into tools/test/media/:
#
#   movies/  Counter Film   720p, frame number drawn on each frame (judder)
#            Grain Film     720p, heavy noise (transcoder under load)
#            Slow Movie     6 min, for throttled-link buffering
#            Subtitle Film  SRT track (burn-in from a start offset)
#            Xvid Film      AVI Xvid + MP3 with B-frames (stream copy)
#   shows/   Shuffle Show   2 seasons x 5 episodes of 20 s (shuffle, auto-next)
#   mixed/   a movie and a show in a library without a type
#   sync/    Sync Test      white flash + 1 kHz beep every 2 s (A/V offset)
#
# usage: tools/test/make_media.sh   (files already there are kept)
cd "$(dirname "$0")" || exit 1
M=$PWD/media
FF="ffmpeg -loglevel error -y"
X264="-c:v libx264 -preset veryfast -pix_fmt yuv420p"

gen() {  # gen <file> <ffmpeg args...>
    local out="$M/$1"; shift
    [ -f "$out" ] && return
    mkdir -p "$(dirname "$out")"
    echo "  $out"
    $FF "$@" "$out" || { rm -f "$out"; exit 1; }
}

gen "movies/Counter Film (2019)/Counter Film (2019).mkv" \
    -f lavfi -i "testsrc2=size=1280x720:rate=24000/1001,drawtext=text='%{n}':fontsize=200:fontcolor=white:box=1:boxcolor=black:x=40:y=40" \
    -f lavfi -i "sine=f=330:sample_rate=48000" -t 150 $X264 -crf 22 -c:a aac
gen "movies/Grain Film (2018)/Grain Film (2018).mkv" \
    -f lavfi -i "testsrc2=size=1280x720:rate=24000/1001,noise=alls=30:allf=t+u" \
    -f lavfi -i "sine=f=330:sample_rate=48000" -t 90 $X264 -crf 20 -c:a aac
gen "movies/Slow Movie (2021)/Slow Movie (2021).mkv" \
    -f lavfi -i "testsrc2=size=640x360:rate=24" \
    -f lavfi -i "sine=f=440:sample_rate=48000" -t 360 $X264 -crf 20 -c:a aac -b:a 128k

SRT=$(mktemp --suffix=.srt)
for i in $(seq 0 59); do
    printf '%d\n00:%02d:%02d,000 --> 00:%02d:%02d,000\nSUBTITLE LINE %d\n\n' \
        $((i + 1)) $((i * 2 / 60)) $((i * 2 % 60)) $(((i * 2 + 2) / 60)) $(((i * 2 + 2) % 60)) $((i + 1))
done > "$SRT"
gen "movies/Subtitle Film (2017)/Subtitle Film (2017).mkv" \
    -f lavfi -i "testsrc2=size=640x360:rate=24" -f lavfi -i "sine=f=440:sample_rate=48000" -i "$SRT" \
    -t 120 -map 0 -map 1 -map 2 $X264 -c:a aac -c:s srt -metadata:s:s:0 language=eng
rm -f "$SRT"

gen "movies/Xvid Film (2005)/Xvid Film (2005).avi" \
    -f lavfi -i "testsrc2=size=512x384:rate=24000/1001,drawtext=text='XVID':fontsize=80:fontcolor=white:box=1:boxcolor=black:x=(w-tw)/2:y=(h-th)/2" \
    -f lavfi -i "sine=f=600:sample_rate=48000" -t 90 \
    -c:v mpeg4 -vtag XVID -b:v 1083k -bf 2 -g 250 -c:a libmp3lame -b:a 128k -ac 2

for s in 1 2; do
    for e in 1 2 3 4 5; do
        gen "shows/Shuffle Show/Season $s/Shuffle Show S0${s}E0${e}.mkv" \
            -f lavfi -i "testsrc2=size=640x360:rate=24,drawtext=text='S0${s}E0${e}':fontsize=120:fontcolor=white:box=1:boxcolor=black:x=(w-tw)/2:y=(h-th)/2" \
            -f lavfi -i "sine=f=$((300 + e * 40)):sample_rate=48000" -t 20 $X264 -c:a aac
    done
done

gen "mixed/Mixed Clip (2016)/Mixed Clip (2016).mkv" \
    -f lavfi -i "testsrc2=size=640x360:rate=24,drawtext=text='MIXED':fontsize=100:fontcolor=white:box=1:boxcolor=black:x=(w-tw)/2:y=(h-th)/2" \
    -f lavfi -i "sine=f=500:sample_rate=48000" -t 60 $X264 -c:a aac
[ -d "$M/mixed/Shuffle Show" ] || cp -r "$M/shows/Shuffle Show" "$M/mixed/"

gen "sync/Sync Test (2020)/Sync Test (2020).mkv" \
    -f lavfi -i "color=c=black:s=640x360:r=24000/1001,drawbox=x=0:y=0:w=640:h=360:color=white:t=fill:enable='lt(mod(t,2),0.04)'" \
    -f lavfi -i "aevalsrc='if(lt(mod(t,2),0.05),0.8*sin(2*PI*1000*t),0)':s=48000" \
    -t 40 $X264 -c:a aac -b:a 128k

echo "media ready in $M"
