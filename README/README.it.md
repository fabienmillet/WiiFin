<p align="center">
  <img src="https://raw.githubusercontent.com/fabienmillet/WiiFin/refs/heads/main/assets/logo_wiifin_banner.png" alt="Logo WiiFin" width="600"/><br>
  <em>Client Jellyfin per Nintendo Wii</em>
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
<strong>WiiFin</strong> è un client homebrew altamente sperimentale per <a href="https://jellyfin.org">Jellyfin</a>, progettato specificamente per la Nintendo Wii.  
L'obiettivo è fornire un'esperienza di navigazione multimediale leggera e adatta alla console, scritto in C++ utilizzando <a href="https://github.com/GRRLIB/GRRLIB">GRRLIB</a>.
</p>

---

## ⚠️ Stato del progetto

> 🚧 **In sviluppo attivo** – al momento il progetto non supporta ancora la riproduzione multimediale.

### ✅ Funziona già:
- L'interfaccia utente viene visualizzata correttamente sia su hardware Wii reale che su emulatore Dolphin.

### ❌ Ancora non funzionante:
- Nessuna integrazione con l'API di Jellyfin.
- Nessuna riproduzione di file multimediali.

Obiettivi attuali dello sviluppo:
- Layout dell'interfaccia
- Supporto per il puntatore Wiimote
- Struttura e navigazione principali

---

## 🔧 Istruzioni di compilazione

### Requisiti:

- [devkitPro](https://devkitpro.org)
- `devkitPPC`, `libogc`
- Librerie grafiche: `GRRLIB`, `libpngu`, `freetype`, `libjpeg`

### Compilazione:

```bash
./build.sh
````

### Esecuzione:

Su **Dolphin Emulator**:

```bash
dolphin-emu -e WiiFin.elf
```

Su **hardware Wii reale**, converti il file ELF in `.dol` o `.wad`.

---

## 📁 Struttura del progetto

```
WiiFin/
├── source/          # Codice sorgente C++
├── data/            # Asset grafici (PNG, TTF)
├── Makefile         # Script di build compatibile con devkitPro
└── README.md
```

---

## 🚀 Roadmap

* [ ] Supporto completo all’API Jellyfin (navigazione, riproduzione)
* [ ] Rilevamento automatico del server e interfaccia delle impostazioni
* [ ] Localizzazione / supporto multilingua

---

## 📸 Screenshot

<img src="https://github.com/fabienmillet/WiiFin/blob/main/assets/preview.png?raw=true" alt="Screenshot del menu di WiiFin" width="500"/><br> <em>Menu di WiiFin in esecuzione su Dolphin Emulator</em>

---

## 🤝 Contribuire

WiiFin è aperto a pull request, segnalazioni di bug e proposte di nuove funzionalità.

* 📘 Leggi le [linee guida per contribuire](CONTRIBUTING.md)
* 🐛 Usa il [modulo di segnalazione bug](.github/ISSUE_TEMPLATE/bug_report.md)
* 💡 Hai un'idea? Usa il [modulo per le richieste di funzionalità](.github/ISSUE_TEMPLATE/feature_request.md)

<a href="https://discord.gg/p9DXfEmUYu">
  <img src="https://img.shields.io/badge/Unisciti%20al%20nostro%20Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Badge Discord"/>
</a>

---

## 📜 Licenza

Questo progetto è rilasciato sotto licenza **GPLv3**.
Consulta il file [LICENSE](LICENSE) per maggiori dettagli.