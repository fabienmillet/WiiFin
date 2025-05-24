<p align="center">
  <img src="https://raw.githubusercontent.com/fabienmillet/WiiFin/refs/heads/main/assets/logo_wiifin_banner.png" alt="Logo WiiFin" width="600"/><br>
  <em>Client Jellyfin pour la Nintendo Wii</em>
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
<strong>WiiFin</strong> est un client homebrew très expérimental pour <a href="https://jellyfin.org">Jellyfin</a>, développé spécifiquement pour la Nintendo Wii.  
Il vise à offrir une expérience de navigation multimédia légère et adaptée à la console, codée en C++ avec <a href="https://github.com/GRRLIB/GRRLIB">GRRLIB</a>.
</p>

---

## ⚠️ État du projet

> 🚧 **En cours de développement** – ce projet est activement développé mais ne permet pas encore la lecture de contenu multimédia.

### ✅ Fonctionnel actuellement :
- L’interface s’affiche correctement sur une vraie Wii et dans l’émulateur Dolphin.

### ❌ Non fonctionnel pour l’instant :
- Aucune intégration de l’API Jellyfin.
- Aucune lecture de fichiers multimédia.

Les priorités de développement actuelles sont :
- Mise en page de l’interface
- Support de la Wiimote
- Structure et navigation de base

---

## 🔧 Instructions de compilation

### Prérequis :

- [devkitPro](https://devkitpro.org)
- `devkitPPC`, `libogc`
- Bibliothèques graphiques : `GRRLIB`, `libpngu`, `freetype`, `libjpeg`

### Compilation :

```bash
./build.sh
````

### Exécution :

Sur **émulateur Dolphin** :

```bash
dolphin-emu -e WiiFin.elf
```

Sur une **Wii réelle**, convertis le fichier ELF en `.dol` ou `.wad`.

---

## 📁 Structure du projet

```
WiiFin/
├── source/          # Fichiers source C++ principaux
├── data/            # Assets graphiques (PNG, TTF)
├── Makefile         # Script de build compatible devkitPro
└── README.md
```

---

## 🚀 Feuille de route

* [ ] Intégration complète de l’API Jellyfin (navigation, lecture)
* [ ] Détection automatique des serveurs + interface de configuration
* [ ] Localisation / support multilingue

---

## 📸 Captures d’écran

<img src="https://github.com/fabienmillet/WiiFin/blob/main/assets/preview.png?raw=true" alt="Capture du menu WiiFin" width="500"/><br> <em>Menu de WiiFin dans l’émulateur Dolphin</em>

---

## 🤝 Contribuer

WiiFin est ouvert aux pull requests, rapports de bugs et suggestions.

* 📘 Lis le fichier [CONTRIBUTING.md](CONTRIBUTING.md)
* 🐛 Utilise le [modèle de rapport de bug](.github/ISSUE_TEMPLATE/bug_report.md)
* 💡 Une idée ? Utilise le [modèle de demande de fonctionnalité](.github/ISSUE_TEMPLATE/feature_request.md)

<a href="https://discord.gg/p9DXfEmUYu">
 <img src="https://img.shields.io/badge/Rejoins-nous%20sur%20Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Badge Discord"/>
</a>

---

## 📜 Licence

Ce projet est sous licence **GPLv3**.
Voir le fichier [LICENSE](LICENSE) pour plus d’informations.
