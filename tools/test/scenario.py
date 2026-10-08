#!/usr/bin/env python3
"""Playback scenarios (tools/test/scenarios/*.txt), used by playback.sh.

    scenario.py sd <scenario> <sd folder> <session file>
        writes apps/WiiFin/wiifin.cfg (profile, settings) and tour.txt (presses)
    scenario.py positions <scenario> <jellyfin url> <session file>
        sets the saved positions on the server
    scenario.py check <scenario> <log>
        checks WiiFin's log against the expectations
    scenario.py remote <scenario> <dolphin log> <jellyfin url> <session file>
        sends the scenario's remote commands as WiiFin's log reaches them
        (playback.sh runs it beside Dolphin)

A scenario file holds, one per line (# starts a comment):
    server <url>            the saved profile's server (default: Jellyfin direct)
    cfg <key>=<value>       more wiifin.cfg settings
    position <name> <s>     saved position of the item whose name or file
                            starts with <name> (quotes around spaces)
    positions-reset         every other film and episode back to the start
                            (Continue Watching then holds only those given)
    sound <name.wav|.mp3>   a short tone as that custom sound (sounds/ on the
                            SD card; ffmpeg makes the MP3s)
    overview <name> <text>  the synopsis of that film or episode on the server
                            (\\n: a new paragraph), locked against refreshes
    sdfile <file> <to>      a file of the repository (path from its root) to
                            apps/WiiFin/<to> on the SD card (e.g. a font)
    remote <log text> <ms> <command> [args]
                            the server's remote control: ms after the first
                            log line containing <log text>, the command to
                            WiiFin's session: Pause, Unpause, PlayPause, Stop,
                            NextTrack, PreviousTrack, Seek <s>,
                            Message <header> <text>, Play <item name> ("Play
                            on..."), PlayNext / PlayLast <item name> (the
                            queue), a general command with key=value
                            arguments (VolumeUp, SetVolume Volume=40...), or
                            ExpectVolume <n>: the server shows that volume
                            (remote: lines, added to the log)
    <ms> <button> [note]    a press, ms after WiiFin first reads the buttons:
                            A B 1 2 PLUS MINUS HOME UP DOWN LEFT RIGHT, ZL (the
                            zoom button, as the GameCube's Z), POINTER /
                            NOPOINTER (shows / hides a pointer), or END (the end)
    expect streams <n>      number of streams opened
    expect advance <s>      least video progress per stream (default 5)
    expect url <k> <text>   stream k's URL contains <text>
    expect log <text>       a log line contains <text>
    expect nolog <text>     no log line contains <text>
    expect nologafter <a> <b>  no line containing b after the first containing a
    expect logre <regex>    a log line matches <regex> (Python re.search)
    expect logcount <n> <text>  at least n log lines contain <text>
    expect logorder <a> <b> ... log lines containing a, then b... in that order
Every stream must also start, show its first picture within 1.5 s of
video and decode without errors (DECODE_ERRORS: a broken picture), no
error screen may show, and every screen
shown between POINTER and NOPOINTER must draw the pointer (the note after
POINTER names the screen).
"""
import json
import math
import os
import re
import shlex
import shutil
import struct
import subprocess
import sys
import time
import urllib.request
import wave

BUTTONS = {'A': 0x8, 'B': 0x4, '1': 0x2, '2': 0x1, 'PLUS': 0x1000, 'MINUS': 0x10,
           'HOME': 0x80, 'UP': 0x800, 'DOWN': 0x400, 'LEFT': 0x100, 'RIGHT': 0x200, 'END': 0,
           # a Classic Controller's ZL: the zoom button (GameCube Z), + on its own in the menus
           'ZL': 0x00800000,
           # the test build's pointer (tour.sh), at a fixed spot
           'POINTER': 0x04000000, 'NOPOINTER': 0x02000000}
DIRECT = 'http://127.0.0.1:18096'
PICTURE = 1.5      # s of video before the first picture, at most
DECODE_ERRORS = ('Error at MB', 'error while decoding MB', 'Frame num gap', 'mmco: unref')


def parse(path):
    sc = {'server': DIRECT, 'cfg': [], 'positions': [], 'presses': [], 'expect': []}
    for n, line in enumerate(open(path), 1):
        w = shlex.split(line, comments=True)
        if not w:
            continue
        if w[0] == 'server':
            sc['server'] = w[1]
        elif w[0] == 'cfg':
            sc['cfg'].append(w[1])
        elif w[0] == 'position':
            sc['positions'].append((w[1], int(w[2])))
        elif w[0] == 'positions-reset':
            sc['positions_reset'] = True
        elif w[0] == 'overview':
            sc.setdefault('overviews', []).append((w[1], w[2].replace('\\n', '\n')))
        elif w[0] == 'sound':
            sc.setdefault('sounds', []).append(w[1])
        elif w[0] == 'sdfile':
            sc.setdefault('sdfiles', []).append((w[1], w[2]))
        elif w[0] == 'remote':
            sc.setdefault('remote', []).append((w[1], int(w[2]), w[3], w[4:]))
        elif w[0] == 'expect':
            sc['expect'].append(w[1:])
        elif w[0].isdigit():
            if w[1] not in BUTTONS:
                sys.exit('%s:%d: unknown button %s' % (path, n, w[1]))
            sc['presses'].append((int(w[0]), BUTTONS[w[1]], ' '.join(w[2:]) or w[1]))
        else:
            sys.exit('%s:%d: cannot read %r' % (path, n, line.strip()))
    return sc


def session(path):
    token, user = open(path).read().split()[:2]
    return token, user


def write_sd(sc, sd, sess):
    token, user = session(sess)
    d = os.path.join(sd, 'apps', 'WiiFin')
    os.makedirs(d, exist_ok=True)
    # device_id: the tests' own, apart from a Dolphin of the user's on the same server
    cfg = ['music_enabled=0', 'home_layout=0', 'library_view=2', 'screen_area_asked=1',
           'device_id=wiifin-test-dolphin'] + sc['cfg'] + [
        'profile_count=1', 'profile.0.server_url=' + sc['server'], 'profile.0.username=wii',
        'profile.0.server_name=Jellyfin Test', 'profile.0.user_id=' + user, 'profile.0.access_token=' + token]
    open(os.path.join(d, 'wiifin.cfg'), 'w').write('\n'.join(cfg) + '\n')
    open(os.path.join(d, 'tour.txt'), 'w').write(''.join('%d %x\n' % p[:2] for p in sc['presses']))
    # custom sounds and files: this scenario's only (another one's would stay
    # on the card)
    sd_sounds = os.path.join(d, 'sounds')
    shutil.rmtree(sd_sounds, ignore_errors=True)
    shutil.rmtree(os.path.join(d, 'fonts'), ignore_errors=True)
    root = os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', '..')
    for src, to in sc.get('sdfiles', []):
        os.makedirs(os.path.dirname(os.path.join(d, to)), exist_ok=True)
        shutil.copy(os.path.join(root, src), os.path.join(d, to))
    for name in sc.get('sounds', []):
        os.makedirs(sd_sounds, exist_ok=True)
        make_sound(os.path.join(sd_sounds, name))


def make_sound(path):
    """A short tone: name.wav written here, name.mp3 by ffmpeg"""
    if path.endswith('.wav'):
        rate, n = 32000, 6400   # 0.2 s, mono 16-bit
        with wave.open(path, 'wb') as w:
            w.setnchannels(1); w.setsampwidth(2); w.setframerate(rate)
            w.writeframes(b''.join(struct.pack('<h', int(8000 * math.sin(2 * math.pi * 880 * i / rate)))
                                   for i in range(n)))
    else:
        subprocess.run(['ffmpeg', '-v', 'error', '-y', '-f', 'lavfi', '-i', 'sine=frequency=660:duration=3',
                        '-ac', '2', '-b:a', '96k', path], check=True)


def set_positions(sc, url, sess):
    token, user = session(sess)
    auth = {'Authorization': 'MediaBrowser Client="WiiFinTest", Device="test", '
                             'DeviceId="wiifin-test", Version="1", Token="%s"' % token}
    q = urllib.request.Request('%s/Items?Recursive=true&IncludeItemTypes=Movie,Episode&Fields=Path&userId=%s'
                               % (url, user), headers=auth)
    items = json.load(urllib.request.urlopen(q))['Items']
    positions = sc['positions']
    if sc.get('positions_reset'):   # nothing else in Continue Watching
        names = [n for n, _ in positions]
        positions = [(i['Name'], 0) for i in items if i['Name'] not in names] + positions
    for name, secs in positions:
        it = next((i for i in items if i['Name'].startswith(name)
                   or os.path.basename(i.get('Path', '')).startswith(name)), None)
        if not it:
            sys.exit('no item "%s" on the server' % name)
        body = json.dumps({'PlaybackPositionTicks': secs * 10000000, 'Played': False}).encode()
        q = urllib.request.Request('%s/UserItems/%s/UserData?userId=%s' % (url, it['Id'], user), data=body,
                                   headers=dict(auth, **{'Content-Type': 'application/json'}), method='POST')
        urllib.request.urlopen(q).read()
    for name, text in sc.get('overviews', []):
        it = next((i for i in items if i['Name'].startswith(name)
                   or os.path.basename(i.get('Path', '')).startswith(name)), None)
        if not it:
            sys.exit('no item "%s" on the server' % name)
        q = urllib.request.Request('%s/Items/%s?userId=%s' % (url, it['Id'], user), headers=auth)
        full = json.load(urllib.request.urlopen(q))
        # the item's update takes the whole of it, lists included
        body = {k: full.get(k) for k in ('Id', 'Name', 'OriginalTitle', 'ProductionYear', 'PremiereDate',
                                          'OfficialRating', 'IndexNumber', 'ParentIndexNumber')}
        body.update(Overview=text, LockData=True, LockedFields=['Overview'], Genres=full.get('Genres', []),
                    Tags=full.get('Tags', []), People=full.get('People', []), Studios=[],
                    ProviderIds=full.get('ProviderIds', {}), Taglines=full.get('Taglines', []))
        q = urllib.request.Request('%s/Items/%s' % (url, it['Id']), data=json.dumps(body).encode(),
                                   headers=dict(auth, **{'Content-Type': 'application/json'}), method='POST')
        urllib.request.urlopen(q).read()


def check(sc, log_path):
    log = open(log_path, errors='replace').read().split('\n')
    fails = []
    if not any(re.match(r'\[TOUR\] t=\d+ press 0$', l) for l in log):
        fails.append('the scenario did not reach its END')
    starts = [i for i, l in enumerate(log) if l.startswith('[WiiPlayer] play:')]
    advance = 5.0
    for e in sc['expect']:
        if e[0] == 'advance':
            advance = float(e[1])
    for n, s in enumerate(starts):
        seg = log[s:starts[n + 1] if n + 1 < len(starts) else len(log)]
        v = [float(m.group(1)) for l in seg for m in [re.search(r'\bV:\s*(-?[\d.]+)', l)] if m]
        # MPEG-4 ("Error at MB") and H.264 (missing references) decoding errors
        errs = sum(any(e in l for e in DECODE_ERRORS) for l in seg)
        shown = (max(v) - min(v)) if v else 0.0
        started = any('Starting playback' in l for l in seg)
        # video played before the first picture shown (vo_wiifin.c): MPlayer's
        # status lines up to it, as its timestamps and theirs differ by stream
        first = next((i for i, l in enumerate(seg) if l.startswith('[vo] first picture')), None)
        before = [float(m.group(1)) for l in seg[:first] for m in [re.search(r'\bV:\s*(-?[\d.]+)', l)] if m]
        black = None if first is None else (max(before) - min(before) if before else 0.0)
        ok = started and shown >= advance and errs == 0 and black is not None and black <= PICTURE
        print('stream %d: %s, video %.1f s, %s, %d decoding errors  %s'
              % (n + 1, 'started' if started else 'NOT STARTED', shown,
                 'NO PICTURE' if black is None else 'first picture after %.1f s' % black,
                 errs, 'ok' if ok else 'FAILED'))
        if not ok:
            fails.append('stream %d' % (n + 1))
    for e in sc['expect']:
        what, args = e[0], e[1:]
        text = ' '.join(args)
        if what == 'streams' and len(starts) != int(args[0]):
            fails.append('%d streams opened, expected %s' % (len(starts), args[0]))
        elif what == 'url':
            k, text = int(args[0]), ' '.join(args[1:])
            if k > len(starts) or text not in log[starts[k - 1]]:
                fails.append('stream %d URL without %s' % (k, text))
        elif what == 'log' and not any(text in l for l in log):
            fails.append('no "%s" in the log' % text)
        elif what == 'nolog' and any(text in l for l in log):
            fails.append('"%s" in the log' % text)
        elif what == 'nologafter':
            at = next((i for i, l in enumerate(log) if args[0] in l), None)
            if at is None:
                fails.append('no "%s" in the log' % args[0])
            elif any(args[1] in l for l in log[at + 1:]):
                fails.append('"%s" in the log after "%s"' % (args[1], args[0]))
        elif what == 'logre' and not any(re.search(text, l) for l in log):
            fails.append('no line matching "%s" in the log' % text)
        elif what == 'logcount':
            n, text = int(args[0]), ' '.join(args[1:])
            got = sum(text in l for l in log)
            if got < n:
                fails.append('"%s" %d times in the log, expected %d' % (text, got, n))
        elif what == 'logorder':
            at, prev = -1, None
            for t in args:
                nxt = next((i for i in range(at + 1, len(log)) if t in log[i]), None)
                if nxt is None:
                    fails.append('no "%s" in the log%s' % (t, ' after "%s"' % prev if prev else ''))
                    break
                at, prev = nxt, t
    # an error is a failure, unless the scenario expects that one (expect log)
    wanted = [' '.join(e[1:]) for e in sc['expect'] if e[0] == 'log']
    for l in log:
        if (l.startswith('[Library] error screen') or l.startswith('[runPlay] giving up')) and \
                not any(w in l for w in wanted):
            fails.append(l)
    # each POINTER .. NOPOINTER stretch: the screen must draw the pointer
    notes = [n for _, b, n in sc['presses'] if b == BUTTONS['POINTER']]
    checks = [tuple(map(int, m.groups())) for l in log
              for m in [re.match(r'\[POINTER\] (\d+) frames, pointer drawn on (\d+)', l)] if m]
    for k, (frames, drawn) in enumerate(checks):
        where = notes[k] if k < len(notes) else '#%d' % (k + 1)
        ok = frames > 0 and drawn >= 0.9 * frames
        print('pointer on "%s": drawn on %d of %d frames  %s' % (where, drawn, frames, 'ok' if ok else 'MISSING'))
        if not ok:
            fails.append('no pointer on "%s"' % where)
    if fails:
        print('FAILED: ' + '; '.join(fails))
        return 1
    print('OK')
    return 0


def remote(sc, dolphin_log, url, sess):
    """The scenario's remote commands, each once its log line has come"""
    token, _ = session(sess)
    auth = {'Authorization': 'MediaBrowser Client="WiiFinTest", Device="test", '
                             'DeviceId="wiifin-test", Version="1", Token="%s"' % token}

    def call(path, method='POST', body=None):
        data = json.dumps(body).encode() if body is not None else None
        h = dict(auth, **({'Content-Type': 'application/json'} if data else {}))
        r = urllib.request.urlopen(urllib.request.Request(url + path, data=data, headers=h, method=method))
        d = r.read()
        return json.loads(d) if d else None

    pending = list(sc.get('remote', []))
    seen = {}      # log text -> when it first showed up (s)
    while pending:
        time.sleep(0.2)
        try:
            lines = [re.sub(r'^.*OSREPORT[^:]*\]: ', '', l).rstrip('\r\n')
                     for l in open(dolphin_log, errors='replace') if 'OSREPORT' in l]
        except OSError:
            continue
        now = time.time()
        for anchor, _, _, _ in pending:
            if anchor not in seen and any(anchor in l for l in lines):
                seen[anchor] = now
        for item in list(pending):
            anchor, delay, cmd, args = item
            if anchor not in seen or now < seen[anchor] + delay / 1000.0:
                continue
            pending.remove(item)
            sessions = [x for x in call('/Sessions?deviceId=wiifin-test-dolphin', 'GET') or []
                        if x.get('SupportsRemoteControl')]
            if not sessions:
                print('remote: no remote-controllable session for %s' % cmd, flush=True)
                continue
            sid = sessions[0]['Id']
            if cmd == 'ExpectVolume':
                got = (sessions[0].get('PlayState') or {}).get('VolumeLevel')
                print('remote: volume %s %s' % (got, 'reported' if str(got) == args[0] else
                                                'reported, expected %s' % args[0]), flush=True)
                continue
            if cmd in ('Play', 'PlayNext', 'PlayLast'):
                _, user = session(sess)
                items = call('/Items?Recursive=true&IncludeItemTypes=Movie,Episode,Audio&userId=%s' % user,
                             'GET')['Items']
                it = next((i for i in items if i['Name'].startswith(args[0])), None)
                if not it:
                    print('remote: no item "%s"' % args[0], flush=True)
                    continue
                call('/Sessions/%s/Playing?playCommand=%s&itemIds=%s'
                     % (sid, 'PlayNow' if cmd == 'Play' else cmd, it['Id']))
                print('remote: %s %s sent' % (cmd, args[0]), flush=True)
                continue
            if cmd == 'Seek':
                call('/Sessions/%s/Playing/Seek?seekPositionTicks=%d' % (sid, int(float(args[0]) * 1e7)))
            elif cmd in ('Pause', 'Unpause', 'PlayPause', 'Stop', 'NextTrack', 'PreviousTrack',
                         'FastForward', 'Rewind'):
                call('/Sessions/%s/Playing/%s' % (sid, cmd))
            elif cmd == 'Message':
                call('/Sessions/%s/Message' % sid, body={'Header': args[0], 'Text': args[1], 'TimeoutMs': 6000})
            else:
                body = {'Name': cmd, 'Arguments': dict(a.split('=', 1) for a in args)}
                call('/Sessions/%s/Command' % sid, body=body)
            print('remote: %s sent' % cmd, flush=True)


if __name__ == '__main__':
    cmd, sc = sys.argv[1], parse(sys.argv[2])
    if cmd == 'sd':
        write_sd(sc, sys.argv[3], sys.argv[4])
    elif cmd == 'positions':
        set_positions(sc, sys.argv[3], sys.argv[4])
    elif cmd == 'check':
        sys.exit(check(sc, sys.argv[3]))
    elif cmd == 'remote':
        remote(sc, sys.argv[3], sys.argv[4], sys.argv[5])
    else:
        sys.exit(__doc__)
