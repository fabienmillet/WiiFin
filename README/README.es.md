<p align="center">
  <img src="https://raw.githubusercontent.com/fabienmillet/WiiFin/refs/heads/main/assets/logo_wiifin_banner.png" alt="Logo de WiiFin" width="600"/><br>
  <em>Cliente de Jellyfin para la Nintendo Wii</em>
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
<strong>WiiFin</strong> es un cliente homebrew experimental para <a href="https://jellyfin.org">Jellyfin</a>, diseñado específicamente para la Nintendo Wii.<br>
Ofrece una experiencia ligera y adaptada a la consola para navegar y reproducir contenido multimedia, desarrollado en C++ con <a href="https://github.com/GRRLIB/GRRLIB">GRRLIB</a> y <a href="https://github.com/dborth/mplayer-ce">MPlayer CE</a>.
</p>

---

## ⚠️ Estado del proyecto

> 🚧 **Experimental** – funcional, pero aún en desarrollo activo. Pueden aparecer errores en hardware real.

### ✅ Lo que funciona:
- **Autenticación**: inicio de sesión con usuario/contraseña o mediante **QuickConnect** (aprobación desde otro dispositivo)
- **Perfiles guardados**: múltiples cuentas almacenadas de forma segura (solo token de acceso, sin contraseñas)
- **Navegación de bibliotecas**: películas, series, música con portadas cargadas desde el servidor
- **Vista de detalle**: sinopsis, clasificación, géneros, reparto, director, selección de pistas de audio y subtítulos
- **Continuar viendo** y **Próximos episodios**
- **Series de TV**: navegación por temporadas y episodios
- **Reproducción de vídeo**: transcodificación en el servidor, transmitida a través del motor MPlayer CE integrado
- **Reproducción de música**: bibliotecas de audio, navegación por álbumes y pistas
- **Overlay del reproductor**: barra de progreso, volumen, episodio anterior/siguiente, cambio de pistas de audio y subtítulos, saltar introducción
- **Informe de reproducción**: el progreso se envía al servidor Jellyfin (continúa donde lo dejaste)
- **HTTPS**: conexiones TLS mediante mbedTLS (certificados autofirmados admitidos)
- **Puntero IR** del Wiimote y **teclado virtual** en pantalla
- **Música de fondo** en los menús
- Se entrega como `.dol` listo para usar y `.wad` instalable (Wii / vWii)

### ⚠️ Limitaciones conocidas:
- No hay reproducción directa (direct-play); todo el vídeo es transcodificado por el servidor
- Sin audio multicanal 5.1 (solo estéreo mediante transcodificación)
- Los subtítulos son incrustados en el flujo de vídeo por el servidor

---

## 🔧 Instrucciones de compilación

### Requisitos:

- [devkitPro](https://devkitpro.org) con `devkitPPC`, `libogc` y los portlibs `wii-dev`
- Bibliotecas gráficas: `GRRLIB`, `libpngu`, `freetype`, `libjpeg`
- mbedTLS (incluido en `libs/`, compilado automáticamente por la CI)
- **Opcional**: MPlayer CE compilado como `libmplayer.a` — necesario para la reproducción de vídeo. Consulta [MPLAYER_CE_BUILD.md](../MPLAYER_CE_BUILD.md). Sin él, WiiFin compila pero la reproducción de vídeo no está disponible.

### Compilar:

```bash
./build.sh
```

### Ejecutar:

En **Dolphin Emulator**:

```bash
dolphin-emu -e WiiFin.dol
```

En una **Wii real**: copia `WiiFin.dol` en `SD:/apps/WiiFin/boot.dol`, o instala `WiiFin.wad` con un gestor de WAD (compatible con vWii).

---

## 📁 Estructura del proyecto

```
WiiFin/
├── source/
│   ├── core/        # Ciclo de vida de la app, música de fondo, utilidades
│   ├── input/       # Entrada de Wiimote + teclado USB
│   ├── jellyfin/    # Cliente HTTP de la API Jellyfin (HTTPS con mbedTLS)
│   ├── player/      # Integración MPlayer CE, HUD de overlay del reproductor
│   └── ui/          # Todas las vistas: Connect, Library, Profile, Settings
├── data/            # Assets gráficos (PNG/TTF)
├── libs/            # mbedTLS incluido
├── tools/           # Empaquetador WAD, generador de banner
├── Makefile         # Script de compilación compatible con devkitPro
└── apps/WiiFin/     # Metadatos del Homebrew Channel
```

---

## 🚀 Hoja de ruta

* [ ] Ordenar/filtrar contenido (por año, género, puntuación)
* [ ] Marcar elementos como favoritos desde la Wii
* [ ] Múltiples temas de color para la interfaz

---

## 📸 Capturas de pantalla

<img src="https://github.com/fabienmillet/WiiFin/blob/main/assets/preview.png?raw=true" alt="Captura de WiiFin" width="500"/><br> <em>WiiFin ejecutándose en Dolphin Emulator</em>

---

## 🤝 Contribuir

WiiFin está abierto a pull requests, reportes de errores e ideas de mejora.

* 📘 Lee las [pautas de contribución](../CONTRIBUTING.md)
* 🐛 Usa la [plantilla de reporte de errores](../.github/ISSUE_TEMPLATE/bug_report.md)
* 💡 ¿Tienes una idea? Usa la [plantilla de solicitud de funcionalidad](../.github/ISSUE_TEMPLATE/feature_request.md)

<a href="https://discord.gg/p9DXfEmUYu">
  <img src="https://img.shields.io/badge/Unirse%20a%20Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Badge de Discord"/>
</a>

---

## 📜 Licencia

Este proyecto está bajo la licencia **GPLv3**.
Consulta el archivo [LICENSE](../LICENSE) para más información.
