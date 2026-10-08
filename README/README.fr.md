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

## ⚠️ État du projet

> 🚧 **Expérimental** : fonctionnel, mais encore en plein développement. Des imperfections sont possibles sur une vraie console.

### ✅ Ce qui fonctionne

**Connexion**
- Connexion par nom d'utilisateur et mot de passe, ou par **Quick Connect** (validé depuis un autre appareil)
- **Découverte des serveurs** sur le réseau local, ou adresse saisie à la main (HTTP ou **HTTPS**, certificats auto-signés acceptés)
- **Profils enregistrés** : plusieurs comptes et serveurs, seul un jeton d'accès est conservé (aucun mot de passe)

**Navigation**
- Un accueil en **rangées** (Continuer à regarder, À suivre, ajouts récents, mieux notés, favoris, genres) ou en **grille** de bibliothèques
- Bibliothèques de films, séries et musique en affiches ou en listes (avec la pochette du titre sélectionné) ; sauts **A-Z** avec gauche / droite
- **Tri et filtre** d'une bibliothèque de films ou de séries (touche 2) : par nom, année, note ou date d'ajout ; un genre ; tout, non vus, vus ou favoris
- **Recherche** (touche 1 sur l'accueil)
- **Fiche détaillée** : synopsis (en entier avec − quand la fiche le coupe), classification, genres, distribution, pistes audio et sous-titres (celles que le serveur choisit pour vous au départ : votre mode de sous-titres et vos langues, les pistes par défaut et forcées du fichier), plusieurs **versions** d'un film, **favoris** (touche 1, aussi sur une série), **vu / non vu** (+, aussi sur une ligne de la liste des épisodes)
- **Bonus** : bandes-annonces, featurettes, coulisses… d'un film (touche 2 sur sa fiche) ou d'une série (après ses saisons)
- **Séries** : saisons (en affiches, ou en liste avec l'affiche de la saison) et épisodes, **lecture aléatoire** (touche 2), épisode suivant / précédent d'une saison à l'autre
- Titres et sous-titres en **japonais, chinois et coréen** : le japonais est intégré, le chinois et le coréen passent par la police de `apps/WiiFin/fonts/` (fournie dans le paquet pour la Chaîne Homebrew)

**Vidéo**
- Lue par le moteur MPlayer CE intégré : **lecture directe** de ce que la Wii décode en temps réel (DivX/Xvid SD, MPEG-1/2, H.264 jusqu'en 480p, VP8 ; AVI, MKV, MP4, MPEG-TS/PS ; avance rapide dans le fichier lui-même), conversion par le serveur pour le reste, et retour à la conversion quand un fichier ne passe pas bien (voir [DIRECT_PLAY.md](../DIRECT_PLAY.md))
- **Lecteur** : barre de progression avec les vignettes du serveur (Jellyfin 10.9+), volume, pistes audio et sous-titres (les sous-titres texte sont dessinés par WiiFin, la vidéo peut donc rester lue telle quelle), zoom (ajusté / plein écran)
- Boutons **Passer l'intro / le résumé / le générique** à partir des segments du serveur (Jellyfin 10.10+ avec un plugin comme Intro Skipper), et **Épisode suivant** pendant le générique
- **Reprise** là où vous vous étiez arrêté ; progression, pauses et mode de lecture (direct ou converti) envoyés au serveur, chaque console comptant comme un appareil à part
- Une connexion lente fait baisser la qualité d'elle-même ; une image qui se fige est relancée autrement, avec un message qui dit ce qui se passe

**Musique**
- Bibliothèques en onglets comme sur Jellyfin : **Albums, Suggestions, Artistes, Playlists, Morceaux** (une bibliothèque de fichiers sans album s'ouvre sur ses morceaux)
- Un écran **Lecture en cours** avec la pochette, un spectre du son en direct, une file d'attente, lecture aléatoire, répétition, favoris, et des morceaux similaires une fois la file terminée

**Commande à distance** depuis le tableau de bord ou les applis Jellyfin
- **Lire sur… Nintendo Wii** (films, épisodes, musique ; « Lire ensuite » et « Ajouter à la file » pour la musique)
- Lecture / pause, arrêt, avance, suivant / précédent, volume et sourdine (le curseur du tableau de bord suit), pistes audio et sous-titres
- Les **messages** envoyés depuis le tableau de bord s'affichent sur n'importe quel écran

**Et aussi**
- Pointeur de la **Wiimote**, **manette Classique**, **Wii U GamePad** (injections Virtual Console) et **manette GameCube** : tout se fait à la croix, sans barre de capteur (voir [Commandes](#-commandes))
- **Sons de l'interface** et **musique de fond**, remplaçables par vos propres fichiers (voir [Sons personnalisés](#-sons-personnalisés))
- Clavier à l'écran ; calibrage de la zone d'affichage pour les téléviseurs qui rognent l'image ; la lumière de la fente du lecteur peut rester éteinte
- Fourni en `.dol` prêt à l'emploi, en `.wad` installable (Wii / vWii) et en paquet pour la Chaîne Homebrew

### 🎮 Commandes

Le pointeur est facultatif : tous les écrans se pilotent à la croix.

| Wiimote | Manette Classique / Wii U GamePad | Manette GameCube | Action |
|---|---|---|---|
| A / B | A / B | A / B | Valider / Retour |
| Croix | Croix ou stick gauche | Croix ou stick | Se déplacer (maintenir pour répéter) ; gauche / droite : A-Z dans les listes, −10 / +10 s dans le lecteur |
| − / + | − / + ou L / R | L / R | Onglets, pages, épisode ou morceau précédent / suivant ; + : vu / non vu (un film, un épisode) ; − : le synopsis en entier (une fiche) |
| 1 | Y | Y | Recherche (accueil), favori (un film, une série), piste audio (lecteur), file d'attente (musique) |
| 2 | X | X | Tri et filtre (listes), bonus (un film), lecture aléatoire (séries, musique), sous-titres (lecteur) |
| HOME | HOME | START | Menu HOME |
| pointeur | ZL / ZR | Z | Zoom de la vidéo (ajusté / plein écran) ; dans les menus, ce que fait + seul : vu / non vu, la page Browse, Entrée du clavier |

Les aides à l'écran montrent les touches de la dernière manette utilisée (GameCube : A vert, B rouge, L / R, Z, START).

### ⚙️ Paramètres

| Paramètre | |
|---|---|
| SSL Verification | Vérifier le certificat HTTPS du serveur (à désactiver pour les certificats auto-signés) |
| Background Music / Interface Sounds | La musique et les sons des menus |
| Video Quality | Le débit de la conversion, de Low (1,5 Mb/s) à Max (5 Mb/s, pour un adaptateur filaire) ; WiiFin mesure aussi la connexion et reste en dessous |
| Direct Play | Lire tels quels les fichiers que la Wii sait décoder, sans conversion par le serveur |
| Smooth Motion | Fondre les images lors des changements, pour que les films à 24 images/s défilent régulièrement sur un téléviseur à 60 Hz |
| Theme / Home Screen / Library View | Clair, Sombre ou Flix ; rangées ou grille ; affiches, liste, ou liste avec la pochette |
| Disc Slot Light | La lumière qui pulse avec le son pendant la lecture, ou éteinte |
| Screen Area | Réduire l'interface quand le téléviseur rogne les bords |
| Backgrounds | Dégradés doux, ou couleurs unies pour les TV qui y montrent des bandes |
| Clock | 24 h ou 12 h (AM/PM, la date en mois/jour) ; 12 h par défaut sur une Wii américaine |

### 🔊 Sons personnalisés

Placez vos propres sons dans `SD:/apps/WiiFin/sounds/` (à côté de `wiifin.cfg`) : un fichier présent remplace le son intégré du même nom, les autres restent. Ils sont lus au démarrage de WiiFin.

| Fichier | Joué quand |
|---|---|
| `move` | la sélection se déplace (listes, rangées, grilles, paramètres) |
| `open` | une série, un film, une saison ou une bibliothèque s'ouvre |
| `back` | B revient en arrière |
| `page` | on change de page ou d'onglet (− / +) |
| `play` | une vidéo ou un morceau démarre |
| `start` | on appuie sur un bouton du menu principal, on change un paramètre |
| `select` | le pointeur passe sur un bouton du menu principal |
| `press_key` / `backspace` | une touche du clavier à l'écran |
| `loading` | un écran de chargement dure (joué en boucle) |
| `menu_enter` / `menu_exit` | le menu HOME s'ouvre / se ferme |
| `bgm` | la musique de fond (MP3 uniquement, jusqu'à 16 Mo, jouée en boucle) |

Chaque son est un `nom.mp3` ou un `nom.wav` (PCM, 8 ou 16 bits, mono ou stéréo, jusqu'à 48 kHz), de 2 Mo et 10 s au plus (au-delà, il est coupé). Un fichier que WiiFin ne sait pas lire est ignoré, et le log dit pourquoi. Paramètres > Interface Sounds coupe tous les sons ; Background Music coupe la musique.

### ⚠️ Limitations connues

- Lecture directe seulement pour ce que la Wii décode en temps réel (résolutions SD, voir [DIRECT_PLAY.md](../DIRECT_PLAY.md)) ; le serveur convertit le reste
- Son en stéréo uniquement (le multicanal est réduit en stéréo)
- Les sous-titres en image (PGS, VobSub) sont incrustés dans la vidéo par le serveur ; les sous-titres texte sont dessinés par WiiFin
- Le chinois et le coréen demandent la police de `SD:/apps/WiiFin/fonts/` : le paquet pour la Chaîne Homebrew la contient ; avec le WAD, copiez-y `data/fonts/cjk/` (toute autre police `.ttf` / `.otf` posée là sert aussi, pour ce qui manque aux autres)

---

## 🔧 Compilation

### Prérequis

- [devkitPro](https://devkitpro.org) avec `devkitPPC`, `libogc` et les portlibs `wii-dev`
- Graphismes : `GRRLIB`, `libpngu`, `freetype`, `libjpeg`
- mbedTLS (inclus dans `libs/`, compilé par `setup.sh`)
- MPlayer CE sous forme de `libmplayer.a`, pour la lecture : précompilé dans `libs/mplayer-ce-build`, recompilable depuis les sources avec `tools/mplayer/build.sh` (voir [MPLAYER_CE_BUILD.md](../MPLAYER_CE_BUILD.md)). Sans lui, WiiFin compile quand même mais ne peut rien lire.

### Compiler

Sur une machine neuve, `./setup.sh` installe devkitPro et les portlibs, compile GRRLIB et mbedTLS, puis WiiFin (distributions basées sur Arch, ou toute machine avec `dkp-pacman`).

```bash
./build.sh          # WiiFin.dol
./build.sh wad      # WiiFin.wad, la chaîne installable (demande libWiiPy, installé par setup.sh)
```

`make wad` place `WiiFin.dol` dans `tools/wad/template.wad` (bannière, chargeur NAND, ticket et TMD du titre `WIFN`) et le signe : voir `tools/make_wad.py`.

### Lancer

Sur une **vraie Wii** : décompressez `WiiFin-hbc.zip` à la racine de la carte SD (il contient `apps/WiiFin/`) pour lancer WiiFin depuis la Chaîne Homebrew, ou installez `WiiFin.wad` avec un gestionnaire de WAD (fonctionne aussi sur vWii). WiiFin garde ses paramètres, ses profils et son log (`wiifin.log`) dans `SD:/apps/WiiFin/`.

Sur **Dolphin** :

```bash
dolphin-emu -e WiiFin.dol
```

Pour tester avec un Jellyfin local dans Docker, avec des appuis de touches scriptés et des captures d'image, voir [tools/test](../tools/test/README.md).

---

## 📁 Structure du projet

```
WiiFin/
├── source/
│   ├── core/        # Application, paramètres, sessions de lecture, sons et musique, texte, log
│   ├── input/       # Wiimote, manette Classique et manette GameCube
│   ├── jellyfin/    # Client de l'API Jellyfin (HTTPS via mbedTLS), commande à distance (WebSocket)
│   ├── player/      # Intégration de MPlayer CE, sortie vidéo, interface du lecteur, sous-titres, vignettes
│   └── ui/          # Les écrans : connexion, profils, accueil, bibliothèques, fiches, musique, paramètres
├── data/            # Polices, sons, images
├── libs/            # mbedTLS, MPlayer CE précompilé
├── tools/           # Création du WAD, script de liaison, compilation de MPlayer CE (mplayer/)
│   └── test/        # Outils de test : Jellyfin dans Docker, parcours scriptés dans Dolphin
├── apps/WiiFin/     # Métadonnées pour la Chaîne Homebrew
├── DIRECT_PLAY.md   # Ce que la Wii lit telle quelle
└── Makefile
```

---

## 🤝 Contribuer

WiiFin accepte les pull requests, les rapports de bugs et les suggestions.

* 📘 Lisez le [guide de contribution](../CONTRIBUTING.md)
* 🐛 Utilisez le [modèle de rapport de bug](../.github/ISSUE_TEMPLATE/bug_report.yml)
* 💡 Une idée de fonctionnalité ? Utilisez le [modèle de demande](../.github/ISSUE_TEMPLATE/feature_request.yml)

<a href="https://discord.gg/p9DXfEmUYu">
  <img src="https://img.shields.io/badge/Join%20us%20on%20Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Discord"/>
</a>

---

## 📜 Licence

Ce projet est sous licence **GPLv3**.
Voir le fichier [LICENSE](../LICENSE) pour plus de détails.
