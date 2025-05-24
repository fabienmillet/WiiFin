<p align="center">
  <img src="https://raw.githubusercontent.com/fabienmillet/WiiFin/refs/heads/main/assets/logo_wiifin_banner.png" alt="WiiFin-Logo" width="600"/><br>
  <em>Jellyfin-Client für die Nintendo Wii</em>
</p>

<p align="center">
  <a href="README/README.md"><img src="https://flagcdn.com/w40/gb.png" width="28" alt="English"/></a>
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
<strong>WiiFin</strong> ist ein stark experimenteller Homebrew-Client für <a href="https://jellyfin.org">Jellyfin</a>, der speziell für die Nintendo Wii entwickelt wurde.  
Er soll ein leichtgewichtiges, konsolenfreundliches Medienbrowser-Erlebnis bieten und wurde in C++ mit <a href="https://github.com/GRRLIB/GRRLIB">GRRLIB</a> geschrieben.
</p>

---

## ⚠️ Projektstatus

> 🚧 **In Entwicklung** – dieses Projekt wird aktiv entwickelt und unterstützt derzeit keine Medienwiedergabe.

### ✅ Was funktioniert:
- Die Benutzeroberfläche wird korrekt auf echter Wii-Hardware und im Dolphin-Emulator dargestellt.

### ❌ Was noch nicht funktioniert:
- Keine Jellyfin-API-Integration.
- Keine Medienwiedergabe.

Derzeitige Entwicklungsziele:
- Benutzeroberfläche und Layout
- Wiimote-Unterstützung
- Grundstruktur und Navigation

---

## 🔧 Build-Anleitung

### Voraussetzungen:

- [devkitPro](https://devkitpro.org)
- `devkitPPC`, `libogc`
- Grafikbibliotheken: `GRRLIB`, `libpngu`, `freetype`, `libjpeg`

### Kompilierung:

```bash
./build.sh
````

### Ausführen:

Im **Dolphin-Emulator**:

```bash
dolphin-emu -e WiiFin.elf
```

Auf echter **Wii-Hardware**: ELF-Datei in `.dol` oder `.wad` konvertieren.

---

## 📁 Projektstruktur

```
WiiFin/
├── source/          # C++-Quellcode
├── data/            # Grafische Ressourcen (PNG, TTF)
├── Makefile         # devkitPro-kompatibles Build-Skript
└── README.md
```

---

## 🚀 Roadmap

* [ ] Vollständige Jellyfin-API-Unterstützung (Browsen, Wiedergabe)
* [ ] Servererkennung und Einstellungsmenü
* [ ] Lokalisierung / Mehrsprachigkeit

---

## 📸 Screenshots

<img src="https://github.com/fabienmillet/WiiFin/blob/main/assets/preview.png?raw=true" alt="WiiFin Menü-Screenshot" width="500"/><br> <em>WiiFin-Menü im Dolphin-Emulator</em>

---

## 🤝 Mitwirken

WiiFin steht für Pull Requests, Bugreports und neue Ideen offen.

* 📘 Lies die [Beitragsrichtlinien](CONTRIBUTING.md)
* 🐛 Verwende die [Bugreport-Vorlage](.github/ISSUE_TEMPLATE/bug_report.md)
* 💡 Für Ideen verwende die [Feature-Vorschlagsvorlage](.github/ISSUE_TEMPLATE/feature_request.md)

<a href="https://discord.gg/p9DXfEmUYu">
  <img src="https://img.shields.io/badge/Tritt%20unserem%20Discord%20bei-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Discord-Abzeichen"/>
</a>

---

## 📜 Lizenz

Dieses Projekt steht unter der **GPLv3**-Lizenz.
Weitere Infos findest du in der Datei [LICENSE](LICENSE).
