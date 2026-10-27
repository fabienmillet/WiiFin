#!/bin/bash
# Build the controller libraries Input uses into libs/controllers (lib/,
# include/), each at a fixed commit:
#   libwupc       the Wii U Pro Controller (FIX94)
#   libwiidrc     the Wii U GamePad, through Wii VC injects (FIX94)
#   libsicksaxis  a DualShock 3 on USB (xerpi)
# The Makefile links each one it finds there (HAVE_WUPC, HAVE_WIIDRC,
# HAVE_SICKSAXIS); without them WiiFin builds without those controllers.
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

mkdir -p "$OUT" "$DEST/lib" "$DEST/include/wupc" "$DEST/include/wiidrc"
fetch libwupc      https://github.com/FIX94/libwupc.git       24145b35dbc2c1950f6f48079d577acc7efb0c4d
fetch libwiidrc    https://github.com/FIX94/libwiidrc.git     4e156201db17231ec6664d66e478151a859f8c5b
fetch libsicksaxis https://github.com/xerpi/libsicksaxis.git  4ddd49e687a7d2466691fd2c73d8a55a90fe1ca6

# libwupc wraps wiiuse_register and bte_registerdeviceasync, which libogc 3
# replaced with wiiuse_accept and bte_listenasync (+ _step2)
python3 - "$OUT/libwupc/source/wupc.c" <<'PY'
import sys
from pathlib import Path
p = Path(sys.argv[1]); s = p.read_text()
def sub(a, b):
    global s
    assert a in s, a
    s = s.replace(a, b)
sub("extern __typeof(wiiuse_register) __real_wiiuse_register;",
    "extern __typeof(wiiuse_accept) __real_wiiuse_accept;")
sub("bte_registerdeviceasync(stat->sock, _bdaddr, __WUPC_HandleConnect);",
    "bte_listenasync(stat->sock, _bdaddr, __WUPC_HandleConnectStep2);")
marker = "static s32 __WUPC_HandleDisconnect(void *arg,struct bte_pcb *pcb __attribute__((unused)),u8 err)"
sub(marker, """static s32 __WUPC_HandleConnectStep2(void *arg,struct bte_pcb *pcb,u8 err)
{
    if(err != ERR_OK)
    {
        bte_disconnect(pcb);
        return err;
    }
    return bte_listenasync_step2(pcb, __WUPC_HandleConnect);
}

""" + marker)
sub("int __wrap_wiiuse_register(struct wiimote_listen_t *wml, struct bd_addr *bdaddr, struct wiimote_t *(*assign_cb)(struct bd_addr *bdaddr))",
    "int __wrap_wiiuse_accept(struct wiimote_listen_t *wml, struct bd_addr *bdaddr, const u8 *name, struct wiimote_t *(*assign_cb)(wiimote_listen *wml, u8 err))")
sub("return __real_wiiuse_register(wml,bdaddr,assign_cb);",
    "return __real_wiiuse_accept(wml,bdaddr,name,assign_cb);")
p.write_text(s)
PY

$CC $CFLAGS -I"$OUT/libwupc/include" -I"$OUT/libwupc/source" -c "$OUT/libwupc/source/wupc.c" -o "$OUT/wupc.o"
rm -f "$DEST/lib/libwupc.a"; $AR rcs "$DEST/lib/libwupc.a" "$OUT/wupc.o"
cp "$OUT/libwupc/include/wupc/wupc.h" "$DEST/include/wupc/"

$CC $CFLAGS -I"$OUT/libwiidrc/include" -I"$OUT/libwiidrc/source" -c "$OUT/libwiidrc/source/wiidrc.c" -o "$OUT/wiidrc.o"
rm -f "$DEST/lib/libwiidrc.a"; $AR rcs "$DEST/lib/libwiidrc.a" "$OUT/wiidrc.o"
cp "$OUT/libwiidrc/include/wiidrc/wiidrc.h" "$DEST/include/wiidrc/"

$CC $CFLAGS -c "$OUT/libsicksaxis/libsicksaxis/sicksaxis.c" -o "$OUT/sicksaxis.o"
rm -f "$DEST/lib/libsicksaxis.a"; $AR rcs "$DEST/lib/libsicksaxis.a" "$OUT/sicksaxis.o"
cp "$OUT/libsicksaxis/libsicksaxis/sicksaxis.h" "$DEST/include/"

echo "Built libwupc, libwiidrc and libsicksaxis into $DEST"
