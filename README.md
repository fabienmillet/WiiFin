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
