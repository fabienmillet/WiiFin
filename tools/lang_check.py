#!/usr/bin/env python3
"""Check that every character of every translation (lang/*.po) is in one
of the fonts WiiFin draws with: the interface's, the Japanese one built in,
or the CJK one the release puts in apps/WiiFin/fonts/ (Chinese, Korean).
A character none has would show as a gap on the Wii.

    tools/lang_check.py        (needs fc-query, from fontconfig)

Characters Text.cpp draws as others (typographic quotes and dashes, the
no-break and narrow spaces...) are allowed too.
"""
import re
import subprocess
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
FONTS = [ROOT / 'data/fonts/wii_font.ttf', ROOT / 'data/fonts/jp_font.ttf',
         ROOT / 'data/fonts/cjk/NotoSansCJK-WiiFin.otf']
# drawn as something else (source/core/Text.cpp: normalize, substitute)
DRAWN_AS_OTHER = set(range(0x2018, 0x2020)) | {0x00AB, 0x00BB, 0x2026, 0x2032, 0x2033} | \
    set(range(0x2010, 0x2016)) | set(range(0xFF01, 0xFF5F)) | \
    {0x00A0, 0x202F, 0x205F, 0x3000, 0x2028, 0x2029} | set(range(0x2000, 0x200B))


def charset(font):
    out = subprocess.run(['fc-query', '--format', '%{charset}', str(font)],
                         capture_output=True, text=True, check=True).stdout
    have = set()
    for part in out.split():
        a, _, b = part.partition('-')
        have.update(range(int(a, 16), int(b or a, 16) + 1))
    return have


def msgstrs(po):
    text = po.read_text(encoding='utf-8')
    for m in re.finditer(r'^msgstr "(.*)"$', text, re.M):
        yield m.group(1).encode('utf-8').decode('unicode_escape').encode('latin-1').decode('utf-8')


def main():
    have = set()
    for f in FONTS:
        have |= charset(f)
    bad = 0
    for po in sorted((ROOT / 'lang').glob('*.po')):
        missing = {}
        for s in msgstrs(po):
            for ch in s:
                cp = ord(ch)
                if cp >= 0x20 and cp not in have and cp not in DRAWN_AS_OTHER:
                    missing.setdefault(ch, s)
        if missing:
            bad += 1
            print('%s: %d character(s) in no font: %s' % (
                po.name, len(missing), ' '.join('%s (U+%04X)' % (c, ord(c)) for c in sorted(missing))))
        else:
            print('%s: every character drawn' % po.name)
    return 1 if bad else 0


if __name__ == '__main__':
    sys.exit(main())
