<p align="center">
  <img src="https://raw.githubusercontent.com/fabienmillet/WiiFin/refs/heads/main/assets/logo_wiifin_banner.png" alt="Logo WiiFin" width="600"/><br>
  <em>Client Jellyfin per Nintendo Wii</em>
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
<strong>WiiFin</strong> è un client homebrew sperimentale per <a href="https://jellyfin.org">Jellyfin</a>, progettato specificamente per la Nintendo Wii.<br>
Offre un'esperienza leggera e adatta alla console per navigare e riprodurre contenuti multimediali, scritto in C++ con <a href="https://github.com/GRRLIB/GRRLIB">GRRLIB</a> e <a href="https://github.com/dborth/mplayer-ce">MPlayer CE</a>.
</p>

---

## ⚠️ Stato del progetto

> 🚧 **Sperimentale** – funzionante, ma ancora in sviluppo attivo. Possibili irregolarità su hardware reale.

### ✅ Cosa funziona:
- **Autenticazione**: accesso con nome utente/password o tramite **QuickConnect** (approvazione da un altro dispositivo)
- **Profili salvati**: più account memorizzati in modo sicuro (solo token di accesso, nessuna password salvata)
- **Navigazione nelle librerie**: film, serie TV, musica con copertine caricate dal server
- **Vista dettaglio**: sinossi, classificazione, generi, cast, regista, selezione delle tracce audio e sottotitoli
- **Continua a guardare** e **Prossimi episodi**
- **Serie TV**: navigazione per stagione ed episodio
- **Riproduzione video**: transcodifica lato server, trasmessa tramite il motore MPlayer CE integrato
- **Riproduzione musicale**: librerie audio, navigazione album/tracce
- **Overlay del player**: barra di avanzamento, volume, episodio precedente/successivo, cambio traccia audio e sottotitoli, salta sigla
- **Report di riproduzione**: il progresso viene inviato al server Jellyfin (riprendi da dove hai lasciato)
- **HTTPS**: connessioni TLS tramite mbedTLS (certificati autofirmati supportati)
- **Puntatore IR** del Wiimote e **tastiera virtuale** a schermo
- **Musica di sottofondo** nei menu
- Distribuito come `.dol` pronto all'uso e `.wad` installabile (Wii / vWii)

### ⚠️ Limitazioni note:
- Nessuna riproduzione diretta (direct-play); tutto il video viene transcodificato dal server
- Nessun audio multicanale 5.1 (solo stereo tramite transcodifica)
- I sottotitoli vengono incorporati nel flusso video dal server

---

## 🔧 Istruzioni di compilazione

### Requisiti:

- [devkitPro](https://devkitpro.org) con `devkitPPC`, `libogc` e i portlib `wii-dev`
- Librerie grafiche: `GRRLIB`, `libpngu`, `freetype`, `libjpeg`
- mbedTLS (incluso in `libs/`, compilato automaticamente dalla CI)
- **Opzionale**: MPlayer CE compilato come `libmplayer.a` — necessario per la riproduzione video. Vedi [MPLAYER_CE_BUILD.md](../MPLAYER_CE_BUILD.md). Senza di esso WiiFin compila, ma la riproduzione video non è disponibile.

### Compilazione:

```bash
./build.sh
```

### Esecuzione:

Su **Dolphin Emulator**:

```bash
dolphin-emu -e WiiFin.dol
```

Su **hardware Wii reale**: copia `WiiFin.dol` in `SD:/apps/WiiFin/boot.dol`, oppure installa `WiiFin.wad` tramite un WAD manager (compatibile con vWii).

---

## 📁 Struttura del progetto

```
WiiFin/
├── source/
│   ├── core/        # Ciclo di vita dell'app, musica di sottofondo, utilità
│   ├── input/       # Input Wiimote + tastiera USB
│   ├── jellyfin/    # Client HTTP API Jellyfin (HTTPS con mbedTLS)
│   ├── player/      # Integrazione MPlayer CE, HUD overlay del player
│   └── ui/          # Tutte le view: Connect, Library, Profile, Settings
├── data/            # Asset grafici (PNG/TTF)
├── libs/            # mbedTLS incluso
├── tools/           # Packager WAD, generatore di banner
├── Makefile         # Script di build compatibile con devkitPro
└── apps/WiiFin/     # Metadati per Homebrew Channel
```

---

## 🚀 Roadmap

* [ ] Ordinamento/filtro dei contenuti (per anno, genere, valutazione)
* [ ] Aggiungere elementi ai preferiti dalla Wii
* [ ] Temi di colore multipli per l'interfaccia

---

## 📸 Screenshot

<img src="https://github.com/fabienmillet/WiiFin/blob/main/assets/preview.png?raw=true" alt="Screenshot di WiiFin" width="500"/><br> <em>WiiFin in esecuzione su Dolphin Emulator</em>

---

## 🤝 Contribuire

WiiFin è aperto a pull request, segnalazioni di bug e proposte di nuove funzionalità.

* 📘 Leggi le [linee guida per i contributi](../CONTRIBUTING.md)
* 🐛 Usa il [template per i bug report](../.github/ISSUE_TEMPLATE/bug_report.md)
* 💡 Hai un'idea? Usa il [template per le richieste di funzionalità](../.github/ISSUE_TEMPLATE/feature_request.md)

<a href="https://discord.gg/p9DXfEmUYu">
  <img src="https://img.shields.io/badge/Unisciti%20su%20Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Badge Discord"/>
</a>

---

## 📜 Licenza

Questo progetto è rilasciato sotto licenza **GPLv3**.
Consulta il file [LICENSE](../LICENSE) per ulteriori informazioni.
