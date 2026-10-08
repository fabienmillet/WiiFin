#!/bin/bash
# Makes data/fonts/cjk/NotoSansCJK-WiiFin.otf: Noto Sans CJK (SC face) cut
# down to what WiiFin draws with it, the characters its built-in fonts lack:
# Korean (Hangul syllables and jamo), the CJK ideographs of Chinese, Japanese
# and Korean, kana, bopomofo, CJK punctuation and full-width forms.  6 MB:
# too big for the DOL (MEM1), so it goes to the SD card (apps/WiiFin/fonts/,
# in the Homebrew Channel package), read into MEM2 at start-up.
#
# usage: tools/make_cjk_font.sh [NotoSansCJK-Regular.ttc]
#   default: the system's (Arch: noto-fonts-cjk), else downloaded from
#   github.com/notofonts/noto-cjk.  Needs Python 3 (fonttools in a venv).
set -e
T=$(dirname "$(realpath "$0")"); R=$(realpath "$T/..")
SRC=${1:-/usr/share/fonts/noto-cjk/NotoSansCJK-Regular.ttc}
W=$(mktemp -d); trap 'rm -rf "$W"' EXIT
if [ ! -f "$SRC" ]; then
    SRC=$W/NotoSansCJK-Regular.ttc
    curl -fsSL -o "$SRC" https://github.com/notofonts/noto-cjk/raw/main/Sans/OTC/NotoSansCJK-Regular.ttc
fi
python3 -m venv "$W/venv" && "$W/venv/bin/pip" -q install fonttools
# face 2: Noto Sans CJK SC (the Chinese forms of the shared ideographs)
"$W/venv/bin/pyftsubset" "$SRC" --font-number=2 \
    --unicodes="U+1100-11FF,U+3000-303F,U+3040-30FF,U+3100-312F,U+3130-318F,U+31F0-31FF,U+3200-32FF,U+4E00-9FFF,U+AC00-D7A3,U+FF00-FFEF" \
    --layout-features='' --no-hinting --drop-tables+=GSUB,GPOS,vhea,vmtx,VORG,DSIG,BASE --name-IDs='*' \
    --output-file="$R/data/fonts/cjk/NotoSansCJK-WiiFin.otf"
ls -l "$R/data/fonts/cjk/NotoSansCJK-WiiFin.otf"
