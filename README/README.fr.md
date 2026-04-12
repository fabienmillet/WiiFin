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
<strong>WiiFin</strong> est un client homebrew expérimental pour <a href="https://jellyfin.org">Jellyfin</a>, développé spécifiquement pour la Nintendo Wii.<br>
Il offre une expérience légère et adaptée à la console pour naviguer et lire du contenu multimédia, écrit en C++ avec <a href="https://github.com/GRRLIB/GRRLIB">GRRLIB</a> et <a href="https://github.com/dborth/mplayer-ce">MPlayer CE</a>.
</p>

---

## ⚠️ État du projet

> 🚧 **Expérimental** – fonctionnel mais encore en développement actif. Des imperfections sont possibles sur hardware réel.

### ✅ Ce qui fonctionne :
- **Authentification** : connexion par identifiants ou via **QuickConnect** (validation depuis un autre appareil)
- **Profils sauvegardés** : plusieurs comptes stockés de façon sécurisée (token d'accès uniquement, aucun mot de passe conservé)
- **Navigation dans les bibliothèques** : films, séries, musique avec pochettes chargées depuis le serveur
- **Vue détaillée** : synopsis, classification, genres, casting, réalisateur, sélection des pistes audio et sous-titres
- **Continuer à regarder** et **Prochains épisodes**
- **Séries TV** : navigation par saison et par épisode
- **Lecture vidéo** : transcodage côté serveur, diffusé via le moteur MPlayer CE intégré
- **Lecture musicale** : bibliothèques audio, navigation albums/pistes
- **Overlay lecteur** : barre de progression, volume, épisode précédent/suivant, changement de piste audio et sous-titres, saut d'introduction
- **Rapport de lecture** : progression envoyée au serveur Jellyfin (reprise là où on s'est arrêté)
- **HTTPS** : connexions TLS via mbedTLS (certificats auto-signés acceptés)
- **Pointeur IR** de la Wiimote et **clavier virtuel** à l'écran
- **Musique de fond** dans les menus
- Livré en `.dol` prêt à l'emploi et en `.wad` installable (Wii / vWii)

### ⚠️ Limitations connues :
- Pas de lecture directe (direct-play) ; toute la vidéo est transcodée par le serveur
- Pas d'audio multicanal 5.1 (stéréo uniquement via le transcodage)
- Les sous-titres sont intégrés dans le flux vidéo par le serveur

---

## 🔧 Instructions de compilation

### Prérequis :

- [devkitPro](https://devkitpro.org) avec `devkitPPC`, `libogc` et les portlibs `wii-dev`
- Graphismes : `GRRLIB`, `libpngu`, `freetype`, `libjpeg`
- mbedTLS (inclus dans `libs/`, compilé automatiquement par la CI)
- **Optionnel** : MPlayer CE compilé en `libmplayer.a` — requis pour la lecture vidéo. Voir [MPLAYER_CE_BUILD.md](../MPLAYER_CE_BUILD.md). Sans lui, WiiFin compile mais la lecture vidéo est indisponible.

### Compilation :

```bash
./build.sh
```

### Exécution :

Sur **l'émulateur Dolphin** :

```bash
dolphin-emu -e WiiFin.dol
```

Sur une **Wii réelle** : copiez `WiiFin.dol` dans `SD:/apps/WiiFin/boot.dol`, ou installez `WiiFin.wad` via un gestionnaire WAD (compatible vWii).

---

## 📁 Structure du projet

```
WiiFin/
├── source/
│   ├── core/        # Cycle de vie de l'app, musique de fond, utilitaires
│   ├── input/       # Saisie Wiimote + clavier USB
│   ├── jellyfin/    # Client API HTTP Jellyfin (HTTPS via mbedTLS)
│   ├── player/      # Intégration MPlayer CE, overlay HUD du lecteur
│   └── ui/          # Toutes les vues : Connect, Library, Profile, Settings
├── data/            # Assets graphiques (PNG/TTF)
├── libs/            # mbedTLS intégré
├── tools/           # Packager WAD, générateur de bannière
├── Makefile         # Script de build compatible devkitPro
└── apps/WiiFin/     # Métadonnées Homebrew Channel
```

---

## 🚀 Feuille de route

* [ ] Tri/filtre des contenus (par année, genre, note)
* [ ] Marquer des éléments en favoris depuis la Wii
* [ ] Plusieurs thèmes de couleurs pour l'interface

---

## 📸 Captures d'écran

<img src="https://github.com/fabienmillet/WiiFin/blob/main/assets/preview.png?raw=true" alt="Capture de WiiFin" width="500"/><br> <em>WiiFin dans l'émulateur Dolphin</em>

---

## 🤝 Contribuer

WiiFin est ouvert aux pull requests, rapports de bugs et suggestions.

* 📘 Lisez les [directives de contribution](../CONTRIBUTING.md)
* 🐛 Utilisez le [modèle de rapport de bug](../.github/ISSUE_TEMPLATE/bug_report.md)
* 💡 Une idée ? Utilisez le [modèle de demande de fonctionnalité](../.github/ISSUE_TEMPLATE/feature_request.md)

<a href="https://discord.gg/p9DXfEmUYu">
  <img src="https://img.shields.io/badge/Rejoindre%20Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Badge Discord"/>
</a>

---

## 📜 Licence

Ce projet est sous licence **GPLv3**.
Voir le fichier [LICENSE](../LICENSE) pour plus d'informations.
