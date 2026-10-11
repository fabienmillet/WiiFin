<p align="center">
  <img src="https://raw.githubusercontent.com/fabienmillet/WiiFin/refs/heads/main/assets/logo_wiifin_banner.png" alt="Logo WiiFin" width="600"/><br>
  <em>Client Jellyfin pour la Nintendo Wii</em>
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
<strong>WiiFin</strong> est un client homebrew expérimental pour <a href="https://jellyfin.org">Jellyfin</a>, conçu pour la Nintendo Wii.<br>
Il parcourt et lit vos films, séries et musiques sur la console elle-même. Il est écrit en C++ avec <a href="https://github.com/GRRLIB/GRRLIB">GRRLIB</a> et <a href="https://github.com/extremscorner/mplayer-ce">MPlayer CE</a>.
</p>

<table align="center">
  <tr>
    <td><img src="../assets/screenshots/main-menu.png" alt="Menu principal" width="420"/></td>
    <td><img src="../assets/screenshots/film.png" alt="Un film" width="420"/></td>
  </tr>
  <tr>
    <td><img src="../assets/screenshots/player.png" alt="Le lecteur" width="420"/></td>
    <td><img src="../assets/screenshots/now-playing.png" alt="Le lecteur de musique" width="420"/></td>
  </tr>
</table>
<p align="center"><sub><em>Big Buck Bunny</em> © Blender Foundation, <a href="https://peach.blender.org">peach.blender.org</a> (CC BY 3.0) · <em>Pill</em> de Heuse & Zeus X Crona feat. Emma Sameth, <a href="https://ncs.io">NCS</a></sub></p>

---

## 📦 Installation

Téléchargez les fichiers de la [dernière version](https://github.com/fabienmillet/WiiFin/releases) :

- **Chaîne Homebrew** : décompressez `WiiFin-hbc.zip` à la racine de votre carte SD
- **Chaîne du menu Wii** : installez `WiiFin.wad` avec un gestionnaire de WAD (Wii et vWii) ; pour le chinois ou le coréen, copiez aussi `apps/WiiFin/fonts/` du zip sur la carte SD
- **Dolphin** : ouvrez `WiiFin.dol`

Connectez-vous ensuite à votre serveur Jellyfin avec votre nom d'utilisateur et votre mot de passe, ou par Quick Connect.

## ✨ Points forts

- **Films, séries et musique** de votre Jellyfin : lecture directe de ce que la Wii décode, le serveur convertit le reste
- Un accueil en **rangées** ou en **grille**, des onglets, tri et filtre, recherche, A-Z
- Un **lecteur** avec les vignettes du serveur, les scènes et les chapitres, passer l'intro / le générique, épisode suivant, pistes audio et sous-titres, reprise
- **Musique** : un écran En lecture avec des visualiseurs et les paroles synchronisées ; elle continue pendant la navigation
- **Photos** avec un diaporama, et playlists de vidéos
- **Profils** : plusieurs comptes et serveurs, un code si vous le souhaitez, chacun avec son thème
- **Commande à distance** depuis le tableau de bord et les applis Jellyfin : Lire sur… Nintendo Wii
- Les **dix langues de la Wii** ; Wiimote, manette Classique, manette GameCube ou Wii U GamePad, sans barre de capteur

Tout ce qu'il fait, les commandes, les paramètres et les sons personnalisés : le **[guide d'utilisation](../docs/USER_GUIDE.md)** (en anglais).

## ⚠️ État du projet

> 🚧 **Expérimental** : fonctionnel, mais encore en plein développement. Des imperfections sont possibles sur une vraie console.

- Lecture directe des vidéos SD seulement (voir [DIRECT_PLAY.md](../DIRECT_PLAY.md)) ; son en stéréo
- Les sous-titres en image (PGS, VobSub) sont incrustés par le serveur
- La suite dans les [limitations connues](../docs/USER_GUIDE.md#%EF%B8%8F-known-limitations)

## 🤝 Contribuer

Les pull requests, les rapports de bugs et les idées sont les bienvenus :

* 📘 Le [guide de contribution](../CONTRIBUTING.md), et comment [compiler WiiFin](../docs/BUILDING.md)
* 🌍 [Traduire](../lang/README.md) WiiFin dans votre langue
* 🐛 Un [rapport de bug](../.github/ISSUE_TEMPLATE/bug_report.yml) (avec `apps/WiiFin/wiifin.log` de la carte SD) ou une 💡 [demande de fonctionnalité](../.github/ISSUE_TEMPLATE/feature_request.yml)

<a href="https://discord.gg/p9DXfEmUYu">
  <img src="https://img.shields.io/badge/Join%20us%20on%20Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Discord"/>
</a>

## 📜 Licence

Ce projet est sous licence **GPLv3**.
Voir le fichier [LICENSE](../LICENSE) pour plus de détails.
