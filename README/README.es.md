<p align="center">
  <img src="https://raw.githubusercontent.com/fabienmillet/WiiFin/refs/heads/main/assets/logo_wiifin_banner.png" alt="Logo de WiiFin" width="600"/><br>
  <em>Cliente de Jellyfin para Nintendo Wii</em>
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
<strong>WiiFin</strong> es un cliente homebrew experimental para <a href="https://jellyfin.org">Jellyfin</a>, creado específicamente para Nintendo Wii.<br>
Explora y reproduce tus películas, series y música en la propia consola. Está escrito en C++ con <a href="https://github.com/GRRLIB/GRRLIB">GRRLIB</a> y <a href="https://github.com/extremscorner/mplayer-ce">MPlayer CE</a>.
</p>

<table align="center">
  <tr>
    <td><img src="../assets/screenshots/main-menu.png" alt="Menú principal" width="420"/></td>
    <td><img src="../assets/screenshots/film.png" alt="Una película" width="420"/></td>
  </tr>
  <tr>
    <td><img src="../assets/screenshots/player.png" alt="El reproductor" width="420"/></td>
    <td><img src="../assets/screenshots/now-playing.png" alt="El reproductor de música" width="420"/></td>
  </tr>
</table>
<p align="center"><sub><em>Big Buck Bunny</em> © Blender Foundation, <a href="https://peach.blender.org">peach.blender.org</a> (CC BY 3.0) · <em>Pill</em> de Heuse & Zeus X Crona feat. Emma Sameth, <a href="https://ncs.io">NCS</a></sub></p>

---

## ⚠️ Estado del proyecto

> 🚧 **Experimental**: funcional, pero aún en pleno desarrollo. Puede haber detalles por pulir en una consola real.

### ✅ Qué funciona

**Conexión**
- Inicio de sesión con usuario y contraseña, o con **Quick Connect** (aprobado desde otro dispositivo)
- **Búsqueda de servidores** en la red local, o la dirección escrita a mano (HTTP o **HTTPS**, se aceptan certificados autofirmados)
- **Perfiles guardados**: varias cuentas y servidores; solo se guarda un token de acceso (ninguna contraseña)

**Navegación**
- Una pantalla de inicio en **filas** (Seguir viendo, A continuación, últimos añadidos, mejor valorados, favoritos, géneros) o una **cuadrícula** de bibliotecas
- Bibliotecas de películas, series y música en carteles o en listas (con la portada del título seleccionado); saltos **A-Z** con izquierda / derecha
- **Ordenar y filtrar** una biblioteca de películas o series (botón 2): por nombre, año, valoración o fecha de adición; un género; todo, no vistos, vistos o favoritos
- **Búsqueda** (botón 1 en el inicio)
- **Ficha de detalles**: sinopsis, clasificación, géneros, reparto, pistas de audio y subtítulos (al principio las que el servidor elige para ti: tu modo de subtítulos y tus idiomas, las pistas predeterminadas y forzadas del archivo), varias **versiones** de una película, **favoritos** (botón 1, también en una serie), **visto / no visto** (+, también en una fila de la lista de episodios)
- **Contenido extra**: tráileres, featurettes, detrás de las cámaras… de una película (botón 2 en su ficha) o de una serie (tras sus temporadas)
- **Series**: temporadas y episodios, **aleatorio** (botón 2), episodio siguiente / anterior entre temporadas
- Títulos y subtítulos en **japonés, chino y coreano**: el japonés viene integrado; el chino y el coreano, con la fuente de `apps/WiiFin/fonts/` (incluida en el paquete para el Homebrew Channel)

**Vídeo**
- Reproducido por el motor MPlayer CE integrado: **reproducción directa** de lo que la Wii decodifica en tiempo real (DivX/Xvid SD, MPEG-1/2, H.264 hasta 480p, VP8; AVI, MKV, MP4, MPEG-TS/PS; avance dentro del propio archivo), conversión por el servidor para el resto, y vuelta a la conversión cuando un archivo no va fluido (ver [DIRECT_PLAY.md](../DIRECT_PLAY.md))
- **Reproductor**: barra de progreso con las miniaturas del servidor (Jellyfin 10.9+), volumen, pistas de audio y subtítulos (WiiFin dibuja los subtítulos de texto, así el vídeo puede reproducirse tal cual), zoom (ajustar / llenar)
- Botones **Saltar intro / resumen / créditos** a partir de los segmentos del servidor (Jellyfin 10.10+ con un plugin como Intro Skipper), y **Episodio siguiente** durante los créditos
- **Continuar** donde lo dejaste; el progreso, las pausas y el modo de reproducción (directo o convertido) se envían al servidor, cada consola como un dispositivo propio
- Una conexión lenta baja la calidad por sí sola; una imagen que se queda congelada se reintenta de otra forma, con un mensaje que explica lo que pasa

**Música**
- Bibliotecas en pestañas como en Jellyfin: **Álbumes, Sugerencias, Artistas, Listas, Canciones** (una biblioteca de archivos sueltos se abre en sus canciones)
- Una pantalla **Reproduciendo** con la portada, un espectro del sonido en directo, una cola, aleatorio, repetición, favoritos y canciones similares cuando se acaba la cola

**Control remoto** desde el panel o las apps de Jellyfin
- **Reproducir en… Nintendo Wii** (películas, episodios, música; «Reproducir a continuación» y «Añadir a la cola» para la música)
- Reproducir / pausa, detener, avanzar, siguiente / anterior, volumen y silencio (el control del panel sigue al de la Wii), pistas de audio y subtítulos
- Los **mensajes** enviados desde el panel aparecen sobre cualquier pantalla

**Y además**
- Puntero del **Wiimote**, **Mando Clásico**, **Wii U GamePad** (inyecciones de Virtual Console) y **mando de GameCube**: todo se hace con la cruceta, sin barra de sensores (ver [Controles](#-controles))
- **Sonidos de la interfaz** y **música de fondo**, ambos reemplazables por tus propios archivos (ver [Sonidos personalizados](#-sonidos-personalizados))
- Teclado en pantalla; calibración del área de pantalla para televisores que recortan la imagen; la luz de la ranura del disco puede quedarse apagada
- Disponible como `.dol` listo para usar, `.wad` instalable (Wii / vWii) y paquete para el Homebrew Channel

### 🎮 Controles

El puntero es opcional: todas las pantallas funcionan con la cruceta.

| Wiimote | Mando Clásico / Wii U GamePad | Mando de GameCube | Acción |
|---|---|---|---|
| A / B | A / B | A / B | Seleccionar / Volver |
| Cruceta | Cruceta o stick izquierdo | Cruceta o stick | Moverse (mantener para repetir); izquierda / derecha: A-Z en listas, −10 / +10 s en el reproductor |
| − / + | − / + o L / R | L / R | Pestañas, páginas, episodio o canción anterior / siguiente; + : visto / no visto (una película, un episodio) |
| 1 | Y | Y | Búsqueda (inicio), favorito (una película, una serie), pista de audio (reproductor), cola (música) |
| 2 | X | X | Ordenar y filtrar (listas), extras (una película), aleatorio (series, música), subtítulos (reproductor) |
| HOME | HOME | START | Menú HOME |
| puntero | ZL / ZR | Z | Zoom del vídeo (ajustar / llenar); en los menús, lo que hace + solo: visto / no visto, la página Browse, Intro del teclado |

Las ayudas en pantalla muestran los botones del último mando usado (GameCube: A verde, B rojo, L / R, START).

### ⚙️ Ajustes

| Ajuste | |
|---|---|
| SSL Verification | Comprobar el certificado HTTPS del servidor (desactivar para los autofirmados) |
| Background Music / Interface Sounds | La música y los sonidos de los menús |
| Video Quality | La tasa de bits de la conversión, de Low (1,5 Mb/s) a Max (5 Mb/s, para un adaptador por cable); WiiFin también mide la conexión y se queda por debajo |
| Direct Play | Reproducir tal cual los archivos que la Wii sabe decodificar, sin conversión por el servidor |
| Smooth Motion | Fundir los fotogramas en los cambios de imagen, para que las películas a 24 fps se vean fluidas en televisores de 60 Hz |
| Theme / Home Screen / Library View | Claro, Oscuro o Flix; filas o cuadrícula; carteles, lista o lista con portada |
| Disc Slot Light | La luz que late con el sonido durante la reproducción, o apagada |
| Screen Area | Reducir la interfaz cuando el televisor recorta los bordes |
| Backgrounds | Degradados suaves, o colores lisos para los televisores que muestran bandas en ellos |
| Clock | 24 o 12 horas (AM/PM, la fecha como mes/día); 12 horas por defecto en una Wii estadounidense |

### 🔊 Sonidos personalizados

Pon tus propios sonidos en `SD:/apps/WiiFin/sounds/` (junto a `wiifin.cfg`): un archivo ahí reemplaza el sonido integrado del mismo nombre; los demás se quedan. Se leen al arrancar WiiFin.

| Archivo | Suena cuando |
|---|---|
| `move` | la selección se mueve (listas, filas, cuadrículas, ajustes) |
| `open` | se abre una serie, una película, una temporada o una biblioteca |
| `back` | B vuelve atrás |
| `page` | cambia una página o una pestaña (− / +) |
| `play` | empieza un vídeo o una canción |
| `start` | se pulsa un botón del menú principal o cambia un ajuste |
| `select` | el puntero pasa sobre un botón del menú principal |
| `press_key` / `backspace` | una tecla del teclado en pantalla |
| `loading` | una pantalla de carga se alarga (en bucle) |
| `menu_enter` / `menu_exit` | el menú HOME se abre / se cierra |
| `bgm` | música de fondo (solo MP3, hasta 16 MB, en bucle) |

Cada sonido es `nombre.mp3` o `nombre.wav` (PCM, 8 o 16 bits, mono o estéreo, hasta 48 kHz), de 2 MB y 10 s como máximo (los más largos se cortan). Un archivo que WiiFin no puede leer se ignora, y el registro dice por qué. Ajustes > Interface Sounds apaga todos los sonidos; Background Music, la música.

### ⚠️ Limitaciones conocidas

- Reproducción directa solo para lo que la Wii decodifica en tiempo real (resoluciones SD, ver [DIRECT_PLAY.md](../DIRECT_PLAY.md)); el servidor convierte el resto
- Solo salida estéreo (el sonido multicanal se mezcla en estéreo)
- Los subtítulos de imagen (PGS, VobSub) los incrusta el servidor en el vídeo; los de texto los dibuja WiiFin
- El chino y el coreano necesitan la fuente de `SD:/apps/WiiFin/fonts/`: el paquete para el Homebrew Channel la incluye; con el WAD, copia ahí `data/fonts/cjk/` (cualquier otra `.ttf` / `.otf` que pongas ahí también se usa, para lo que les falta a las demás)

---

## 🔧 Compilación

### Requisitos

- [devkitPro](https://devkitpro.org) con `devkitPPC`, `libogc` y los portlibs `wii-dev`
- Gráficos: `GRRLIB`, `libpngu`, `freetype`, `libjpeg`
- mbedTLS (incluido en `libs/`, compilado por `setup.sh`)
- MPlayer CE como `libmplayer.a`, para la reproducción: precompilado en `libs/mplayer-ce-build`, recompilable desde el código con `tools/mplayer/build.sh` (ver [MPLAYER_CE_BUILD.md](../MPLAYER_CE_BUILD.md)). Sin él, WiiFin compila igualmente pero no puede reproducir nada.

### Compilar

En una máquina nueva, `./setup.sh` instala devkitPro y los portlibs, compila GRRLIB y mbedTLS y luego WiiFin (distribuciones basadas en Arch, o cualquier equipo con `dkp-pacman`).

```bash
./build.sh          # WiiFin.dol
./build.sh wad      # WiiFin.wad, el canal instalable (necesita libWiiPy, instalado por setup.sh)
```

`make wad` mete `WiiFin.dol` en `tools/wad/template.wad` (banner, cargador NAND, ticket y TMD del título `WIFN`) y lo firma: ver `tools/make_wad.py`.

### Ejecutar

En una **Wii real**: descomprime `WiiFin-hbc.zip` en la raíz de la tarjeta SD (contiene `apps/WiiFin/`) para lanzar WiiFin desde el Homebrew Channel, o instala `WiiFin.wad` con un gestor de WAD (también funciona en vWii). WiiFin guarda sus ajustes, perfiles y su registro (`wiifin.log`) en `SD:/apps/WiiFin/`.

En **Dolphin**:

```bash
dolphin-emu -e WiiFin.dol
```

Para probar con un Jellyfin local en Docker, con pulsaciones de botones programadas y capturas de imagen, ver [tools/test](../tools/test/README.md).

---

## 📁 Estructura del proyecto

```
WiiFin/
├── source/
│   ├── core/        # Aplicación, ajustes, sesiones de reproducción, sonidos y música, texto, registro
│   ├── input/       # Wiimote, Mando Clásico y mando de GameCube
│   ├── jellyfin/    # Cliente de la API de Jellyfin (HTTPS vía mbedTLS), control remoto (WebSocket)
│   ├── player/      # Integración de MPlayer CE, salida de vídeo, interfaz del reproductor, subtítulos, miniaturas
│   └── ui/          # Las pantallas: conexión, perfiles, inicio, bibliotecas, fichas, música, ajustes
├── data/            # Fuentes, sonidos, imágenes
├── libs/            # mbedTLS, MPlayer CE precompilado
├── tools/           # Empaquetador de WAD, script de enlazado, compilación de MPlayer CE (mplayer/)
│   └── test/        # Entorno de pruebas: Jellyfin en Docker, recorridos programados en Dolphin
├── apps/WiiFin/     # Metadatos para el Homebrew Channel
├── DIRECT_PLAY.md   # Lo que la Wii reproduce tal cual
└── Makefile
```

---

## 🤝 Contribuir

WiiFin acepta pull requests, informes de errores y sugerencias.

* 📘 Lee la [guía de contribución](../CONTRIBUTING.md)
* 🐛 Usa la [plantilla de informe de errores](../.github/ISSUE_TEMPLATE/bug_report.yml)
* 💡 ¿Una idea? Usa la [plantilla de petición de funciones](../.github/ISSUE_TEMPLATE/feature_request.yml)

<a href="https://discord.gg/p9DXfEmUYu">
  <img src="https://img.shields.io/badge/Join%20us%20on%20Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Discord"/>
</a>

---

## 📜 Licencia

Este proyecto está bajo la licencia **GPLv3**.
Consulta el archivo [LICENSE](../LICENSE) para más detalles.
