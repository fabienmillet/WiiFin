# 🤝 Contributing to WiiFin

Thank you for your interest in the project!  
**WiiFin** is a **highly experimental** Jellyfin client for the Nintendo Wii.  
Any help is welcome — but please note that many features are still incomplete or in testing.

---

## 🚧 Project Status

- 🔬 **Only the basic user interface is currently functional.**
- 🎮 Wiimote pointer/navigation support is in development.
- 📡 Jellyfin integration (auth, API requests) is still a work in progress.

---

## 🧰 Requirements

- A properly configured Wii development environment (devkitPro, devkitPPC, libogc, GRRLIB, etc.)
- A build system that supports `make` (Linux or MSYS2 recommended)
- Dolphin Emulator for quick testing
- A real Wii with the Homebrew Channel for final testing

---

## 📁 Project Structure

- `source/` – main C++ source code
- `textures/` – visual assets (PNG, TTF)
- `build/` – compiled object files
- `Makefile` – builds the project with automatic dependency management

---

## 🧪 How to Contribute

1. **Fork** the repository and create a new branch.
2. **Make clear and atomic commits.**
3. **Test your code in Dolphin and/or on a real Wii if possible.**
4. **Open a Pull Request** to the `main` branch.

---

## 🧭 Guidelines

- Use clear, descriptive variable names.
- Follow existing code style and indentation.
- Keep Wii limitations in mind (low RAM, 640x480 resolution, etc.).

---

## 📝 License

This project is licensed under **GPLv3**.  
By contributing, you agree that your work will be released under this license.

---

Thanks 🙌
