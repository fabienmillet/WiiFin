# Building MPlayer CE for WiiFin

WiiFin uses a patched build of [MPlayer CE](https://github.com/dborth/mplayer-ce) compiled as a static library (`libmplayer.a`).  
The build is **optional** — without it, WiiFin falls back to a stub player and still compiles cleanly.

---

## Prerequisites

- devkitPro with `devkitPPC`, `libogc`, and `wii-dev` portlibs installed
- `ppc-fribidi` built and available (see below)
- Standard build tools: `make`, `git`, `ar`

---

## 1. Clone MPlayer CE

```bash
git clone https://github.com/dborth/mplayer-ce.git ~/mplayer-ce
cd ~/mplayer-ce/mplayer
```

---

## 2. Apply the WiiFin patch

The patch is in `libs/mplayer-ce/wii_player_patch.diff` and covers:

- Renaming `main()` → `mplayer_main()` under `DISABLE_MAIN`
- `setjmp`/`longjmp`-based exit so WiiFin can stop playback without `exit()`
- Stream-opened callback (`g_stream_opened_cb`) for Jellyfin session reporting
- Overlay integration globals (`g_mplayer_time_pos`, `g_mplayer_duration`, `g_mplayer_paused`, seek/volume control)
- Loading indicator logic with `g_wiifin_loading_active` and `mpgxForceLoadingFrame()`
- GX overlay callback slots in `gx_supp.c`/`gx_supp.h` (`g_wiifin_overlay_cb`, `g_wiifin_gx_overlay_cb`)
- Y/U/V texture clearing to proper YCbCr black (avoids purple screen during loading)
- XFB allocation with black-clear in `mpgxInit()`
- `mpgxRunOverlay()` called in `flip_page()` before `mpgxPushFrame()`
- Fix `ov_gx_draw()` loading indicator ordering (before `ov_visible` check)
- Jellyfin known-duration fallback when MPEG-TS demuxer cannot determine length

Apply with:

```bash
patch -p2 < /path/to/WiiFin/libs/mplayer-ce/wii_player_patch.diff
```

> The patch is intentionally a guide rather than a mechanical diff — some hunks require manual context matching due to upstream changes. Read through it if `patch` rejects hunks.

---

## 3. Configure

Copy and adapt the provided GC config as a starting point:

```bash
cp config.gc.mak config.mak
```

Key changes required in `config.mak`:

| Option | Value | Reason |
|--------|-------|--------|
| Remove `-mpaired`, `-mstring` | — | Dropped in GCC 15 |
| `-mogc` → `-mrvl` | everywhere | Wii target |
| Add to CFLAGS | `-DHW_RVL -DDISABLE_MAIN` | Wii + library mode |
| Add to CFLAGS | `-Wno-implicit-function-declaration -Wno-incompatible-pointer-types -Wno-int-conversion -Wno-discarded-qualifiers` | GCC 15 compat |
| `WII` | `no` | Use `vo_gx.c`, not `vo_wii.c` |
| `HAVE_PAIRED` | `no` | No paired-single intrinsics |
| `HAVE_PTHREADS` | `yes` | |
| `HW_RVL` | `yes` | |
| `GEKKO` | `yes` | |
| Remove from EXTRALIBS | `-liconv -lbba` | Not available |
| Add include paths | freetype2, fribidi | |

In `config.h`, ensure:

```c
#define MAXPATHLEN 4096
#define HAVE_PTHREADS 1
#define HAVE_PAIRED 0
#define HAVE_THREADS 1
#undef CONFIG_ICONV
```

Also add `extern` to the `MPLAYER_DATADIR`/`MPLAYER_CONFDIR` declarations if they cause duplicate-symbol errors.

---

## 4. Build fribidi (cross-compile)

```bash
# From the mplayer-ce repo root
cd libfribidi   # or wherever fribidi sources are
./configure --host=powerpc-eabi --prefix="$(pwd)/out" CC=powerpc-eabi-gcc
# Patch libtool: replace powerpc-gekko-* with powerpc-eabi-*
sed -i 's/powerpc-gekko-/powerpc-eabi-/g' libtool
make && make install
```

---

## 5. Build `libmplayer.a`

```bash
cd ~/mplayer-ce/mplayer
make   # compilation succeeds; the final link step will fail — that's expected
```

Then bundle all objects (including FFmpeg sublibraries) into a single archive:

```bash
AR=/opt/devkitpro/devkitPPC/bin/powerpc-eabi-ar

rm -rf /tmp/mplayer_objs
mkdir -p /tmp/mplayer_objs/{avformat,avcodec,avutil,postproc,swscale}

for lib in avformat avcodec avutil postproc swscale; do
  (cd /tmp/mplayer_objs/$lib && $AR x ~/mplayer-ce/mplayer/ffmpeg/lib${lib}/lib${lib}.a)
done

# Collect WiiFin-specific mplayer objects (exclude ffmpeg/ subdirs)
find . -name "*.o" ! -path "./ffmpeg/*" | xargs $AR rcs libmplayer.a
# Append ffmpeg objects
find /tmp/mplayer_objs -name "*.o" | xargs $AR rs libmplayer.a
```

The `--start-group`/`--end-group` flags in `WiiFin/Makefile` handle circular references between FFmpeg sub-libraries at link time.

---

## 6. Add Wii HTTP/HTTPS stream modules

`libmplayer.a` has no built-in HTTP stream handler (only `stream_ffmpeg` for `ffmpeg://`). WiiFin ships two custom stream modules:

| File | Protocol | Notes |
|------|----------|-------|
| `stream/stream_https_wii.c` | `https://` | mbedTLS + libogc |
| `stream/stream_http_wii.c` | `http://` | plain TCP + libogc, no TLS |

Both must be present in the cloned `mplayer-ce/mplayer/stream/` directory (they are tracked in the WiiFin repo as source files).  
`stream/stream.c` must also declare and register both modules in `auto_open_streams[]` under `#ifdef GEKKO`.

Use the helper script to compile and inject all three objects in one step:

```bash
bash /path/to/WiiFin/repo/../mplayer-ce/build_https.sh
```

> `build_https.sh` compiles `stream_https_wii.c`, `stream_http_wii.c`, and `stream.c` (with `-Iffmpeg` for `libavutil` headers), then adds all three `.o` files to both library paths via `ar r`.

Alternatively, compile manually:

```bash
GCC=/opt/devkitpro/devkitPPC/bin/powerpc-eabi-gcc
AR=/opt/devkitpro/devkitPPC/bin/powerpc-eabi-ar
CFLAGS="-O2 -mcpu=750 -meabi -mrvl -msdata -mmultiple -pipe \
  -std=gnu99 -DGEKKO -DHW_RVL -DDISABLE_MAIN \
  -D_LARGEFILE_SOURCE -D_FILE_OFFSET_BITS=64 -D_LARGEFILE64_SOURCE \
  -I/opt/devkitpro/portlibs/ppc/include \
  -I/opt/devkitpro/libogc/include \
  -I/path/to/WiiFin/libs/mbedtls/include \
  -I. -Wno-undef -Wno-redundant-decls"

cd ~/mplayer-ce/mplayer
$GCC $CFLAGS -c -o stream/stream_https_wii.o stream/stream_https_wii.c
$GCC $CFLAGS -c -o stream/stream_http_wii.o  stream/stream_http_wii.c
$GCC $CFLAGS -Iffmpeg -c -o stream/stream.o  stream/stream.c
$AR r /path/to/WiiFin/libs/mplayer-ce-build/libmplayer.a \
    stream/stream_https_wii.o stream/stream_http_wii.o stream/stream.o
```

---

## 7. Install into WiiFin

```bash
mkdir -p /path/to/WiiFin/libs/mplayer-ce-build
cp libmplayer.a /path/to/WiiFin/libs/mplayer-ce-build/
cp /path/to/fribidi/out/lib/libfribidi.a /path/to/WiiFin/libs/mplayer-ce-build/
```

The WiiFin `Makefile` automatically detects `libs/mplayer-ce-build/libmplayer.a` and links it in.

---

## Playback configuration

The transcoding parameters are tuned for real Wii hardware in `source/jellyfin/JellyfinClient.cpp`:

| Parameter | Value |
|-----------|-------|
| Video codec | `mpeg2video` |
| Audio codec | `mp3` |
| Container | `ts` (MPEG-TS) |
| Max resolution | 848×480 |
| Max framerate | 24 fps |
| Max streaming bitrate | 1 Mbps |
| Max audio channels | 2 |

MPlayer is invoked (see `source/player/WiiPlayer.cpp`) with:

```
mplayer -noconsolecontrols -v -msglevel demux=4 -fs \
  -demuxer lavf \
  -lavfdopts format=mpegts:probesize=32768:analyzeduration=1 \
  -vo gx:colorspace=1 -ao gekko \
  -cache 4096 -cache-min 5 -cache-seek-min 5 \
  -autosync 10 -mc 15 -delay 0.3 \
  -lavdopts fast:skiploopfilter=all:skipidct=nonref:skipframe=nonref \
  -hardframedrop \
  [-ss <secs>]   # conditional: applied when resuming mid-stream
```
