#!/bin/bash
# Build libmplayer.a and libfribidi.a (libs/mplayer-ce-build) from source:
# MPlayer CE at a fixed commit, configured by configure.py and patched by
# wiifin.patch.  See MPLAYER_CE_BUILD.md.
#
# usage: tools/mplayer/build.sh [--install]
#          builds into tools/mplayer/out/lib, and compares the result with
#          libs/mplayer-ce-build (out/compare.txt); --install then copies it
#          over libs/mplayer-ce-build
#        tools/mplayer/build.sh --work
#          sets up out/work, a git tree of MPlayer CE with wiifin.patch
#          applied, to change MPlayer
#        tools/mplayer/build.sh --patch
#          writes wiifin.patch from out/work
#
# It compiles in devkitpro/devkitppc:20260221 (GCC 15.2, the compiler of the
# libraries in the repository) through Docker; with NATIVE=1 it uses the
# devkitPPC of this machine instead.
set -e
T=$(dirname "$(realpath "$0")"); REPO=$(realpath "$T/../..")
IMAGE=devkitpro/devkitppc:20260221
COMMIT=9ea3e57627727be964f98102daa8d17778b22979   # 2011-07-07, the last one
OUT=$T/out
SRC=$OUT/src
WORK=$OUT/work

fetch() {  # the commit hash vouches for the content
    if [ ! -d "$OUT/mplayer-ce.git" ]; then
        mkdir -p "$OUT"
        git init -q --bare "$OUT/mplayer-ce.git"
        git -C "$OUT/mplayer-ce.git" fetch -q --depth 1 https://github.com/extremscorner/mplayer-ce.git "$COMMIT"
    fi
    git -C "$OUT/mplayer-ce.git" update-ref refs/heads/master "$COMMIT"
}

case "$1" in
--work)
    fetch
    [ -e "$WORK" ] && { echo "build.sh: $WORK exists; remove it first"; exit 1; }
    git clone -q "$OUT/mplayer-ce.git" "$WORK"
    (cd "$WORK" && patch -s -p1 < "$T/wiifin.patch")
    echo "$WORK: MPlayer CE with wiifin.patch; edit, then build.sh --patch"
    exit ;;
--patch)
    [ -d "$WORK/.git" ] || { echo "build.sh: no $WORK (build.sh --work)"; exit 1; }
    # new files under mplayer/wiifin count; the configuration is configure.py's
    git -C "$WORK" add -N mplayer/wiifin
    git -C "$WORK" diff --full-index -- ':!mplayer/config.mak' ':!mplayer/config.h' \
        ':!*.o' ':!*.d' ':!*.a' > "$T/wiifin.patch"
    git -C "$WORK" reset -q
    echo "wrote $T/wiifin.patch"
    exit ;;
esac

if [ -z "$INSIDE" ]; then
    # prepare the source here, then compile (in the pinned image)
    fetch
    rm -rf "$SRC"; mkdir -p "$SRC"
    git -C "$OUT/mplayer-ce.git" archive "$COMMIT" | tar x -C "$SRC"
    python3 "$T/configure.py" "$SRC/mplayer"
    (cd "$SRC" && patch -s -p1 < "$T/wiifin.patch")
    if [ -z "$NATIVE" ]; then
        docker run --rm -v "$REPO:$REPO" -w "$REPO" -e INSIDE=1 -e HOST_UID="$(id -u):$(id -g)" \
            "$IMAGE" "$T/build.sh" "$@"
        exit
    fi
fi

: "${DEVKITPPC:=/opt/devkitpro/devkitPPC}"
export DEVKITPPC
AR=$DEVKITPPC/bin/powerpc-eabi-ar
CC=$DEVKITPPC/bin/powerpc-eabi-gcc
JOBS=$(nproc)
cd "$SRC/mplayer"

# Everything compiles; only linking the mplayer executable fails (no
# main(), DISABLE_MAIN), which -k lets through.  version.h is in the
# repository: version.sh (CRLF) does not run.
make -k -j"$JOBS" -o version.h > "$OUT/make.log" 2>&1 || true
make -j"$JOBS" -o version.h wiifin/register_mpegts.o wiifin/wii_stubs.o >> "$OUT/make.log" 2>&1

# libmplayer.a: MPlayer's objects and FFmpeg's, the list in objects.txt
mkdir -p "$OUT/lib"
missing=0
while read -r o; do
    [ -f "$o" ] || { echo "build.sh: $o was not built (see $OUT/make.log)"; missing=1; }
done < "$T/objects.txt"
[ $missing -eq 0 ] || exit 1
rm -f "$OUT/lib/libmplayer.a"
xargs "$AR" rcsD "$OUT/lib/libmplayer.a" < "$T/objects.txt"

# libfribidi.a, from the copy in MPlayer CE's repository
F=$SRC/libs/fribidi
rm -rf "$OUT/fribidi"; mkdir -p "$OUT/fribidi"
for c in "$F"/lib/*.c "$F"/charset/*.c; do
    "$CC" -O2 -mcpu=750 -meabi -mrvl -mhard-float -DHAVE_CONFIG_H -I"$F" -I"$F/lib" -I"$F/charset" \
        -c "$c" -o "$OUT/fribidi/$(basename "${c%.c}").o" 2>> "$OUT/make.log"
done
rm -f "$OUT/lib/libfribidi.a"
"$AR" rcsD "$OUT/lib/libfribidi.a" "$OUT"/fribidi/*.o

# object by object against the libraries in the repository
python3 "$T/compare.py" "$REPO/libs/mplayer-ce-build/libmplayer.a" "$OUT/lib/libmplayer.a" \
    > "$OUT/compare.txt" || true
echo "compared with libs/mplayer-ce-build: $(tail -1 "$OUT/compare.txt") (out/compare.txt)"

if [ "$1" = --install ]; then
    cp "$OUT/lib/libmplayer.a" "$OUT/lib/libfribidi.a" "$REPO/libs/mplayer-ce-build/"
    echo "installed into libs/mplayer-ce-build"
fi
[ -n "$HOST_UID" ] && chown -R "$HOST_UID" "$OUT" "$REPO/libs/mplayer-ce-build"
ls -l "$OUT"/lib/*.a
