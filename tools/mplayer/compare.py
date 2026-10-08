#!/usr/bin/env python3
"""Compare two builds of libmplayer.a object by object.

    tools/mplayer/compare.py <reference.a> <new.a>

For each member: code size (all .text sections) and the symbols it defines
and uses.  Built from the same source with the same compiler and options,
an object comes out identical; a difference points at a changed source
file or option.  Exit status 1 when anything differs.
"""
import os
import subprocess
import sys
import tempfile

TOOLS = os.environ.get('DEVKITPPC', '/opt/devkitpro/devkitPPC') + '/bin/powerpc-eabi-'


def members(archive):
    d = tempfile.mkdtemp()
    names = subprocess.run([TOOLS + 'ar', 't', archive], capture_output=True, text=True,
                           check=True).stdout.split()
    info = {}
    seen = {}
    for name in names:
        # duplicate names (utils.o of several FFmpeg libraries): ar xN
        seen[name] = seen.get(name, 0) + 1
        out = os.path.join(d, '%s.%d' % (name, seen[name]))
        os.makedirs(out, exist_ok=True)
        subprocess.run([TOOLS + 'ar', 'xN', str(seen[name]), os.path.abspath(archive), name],
                       cwd=out, check=True)
        obj = os.path.join(out, name)
        text = 0
        for line in subprocess.run([TOOLS + 'objdump', '-h', obj], capture_output=True,
                                   text=True).stdout.split('\n'):
            w = line.split()
            if len(w) > 2 and w[1].startswith('.text'):
                text += int(w[2], 16)
        defined, used = set(), set()
        for line in subprocess.run([TOOLS + 'nm', obj], capture_output=True, text=True).stdout.split('\n'):
            w = line.split()
            if len(w) == 2 and w[0] == 'U':
                used.add(w[1])
            elif len(w) == 3 and w[1] in 'TDBRC':
                defined.add(w[2])
        info['%s#%d' % (name, seen[name])] = (text, defined, used)
    return info


def main():
    ref, new = members(sys.argv[1]), members(sys.argv[2])
    diff = 0
    for k in sorted(set(ref) | set(new)):
        if k not in new:
            print('only in the reference: ' + k); diff += 1; continue
        if k not in ref:
            print('only in the new build: ' + k); diff += 1; continue
        (t1, d1, u1), (t2, d2, u2) = ref[k], new[k]
        notes = []
        if t1 != t2:
            notes.append('code %d -> %d bytes' % (t1, t2))
        if d1 != d2:
            notes.append('defines ' + ' '.join(['-' + s for s in sorted(d1 - d2)] + ['+' + s for s in sorted(d2 - d1)]))
        if u1 != u2:
            notes.append('uses ' + ' '.join(['-' + s for s in sorted(u1 - u2)] + ['+' + s for s in sorted(u2 - u1)]))
        if notes:
            print('%-28s %s' % (k, '; '.join(notes)))
            diff += 1
    print('%d of %d members differ' % (diff, len(set(ref) | set(new))))
    return 1 if diff else 0


if __name__ == '__main__':
    sys.exit(main())
