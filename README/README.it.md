<p align="center">
  <img src="https://raw.githubusercontent.com/fabienmillet/WiiFin/refs/heads/main/assets/logo_wiifin_banner.png" alt="Logo di WiiFin" width="600"/><br>
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
<strong>WiiFin</strong> è un client homebrew sperimentale per <a href="https://jellyfin.org">Jellyfin</a>, pensato apposta per Nintendo Wii.<br>
Sfoglia e riproduce i tuoi film, serie e musica direttamente sulla console. È scritto in C++ con <a href="https://github.com/GRRLIB/GRRLIB">GRRLIB</a> e <a href="https://github.com/extremscorner/mplayer-ce">MPlayer CE</a>.
</p>

<table align="center">
  <tr>
    <td><img src="../assets/screenshots/main-menu.png" alt="Menu principale" width="420"/></td>
    <td><img src="../assets/screenshots/film.png" alt="Un film" width="420"/></td>
  </tr>
  <tr>
    <td><img src="../assets/screenshots/player.png" alt="Il lettore" width="420"/></td>
    <td><img src="../assets/screenshots/now-playing.png" alt="Il lettore musicale" width="420"/></td>
  </tr>
</table>
<p align="center"><sub><em>Big Buck Bunny</em> © Blender Foundation, <a href="https://peach.blender.org">peach.blender.org</a> (CC BY 3.0) · <em>Pill</em> di Heuse & Zeus X Crona feat. Emma Sameth, <a href="https://ncs.io">NCS</a></sub></p>

---

## ⚠️ Stato del progetto

> 🚧 **Sperimentale**: funzionante, ma ancora in pieno sviluppo. Su una console vera possono esserci imperfezioni.

### ✅ Cosa funziona

**Connessione**
- Accesso con nome utente e password, oppure con **Quick Connect** (approvato da un altro dispositivo)
- **Ricerca dei server** nella rete locale, oppure l'indirizzo digitato a mano (HTTP o **HTTPS**, certificati autofirmati accettati)
- **Profili salvati**: più account e server; viene conservato solo un token di accesso (nessuna password)

**Navigazione**
- Una schermata iniziale a **righe** (Continua a guardare, Prossimi episodi, ultimi aggiunti, più votati, preferiti, generi) oppure a **griglia** di librerie
- Librerie di film, serie e musica come locandine o elenchi (con la copertina del titolo selezionato); salti **A-Z** con sinistra / destra
- **Ordinare e filtrare** una libreria di film o serie (tasto 2): per nome, anno, voto o data di aggiunta; un genere; tutto, non visti, visti o preferiti
- **Ricerca** (tasto 1 nella schermata iniziale)
- **Scheda dettagli**: trama (intera con − quando la scheda la taglia), classificazione, generi, cast, tracce audio e sottotitoli (all'inizio quelle che il server sceglie per te: la tua modalità sottotitoli e le tue lingue, le tracce predefinite e forzate del file), più **versioni** di un film, **preferiti** (tasto 1, anche su una serie), **visto / non visto** (+, anche su una riga dell'elenco episodi)
- **Contenuti speciali**: trailer, featurette, dietro le quinte… di un film (tasto 2 nella sua scheda) o di una serie (dopo le sue stagioni)
- **Serie**: stagioni (come locandine, o in elenco con la copertina della stagione) ed episodi, **riproduzione casuale** (tasto 2), episodio successivo / precedente tra una stagione e l'altra
- Titoli e sottotitoli in **giapponese, cinese e coreano**: il giapponese è integrato, cinese e coreano usano il font di `apps/WiiFin/fonts/` (incluso nel pacchetto per l'Homebrew Channel)

**Video**
- Riprodotto dal motore MPlayer CE integrato: **riproduzione diretta** di ciò che la Wii decodifica in tempo reale (DivX/Xvid SD, MPEG-1/2, H.264 fino a 480p, VP8; AVI, MKV, MP4, MPEG-TS/PS; avanzamento nel file stesso), conversione da parte del server per il resto, e ritorno alla conversione quando un file non scorre bene (vedi [DIRECT_PLAY.md](../DIRECT_PLAY.md))
- **Lettore**: barra di avanzamento con le miniature del server (Jellyfin 10.9+), volume, tracce audio e sottotitoli (i sottotitoli testuali li disegna WiiFin, così il video può essere riprodotto così com'è), zoom (adatta / riempi)
- Pulsanti **Salta intro / riassunto / titoli di coda** dai segmenti del server (Jellyfin 10.10+ con un plugin come Intro Skipper), e **Episodio successivo** durante i titoli di coda
- **Riprendi** da dove avevi lasciato; avanzamento, pause e modalità di riproduzione (diretta o convertita) inviati al server, ogni console come un dispositivo a sé
- Una connessione lenta abbassa la qualità da sola; un'immagine che si blocca viene riprovata in un altro modo, con un messaggio che spiega cosa succede

**Musica**
- Librerie a schede come su Jellyfin: **Album, Suggerimenti, Artisti, Playlist, Brani** (una libreria di file sparsi si apre sui suoi brani)
- Una schermata **In riproduzione** con la copertina, uno spettro del suono in tempo reale, una coda, riproduzione casuale, ripetizione, preferiti e brani simili quando la coda finisce

**Controllo remoto** dalla dashboard o dalle app di Jellyfin
- **Riproduci su… Nintendo Wii** (film, episodi, musica; «Riproduci dopo» e «Aggiungi alla coda» per la musica)
- Riproduci / pausa, stop, avanzamento, successivo / precedente, volume e muto (il cursore della dashboard segue), tracce audio e sottotitoli
- I **messaggi** inviati dalla dashboard compaiono sopra qualsiasi schermata

**E inoltre**
- Puntatore del **Wiimote**, **Controller Classico**, **Wii U GamePad** (iniezioni Virtual Console) e **controller GameCube**: tutto si fa con la croce direzionale, senza barra sensore (vedi [Controlli](#-controlli))
- **Suoni dell'interfaccia** e **musica di sottofondo**, entrambi sostituibili con i tuoi file (vedi [Suoni personalizzati](#-suoni-personalizzati))
- Tastiera su schermo; calibrazione dell'area dello schermo per i televisori che tagliano l'immagine; la luce della fessura del disco può restare spenta
- Disponibile come `.dol` pronto all'uso, `.wad` installabile (Wii / vWii) e pacchetto per l'Homebrew Channel

### 🎮 Controlli

Il puntatore è facoltativo: ogni schermata funziona con la croce direzionale.

| Wiimote | Controller Classico / Wii U GamePad | Controller GameCube | Azione |
|---|---|---|---|
| A / B | A / B | A / B | Seleziona / Indietro |
| Croce direzionale | Croce o levetta sinistra | Croce o levetta | Muoversi (tieni premuto per ripetere); sinistra / destra: A-Z negli elenchi, −10 / +10 s nel lettore |
| − / + | − / + o L / R | L / R | Schede, pagine, episodio o brano precedente / successivo; + : visto / non visto (un film, un episodio); − : la trama intera (una scheda) |
| 1 | Y | Y | Ricerca (inizio), preferito (un film, una serie), traccia audio (lettore), coda (musica) |
| 2 | X | X | Ordina e filtra (elenchi), contenuti speciali (un film), casuale (serie, musica), sottotitoli (lettore) |
| HOME | HOME | START | Menu HOME |
| puntatore | ZL / ZR | Z | Zoom del video (adatta / riempi); nei menu, ciò che fa + da solo: visto / non visto, la pagina Browse, Invio della tastiera |

I suggerimenti a schermo mostrano i tasti dell'ultimo controller usato (GameCube: A verde, B rosso, L / R, Z, START).

### ⚙️ Impostazioni

| Impostazione | |
|---|---|
| SSL Verification | Controllare il certificato HTTPS del server (da disattivare per quelli autofirmati) |
| Background Music / Interface Sounds | La musica e i suoni dei menu |
| Video Quality | Il bitrate della conversione, da Low (1,5 Mb/s) a Max (5 Mb/s, per un adattatore cablato); WiiFin misura anche la connessione e resta al di sotto |
| Direct Play | Riprodurre così come sono i file che la Wii sa decodificare, senza conversione da parte del server |
| Smooth Motion | Sfumare i fotogrammi ai cambi d'immagine, perché i film a 24 fps scorrano regolari sui televisori a 60 Hz |
| Theme / Home Screen / Library View | Chiaro, Scuro o Flix; righe o griglia; locandine, elenco o elenco con copertina |
| Disc Slot Light | La luce che pulsa con il suono durante la riproduzione, oppure spenta |
| Screen Area | Ridurre l'interfaccia quando il televisore taglia i bordi |
| Backgrounds | Sfumature morbide, o colori pieni per i televisori che vi mostrano delle bande |
| Clock | 24 o 12 ore (AM/PM, la data come mese/giorno); 12 ore di default su una Wii americana |

### 🔊 Suoni personalizzati

Metti i tuoi suoni in `SD:/apps/WiiFin/sounds/` (accanto a `wiifin.cfg`): un file lì sostituisce il suono integrato con lo stesso nome; gli altri restano. Vengono letti all'avvio di WiiFin.

| File | Suona quando |
|---|---|
| `move` | la selezione si sposta (elenchi, righe, griglie, impostazioni) |
| `open` | si apre una serie, un film, una stagione o una libreria |
| `back` | B torna indietro |
| `page` | cambia una pagina o una scheda (− / +) |
| `play` | parte un video o un brano |
| `start` | si preme un pulsante del menu principale o cambia un'impostazione |
| `select` | il puntatore passa su un pulsante del menu principale |
| `press_key` / `backspace` | un tasto della tastiera su schermo |
| `loading` | una schermata di caricamento si prolunga (in loop) |
| `menu_enter` / `menu_exit` | il menu HOME si apre / si chiude |
| `bgm` | musica di sottofondo (solo MP3, fino a 16 MB, in loop) |

Ogni suono è `nome.mp3` o `nome.wav` (PCM, 8 o 16 bit, mono o stereo, fino a 48 kHz), al massimo 2 MB e 10 s (quelli più lunghi vengono tagliati). Un file che WiiFin non sa leggere viene ignorato, e il log dice perché. Impostazioni > Interface Sounds spegne tutti i suoni; Background Music, la musica.

### ⚠️ Limitazioni note

- Riproduzione diretta solo per ciò che la Wii decodifica in tempo reale (risoluzioni SD, vedi [DIRECT_PLAY.md](../DIRECT_PLAY.md)); il server converte il resto
- Solo uscita stereo (l'audio multicanale viene ridotto in stereo)
- I sottotitoli a immagine (PGS, VobSub) li imprime il server nel video; quelli testuali li disegna WiiFin
- Cinese e coreano richiedono il font in `SD:/apps/WiiFin/fonts/`: il pacchetto per l'Homebrew Channel lo contiene; con il WAD, copiaci `data/fonts/cjk/` (qualsiasi altro `.ttf` / `.otf` messo lì viene usato anch'esso, per ciò che manca agli altri)

---

## 🔧 Compilazione

### Requisiti

- [devkitPro](https://devkitpro.org) con `devkitPPC`, `libogc` e i portlibs `wii-dev`
- Grafica: `GRRLIB`, `libpngu`, `freetype`, `libjpeg`
- mbedTLS (incluso in `libs/`, compilato da `setup.sh`)
- MPlayer CE come `libmplayer.a`, per la riproduzione: precompilato in `libs/mplayer-ce-build`, ricompilabile dai sorgenti con `tools/mplayer/build.sh` (vedi [MPLAYER_CE_BUILD.md](../MPLAYER_CE_BUILD.md)). Senza, WiiFin compila comunque ma non può riprodurre nulla.

### Compilare

Su una macchina nuova, `./setup.sh` installa devkitPro e i portlibs, compila GRRLIB e mbedTLS e poi WiiFin (distribuzioni basate su Arch, o qualsiasi macchina con `dkp-pacman`).

```bash
./build.sh          # WiiFin.dol
./build.sh wad      # WiiFin.wad, il canale installabile (richiede libWiiPy, installato da setup.sh)
```

`make wad` inserisce `WiiFin.dol` in `tools/wad/template.wad` (banner, loader NAND, ticket e TMD del titolo `WIFN`) e lo firma: vedi `tools/make_wad.py`.

### Avviare

Su una **Wii vera**: estrai `WiiFin-hbc.zip` nella radice della scheda SD (contiene `apps/WiiFin/`) per avviare WiiFin dall'Homebrew Channel, oppure installa `WiiFin.wad` con un gestore di WAD (funziona anche su vWii). WiiFin tiene impostazioni, profili e log (`wiifin.log`) in `SD:/apps/WiiFin/`.

Su **Dolphin**:

```bash
dolphin-emu -e WiiFin.dol
```

Per provare con un Jellyfin locale in Docker, con pressioni dei tasti programmate e catture delle immagini, vedi [tools/test](../tools/test/README.md).

---

## 📁 Struttura del progetto

```
WiiFin/
├── source/
│   ├── core/        # Applicazione, impostazioni, sessioni di riproduzione, suoni e musica, testo, log
│   ├── input/       # Wiimote, Controller Classico e controller GameCube
│   ├── jellyfin/    # Client dell'API di Jellyfin (HTTPS tramite mbedTLS), controllo remoto (WebSocket)
│   ├── player/      # Integrazione di MPlayer CE, uscita video, interfaccia del lettore, sottotitoli, miniature
│   └── ui/          # Le schermate: connessione, profili, inizio, librerie, schede, musica, impostazioni
├── data/            # Font, suoni, immagini
├── libs/            # mbedTLS, MPlayer CE precompilato
├── tools/           # Creazione del WAD, script del linker, compilazione di MPlayer CE (mplayer/)
│   └── test/        # Ambiente di test: Jellyfin in Docker, percorsi programmati in Dolphin
├── apps/WiiFin/     # Metadati per l'Homebrew Channel
├── DIRECT_PLAY.md   # Ciò che la Wii riproduce così com'è
└── Makefile
```

---

## 🤝 Contribuire

WiiFin accetta pull request, segnalazioni di bug e suggerimenti.

* 📘 Leggi le [linee guida per contribuire](../CONTRIBUTING.md)
* 🐛 Usa il [modello di segnalazione bug](../.github/ISSUE_TEMPLATE/bug_report.yml)
* 💡 Un'idea? Usa il [modello di richiesta funzionalità](../.github/ISSUE_TEMPLATE/feature_request.yml)

<a href="https://discord.gg/p9DXfEmUYu">
  <img src="https://img.shields.io/badge/Join%20us%20on%20Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Discord"/>
</a>

---

## 📜 Licenza

Questo progetto è distribuito con licenza **GPLv3**.
Vedi il file [LICENSE](../LICENSE) per i dettagli.
