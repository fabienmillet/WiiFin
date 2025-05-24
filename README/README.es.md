<p align="center">
  <img src="https://raw.githubusercontent.com/fabienmillet/WiiFin/refs/heads/main/assets/logo_wiifin_banner.png" alt="Logo de WiiFin" width="600"/><br>
  <em>Cliente de Jellyfin para la Nintendo Wii</em>
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
<strong>WiiFin</strong> es un cliente homebrew muy experimental para <a href="https://jellyfin.org">Jellyfin</a>, diseñado específicamente para la Nintendo Wii.  
Su objetivo es ofrecer una experiencia ligera y adaptada a la consola para la navegación multimedia, desarrollado en C++ usando <a href="https://github.com/GRRLIB/GRRLIB">GRRLIB</a>.
</p>

---

## ⚠️ Estado del proyecto

> 🚧 **En desarrollo activo** – actualmente el proyecto no permite reproducir contenido multimedia.

### ✅ Lo que funciona:
- La interfaz gráfica se muestra correctamente tanto en hardware real como en el emulador Dolphin.

### ❌ Lo que aún no funciona:
- No hay integración con la API de Jellyfin.
- No hay reproducción de medios.

Actualmente se está trabajando en:
- Diseño de la interfaz
- Soporte del puntero de la Wiimote
- Estructura base y navegación

---

## 🔧 Instrucciones de compilación

### Requisitos:

- [devkitPro](https://devkitpro.org)
- `devkitPPC`, `libogc`
- Bibliotecas gráficas: `GRRLIB`, `libpngu`, `freetype`, `libjpeg`

### Compilar:

```bash
./build.sh
````

### Ejecutar:

En **Dolphin Emulator**:

```bash
dolphin-emu -e WiiFin.elf
```

En una **Wii real**, convierte el archivo ELF a formato `.dol` o `.wad`.

---

## 📁 Estructura del proyecto

```
WiiFin/
├── source/          # Archivos fuente en C++
├── data/            # Recursos gráficos (PNG, TTF)
├── Makefile         # Script de compilación compatible con devkitPro
└── README.md
```

---

## 🚀 Hoja de ruta

* [ ] Soporte completo de la API de Jellyfin (navegación, reproducción)
* [ ] Descubrimiento automático de servidores + interfaz de configuración
* [ ] Soporte multilingüe / localización

---

## 📸 Capturas

<img src="https://github.com/fabienmillet/WiiFin/blob/main/assets/preview.png?raw=true" alt="Captura del menú de WiiFin" width="500"/><br> <em>Menú de WiiFin ejecutándose en Dolphin Emulator</em>

---

## 🤝 Contribuir

WiiFin está abierto a pull requests, reportes de errores e ideas de mejora.

* 📘 Consulta la [guía de contribución](CONTRIBUTING.md)
* 🐛 Usa la [plantilla de reporte de errores](.github/ISSUE_TEMPLATE/bug_report.md)
* 💡 ¿Tienes una idea? Usa la [plantilla de solicitud de funcionalidad](.github/ISSUE_TEMPLATE/feature_request.md)

<a href="https://discord.gg/p9DXfEmUYu">
  <img src="https://img.shields.io/badge/Únete%20a%20nuestro%20Discord-5865F2?style=for-the-badge&logo=discord&logoColor=white" alt="Badge de Discord"/>
</a>

---

## 📜 Licencia

Este proyecto está licenciado bajo **GPLv3**.
Consulta el archivo [LICENSE](LICENSE) para más información.
