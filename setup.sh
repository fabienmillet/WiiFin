#!/usr/bin/env bash
#---------------------------------------------------------------------------------
# WiiFin development environment setup
#
# Installs everything needed to build WiiFin:
#   1. devkitPro pacman repositories + devkitPPC / libogc / portlibs
#   2. GRRLIB + libpngu (built from source, installed into portlibs/wii)
#   3. mbedTLS (cross-compiled with libs/mbedtls/include/mbedtls/mbedtls_config.h)
#   4. Builds WiiFin (skip with --no-build)
#
# Supported hosts: Arch-based distros (pacman), or any host where devkitPro's
# dkp-pacman is already installed (Debian/Ubuntu/macOS).
#
# Usage: ./setup.sh [--no-build]
#---------------------------------------------------------------------------------
set -euo pipefail

MBEDTLS_VERSION="3.6.3"
GRRLIB_REPO="https://github.com/GRRLIB/GRRLIB.git"
DKP_KEY="BC26F752D25B92CE272E0F44F7FD5492264BB9D0"

DKP_PACKAGES=(
	wii-dev
	ppc-freetype
	ppc-libpng
	ppc-libjpeg-turbo
	ppc-zlib
	ppc-brotli
	ppc-bzip2
)

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEPS_DIR="${XDG_CACHE_HOME:-$HOME/.cache}/wiifin-deps"
JOBS=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)

DO_BUILD=1
for arg in "$@"; do
	case "$arg" in
		--no-build) DO_BUILD=0 ;;
		-h|--help) sed -n '3,14p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
		*) echo "Unknown option: $arg" >&2; exit 1 ;;
	esac
done

info() { printf '\033[1;34m==>\033[0m %s\n' "$*"; }
ok()   { printf '\033[1;32m  ✓\033[0m %s\n' "$*"; }
warn() { printf '\033[1;33m  !\033[0m %s\n' "$*"; }
die()  { printf '\033[1;31merror:\033[0m %s\n' "$*" >&2; exit 1; }

# Run a command as root only when the target directory is not writable
as_root_for() {
	local dir="$1"; shift
	while [ ! -e "$dir" ]; do dir="$(dirname "$dir")"; done
	if [ -w "$dir" ]; then "$@"; else sudo "$@"; fi
}

#---------------------------------------------------------------------------------
# 0. Host tools
#---------------------------------------------------------------------------------
info "Checking host tools"
declare -A HOST_PKG=( [git]=git [make]=make [curl]=curl [tar]=tar [bzip2]=bzip2 [xxd]=tinyxxd [pkg-config]=pkgconf )
missing=()
for tool in "${!HOST_PKG[@]}"; do
	command -v "$tool" >/dev/null 2>&1 || missing+=("$tool")
done
if [ ${#missing[@]} -gt 0 ]; then
	if command -v pacman >/dev/null 2>&1; then
		pkgs=(); for t in "${missing[@]}"; do pkgs+=("${HOST_PKG[$t]}"); done
		info "Installing missing host tools: ${pkgs[*]}"
		sudo pacman -S --needed --noconfirm "${pkgs[@]}"
	else
		die "Missing host tools: ${missing[*]} — install them with your package manager and re-run."
	fi
fi
ok "Host tools present"

#---------------------------------------------------------------------------------
# 1. devkitPro toolchain + portlibs
#---------------------------------------------------------------------------------
info "Setting up devkitPro"
if command -v dkp-pacman >/dev/null 2>&1; then
	PACMAN=dkp-pacman
elif command -v pacman >/dev/null 2>&1; then
	PACMAN=pacman
	if ! grep -q '^\[dkp-libs\]' /etc/pacman.conf; then
		info "Adding devkitPro repositories to /etc/pacman.conf"
		sudo pacman-key --recv "$DKP_KEY" --keyserver keyserver.ubuntu.com
		sudo pacman-key --lsign "$DKP_KEY"
		sudo pacman -U --noconfirm https://pkg.devkitpro.org/devkitpro-keyring.pkg.tar.zst
		sudo tee -a /etc/pacman.conf >/dev/null <<'EOF'

[dkp-libs]
Server = https://pkg.devkitpro.org/packages

[dkp-linux]
Server = https://pkg.devkitpro.org/packages/linux/$arch/
EOF
		ok "Repositories added"
	else
		ok "devkitPro repositories already configured"
	fi
else
	die "No pacman found. Install devkitPro first: https://devkitpro.org/wiki/Getting_Started"
fi

# Arch does not support partial upgrades, so sync + upgrade + install in one go
info "Installing ${DKP_PACKAGES[*]} (this also upgrades the system on Arch)"
sudo "$PACMAN" -Syu --needed --noconfirm "${DKP_PACKAGES[@]}"

export DEVKITPRO="${DEVKITPRO:-/opt/devkitpro}"
export DEVKITPPC="${DEVKITPPC:-$DEVKITPRO/devkitPPC}"
export PATH="$DEVKITPPC/bin:$DEVKITPRO/tools/bin:$PATH"

[ -x "$DEVKITPPC/bin/powerpc-eabi-gcc" ] || die "powerpc-eabi-gcc not found in $DEVKITPPC/bin"
command -v elf2dol >/dev/null 2>&1 || die "elf2dol not found in $DEVKITPRO/tools/bin"
ok "devkitPPC: $("$DEVKITPPC/bin/powerpc-eabi-gcc" -dumpversion)"

mkdir -p "$DEPS_DIR"

#---------------------------------------------------------------------------------
# 2. GRRLIB + libpngu
#---------------------------------------------------------------------------------
PORTLIBS_WII="$DEVKITPRO/portlibs/wii"
if [ -f "$PORTLIBS_WII/lib/libgrrlib.a" ] && [ -f "$PORTLIBS_WII/lib/libpngu.a" ]; then
	ok "GRRLIB already installed"
else
	info "Building GRRLIB + libpngu"
	if [ -d "$DEPS_DIR/GRRLIB/.git" ]; then
		git -C "$DEPS_DIR/GRRLIB" pull --ff-only
	else
		git clone --depth 1 "$GRRLIB_REPO" "$DEPS_DIR/GRRLIB"
	fi
	make -C "$DEPS_DIR/GRRLIB/GRRLIB" clean
	make -C "$DEPS_DIR/GRRLIB/GRRLIB" -j"$JOBS" all
	as_root_for "$PORTLIBS_WII" env DEVKITPRO="$DEVKITPRO" DEVKITPPC="$DEVKITPPC" PATH="$PATH" \
		make -C "$DEPS_DIR/GRRLIB/GRRLIB" install
	ok "GRRLIB installed into $PORTLIBS_WII"
fi

#---------------------------------------------------------------------------------
# 3. mbedTLS (must use WiiFin's mbedtls_config.h, same as the bundled headers)
#---------------------------------------------------------------------------------
MBEDTLS_OUT="$REPO_DIR/libs/mbedtls/lib"
MBEDTLS_CONFIG="$REPO_DIR/libs/mbedtls/include/mbedtls/mbedtls_config.h"
MBEDTLS_STAMP="$MBEDTLS_OUT/.config.sha256"   # rebuild when the config changes
MBEDTLS_HASH="$(sha256sum "$MBEDTLS_CONFIG" | cut -d' ' -f1)"
if [ -f "$MBEDTLS_OUT/libmbedtls.a" ] && [ -f "$MBEDTLS_OUT/libmbedx509.a" ] && [ -f "$MBEDTLS_OUT/libmbedcrypto.a" ] &&
   [ "$(cat "$MBEDTLS_STAMP" 2>/dev/null)" = "$MBEDTLS_HASH" ]; then
	ok "mbedTLS libraries already built"
else
	info "Cross-compiling mbedTLS $MBEDTLS_VERSION"
	MBEDTLS_SRC="$DEPS_DIR/mbedtls-$MBEDTLS_VERSION"
	if [ ! -d "$MBEDTLS_SRC" ]; then
		curl -fL -o "$DEPS_DIR/mbedtls-$MBEDTLS_VERSION.tar.bz2" \
			"https://github.com/Mbed-TLS/mbedtls/releases/download/mbedtls-$MBEDTLS_VERSION/mbedtls-$MBEDTLS_VERSION.tar.bz2"
		tar -xjf "$DEPS_DIR/mbedtls-$MBEDTLS_VERSION.tar.bz2" -C "$DEPS_DIR"
	fi
	cp "$MBEDTLS_CONFIG" "$MBEDTLS_SRC/include/mbedtls/mbedtls_config.h"
	make -C "$MBEDTLS_SRC/library" clean >/dev/null
	make -C "$MBEDTLS_SRC/library" -j"$JOBS" \
		CC="$DEVKITPPC/bin/powerpc-eabi-gcc" \
		AR="$DEVKITPPC/bin/powerpc-eabi-ar" \
		CFLAGS="-O2 -DGEKKO -mrvl -mcpu=750 -meabi -mhard-float" \
		static
	mkdir -p "$MBEDTLS_OUT"
	cp "$MBEDTLS_SRC/library/"libmbed{tls,x509,crypto}.a "$MBEDTLS_OUT/"
	echo "$MBEDTLS_HASH" > "$MBEDTLS_STAMP"
	ok "mbedTLS installed into libs/mbedtls/lib"
fi

#---------------------------------------------------------------------------------
# 4. Sanity checks
#---------------------------------------------------------------------------------
info "Checking portlibs"
PKG_CONFIG_LIBDIR="$DEVKITPRO/portlibs/ppc/lib/pkgconfig" pkg-config --exists freetype2 libpng libjpeg \
	|| die "pkg-config cannot find freetype2/libpng/libjpeg in $DEVKITPRO/portlibs/ppc"
ok "freetype2, libpng, libjpeg found"

if [ -f "$REPO_DIR/libs/mplayer-ce-build/libmplayer.a" ]; then
	ok "MPlayer CE found — video playback will be enabled"
else
	warn "libs/mplayer-ce-build/libmplayer.a missing — building without video playback (see MPLAYER_CE_BUILD.md)"
fi

#---------------------------------------------------------------------------------
# 5. Build
#---------------------------------------------------------------------------------
if [ "$DO_BUILD" -eq 1 ]; then
	info "Building WiiFin"
	"$REPO_DIR/build.sh"
	ok "Built $REPO_DIR/WiiFin.dol"
fi

echo
ok "Setup complete. Open a new shell (or log out/in) so DEVKITPRO/DEVKITPPC are set by /etc/profile.d."
