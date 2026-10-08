#!/usr/bin/env python3
"""Find screens that flash for a frame or two in a Dolphin frame dump.

    tools/test/flicker.py <dump.avi> [out folder]

The dump is cut into stable stretches wherever the picture changes a lot
from one frame to the next.  A stretch shorter than 100 ms is a flash when
it does not lie between its neighbours: a step of a fade or a slide is
about as far from the picture before as from the one after (A -> A/B -> B),
a flash is far from both (A -> X -> A, or A -> X -> B with X unrelated).
A fade through black is no flash: a black stretch that the frames after it
brighten out of step by step (or that the frames before it darken into).
Each flash is saved as before / flash / after pictures in the out folder.
Exit status 1 when there is one.  Needs ffmpeg and numpy.
"""
import os
import subprocess
import sys

import numpy as np

W, H = 96, 56          # small greyscale frames: enough to tell screens apart
CUT = 6.0              # mean difference (0-255) of a change of screen
SHORT = 0.1            # s: a stretch this short can be a flash
FOREIGN = 1.8          # (before->X + X->after) / (before->after) above: a flash
DARK = 8.0             # mean level (0-255) of a black picture
FADE = 5               # frames of steady brightening / darkening: a fade


def frames(path):
    pts = [float(x) for x in subprocess.run(
        ['ffprobe', '-v', 'error', '-select_streams', 'v', '-show_entries', 'frame=pts_time',
         '-of', 'csv=p=0', path], capture_output=True, text=True).stdout.split() if x]
    raw = subprocess.run(['ffmpeg', '-loglevel', 'error', '-i', path, '-fps_mode', 'passthrough',
                          '-vf', 'scale=%d:%d,format=gray' % (W, H), '-f', 'rawvideo', '-'],
                         capture_output=True).stdout
    n = min(len(raw) // (W * H), len(pts))
    return np.frombuffer(raw[:n * W * H], np.uint8).reshape(n, H, W).astype(np.int16), pts[:n]


def diff(a, b):
    return float(np.abs(a - b).mean())


def fade(f, a, b):
    """The stretch [a, b) is black, and the frames after it brighten out of it
    one step after another, or the frames before it darkened into it."""
    if f[a].mean() > DARK:
        return False
    def steady(levels):
        return len(levels) > FADE and all(y > x + 0.5 for x, y in zip(levels, levels[1:]))
    after = [float(f[i].mean()) for i in range(b - 1, min(b + FADE, len(f)))]
    before = [float(f[i].mean()) for i in range(a, max(a - FADE - 1, -1), -1)]
    return steady(after) or steady(before)


def main():
    path = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(os.path.dirname(path), 'flicker')
    f, pts = frames(path)
    if len(f) < 3:
        sys.exit('flicker: no frames in %s' % path)
    # stretches: [start, end) frame indexes
    cuts = [0] + [i for i in range(1, len(f)) if diff(f[i - 1], f[i]) > CUT] + [len(f)]
    runs = [(a, b) for a, b in zip(cuts, cuts[1:]) if b > a]
    found = []
    for k in range(1, len(runs) - 1):
        a, b = runs[k]
        dur = (pts[b] if b < len(pts) else pts[-1] + 1 / 60) - pts[a]
        if dur >= SHORT:
            continue
        before, x, after = f[runs[k - 1][1] - 1], f[a], f[runs[k + 1][0]]
        bx, xa, ba = diff(before, x), diff(x, after), diff(before, after)
        if bx > CUT and xa > CUT and (bx + xa) > FOREIGN * max(ba, CUT) and not fade(f, a, b):
            found.append((pts[a], dur, runs[k - 1][1] - 1, a, runs[k + 1][0]))
    os.makedirs(out, exist_ok=True)
    for t, dur, i0, i1, i2 in found:
        print('flash at %.2f s for %d ms' % (t, round(dur * 1000)))
        for name, i in (('before', i0), ('flash', i1), ('after', i2)):
            subprocess.run(['ffmpeg', '-loglevel', 'error', '-y', '-i', path, '-vf',
                            r'select=eq(n\,%d),scale=480:-1' % i, '-frames:v', '1',
                            os.path.join(out, '%07.2f-%s.png' % (t, name))])
    print('flicker: %d flash(es) in %d frames (%.0f s)' % (len(found), len(f), pts[-1] - pts[0]))
    return 1 if found else 0


if __name__ == '__main__':
    sys.exit(main())
