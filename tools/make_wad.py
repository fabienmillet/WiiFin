#!/usr/bin/env python3
"""Build WiiFin.wad from WiiFin.dol (Linux, macOS or Windows; needs libWiiPy).

    python3 tools/make_wad.py [WiiFin.dol] [WiiFin.wad]

tools/wad/template.wad is the channel without the app: ticket, TMD and
certificates of title 00010001-WIFN, the banner (content 0) and the
CustomizeMii NAND loader (content 1, the boot content).  The loader reads
content 2 and jumps to it; this script puts WiiFin.dol there, updates the
TMD (size, hash) and fakesigns the ticket and TMD, so the WAD installs with
any WAD manager running on a patched IOS.

The loader runs from 0x80804000-0x80831E40, a range WiiFin's own data covers:
WiiFin embeds the loader's bytes there (tools/stub_zone.bin, with one branch
retargeted to WiiFin's entry point) so the loader survives being written
over.  The check below makes sure both copies still match.
"""
import struct
import sys
from pathlib import Path

try:
    import libWiiPy
except ImportError:
    sys.exit("make_wad.py needs libWiiPy:\n"
             "  python3 -m venv tools/.venv && tools/.venv/bin/pip install libWiiPy\n"
             "  make wad WADPY=tools/.venv/bin/python")

TOOLS = Path(__file__).resolve().parent
TEMPLATE = TOOLS / "wad" / "template.wad"
STUB_ZONE = TOOLS / "stub_zone.bin"
ZONE_START, ZONE_END = 0x80804000, 0x80831E40
PATCHED_WORD = 0x8081B768          # bctrl -> b 0x80003F00 in stub_zone.bin
APP_INDEX = 2                      # content the loader reads


def loader_image(dol: bytes) -> bytearray:
    """The loader as it sits in memory over the stub zone."""
    off = struct.unpack(">18I", dol[0:72])
    addr = struct.unpack(">18I", dol[72:144])
    size = struct.unpack(">18I", dol[144:216])
    img = bytearray(ZONE_END - ZONE_START)
    for o, a, s in zip(off, addr, size):
        if s and ZONE_START <= a < ZONE_END:
            img[a - ZONE_START:a - ZONE_START + s] = dol[o:o + s]
    return img


def main() -> None:
    dol_path = Path(sys.argv[1] if len(sys.argv) > 1 else "WiiFin.dol")
    wad_path = Path(sys.argv[2] if len(sys.argv) > 2 else "WiiFin.wad")
    dol = dol_path.read_bytes()
    if len(dol) < 0x100 or struct.unpack(">I", dol[0xE0:0xE4])[0] != 0x80003F00:
        sys.exit(f"{dol_path}: not WiiFin's DOL (entry point is not 0x80003F00)")

    title = libWiiPy.title.Title()
    title.load_wad(TEMPLATE.read_bytes())

    img = loader_image(title.get_content_by_index(title.tmd.boot_index))
    ref = STUB_ZONE.read_bytes()
    w = PATCHED_WORD - ZONE_START
    if len(img) != len(ref) or img[:w] != ref[:w] or img[w + 4:] != ref[w + 4:]:
        sys.exit("the template's NAND loader no longer matches tools/stub_zone.bin")

    title.set_content(dol, APP_INDEX)
    title.fakesign()
    wad_path.write_bytes(title.dump_wad())
    print(f"{wad_path}: title {title.tmd.title_id}, IOS{int(title.tmd.ios_tid, 16) & 0xFF}, "
          f"{len(dol)} bytes of DOL, fakesigned")


if __name__ == "__main__":
    main()
