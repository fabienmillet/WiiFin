# Building WiiFin

How to build WiiFin from source and run it while working on it. To install a release, see the [user guide](USER_GUIDE.md); to contribute, [CONTRIBUTING.md](../CONTRIBUTING.md).

## Requirements

- [devkitPro](https://devkitpro.org) with `devkitPPC`, `libogc`, and `wii-dev` portlibs
- Graphics: `GRRLIB`, `libpngu`, `freetype`, `libjpeg`
- mbedTLS (bundled under `libs/`, cross-compiled by `setup.sh`)
- MPlayer CE as `libmplayer.a`, for playback: prebuilt in `libs/mplayer-ce-build`, rebuilt from source by `tools/mplayer/build.sh` (see [MPLAYER_CE_BUILD.md](../MPLAYER_CE_BUILD.md)). Without it, WiiFin still compiles but cannot play.

## Building

On a fresh machine, `./setup.sh` installs devkitPro and the portlibs, builds GRRLIB and mbedTLS, then compiles WiiFin (Arch-based distros, or any host with `dkp-pacman`).

```bash
./build.sh          # WiiFin.dol
./build.sh wad      # WiiFin.wad, the installable channel (needs libWiiPy, installed by setup.sh)
```

`make wad` puts `WiiFin.dol` into `tools/wad/template.wad` (banner, NAND loader, ticket and TMD of title `WIFN`) and fakesigns it: see `tools/make_wad.py`.

## Running

On **real Wii hardware**: unzip `WiiFin-hbc.zip` at the root of the SD card (it holds `apps/WiiFin/`) to start WiiFin from the Homebrew Channel, or install `WiiFin.wad` with a WAD manager (works on vWii too). WiiFin keeps its settings, profiles and log (`wiifin.log`) in `SD:/apps/WiiFin/`.

On **Dolphin Emulator**:

```bash
dolphin-emu -e WiiFin.dol
```

To test against a local Jellyfin in Docker, with scripted button presses and frame captures, see [tools/test](../tools/test/README.md).

## Project structure

```
WiiFin/
├── source/
│   ├── core/        # App, settings, playback sessions, sounds and music, text drawing, log
│   ├── input/       # Wii Remote, Classic Controller and GameCube controller
│   ├── jellyfin/    # Jellyfin API client (HTTPS via mbedTLS), remote control (WebSocket)
│   ├── player/      # MPlayer CE integration, video output, player overlay, subtitles, thumbnails
│   └── ui/          # The screens: connection, profiles, home, libraries, details, music, settings
├── data/            # Fonts, sounds, pictures
├── docs/            # The user guide, this file
├── libs/            # mbedTLS, the prebuilt MPlayer CE
├── tools/           # WAD packager, linker script, MPlayer CE build (mplayer/)
│   └── test/        # Test harness: Jellyfin in Docker, scripted runs in Dolphin
├── apps/WiiFin/     # Homebrew Channel metadata
├── DIRECT_PLAY.md   # What the Wii plays as it is
└── Makefile
```

