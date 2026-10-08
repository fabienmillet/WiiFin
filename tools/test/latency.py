#!/usr/bin/env python3
"""How long pause, resume and seeks take, from a playback.sh run.

    tools/test/playback.sh latency
    tools/test/latency.py [tools/test/out/playback/latency/dolphin/Logs/dolphin.log]

Reads Dolphin's log, whose lines carry the host time (ms), and the
button presses of the scenario ([TOUR] lines):
- A while playing: pause.  MPlayer prints a status line ("A: ... V: ...")
  for each frame; the pause is the last one after the press.
- A while paused: resume, the first status line after the press.
- RIGHT / LEFT: a +10 s / -10 s seek, which WiiFin plays as a new stream;
  the stages of the new stream are listed until its first picture.
"""
import re
import sys
from pathlib import Path

LOG = sys.argv[1] if len(sys.argv) > 1 else str(
    Path(__file__).resolve().parent / 'out' / 'playback' / 'latency' / 'dolphin' / 'Logs' / 'dolphin.log')

STAGES = [
    ('[WiiPlayer] mplayer exited', 'previous stream stopped'),
    ('[PlaybackInfo] POST', 'stream requested from Jellyfin'),
    ('[PlaybackInfo] HTTP status', 'Jellyfin answered'),
    ('[WiiPlayer] play:', 'MPlayer started'),
    ('open_stream() returned', 'stream connected'),
    ('stream_enable_cache returned', 'cache filled'),
    ('demux_open() returned', 'stream format read'),
    ('Starting playback', 'playback starts'),
    ('loading_active cleared', 'first picture'),
]
BUTTONS = {0x8: 'A', 0x200: 'RIGHT', 0x100: 'LEFT'}


def read():
    events, last_ms, base = [], None, 0
    for raw in open(LOG, errors='replace'):
        m = re.match(r'(\d+):(\d+):(\d+) .*?\]: (.*)', raw.rstrip('\r\n'))
        if not m:
            continue
        ms = (int(m.group(1)) * 60 + int(m.group(2))) * 1000 + int(m.group(3)) + base
        if last_ms is not None and ms < last_ms - 1800000:   # the hour turned
            base += 3600000
            ms += 3600000
        last_ms = ms
        events.append((ms, m.group(4)))
    return events


def status(text):
    m = re.match(r'A:\s*(-?[\d.]+) V:\s*(-?[\d.]+)', text)
    return (float(m.group(1)), float(m.group(2))) if m else None


def main():
    ev = read()
    paused = False
    playing = False   # presses before the first stream are menu presses
    for i, (t, text) in enumerate(ev):
        playing = playing or 'Starting playback' in text
        m = re.match(r'\[TOUR\] t=\d+ press ([0-9a-f]+)$', text)
        if not playing or not m or int(m.group(1), 16) not in BUTTONS:
            continue
        button = BUTTONS[int(m.group(1), 16)]
        before = next((status(x) for _, x in reversed(ev[:i]) if status(x)), None)
        after = [(u, status(x)) for u, x in ev[i + 1:] if status(x)]
        if button == 'A' and not paused:
            # the last status line before a gap of more than 0.5 s
            stop = None
            for k, (u, s) in enumerate(after):
                nxt = after[k + 1][0] if k + 1 < len(after) else None
                if nxt is None or nxt - u > 500:
                    stop = (u, s)
                    break
            if stop and before:
                print('pause:  picture and sound stop %4d ms after the press, %.2f s of video later'
                      % (stop[0] - t, stop[1][1] - before[1]))
            paused = True
        elif button == 'A':
            if after:
                print('resume: playback goes on %4d ms after the press' % (after[0][0] - t))
            paused = False
        else:
            print('%s (seek %s10 s):' % (button, '+' if button == 'RIGHT' else '-'))
            seen = set()
            for u, x in ev[i + 1:]:
                for key, name in STAGES:
                    if key in x and key not in seen:
                        seen.add(key)
                        print('  %6d ms  %s' % (u - t, name))
                if 'loading_active cleared' in x or re.match(r'\[TOUR\] t=\d+ press', x):
                    break
            paused = False


if __name__ == '__main__':
    main()
