# 🤝 Contributing to WiiFin

Thank you for your interest in the project!  
**WiiFin** is an experimental Jellyfin client for the Nintendo Wii.  
Any help is welcome — but please note that the project is still under active development and some features may be incomplete or unstable on real hardware.

What WiiFin does today is listed in the [README](README.md#-what-works).

---

## 🧰 Requirements

- A Wii development environment: devkitPro with devkitPPC, libogc and the `wii-dev` portlibs (`./setup.sh` installs everything on Arch-based distros or any host with `dkp-pacman`)
- A build system that supports `make` (Linux or MSYS2 recommended)
- Dolphin Emulator for quick testing
- A real Wii with the Homebrew Channel for final testing

---

## 📁 Project Structure

- `source/core/` – App, settings, playback sessions, interface sounds and music, text drawing, log
- `source/input/` – Wii Remote, Classic Controller and GameCube controller
- `source/jellyfin/` – Jellyfin API client (HTTPS via mbedTLS), remote control (WebSocket)
- `source/player/` – MPlayer CE integration, video output, player overlay, subtitles, trickplay thumbnails
- `source/ui/` – The screens: connection, profiles, home, libraries, details, music player, settings
- `data/` – Fonts, sounds, pictures
- `libs/` – Bundled mbedTLS and the prebuilt MPlayer CE (`libmplayer.a`)
- `tools/` – WAD packager, linker script, MPlayer CE build (`tools/mplayer/`), test harness (`tools/test/`)
- `apps/WiiFin/` – Homebrew Channel metadata
- `Makefile` – devkitPro-compatible build script

---

## 🧪 How to Contribute

1. **Fork** the repository and create a new branch.
2. **Make clear and atomic commits.**
3. **Build without warnings**: the CI builds with `-Werror` (`WERROR=-Werror make`).
4. **Test your change in Dolphin and, if you can, on a real Wii.** [tools/test](tools/test/README.md) runs WiiFin in Dolphin against a Jellyfin in Docker with scripted button presses; its playback scenarios run in the CI on every push and pull request. A new feature is best given a scenario of its own.
5. **Open a Pull Request** to the `main` branch.

When reporting a bug from a real Wii, attach `SD:/apps/WiiFin/wiifin.log`: it hides addresses, tokens and user ids, so it can be shared as it is.

---

## 🧭 Guidelines

- Use clear, descriptive variable names.
- Follow existing code style and indentation.
- Keep Wii limitations in mind (24 MB of MEM1 and 64 MB of MEM2, a 729 MHz CPU, 640x480 output, 24 network sockets at most).

---

## 📝 License

This project is licensed under **GPLv3**.  
By contributing, you agree that your work will be released under this license.

---

Thanks 🙌
