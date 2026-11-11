#!/bin/bash
# Build the controller libraries Input uses into libs/controllers (lib/,
# include/), each at a fixed commit:
#   libwiidrc     the Wii U GamePad, through Wii VC injects (FIX94, MIT)
# The Makefile links it if found there (HAVE_WIIDRC); without it WiiFin
# builds without the GamePad.
#
# libwupc (FIX94) and libsicksaxis (xerpi) are left out on purpose: neither
# has a license, so builds that include them can't be shipped. Re-add them
# if their authors add one.
#
# usage: tools/controllers/build.sh
set -e
T=$(dirname "$(realpath "$0")"); REPO=$(realpath "$T/../..")
OUT=$T/out
DEST=$REPO/libs/controllers
: "${DEVKITPRO:=/opt/devkitpro}"
: "${DEVKITPPC:=$DEVKITPRO/devkitPPC}"
CC=$DEVKITPPC/bin/powerpc-eabi-gcc
AR=$DEVKITPPC/bin/powerpc-eabi-ar
CFLAGS="-O2 -Wall -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float -I${LIBOGC_INC:-$DEVKITPRO/libogc/include}"

fetch() {  # name url commit; the commit hash vouches for the content
    if [ ! -d "$OUT/$1/.git" ]; then
        git init -q "$OUT/$1"
        git -C "$OUT/$1" fetch -q --depth 1 "$2" "$3"
    fi
    git -C "$OUT/$1" checkout -q -f "$3"
}

mkdir -p "$OUT" "$DEST/lib" "$DEST/include/wiidrc" "$DEST/licenses"
# drop what older versions of this script built (unlicensed, can't be shipped)
rm -rf "$DEST/lib/libwupc.a" "$DEST/lib/libsicksaxis.a" "$DEST/include/wupc" "$DEST/include/sicksaxis.h"
fetch libwiidrc    https://github.com/FIX94/libwiidrc.git     4e156201db17231ec6664d66e478151a859f8c5b

$CC $CFLAGS -I"$OUT/libwiidrc/include" -I"$OUT/libwiidrc/source" -c "$OUT/libwiidrc/source/wiidrc.c" -o "$OUT/wiidrc.o"
rm -f "$DEST/lib/libwiidrc.a"; $AR rcs "$DEST/lib/libwiidrc.a" "$OUT/wiidrc.o"
cp "$OUT/libwiidrc/include/wiidrc/wiidrc.h" "$DEST/include/wiidrc/"
cp "$OUT/libwiidrc/LICENSE" "$DEST/licenses/libwiidrc-LICENSE"  # MIT: the notice ships with the zip

echo "Built libwiidrc into $DEST"
