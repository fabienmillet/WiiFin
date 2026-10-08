#!/bin/bash
# Generate the test library (ffmpeg only, ~700 MB) into tools/test/media/:
#
#   movies/  Counter Film   720p, frame number drawn on each frame (judder);
#                           a trailer and a featurette (special features)
#            Grain Film     720p, heavy noise (transcoder under load)
#            Slow Movie     6 min, for throttled-link buffering
#            Subtitle Film  SRT track (burn-in from a start offset); the SRT
#                           tracks here are marked default: the server picks them
#            Xvid Film      AVI Xvid + MP3 with B-frames (stream copy), made by
#                           libxvid: MPlayer CE's decoder misreads some B-frames
#                           of ffmpeg's own mpeg4 encoder ("illegal MB_type")
#            Z1..Z9         direct play, one of each (formats): AVI Xvid + AC3,
#                           MKV H.264 480p + AC3 5.1, MP4 index first, MP4 index
#                           last, MPEG-2 PS, MPEG-2 TS, MKV with two audio
#                           tracks, MKV with a 600 KB subtitle track, a
#                           Japanese title with Japanese subtitles (Z9), and
#                           Korean and Chinese ones (ZA); frame number drawn
#            ZB             two versions of one film (Jellyfin's "Name - label"
#                           files): Widescreen 16:9 with one audio track,
#                           Fullscreen 4:3 with two
#   shows/   Shuffle Show   2 seasons x 5 episodes of 20 s (shuffle, auto-next)
#   mixed/   a movie and a show in a library without a type
#   sync/    Sync Test      white flash + 1 kHz beep every 2 s (A/V offset)
#   music/   Test Album     5 tagged tracks of 12 s, a cover: MP3, FLAC, WAV
#                           (played as they are), FLAC 24-bit 96 kHz and AAC
#                           (transcoded); server.sh's "Singles" library
#   playback/ Counter, Subtitle, Xvid Film and the Z formats (playback.sh), hard links
#
# usage: tools/test/make_media.sh [name...]   (files already there are kept)
#   names: counter grain slow subtitle xvid formats shows mixed sync music playback
#   (default: all)
WANT=" $* "
[[ "$WANT" == *" playback "* ]] && WANT="$WANT counter subtitle xvid formats shows "
cd "$(dirname "$0")" || exit 1
M=$PWD/media
FF="ffmpeg -loglevel error -y"
X264="-c:v libx264 -preset veryfast -pix_fmt yuv420p"

want() { [ "$WANT" = "  " ] || [[ "$WANT" == *" $1 "* ]]; }

gen() {  # gen <file> <ffmpeg args...>
    local out="$M/$1"; shift
    [ -f "$out" ] && return
    mkdir -p "$(dirname "$out")"
    echo "  $out"
    $FF "$@" "$out" || { rm -f "$out"; exit 1; }
}

want counter && gen "movies/Counter Film (2019)/Counter Film (2019).mkv" \
    -f lavfi -i "testsrc2=size=1280x720:rate=24000/1001,drawtext=text='%{n}':fontsize=200:fontcolor=white:box=1:boxcolor=black:x=40:y=40" \
    -f lavfi -i "sine=f=330:sample_rate=48000" -t 150 $X264 -crf 22 -c:a aac
# its special features: a trailer and a featurette, in the folders Jellyfin
# reads extras from
for x in "trailers/Counter Film Trailer" "featurettes/Making of Counter Film"; do
    want counter && gen "movies/Counter Film (2019)/$x.mkv" \
        -f lavfi -i "testsrc2=size=640x360:rate=24,drawtext=text='$(basename "$x")':fontsize=36:fontcolor=white:box=1:boxcolor=black:x=20:y=20" \
        -f lavfi -i "sine=f=550:sample_rate=48000" -t 15 $X264 -c:a aac
done
want grain && gen "movies/Grain Film (2018)/Grain Film (2018).mkv" \
    -f lavfi -i "testsrc2=size=1280x720:rate=24000/1001,noise=alls=30:allf=t+u" \
    -f lavfi -i "sine=f=330:sample_rate=48000" -t 90 $X264 -crf 20 -c:a aac
want slow && gen "movies/Slow Movie (2021)/Slow Movie (2021).mkv" \
    -f lavfi -i "testsrc2=size=640x360:rate=24" \
    -f lavfi -i "sine=f=440:sample_rate=48000" -t 360 $X264 -crf 20 -c:a aac -b:a 128k

if want subtitle; then
    SRT=$(mktemp --suffix=.srt)
    for i in $(seq 0 59); do
        printf '%d\n00:%02d:%02d,000 --> 00:%02d:%02d,000\nSUBTITLE LINE %d\n\n' \
            $((i + 1)) $((i * 2 / 60)) $((i * 2 % 60)) $(((i * 2 + 2) / 60)) $(((i * 2 + 2) % 60)) $((i + 1))
    done > "$SRT"
    gen "movies/Subtitle Film (2017)/Subtitle Film (2017).mkv" \
        -f lavfi -i "testsrc2=size=640x360:rate=24" -f lavfi -i "sine=f=440:sample_rate=48000" -i "$SRT" \
        -t 120 -map 0 -map 1 -map 2 $X264 -c:a aac -c:s srt -metadata:s:s:0 language=eng -disposition:s:0 default
    rm -f "$SRT"
fi

want xvid && gen "movies/Xvid Film (2005)/Xvid Film (2005).avi" \
    -f lavfi -i "testsrc2=size=512x384:rate=24000/1001,drawtext=text='XVID':fontsize=80:fontcolor=white:box=1:boxcolor=black:x=(w-tw)/2:y=(h-th)/2" \
    -f lavfi -i "sine=f=600:sample_rate=48000" -t 90 \
    -c:v libxvid -vtag XVID -b:v 1083k -bf 2 -g 250 -c:a libmp3lame -b:a 128k -ac 2

# Direct play: what the Wii decodes as it is, one file per container.  Named
# to sort after the other films, which the scenarios reach by position.
if want formats; then
    num() {  # num <label> <size>: a test picture with the frame number and the label
        echo "testsrc2=size=$2:rate=24000/1001,drawtext=text='$1 %{n}':fontsize=56:fontcolor=white:box=1:boxcolor=black:x=(w-tw)/2:y=(h-th)/2"
    }
    TONE=(-f lavfi -i "sine=f=440:sample_rate=48000")
    gen "movies/Z1 AVI Xvid AC3 (2006)/Z1 AVI Xvid AC3 (2006).avi" \
        -f lavfi -i "$(num AVI 640x352)" "${TONE[@]}" -t 60 \
        -c:v libxvid -vtag XVID -b:v 1200k -bf 2 -g 250 -c:a ac3 -ac 2 -b:a 192k
    gen "movies/Z2 MKV H264 (2010)/Z2 MKV H264 (2010).mkv" \
        -f lavfi -i "$(num MKV 640x360)" "${TONE[@]}" -t 60 \
        $X264 -profile:v high -level 3.0 -b:v 1200k -c:a ac3 -ac 6 -b:a 384k
    gen "movies/Z3 MP4 Index First (2012)/Z3 MP4 Index First (2012).mp4" \
        -f lavfi -i "$(num MP4 640x360)" "${TONE[@]}" -t 60 \
        $X264 -profile:v main -level 3.0 -b:v 1200k -c:a aac -b:a 128k -movflags +faststart
    gen "movies/Z4 MP4 Index Last (2013)/Z4 MP4 Index Last (2013).mp4" \
        -f lavfi -i "$(num MP4END 640x360)" "${TONE[@]}" -t 60 \
        $X264 -profile:v main -level 3.0 -b:v 1200k -c:a aac -b:a 128k
    gen "movies/Z5 MPEG2 PS (2003)/Z5 MPEG2 PS (2003).mpg" \
        -f lavfi -i "$(num MPEG2 720x480)" "${TONE[@]}" -t 60 \
        -c:v mpeg2video -b:v 3500k -maxrate 5000k -bufsize 1835k -g 15 -c:a mp2 -b:a 192k -f vob
    gen "movies/Z6 MPEG2 TS (2008)/Z6 MPEG2 TS (2008).ts" \
        -f lavfi -i "$(num TS 720x480)" "${TONE[@]}" -t 60 \
        -c:v mpeg2video -b:v 3500k -maxrate 5000k -bufsize 1835k -g 15 -c:a ac3 -ac 2 -b:a 192k
    # two audio tracks (an anime's Japanese and English): the second one is
    # picked in the detail page, MPlayer plays it as it is (-aid)
    gen "movies/Z7 MKV Two Audio (2015)/Z7 MKV Two Audio (2015).mkv" \
        -f lavfi -i "$(num TWOAUDIO 640x360)" "${TONE[@]}" -f lavfi -i "sine=f=880:sample_rate=48000" -t 40 \
        -map 0 -map 1 -map 2 $X264 -profile:v main -level 3.0 -b:v 1000k \
        -c:a:0 aac -b:a:0 128k -metadata:s:a:0 language=jpn -c:a:1 ac3 -ac:a:1 2 -b:a:1 192k -metadata:s:a:1 language=eng
    # a subtitle track bigger than the 256 KB response buffer (an anime's
    # ASS with karaoke and effects, once Jellyfin makes it SRT)
    if want formats && [ ! -f "$M/movies/Z8 MKV Big Subtitles (2016)/Z8 MKV Big Subtitles (2016).mkv" ]; then
        SRT=$(mktemp --suffix=.srt)
        python3 -c '
import sys
pad = "karaoke effect line padding " * 7
for i in range(3000):
    t0, t1 = i * 13, i * 13 + 400   # ms: 3000 cues over 39 s, each shown 0.4 s
    f = lambda ms: "00:00:%02d,%03d" % (ms // 1000, ms % 1000)
    print("%d\n%s --> %s\nBIG SUB %d %s\n" % (i + 1, f(t0), f(t1), i + 1, pad))
' > "$SRT"
        gen "movies/Z8 MKV Big Subtitles (2016)/Z8 MKV Big Subtitles (2016).mkv" \
            -f lavfi -i "$(num BIGSUB 640x360)" "${TONE[@]}" -i "$SRT" -t 40 -map 0 -map 1 -map 2 \
            $X264 -profile:v main -level 3.0 -b:v 1000k -c:a aac -b:a 128k -c:s srt -metadata:s:s:0 language=eng -disposition:s:0 default
        rm -f "$SRT"
    fi
    # Japanese: the title, and a subtitle track of kana, kanji, full-width
    # punctuation and Latin (Text's Japanese fallback font)
    JP="Z9 あずまんが大王 THE ANIMATION (2002)"
    if want formats && [ ! -f "$M/movies/$JP/$JP.mkv" ]; then
        SRT=$(mktemp --suffix=.srt)
        cat > "$SRT" <<'SRTEOF'
1
00:00:01,000 --> 00:00:15,000
大阪さん、おはよう！今日は学校で何をするの？
ちよちゃんは天才だから、もう全部わかってるよね〜

2
00:00:15,000 --> 00:00:30,000
「よみ」と「とも」は THE ANIMATION の第１話から一緒です。
SRTEOF
        gen "movies/$JP/$JP.mkv" \
            -f lavfi -i "$(num JAPANESE 640x360)" "${TONE[@]}" -i "$SRT" -t 30 -map 0 -map 1 -map 2 \
            $X264 -profile:v main -level 3.0 -b:v 1000k -c:a aac -b:a 128k -c:s srt -metadata:s:s:0 language=jpn -disposition:s:0 default
        rm -f "$SRT"
    fi
    # Korean and Chinese: the title, and subtitles in both (the SD card's
    # font, data/fonts/cjk: the built-in ones have neither)
    KO="ZA 서울의 봄 北京故事 (2020)"
    if want formats && [ ! -f "$M/movies/$KO/$KO.mkv" ]; then
        SRT=$(mktemp --suffix=.srt)
        cat > "$SRT" <<'SRTEOF'
1
00:00:01,000 --> 00:00:15,000
안녕하세요! 오늘 서울의 날씨는 정말 좋네요.
지하철을 타고 한강 공원에 갈까요?

2
00:00:15,000 --> 00:00:30,000
我们明天去北京看长城，好吗？
繁體字也可以：臺灣、廣東話、學習。
SRTEOF
        gen "movies/$KO/$KO.mkv" \
            -f lavfi -i "$(num CJK 640x360)" "${TONE[@]}" -i "$SRT" -t 30 -map 0 -map 1 -map 2 \
            $X264 -profile:v main -level 3.0 -b:v 1000k -c:a aac -b:a 128k -c:s srt -metadata:s:s:0 language=kor -disposition:s:0 default
        rm -f "$SRT"
    fi
    # its title kept: online, Jellyfin took it for another film (renamed,
    # at the top of the list); lockdata keeps the server off it
    want formats && [ ! -f "$M/movies/$KO/movie.nfo" ] &&
        printf '<?xml version="1.0" encoding="utf-8"?>\n<movie>\n  <title>%s</title>\n  <year>2020</year>\n  <lockdata>true</lockdata>\n</movie>\n' \
            "${KO% (2020)}" > "$M/movies/$KO/movie.nfo"
fi

# two versions of a film: the files "<folder> - <label>" of one folder
ZB="ZB Two Versions (2019)"
if want formats; then
    gen "movies/$ZB/$ZB - Widescreen.mkv" \
        -f lavfi -i "testsrc2=size=640x360:rate=24,drawtext=text='WIDESCREEN %{n}':fontsize=40:fontcolor=white:box=1:boxcolor=black:x=20:y=20" \
        -f lavfi -i "sine=f=400:sample_rate=48000" -t 30 $X264 -c:a aac -metadata:s:a:0 language=eng
    gen "movies/$ZB/$ZB - Fullscreen.mkv" \
        -f lavfi -i "testsrc2=size=480x360:rate=24,drawtext=text='FULLSCREEN %{n}':fontsize=36:fontcolor=white:box=1:boxcolor=black:x=20:y=20" \
        -f lavfi -i "sine=f=400:sample_rate=48000" -f lavfi -i "sine=f=700:sample_rate=48000" -t 30 \
        -map 0 -map 1 -map 2 $X264 -c:a aac -metadata:s:a:0 language=eng -metadata:s:a:1 language=fre
    [ -f "$M/movies/$ZB/movie.nfo" ] ||
        printf '<?xml version="1.0" encoding="utf-8"?>\n<movie>\n  <title>ZB Two Versions</title>\n  <year>2019</year>\n  <lockdata>true</lockdata>\n</movie>\n' \
            > "$M/movies/$ZB/movie.nfo"
fi

want shows && for s in 1 2; do
    for e in 1 2 3 4 5; do
        gen "shows/Shuffle Show/Season $s/Shuffle Show S0${s}E0${e}.mkv" \
            -f lavfi -i "testsrc2=size=640x360:rate=24,drawtext=text='S0${s}E0${e}':fontsize=120:fontcolor=white:box=1:boxcolor=black:x=(w-tw)/2:y=(h-th)/2" \
            -f lavfi -i "sine=f=$((300 + e * 40)):sample_rate=48000" -t 20 $X264 -c:a aac
    done
done

want mixed && gen "mixed/Mixed Clip (2016)/Mixed Clip (2016).mkv" \
    -f lavfi -i "testsrc2=size=640x360:rate=24,drawtext=text='MIXED':fontsize=100:fontcolor=white:box=1:boxcolor=black:x=(w-tw)/2:y=(h-th)/2" \
    -f lavfi -i "sine=f=500:sample_rate=48000" -t 60 $X264 -c:a aac
if want mixed && want shows && [ ! -d "$M/mixed/Shuffle Show" ]; then
    cp -r "$M/shows/Shuffle Show" "$M/mixed/"
fi

want sync && gen "sync/Sync Test (2020)/Sync Test (2020).mkv" \
    -f lavfi -i "color=c=black:s=640x360:r=24000/1001,drawbox=x=0:y=0:w=640:h=360:color=white:t=fill:enable='lt(mod(t,2),0.04)'" \
    -f lavfi -i "aevalsrc='if(lt(mod(t,2),0.05),0.8*sin(2*PI*1000*t),0)':s=48000" \
    -t 40 $X264 -c:a aac -b:a 128k

if want music; then
    COVER=$(mktemp --suffix=.png)
    $FF -f lavfi -i "testsrc2=size=320x320,drawtext=text='WIIFIN':fontsize=60:fontcolor=white:box=1:boxcolor=black:x=(w-tw)/2:y=(h-th)/2" \
        -frames:v 1 "$COVER"
    A="music/WiiFin Test/Test Album"
    tags() {  # tags <track number> <title>: the tags, in TAGS
        TAGS=(-metadata "title=$2" -metadata "artist=WiiFin Test" -metadata "album_artist=WiiFin Test"
              -metadata "album=Test Album" -metadata "track=$1" -metadata "date=2024" -metadata "genre=Electronic")
    }
    # tone <Hz> <rate>: 12 s of it (-t before -i: of this input, not the next)
    tone() { TONE_IN=(-t 12 -f lavfi -i "sine=f=$1:sample_rate=$2"); }
    tone 440 44100; tags 1 "MP3 Track"
    gen "$A/01 MP3 Track.mp3" "${TONE_IN[@]}" -i "$COVER" -map 0 -map 1 \
        -c:a libmp3lame -b:a 320k -c:v mjpeg -disposition:v attached_pic -id3v2_version 3 "${TAGS[@]}"
    tone 520 44100; tags 2 "FLAC Track"
    gen "$A/02 FLAC Track.flac" "${TONE_IN[@]}" -c:a flac -sample_fmt s16 "${TAGS[@]}"
    tone 600 48000; tags 3 "WAV Track"
    gen "$A/03 WAV Track.wav" "${TONE_IN[@]}" -c:a pcm_s16le "${TAGS[@]}"
    tone 680 96000; tags 4 "Hi-Res FLAC Track"
    gen "$A/04 Hi-Res FLAC Track.flac" "${TONE_IN[@]}" -c:a flac -sample_fmt s32 -bits_per_raw_sample 24 "${TAGS[@]}"
    tone 760 44100; tags 5 "AAC Track"
    gen "$A/05 AAC Track.m4a" "${TONE_IN[@]}" -c:a aac -b:a 192k "${TAGS[@]}"
    rm -f "$COVER"
fi

if want playback; then
    mkdir -p "$M/playback"
    for d in "Counter Film (2019)" "Subtitle Film (2017)" "Xvid Film (2005)" \
             "Z1 AVI Xvid AC3 (2006)" "Z2 MKV H264 (2010)" "Z3 MP4 Index First (2012)" \
             "Z4 MP4 Index Last (2013)" "Z5 MPEG2 PS (2003)" "Z6 MPEG2 TS (2008)" \
             "Z7 MKV Two Audio (2015)" "Z8 MKV Big Subtitles (2016)" \
             "Z9 あずまんが大王 THE ANIMATION (2002)" "ZA 서울의 봄 北京故事 (2020)" \
             "ZB Two Versions (2019)"; do
        # what playback/ lacks (a file deleted to make it again, extras
        # added since) linked in; what it has stays
        cp -al -n "$M/movies/$d" "$M/playback/"
    done
fi

echo "media ready in $M"
