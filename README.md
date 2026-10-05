<p align="center">
  <img src="https://raw.githubusercontent.com/fabienmillet/WiiFin/refs/heads/main/assets/logo_wiifin_banner.png" alt="WiiFin logo" width="600"/><br>
  <em>Jellyfin client for the Nintendo Wii</em>
</p>

<p align="center">
  <a href="../README.md"><img src="https://flagcdn.com/w40/gb.png" width="28" alt="English"/></a>
  &nbsp;
  <a href="README/README.fr.md"><img src="https://flagcdn.com/w40/fr.png" width="28" alt="Français"/></a>
  &nbsp;
  <a href="README/README.de.md"><img src="https://flagcdn.com/w40/de.png" width="28" alt="Deutsch"/></a>
  &nbsp;
  <a href="README/README.es.md"><img src="https://flagcdn.com/w40/es.png" width="28" alt="Español"/></a>
  &nbsp;
  <a href="README/README.it.md"><img src="https://flagcdn.com/w40/it.png" width="28" alt="Italiano"/></a>
</p>

---

<p align="center">
<strong>WiiFin</strong> is an experimental homebrew client for <a href="https://jellyfin.org">Jellyfin</a>, built specifically for the Nintendo Wii.  
It provides a lightweight, console-friendly media browsing and playback experience, written in C++ using <a href="https://github.com/GRRLIB/GRRLIB">GRRLIB</a> and <a href="https://github.com/extremscorner/mplayer-ce">MPlayer CE</a>.
</p>

---

## ⚠️ Project Status

> 🚧 **Experimental** – functional but still under active development. Expect rough edges on real hardware.

### ✅ What works:
- **Authentication**: login with username/password or via **QuickConnect** (approve on another device)
- **Saved profiles**: multiple accounts stored securely (access token only, no password stored)
- **Library browsing**: movies, TV shows, music libraries with cover art loaded from the server
- **Detail view**: synopsis, rating, genres, cast, director, audio/subtitle track selection
- **Continue Watching** and **Next Up** rows
- **TV shows**: season and episode navigation, **shuffle** (button 2: the whole series from the season list, one season from its episode list), next episode plays automatically
- **Video playback**: server-side transcoding streamed through the integrated MPlayer CE engine
- **Music playback**: audio libraries, album/track navigation
- **Player overlay**: seek bar, volume control, next/prev episode, audio & subtitle track switching, intro skip
- **Playback reporting**: progress sent back to the Jellyfin server (resume where you left off)
- **HTTPS**: TLS connections via mbedTLS (self-signed certificates supported)
- **Wiimote IR pointer** and **virtual on-screen keyboard**
- **Classic Controller**, **Wii U GamePad** (Virtual Console injects) and **GameCube controller**: everything can be done with the D-pad, no sensor bar needed (see [Controls](#-controls))
- **Background music** on menus
- Ships as a ready-to-use `.dol` and installable `.wad` (Wii / vWii)

### 🎮 Controls

The pointer is optional: every screen works with the D-pad.

| Wii Remote | Classic Controller / Wii U GamePad | GameCube controller | Action |
|---|---|---|---|
| A / B | A / B | A / B | Select / Back |
| D-pad | D-pad or left stick | D-pad or stick | Move (hold to repeat) |
| − / + | − / + or L / R | L / R | Tabs, pages, previous / next episode |
| 1 / 2 | Y / X | Y / X | Search, audio / subtitle tracks |
| HOME | HOME | START | HOME menu |
| pointer | ZL / ZR | Z | Video zoom (fit / fill) |

On-screen hints show the buttons of the controller used last (GameCube: A green, B red, L / R, START).

### ⚠️ Known limitations:
- Direct-play is not supported; all video is transcoded by the server
- No 5.1 multi-channel audio (stereo only via transcoding)
- Subtitle rendering relies on the server embedding them into the video stream

---

## 🔧 Build Instructions

### Requirements:

- [devkitPro](https://devkitpro.org) with `devkitPPC`, `libogc`, and `wii-dev` portlibs
- Graphics: `GRRLIB`, `libpngu`, `freetype`, `libjpeg`
- mbedTLS (bundled under `libs/`, cross-compiled automatically by the CI)
- **Optional**: MPlayer CE compiled as `libmplayer.a` — required for video playback. See [MPLAYER_CE_BUILD.md](MPLAYER_CE_BUILD.md) for instructions. Without it, WiiFin still compiles but video playback is unavailable.

### Building:

On a fresh machine, `./setup.sh` installs devkitPro and the portlibs, builds GRRLIB and mbedTLS, then compiles WiiFin (Arch-based distros, or any host with `dkp-pacman`).

```bash
./build.sh          # WiiFin.dol
./build.sh wad      # WiiFin.wad, the installable channel (needs libWiiPy, installed by setup.sh)
```

The GitHub Actions workflow **Build** runs on every push and pull request. It does the same in devkitPro's Docker image (a compiler warning fails it) and publishes `WiiFin.dol`, `WiiFin.wad` and the Homebrew Channel zip. It then boots the WAD and the DOL in Dolphin and checks that WiiFin reaches its menu (`tools/test/smoke.sh`), and plays, seeks and resumes videos from a Jellyfin in Docker (`tools/test/playback.sh`). Run it by hand (Actions → Build → Run workflow) with a version and *release* ticked to publish a GitHub release once every check passes.

`make wad` puts `WiiFin.dol` into `tools/wad/template.wad` (banner, NAND loader, ticket and TMD of title `WIFN`) and fakesigns it: see `tools/make_wad.py`.

### Running:

On **Dolphin Emulator**:

```bash
dolphin-emu -e WiiFin.dol
```

To test against a local Jellyfin with scripted input and frame captures, see [tools/test](tools/test/README.md).

On **real Wii hardware**: copy `WiiFin.dol` to `SD:/apps/WiiFin/boot.dol`, or install `WiiFin.wad` using a WAD manager (works on vWii too).

---

## 📁 Project Structure

```
WiiFin/
├── source/
│   ├── core/        # App lifecycle, background music, utilities
│   ├── input/       # Wiimote + USB keyboard input
│   ├── jellyfin/    # Jellyfin HTTP API client (HTTPS via mbedTLS)
│   ├── player/      # MPlayer CE integration, player overlay HUD
│   └── ui/          # All views: Connect, Library, Profile, Settings
├── data/            # PNG/TTF graphical assets
├── libs/            # Bundled mbedTLS, MPlayer CE build
├── tools/           # WAD packager (make_wad.py + template), linker script
│   └── test/        # Test harness: Jellyfin in Docker, scripted runs in Dolphin
├── Makefile         # devkitPro-compatible build script
└── apps/WiiFin/     # Homebrew Channel metadata
```

---

## 🚀 Roadmap

* [ ] Sort/filter items (by year, genre, rating)
* [ ] Mark items as favorites from the Wii
* [ ] Multiple UI color themes

---

## 📸 Screenshots

<img src="https://github.com/fabienmillet/WiiFin/blob/main/assets/preview.png?raw=true" alt="WiiFin Menu Screenshot" width="500"/><br> <em>WiiFin running in Dolphin Emulator</em>

---

## 🤝 Contributing

WiiFin is open to pull requests, bug reports, and suggestions.

* 📘 Read the [contribution guidelines](CONTRIBUTING.md)
* 🐛 Use the [bug report template](.github/ISSUE_TEMPLATE/bug_report.md)
* 💡 Got a feature idea? Use the [feature request template](.github/ISSUE_TEMPLATE/feature_request.md)


<a href="https://discord.gg/p9DXfEmUYu">
  <img src="https://img.shields.io/badge/Join%20us%20on%20Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Discord Badge"/>
</a>

---

## 📜 License

This project is licensed under the **GPLv3**.
See the [LICENSE](LICENSE) file for more details.
