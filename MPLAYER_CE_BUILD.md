# Building MPlayer CE for WiiFin

WiiFin plays video with [MPlayer CE](https://github.com/extremscorner/mplayer-ce), built as a static library. The repository ships it prebuilt in `libs/mplayer-ce-build/` (`libmplayer.a`, `libfribidi.a`). The `Makefile` links it when it is there; without it, WiiFin still compiles but cannot play video.

`tools/mplayer/build.sh` rebuilds both libraries from source:

```sh
tools/mplayer/build.sh             # builds into tools/mplayer/out/lib and compares with libs/mplayer-ce-build
tools/mplayer/build.sh --install   # the same, then replaces libs/mplayer-ce-build
```

It needs Docker, git and Python 3. The build takes about a minute. The **MPlayer CE** workflow (Actions → MPlayer CE → Run workflow) does the same on GitHub and offers the libraries as an artifact.

## How it is built

| Step | What it does |
|---|---|
| Source | MPlayer CE at commit `9ea3e576` (2011-07-07, its last), fetched by hash. |
| `tools/mplayer/configure.py` | Adapts MPlayer CE's Wii configuration: a library (`DISABLE_MAIN`: `main()` becomes `mplayer_main()`) for today's devkitPPC, no networking, DVD or libass. Keeps only the FFmpeg decoders, parsers and demuxers WiiFin needs, with `CONFIG_SMALL` and precomputed tables. |
| `tools/mplayer/wiifin.patch` | WiiFin's changes to MPlayer CE's code (below). |
| Compiler | `devkitpro/devkitppc:20260221`: GCC 15.2, the compiler of the libraries in the repository. |
| `tools/mplayer/objects.txt` | The objects that make `libmplayer.a`. |

## WiiFin's changes (`wiifin.patch`)

- **`mplayer.c`**
  - `mplayer_main()` starts from a clean state on each call, since WiiFin plays one video after another in the same process.
  - It ends with a `longjmp` back to WiiFin instead of `exit()`.
  - It reads WiiFin's seek and volume requests and publishes the position, duration and pause state.
  - It handles the loading indicator, calls WiiFin back once the stream is open (playback reporting) and writes `[mplayer] ...` stages to the log.
  - It does not load fonts, and does not pause on a low cache (WiiFin rebuffers itself).
  - It reads `-cache-min` on every call; MPlayer CE kept the first call's value. This one is not in the libraries in the repository.
- **`mp_msg.c`**: MPlayer's messages go to `SYS_Report`, which is WiiFin's log.
- **`stream/stream.c`**: registers WiiFin's HTTP and HTTPS streams (`stream_info_http_wii`, `stream_info_https_wii`, in `source/player/stream_wiifin.cpp`).
- **`stream/cache2.c`**: the cache thread runs at priority 80; WiiFin's `pacePrefetch` adjusts it while playing. The prefill waits without polling input.
- **`libao2/ao_gekko.c`**: 4 audio buffers instead of 32, so little sound is queued ahead of the picture.
- **`libmpdemux/demux_lavf.c`**: prefers the WAVE tag of the codec over the container's (MPEG-TS stream types).
- **`osdep/getch2-gekko.c`**: leaves the controllers to WiiFin.
- **`osdep/plat_gekko.c`**: `plat_init` only sets MPlayer's folders; WiiFin sets up the IOS, the video, the devices and the network.
- **Fixes for the current devkitPPC**:
  - `struct stat`, `nanosleep`, `if_config` and `TVPal576IntDfScale` changed;
  - stubs for what devkitPro no longer has (`wiifin/wii_stubs.c`: iconv, NTFS);
  - headers for ext2 and NTFS (`wiifin/*.h`).
- **`wiifin/register_mpegts.c`**: `register_mpegts_demuxer()`, called by WiiFin, registers FFmpeg's MPEG-TS demuxer.

WiiFin replaces part of the library at link time:
- `source/player/vo_wiifin.c` provides the video output (`video_out_gx` and the `mpgx*` functions), so `vo_gx.o` and `gx_supp.o` are not linked.
- `stream_wiifin.cpp` provides the HTTP and HTTPS streams.

## Changing MPlayer

```sh
tools/mplayer/build.sh --work     # tools/mplayer/out/work: a git tree of MPlayer CE with wiifin.patch applied
# edit tools/mplayer/out/work/mplayer/...
tools/mplayer/build.sh --patch    # writes wiifin.patch from it
tools/mplayer/build.sh            # builds and compares
```

Then test with the new libraries before installing them: the smoke test and the playback scenarios of `tools/test` (see its README).

## Against the libraries in the repository

`build.sh` compares the result with `libs/mplayer-ce-build/libmplayer.a`, object by object (code size, symbols defined and used), in `tools/mplayer/out/compare.txt`. The libraries in the repository were built by hand and their source was lost. `wiifin.patch` was reconstructed from them by comparing objects, and is checked by the playback scenarios: the same results and A/V offset with either library.

The remaining differences, out of 457 objects:

| Objects | Difference |
|---|---|
| `h264`, `h264_cabac`, `h264_cavlc`, `mpeg12`, `mpeg4videodec`, `h263dec`, `ac3dec` | A few bytes of stack frame and register use, with the same symbols; no source change found. |
| `mpegts`, `register_mpegts` | Compiled by hand with other options at the time. |
| `fst`, `gcfst`, `iso`, `ftp_devoptab` | The `st_spare*` fields that no longer exist were rewritten differently. |
| `stream` | Upstream `free_stream()` closes descriptors with `net_close`; WiiFin's streams have none. |
| `mplayer` | 4 bytes: the video output time in MPlayer's status line, a 64-bit timer truncated to 32 bits, is fixed (the reference did not count it). |
| `plat_gekko` | The folder names are copied by `snprintf` in the reference. |
| `gx_supp`, `vo_gx` | Changed in the reference, not linked by WiiFin. |
| `ehcmodule.elf`, `stream_http(s)_wii` | In the reference only; not linked by WiiFin. |
| `utils`, `opt`, `options`, `audioconvert` | Same-name objects in a different order. |
