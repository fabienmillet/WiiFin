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
<strong>WiiFin</strong> ist ein experimenteller Homebrew-Client für <a href="https://jellyfin.org">Jellyfin</a>, eigens für die Nintendo Wii entwickelt.<br>
Er durchsucht und spielt deine Filme, Serien und Musik direkt auf der Konsole ab, geschrieben in C++ mit <a href="https://github.com/GRRLIB/GRRLIB">GRRLIB</a> und <a href="https://github.com/extremscorner/mplayer-ce">MPlayer CE</a>.
</p>

<table align="center">
  <tr>
    <td><img src="../assets/screenshots/main-menu.png" alt="Hauptmenü" width="420"/></td>
    <td><img src="../assets/screenshots/film.png" alt="Ein Film" width="420"/></td>
  </tr>
  <tr>
    <td><img src="../assets/screenshots/player.png" alt="Der Player" width="420"/></td>
    <td><img src="../assets/screenshots/now-playing.png" alt="Der Musikplayer" width="420"/></td>
  </tr>
</table>
<p align="center"><sub><em>Big Buck Bunny</em> © Blender Foundation, <a href="https://peach.blender.org">peach.blender.org</a> (CC BY 3.0) · <em>Pill</em> von Heuse & Zeus X Crona feat. Emma Sameth, <a href="https://ncs.io">NCS</a></sub></p>

---

## ⚠️ Projektstatus

> 🚧 **Experimentell**: funktionsfähig, aber noch in aktiver Entwicklung. Auf echter Hardware kann es noch hakeln.

### ✅ Was funktioniert

**Verbinden**
- Anmeldung mit Benutzername und Passwort oder per **Quick Connect** (auf einem anderen Gerät bestätigt)
- **Serversuche** im lokalen Netz oder Eingabe der Adresse (HTTP oder **HTTPS**, selbstsignierte Zertifikate werden akzeptiert)
- **Gespeicherte Profile**: mehrere Konten und Server, nur ein Zugriffstoken wird behalten (kein Passwort)

**Durchsuchen**
- Ein Startbildschirm aus **Reihen** (Weiterschauen, Als Nächstes, Neu hinzugefügt, Bestbewertet, Favoriten, Genres) oder ein **Raster** der Mediatheken
- Film-, Serien- und Musikmediatheken als Poster oder Listen (mit dem Cover des gewählten Titels); **A-Z**-Sprünge mit links / rechts
- **Sortieren und filtern** einer Film- oder Serienmediathek (Taste 2): nach Name, Jahr, Bewertung oder Hinzufügedatum; ein Genre; alle, nicht gesehen, gesehen oder Favoriten
- **Suche** (Taste 1 auf dem Startbildschirm)
- **Detailseite**: Inhalt (ganz mit −, wenn die Seite ihn kürzt), Altersfreigabe, Genres, Besetzung, Audio- und Untertitelspuren (zu Beginn die, die der Server für dich wählt: dein Untertitelmodus und deine Sprachen, die Standard- und erzwungenen Spuren der Datei), mehrere **Versionen** eines Films, **Favoriten** (Taste 1, auch bei einer Serie), **gesehen / nicht gesehen** (+, auch in der Episodenliste)
- **Extras**: Trailer, Featurettes, Hinter den Kulissen… eines Films (Taste 2 auf seiner Seite) oder einer Serie (nach ihren Staffeln)
- **Serien**: Staffeln (als Poster oder als Liste mit dem Cover der Staffel) und Episoden, **Zufallswiedergabe** (Taste 2), nächste / vorherige Episode über Staffelgrenzen hinweg
- **Japanische, chinesische und koreanische** Titel und Untertitel: Japanisch ist eingebaut, Chinesisch und Koreanisch kommen aus der Schrift in `apps/WiiFin/fonts/` (im Paket für den Homebrew Channel)

**Video**
- Abgespielt von der integrierten MPlayer-CE-Engine: **Direktwiedergabe** dessen, was die Wii in Echtzeit dekodiert (SD-DivX/Xvid, MPEG-1/2, H.264 bis 480p, VP8; AVI, MKV, MP4, MPEG-TS/PS; Spulen in der Datei selbst), Umwandlung durch den Server für den Rest, und zurück zur Umwandlung, wenn eine Datei nicht flüssig läuft (siehe [DIRECT_PLAY.md](../DIRECT_PLAY.md))
- **Player**: Fortschrittsleiste mit den Vorschaubildern des Servers (Jellyfin 10.9+), Lautstärke, Audio- und Untertitelspuren (Textuntertitel zeichnet WiiFin selbst, das Video kann also unverändert laufen), Zoom (einpassen / füllen)
- Schaltflächen **Intro / Rückblick / Abspann überspringen** aus den Mediensegmenten des Servers (Jellyfin 10.10+ mit einem Plugin wie Intro Skipper), und **Nächste Episode** während des Abspanns
- **Fortsetzen**, wo du aufgehört hast; Fortschritt, Pausen und Wiedergabeart (direkt oder umgewandelt) werden an den Server gemeldet, jede Konsole als eigenes Gerät
- Eine langsame Verbindung senkt die Qualität von selbst; ein hängendes Bild wird auf andere Weise neu versucht, mit einer Meldung, die sagt, was passiert

**Musik**
- Mediatheken mit Reitern wie bei Jellyfin: **Alben, Vorschläge, Künstler, Playlists, Titel** (eine Mediathek aus losen Dateien öffnet sich bei ihren Titeln)
- Ein **Jetzt läuft**-Bildschirm mit Cover, einem Live-Spektrum des Klangs, einer Warteschlange, Zufall, Wiederholen, Favoriten und ähnlichen Titeln, wenn die Warteschlange zu Ende ist

**Fernsteuerung** über das Jellyfin-Dashboard oder die Apps
- **Abspielen auf… Nintendo Wii** (Filme, Episoden, Musik; „Als Nächstes abspielen“ und „Zur Warteschlange hinzufügen“ für Musik)
- Wiedergabe / Pause, Stopp, Spulen, nächster / vorheriger, Lautstärke und Stummschaltung (der Regler im Dashboard folgt), Audio- und Untertitelspuren
- **Nachrichten** aus dem Dashboard erscheinen über jedem Bildschirm

**Außerdem**
- **Wiimote**-Zeiger, **Classic Controller**, **Wii U GamePad** (Virtual-Console-Injects) und **GameCube-Controller**: alles geht mit dem Steuerkreuz, keine Sensorleiste nötig (siehe [Steuerung](#-steuerung))
- **Oberflächenklänge** und **Hintergrundmusik**, beide durch eigene Dateien ersetzbar (siehe [Eigene Klänge](#-eigene-klänge))
- Bildschirmtastatur; Kalibrierung des Bildbereichs für Fernseher, die das Bild beschneiden; das Licht des Laufwerksschachts kann aus bleiben
- Erhältlich als fertige `.dol`, installierbare `.wad` (Wii / vWii) und als Paket für den Homebrew Channel

### 🎮 Steuerung

Der Zeiger ist optional: jeder Bildschirm lässt sich mit dem Steuerkreuz bedienen.

| Wiimote | Classic Controller / Wii U GamePad | GameCube-Controller | Aktion |
|---|---|---|---|
| A / B | A / B | A / B | Auswählen / Zurück |
| Steuerkreuz | Steuerkreuz oder linker Stick | Steuerkreuz oder Stick | Bewegen (gedrückt halten zum Wiederholen); links / rechts: A-Z in Listen, −10 / +10 s im Player |
| − / + | − / + oder L / R | L / R | Reiter, Seiten, vorherige / nächste Episode oder Titel; + : gesehen / nicht gesehen (ein Film, eine Episode); − : der ganze Inhalt (eine Detailseite) |
| 1 | Y | Y | Suche (Start), Favorit (ein Film, eine Serie), Audiospur (Player), Warteschlange (Musik) |
| 2 | X | X | Sortieren und filtern (Listen), Extras (ein Film), Zufall (Serien, Musik), Untertitel (Player) |
| HOME | HOME | START | HOME-Menü |
| Zeiger | ZL / ZR | Z | Video-Zoom (einpassen / füllen); in den Menüs das, was + allein tut: gesehen / nicht gesehen, die Browse-Seite, Enter der Tastatur |

Die Hinweise auf dem Bildschirm zeigen die Tasten des zuletzt benutzten Controllers (GameCube: A grün, B rot, L / R, Z, START).

### ⚙️ Einstellungen

| Einstellung | |
|---|---|
| SSL Verification | Das HTTPS-Zertifikat des Servers prüfen (aus für selbstsignierte) |
| Background Music / Interface Sounds | Musik und Klänge der Menüs |
| Video Quality | Die Bitrate der Umwandlung, von Low (1,5 Mb/s) bis Max (5 Mb/s, für einen LAN-Adapter); WiiFin misst auch die Verbindung und bleibt darunter |
| Direct Play | Dateien, die die Wii dekodieren kann, unverändert abspielen, ohne Umwandlung durch den Server |
| Smooth Motion | Bilder bei Bildwechseln überblenden, damit 24-fps-Filme auf 60-Hz-Fernsehern gleichmäßig laufen |
| Theme / Home Screen / Library View | Hell, Dunkel oder Flix; Reihen oder Raster; Poster, Liste oder Liste mit Cover |
| Disc Slot Light | Das Licht, das während der Wiedergabe mit dem Klang pulsiert, oder aus |
| Screen Area | Die Oberfläche verkleinern, wenn der Fernseher die Ränder abschneidet |
| Backgrounds | Sanfte Verläufe oder einfarbige Flächen für Fernseher, die darin Streifen zeigen |
| Clock | 24 oder 12 Stunden (AM/PM, das Datum als Monat/Tag); 12 Stunden standardmäßig auf einer US-Wii |

### 🔊 Eigene Klänge

Lege deine eigenen Klänge in `SD:/apps/WiiFin/sounds/` (neben `wiifin.cfg`): eine Datei dort ersetzt den eingebauten Klang gleichen Namens, die übrigen bleiben. Sie werden beim Start von WiiFin gelesen.

| Datei | Gespielt, wenn |
|---|---|
| `move` | sich die Auswahl bewegt (Listen, Reihen, Raster, Einstellungen) |
| `open` | sich eine Serie, ein Film, eine Staffel oder eine Mediathek öffnet |
| `back` | B zurückgeht |
| `page` | eine Seite oder ein Reiter wechselt (− / +) |
| `play` | ein Video oder ein Titel startet |
| `start` | ein Knopf im Hauptmenü gedrückt oder eine Einstellung geändert wird |
| `select` | der Zeiger auf einen Knopf des Hauptmenüs fährt |
| `press_key` / `backspace` | eine Taste der Bildschirmtastatur |
| `loading` | ein Ladebildschirm andauert (in Schleife) |
| `menu_enter` / `menu_exit` | sich das HOME-Menü öffnet / schließt |
| `bgm` | Hintergrundmusik (nur MP3, bis 16 MB, in Schleife) |

Jeder Klang ist `name.mp3` oder `name.wav` (PCM, 8 oder 16 Bit, mono oder stereo, bis 48 kHz), höchstens 2 MB und 10 s (längere werden abgeschnitten). Eine Datei, die WiiFin nicht lesen kann, wird ignoriert, und das Log sagt warum. Einstellungen > Interface Sounds schaltet alle Klänge aus; Background Music die Musik.

### ⚠️ Bekannte Einschränkungen

- Direktwiedergabe nur für das, was die Wii in Echtzeit dekodiert (SD-Auflösungen, siehe [DIRECT_PLAY.md](../DIRECT_PLAY.md)); den Rest wandelt der Server um
- Nur Stereo-Ausgabe (Mehrkanalton wird heruntergemischt)
- Bilduntertitel (PGS, VobSub) brennt der Server ins Video ein; Textuntertitel zeichnet WiiFin
- Chinesisch und Koreanisch brauchen die Schrift in `SD:/apps/WiiFin/fonts/`: das Paket für den Homebrew Channel enthält sie; mit der WAD kopiere `data/fonts/cjk/` dorthin (jede andere `.ttf` / `.otf` dort wird auch genutzt, für das, was den anderen fehlt)

---

## 🔧 Kompilieren

### Voraussetzungen

- [devkitPro](https://devkitpro.org) mit `devkitPPC`, `libogc` und den `wii-dev`-Portlibs
- Grafik: `GRRLIB`, `libpngu`, `freetype`, `libjpeg`
- mbedTLS (in `libs/` enthalten, von `setup.sh` kompiliert)
- MPlayer CE als `libmplayer.a`, für die Wiedergabe: vorkompiliert in `libs/mplayer-ce-build`, aus den Quellen neu gebaut mit `tools/mplayer/build.sh` (siehe [MPLAYER_CE_BUILD.md](../MPLAYER_CE_BUILD.md)). Ohne ihn kompiliert WiiFin trotzdem, kann aber nichts abspielen.

### Bauen

Auf einem frischen Rechner installiert `./setup.sh` devkitPro und die Portlibs, baut GRRLIB und mbedTLS und kompiliert dann WiiFin (Arch-basierte Distributionen oder jeder Rechner mit `dkp-pacman`).

```bash
./build.sh          # WiiFin.dol
./build.sh wad      # WiiFin.wad, der installierbare Kanal (braucht libWiiPy, von setup.sh installiert)
```

`make wad` legt `WiiFin.dol` in `tools/wad/template.wad` (Banner, NAND-Loader, Ticket und TMD des Titels `WIFN`) und signiert es: siehe `tools/make_wad.py`.

### Starten

Auf **echter Wii-Hardware**: entpacke `WiiFin-hbc.zip` ins Hauptverzeichnis der SD-Karte (es enthält `apps/WiiFin/`), um WiiFin aus dem Homebrew Channel zu starten, oder installiere `WiiFin.wad` mit einem WAD-Manager (geht auch auf der vWii). WiiFin legt seine Einstellungen, Profile und sein Log (`wiifin.log`) in `SD:/apps/WiiFin/` ab.

Im **Dolphin-Emulator**:

```bash
dolphin-emu -e WiiFin.dol
```

Zum Testen mit einem lokalen Jellyfin in Docker, mit geskripteten Tastendrücken und Bildaufnahmen, siehe [tools/test](../tools/test/README.md).

---

## 📁 Projektstruktur

```
WiiFin/
├── source/
│   ├── core/        # App, Einstellungen, Wiedergabesitzungen, Klänge und Musik, Text, Log
│   ├── input/       # Wiimote, Classic Controller und GameCube-Controller
│   ├── jellyfin/    # Jellyfin-API-Client (HTTPS über mbedTLS), Fernsteuerung (WebSocket)
│   ├── player/      # MPlayer-CE-Anbindung, Videoausgabe, Player-Oberfläche, Untertitel, Vorschaubilder
│   └── ui/          # Die Bildschirme: Verbindung, Profile, Start, Mediatheken, Details, Musik, Einstellungen
├── data/            # Schriften, Klänge, Bilder
├── libs/            # mbedTLS, das vorkompilierte MPlayer CE
├── tools/           # WAD-Packer, Linker-Skript, Bau von MPlayer CE (mplayer/)
│   └── test/        # Testumgebung: Jellyfin in Docker, geskriptete Läufe in Dolphin
├── apps/WiiFin/     # Metadaten für den Homebrew Channel
├── DIRECT_PLAY.md   # Was die Wii unverändert abspielt
└── Makefile
```

---

## 🤝 Mitwirken

WiiFin freut sich über Pull Requests, Fehlerberichte und Vorschläge.

* 📘 Lies die [Richtlinien zum Mitwirken](../CONTRIBUTING.md)
* 🐛 Nutze die [Vorlage für Fehlerberichte](../.github/ISSUE_TEMPLATE/bug_report.yml)
* 💡 Eine Idee? Nutze die [Vorlage für Funktionswünsche](../.github/ISSUE_TEMPLATE/feature_request.yml)

<a href="https://discord.gg/p9DXfEmUYu">
  <img src="https://img.shields.io/badge/Join%20us%20on%20Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Discord"/>
</a>

---

## 📜 Lizenz

Dieses Projekt steht unter der **GPLv3**.
Siehe die Datei [LICENSE](../LICENSE) für Details.
