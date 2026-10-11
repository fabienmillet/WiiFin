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

## 📦 Instalación

Descarga los archivos de la [última versión](https://github.com/fabienmillet/WiiFin/releases):

- **Homebrew Channel**: descomprime `WiiFin-hbc.zip` en la raíz de tu tarjeta SD
- **Canal del menú Wii**: instala `WiiFin.wad` con un gestor de WAD (Wii y vWii); para chino o coreano, copia también `apps/WiiFin/fonts/` del zip a la tarjeta SD
- **Dolphin**: abre `WiiFin.dol`

Después, inicia sesión en tu servidor Jellyfin con tu usuario y contraseña, o con Quick Connect.

## ✨ Lo más destacado

- **Películas, series y música** de tu Jellyfin: reproducción directa de lo que la Wii decodifica; el servidor convierte el resto
- Una pantalla de inicio en **filas** o en **cuadrícula**, pestañas, ordenar y filtrar, búsqueda, A-Z
- Un **reproductor** con las miniaturas del servidor, escenas y capítulos, saltar intro / créditos, siguiente episodio, pistas de audio y subtítulos, continuar donde lo dejaste
- **Música**: una pantalla Reproduciendo con visualizadores y la letra sincronizada; sigue sonando mientras navegas
- **Fotos** con presentación, y listas de vídeos
- **Perfiles**: varias cuentas y servidores, un código si quieres, cada uno con su propio tema
- **Control remoto** desde el panel y las apps de Jellyfin: Reproducir en… Nintendo Wii
- Los **diez idiomas de la Wii**; Wiimote, Mando Clásico, mando de GameCube o Wii U GamePad, sin barra de sensores

Todo lo que hace, los controles, los ajustes y los sonidos personalizados: la **[guía de usuario](../docs/USER_GUIDE.md)** (en inglés).

## ⚠️ Estado del proyecto

> 🚧 **Experimental**: funcional, pero aún en pleno desarrollo. Puede haber detalles por pulir en una consola real.

- Reproducción directa solo para vídeos SD (ver [DIRECT_PLAY.md](../DIRECT_PLAY.md)); sonido estéreo
- Los subtítulos de imagen (PGS, VobSub) los incrusta el servidor
- Más en las [limitaciones conocidas](../docs/USER_GUIDE.md#%EF%B8%8F-known-limitations)

## 🤝 Contribuir

Las pull requests, los informes de errores y las ideas son bienvenidos:

* 📘 La [guía de contribución](../CONTRIBUTING.md), y cómo [compilar WiiFin](../docs/BUILDING.md)
* 🌍 [Traducir](../lang/README.md) WiiFin a tu idioma
* 🐛 Un [informe de errores](../.github/ISSUE_TEMPLATE/bug_report.yml) (con `apps/WiiFin/wiifin.log` de la tarjeta SD) o una 💡 [petición de función](../.github/ISSUE_TEMPLATE/feature_request.yml)

<a href="https://discord.gg/p9DXfEmUYu">
  <img src="https://img.shields.io/badge/Join%20us%20on%20Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Discord"/>
</a>

## 📜 Licencia

Este proyecto está bajo la licencia **GPLv3**.
Consulta el archivo [LICENSE](../LICENSE) para más detalles.
