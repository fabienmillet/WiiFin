<p align="center">
  <img src="https://raw.githubusercontent.com/fabienmillet/WiiFin/refs/heads/main/assets/logo_wiifin_banner.png" alt="WiiFin-Logo" width="600"/><br>
  <em>Jellyfin-Client für die Nintendo Wii</em>
</p>

<p align="center">
  <a href="../README.md"><img src="https://flagcdn.com/w40/gb.png" width="28" alt="English"/></a>
  &nbsp;
  <a href="README.fr.md"><img src="https://flagcdn.com/w40/fr.png" width="28" alt="Français"/></a>
  &nbsp;
  <a href="README.de.md"><img src="https://flagcdn.com/w40/de.png" width="28" alt="Deutsch"/></a>
  &nbsp;
  <a href="README.es.md"><img src="https://flagcdn.com/w40/es.png" width="28" alt="Español"/></a>
  &nbsp;
  <a href="README.it.md"><img src="https://flagcdn.com/w40/it.png" width="28" alt="Italiano"/></a>
</p>

---

<p align="center">
<strong>WiiFin</strong> ist ein experimenteller Homebrew-Client für <a href="https://jellyfin.org">Jellyfin</a>, der speziell für die Nintendo Wii entwickelt wurde.<br>
Er bietet ein leichtgewichtiges, konsolenfreundliches Erlebnis zum Durchsuchen und Abspielen von Medien, geschrieben in C++ mit <a href="https://github.com/GRRLIB/GRRLIB">GRRLIB</a> und <a href="https://github.com/dborth/mplayer-ce">MPlayer CE</a>.
</p>

---

## ⚠️ Projektstatus

> 🚧 **Experimentell** – funktionsfähig, aber noch in aktiver Entwicklung. Auf echter Hardware können Fehler auftreten.

### ✅ Was funktioniert:
- **Authentifizierung**: Anmeldung mit Benutzername/Passwort oder per **QuickConnect** (Bestätigung auf einem anderen Gerät)
- **Gespeicherte Profile**: Mehrere Konten werden sicher gespeichert (nur Access-Token, kein Passwort)
- **Mediatheken durchsuchen**: Filme, Serien, Musik mit Cover-Art vom Server
- **Detailansicht**: Synopsis, Altersfreigabe, Genres, Besetzung, Regie, Auswahl von Audio- und Untertitelspuren
- **Weiterschauen** und **Als Nächstes**
- **Serien**: Navigation nach Staffeln und Episoden
- **Videowiedergabe**: Serverseitiges Transcoding, gestreamt über die integrierte MPlayer-CE-Engine
- **Musikwiedergabe**: Audio-Bibliotheken, Album-/Titel-Navigation
- **Player-Overlay**: Fortschrittsleiste, Lautstärke, nächste/vorherige Episode, Audio- und Untertitelspurwechsel, Intro überspringen
- **Wiedergabe-Reporting**: Fortschritt wird an den Jellyfin-Server gemeldet (Weiterschauen wo man aufgehört hat)
- **HTTPS**: TLS-Verbindungen via mbedTLS (selbstsignierte Zertifikate werden unterstützt)
- **Wiimote IR-Zeiger** und **virtuelle Bildschirmtastatur**
- **Hintergrundmusik** in den Menüs
- Wird als sofort nutzbares `.dol` und installierbares `.wad` (Wii / vWii) mitgeliefert

### ⚠️ Bekannte Einschränkungen:
- Kein Direct-Play; alle Videos werden serverseitig transcodiert
- Kein 5.1-Surround-Sound (nur Stereo via Transcoding)
- Untertitel werden vom Server in den Videostream eingebettet

---

## 🔧 Build-Anleitung

### Voraussetzungen:

- [devkitPro](https://devkitpro.org) mit `devkitPPC`, `libogc` und den `wii-dev`-Portlibs
- Grafikbibliotheken: `GRRLIB`, `libpngu`, `freetype`, `libjpeg`
- mbedTLS (enthalten in `libs/`, wird automatisch von der CI kompiliert)
- **Optional**: MPlayer CE als `libmplayer.a` – erforderlich für die Videowiedergabe. Siehe [MPLAYER_CE_BUILD.md](../MPLAYER_CE_BUILD.md). Ohne diese Bibliothek kompiliert WiiFin, aber die Videowiedergabe ist nicht verfügbar.

### Kompilierung:

```bash
./build.sh
```

### Ausführen:

Im **Dolphin-Emulator**:

```bash
dolphin-emu -e WiiFin.dol
```

Auf echter **Wii-Hardware**: `WiiFin.dol` nach `SD:/apps/WiiFin/boot.dol` kopieren, oder `WiiFin.wad` mit einem WAD-Manager installieren (auch auf vWii).

---

## 📁 Projektstruktur

```
WiiFin/
├── source/
│   ├── core/        # App-Lebenszyklus, Hintergrundmusik, Hilfsfunktionen
│   ├── input/       # Wiimote- & USB-Tastatur-Eingabe
│   ├── jellyfin/    # Jellyfin-HTTP-API-Client (HTTPS via mbedTLS)
│   ├── player/      # MPlayer-CE-Integration, Player-Overlay-HUD
│   └── ui/          # Alle Views: Connect, Library, Profile, Settings
├── data/            # Grafische Assets (PNG/TTF)
├── libs/            # Mitgeliefertes mbedTLS
├── tools/           # WAD-Packer, Banner-Generator
├── Makefile         # DevkitPro-kompatibles Build-Skript
└── apps/WiiFin/     # Homebrew-Channel-Metadaten
```

---

## 🚀 Roadmap

* [ ] Sortieren/Filtern von Inhalten (nach Jahr, Genre, Bewertung)
* [ ] Inhalte als Favoriten von der Wii aus markieren
* [ ] Mehrere UI-Farbthemen

---

## 📸 Screenshots

<img src="https://github.com/fabienmillet/WiiFin/blob/main/assets/preview.png?raw=true" alt="WiiFin Screenshot" width="500"/><br> <em>WiiFin im Dolphin-Emulator</em>

---

## 🤝 Mitwirken

WiiFin steht für Pull Requests, Bugreports und neue Ideen offen.

* 📘 Lies die [Beitragsrichtlinien](../CONTRIBUTING.md)
* 🐛 Nutze die [Bugreport-Vorlage](../.github/ISSUE_TEMPLATE/bug_report.md)
* 💡 Idee? Nutze die [Feature-Request-Vorlage](../.github/ISSUE_TEMPLATE/feature_request.md)

<a href="https://discord.gg/p9DXfEmUYu">
  <img src="https://img.shields.io/badge/Discord%20beitreten-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Discord-Badge"/>
</a>

---

## 📜 Lizenz

Dieses Projekt steht unter der **GPLv3**-Lizenz.
Weitere Informationen findest du in der Datei [LICENSE](../LICENSE).
