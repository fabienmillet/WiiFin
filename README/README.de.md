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

## 📦 Installation

Lade die Dateien aus dem [neuesten Release](https://github.com/fabienmillet/WiiFin/releases) herunter:

- **Homebrew Channel**: entpacke `WiiFin-hbc.zip` ins Hauptverzeichnis deiner SD-Karte
- **Kanal im Wii-Menü**: installiere `WiiFin.wad` mit einem WAD-Manager (Wii und vWii); für Chinesisch oder Koreanisch kopiere auch `apps/WiiFin/fonts/` aus dem Zip auf die SD-Karte
- **Dolphin**: öffne `WiiFin.dol`

Melde dich dann mit Benutzername und Passwort oder per Quick Connect an deinem Jellyfin-Server an.

## ✨ Highlights

- **Filme, Serien und Musik** aus deinem Jellyfin: Direktwiedergabe dessen, was die Wii dekodiert, den Rest wandelt der Server um
- Ein Startbildschirm aus **Reihen** oder als **Raster**, Reiter, Sortieren und filtern, Suche, A-Z
- Ein **Player** mit den Vorschaubildern des Servers, Szenen und Kapiteln, Intro / Abspann überspringen, nächste Episode, Audio- und Untertitelspuren, Fortsetzen
- **Musik**: ein „Wird gespielt“-Bildschirm mit Visualisierungen und synchronem Liedtext; sie läuft beim Stöbern weiter
- **Fotos** mit Diashow, und Video-Playlists
- **Profile**: mehrere Konten und Server, eine PIN, wenn du willst, jedes mit eigenem Design
- **Fernsteuerung** über das Jellyfin-Dashboard und die Apps: Abspielen auf… Nintendo Wii
- Die **zehn Sprachen der Wii**; Wiimote, Classic Controller, GameCube-Controller oder Wii U GamePad, keine Sensorleiste nötig

Alles, was er kann, die Steuerung, die Einstellungen und eigene Klänge: das **[Benutzerhandbuch](../docs/USER_GUIDE.md)** (auf Englisch).

## ⚠️ Projektstatus

> 🚧 **Experimentell**: funktionsfähig, aber noch in aktiver Entwicklung. Auf echter Hardware kann es noch hakeln.

- Direktwiedergabe nur für SD-Videos (siehe [DIRECT_PLAY.md](../DIRECT_PLAY.md)); Stereoton
- Bilduntertitel (PGS, VobSub) brennt der Server ins Video ein
- Mehr unter den [bekannten Einschränkungen](../docs/USER_GUIDE.md#%EF%B8%8F-known-limitations)

## 🤝 Mitwirken

Pull Requests, Fehlerberichte und Ideen sind willkommen:

* 📘 Die [Richtlinien zum Mitwirken](../CONTRIBUTING.md) und wie man [WiiFin kompiliert](../docs/BUILDING.md)
* 🌍 WiiFin in deine Sprache [übersetzen](../lang/README.md)
* 🐛 Ein [Fehlerbericht](../.github/ISSUE_TEMPLATE/bug_report.yml) (mit `apps/WiiFin/wiifin.log` von der SD-Karte) oder ein 💡 [Funktionswunsch](../.github/ISSUE_TEMPLATE/feature_request.yml)

<a href="https://discord.gg/p9DXfEmUYu">
  <img src="https://img.shields.io/badge/Join%20us%20on%20Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Discord"/>
</a>

## 📜 Lizenz

Dieses Projekt steht unter der **GPLv3**.
Siehe die Datei [LICENSE](../LICENSE) für Details.
