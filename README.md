# 🎮 WiiFin – Jellyfin client for the Nintendo Wii

![screenshot]()

**WiiFin** is an experimental homebrew client for [Jellyfin](https://jellyfin.org) designed to run on the Nintendo Wii.  
It aims to offer a lightweight and Wii-friendly media browsing experience, written in C++ using GRRLIB.

---

## ⚠️ Project Status

> 🚧 **This project is highly experimental. Use at your own risk.**

- ✅ The **UI renders correctly** on real hardware and in Dolphin Emulator.
- ❌ **No actual media playback or Jellyfin API integration** is functional yet.
- 🛠️ Focus is currently on interface, input handling, and structure.

---

## 🔧 Build Instructions

You will need:

- [devkitPro](https://devkitpro.org)
- `devkitPPC` and `libogc`
- `GRRLIB`, `libpngu`, `freetype`, `libjpeg`

Then:

```bash
./build.sh
````

To run on Dolphin:

```bash
dolphin-emu -e WiiFin.elf
```

Or convert to `.dol` or `.wad` for Wii hardware.

---

## 📁 Folder Structure

```
WiiFin/
├── source/          # Core C++ source files
├── data/            # PNG/TTF assets
├── Makefile         # devkitPro-compatible build script
└── README.md
```

---

## 💡 Roadmap (Planned)

* [ ] Full Jellyfin API support (Browse library, stream)
* [ ] Settings & server management
* [ ] Localization support

---

## 🤝 Contributions

Pull requests, bug reports and improvements are welcome – this is a learning and fun project!

---

## 📜 License

GPLv3 – See [LICENSE](LICENSE) for details.

---
