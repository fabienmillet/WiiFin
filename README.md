<p align="center">
  <img src="https://raw.githubusercontent.com/fabienmillet/WiiFin/refs/heads/main/assets/logo_wiifin_banner.png" alt="WiiFin logo" width="600"/><br>
  <em>Jellyfin client for the Nintendo Wii</em>
</p>

---

<p align="center">
<strong>WiiFin</strong> is a highly experimental homebrew client for <a href="https://jellyfin.org">Jellyfin</a>, built specifically for the Nintendo Wii.  
It aims to provide a lightweight, console-friendly media browsing experience, written in C++ using <a href="https://github.com/GRRLIB/GRRLIB">GRRLIB</a>.
</p>

---

## ⚠️ Project Status

> 🚧 **Work in progress** – this project is under active development and is not yet functional for media playback.

### ✅ What works:
- The UI renders correctly on both real Wii hardware and in Dolphin Emulator.

### ❌ What doesn't (yet):
- No Jellyfin API integration.
- No media playback.

Development is currently focused on:
- Interface layout
- Wiimote input support
- Core structure and navigation

---

## 🔧 Build Instructions

### Requirements:

- [devkitPro](https://devkitpro.org)
- `devkitPPC`, `libogc`
- Graphics libraries: `GRRLIB`, `libpngu`, `freetype`, `libjpeg`

### Building:

```bash
./build.sh
````

### Running:

On **Dolphin Emulator**:

```bash
dolphin-emu -e WiiFin.elf
```

On **real Wii hardware**, convert the ELF file to `.dol` or `.wad` format.

---

## 📁 Project Structure

```
WiiFin/
├── source/          # Core C++ source files
├── data/            # PNG/TTF graphical assets
├── Makefile         # devkitPro-compatible build script
└── README.md
```

---

## 🚀 Roadmap

* [ ] Full Jellyfin API support (browse, playback)
* [ ] Server discovery and settings UI
* [ ] Localization / multi-language support

---

## 📸 Screenshots

<img src="https://github.com/fabienmillet/WiiFin/blob/main/assets/preview.png?raw=true" alt="WiiFin Menu Screenshot" width="500"/><br> <em>WiiFin Menu running in Dolphin Emulator</em>

---

## 🤝 Contributing

This project is open to pull requests, bug reports, and suggestions.
Feel free to contribute and help shape a fun, functional media client for the Wii!

---

## 📜 License

This project is licensed under the **GPLv3**.
See the [LICENSE](LICENSE) file for more details.

---
