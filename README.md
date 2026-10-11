<p align="center">
  <img src="https://raw.githubusercontent.com/fabienmillet/WiiFin/refs/heads/main/assets/logo_wiifin_banner.png" alt="WiiFin logo" width="600"/><br>
  <em>Jellyfin client for the Nintendo Wii</em>
</p>

<p align="center">
  <a href="README.md"><img src="https://flagcdn.com/w40/gb.png" width="28" alt="English"/></a>
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
<strong>WiiFin</strong> is an experimental homebrew client for <a href="https://jellyfin.org">Jellyfin</a>, built specifically for the Nintendo Wii.<br>
It browses and plays your films, series and music on the console itself, written in C++ with <a href="https://github.com/GRRLIB/GRRLIB">GRRLIB</a> and <a href="https://github.com/extremscorner/mplayer-ce">MPlayer CE</a>.
</p>

<table align="center">
  <tr>
    <td><img src="assets/screenshots/main-menu.png" alt="Main menu" width="420"/></td>
    <td><img src="assets/screenshots/film.png" alt="A film" width="420"/></td>
  </tr>
  <tr>
    <td><img src="assets/screenshots/player.png" alt="The player" width="420"/></td>
    <td><img src="assets/screenshots/now-playing.png" alt="The music player" width="420"/></td>
  </tr>
</table>
<p align="center"><sub><em>Big Buck Bunny</em> © Blender Foundation, <a href="https://peach.blender.org">peach.blender.org</a> (CC BY 3.0) · <em>Pill</em> by Heuse & Zeus X Crona feat. Emma Sameth, <a href="https://ncs.io">NCS</a></sub></p>

---

## 📦 Install

Download the files from the [latest release](https://github.com/fabienmillet/WiiFin/releases):

- **Homebrew Channel**: unzip `WiiFin-hbc.zip` at the root of your SD card
- **Wii Menu channel**: install `WiiFin.wad` with a WAD manager (Wii and vWii); for Chinese or Korean, also copy `apps/WiiFin/fonts/` from the zip to the SD card
- **Dolphin**: open `WiiFin.dol`

Then sign in to your Jellyfin server with your username and password, or with Quick Connect.

## ✨ Highlights

- **Films, series and music** from your Jellyfin: direct play of what the Wii decodes, the server converts the rest
- A home screen of **rows** or a **grid**, tabs, sort & filter, search, A-Z
- A **player** with the server's thumbnails, scenes and chapters, skip intro / credits, next episode, audio and subtitle tracks, resume
- **Music**: a Now Playing screen with visualizers and synced lyrics; it keeps playing while you browse
- **Photos** with a slideshow, and playlists of videos
- **Profiles**: several accounts and servers, a passcode if you want, each its own theme
- **Remote control** from Jellyfin's dashboard and apps: Play on... Nintendo Wii
- The **Wii's ten languages**; Wii Remote, Classic Controller, GameCube controller or Wii U GamePad, no sensor bar needed

Everything it does, the controls, the settings and custom sounds: the **[user guide](docs/USER_GUIDE.md)**.

## ⚠️ Project status

> 🚧 **Experimental**: functional but still under active development. Expect rough edges on real hardware.

- Direct play for SD videos only (see [DIRECT_PLAY.md](DIRECT_PLAY.md)); stereo sound
- Picture subtitles (PGS, VobSub) are burned in by the server
- More in the [known limitations](docs/USER_GUIDE.md#%EF%B8%8F-known-limitations)

## 🤝 Contributing

Pull requests, bug reports and ideas are welcome:

* 📘 The [contribution guidelines](CONTRIBUTING.md), and how to [build WiiFin](docs/BUILDING.md)
* 🌍 [Translating](lang/README.md) WiiFin into your language
* 🐛 A [bug report](.github/ISSUE_TEMPLATE/bug_report.yml) (with `apps/WiiFin/wiifin.log` from the SD card) or a 💡 [feature request](.github/ISSUE_TEMPLATE/feature_request.yml)

<a href="https://discord.gg/p9DXfEmUYu">
  <img src="https://img.shields.io/badge/Join%20us%20on%20Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Discord Badge"/>
</a>

## 📜 License

This project is licensed under the **GPLv3**.
See the [LICENSE](LICENSE) file for more details.
