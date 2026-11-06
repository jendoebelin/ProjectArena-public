"""Self-test for SOUND STUDIO's library on the relay (0.3.16): the catalog and downloads over TCP (PASND1), votes over
UDP (SOUNDVOTE), the cap, DELETE MY STATS, and the owner's --sound-totals / --sound-remove. A local relay, fake games.
Usage: python test_relay_sounds.py"""
import argparse
import asyncio
import hashlib
import io
import json
import os
import socket
import struct
import sys
import tempfile
import wave

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import projectarena_relay as R  # noqa: E402
from test_relay_stats import Fake, check  # noqa: E402
import test_relay_stats as T  # noqa: E402

PORT = T.PORT


def wav(seconds=0.2, channels=1, rate=48000, tone=440):
    b = io.BytesIO()
    w = wave.open(b, 'wb')
    w.setnchannels(channels); w.setsampwidth(2); w.setframerate(rate)
    n = int(seconds * rate)
    w.writeframes(b''.join(struct.pack('<h', int(8000 * ((i * tone // rate) % 2 * 2 - 1))) * channels for i in range(n)))
    w.close()
    return b.getvalue()


def tcp(payload):
    s = socket.create_connection(('127.0.0.1', PORT), timeout=3)
    s.sendall(payload)
    got = b''
    while True:
        d = s.recv(65536)
        if not d:
            break
        got += d
    s.close()
    return got


def catalog(key=bytes(16)):
    got = tcp(b'PASND1\n' + bytes([1]) + key)
    if not got.startswith(b'PASND1\n') or not got.endswith(b'END\n'):
        return None
    rows = {}
    for line in got[7:-4].decode().splitlines():
        name, size, sha, vote = line.split('\t')
        rows[name] = (int(size), sha, int(vote))
    return rows


def fetch(name):
    n = name.encode()
    got = tcp(b'PASND1\n' + bytes([2, len(n)]) + n)
    size = struct.unpack_from('<I', got)[0]
    return None if size == 0xFFFFFFFF else got[4:4 + size]


def vote(f, name, v):
    n = name.encode()
    f.raw(R.SOUNDVOTE, bytes([len(n)]) + n + struct.pack('<b', v))


async def run():
    import time
    loop = asyncio.get_running_loop()
    tmp = tempfile.mkdtemp()
    db, lib = os.path.join(tmp, 'stats.db'), os.path.join(tmp, 'sound_library')
    os.makedirs(lib)
    t0 = time.time() - 1000
    files = {}
    for i in range(7):   # seven for ShieldBreak: the five oldest are kept
        n = 'ShieldBreak__try_%d.wav' % i
        files[n] = wav(0.1 + 0.01 * i, tone=300 + 50 * i)
    files['Bounce__boing.wav'] = wav(0.3)
    files['Bounce__stereo.wav'] = wav(0.2, channels=2)           # not mono: left out
    files['Bounce__slow.wav'] = wav(0.2, rate=44100)             # not 48 kHz: left out
    files['Nothing__made_up.wav'] = wav(0.2)                     # no such event
    files['ShieldBreak__Bad Name.wav'] = wav(0.2)                # not a-z0-9_
    for k, (n, data) in enumerate(sorted(files.items())):
        p = os.path.join(lib, n)
        open(p, 'wb').write(data)
        os.utime(p, (t0 + k, t0 + k))
    logs = []
    stats = R.Stats(db, log=lambda m: None)
    relay = R.Relay(8, log=logs.append, stats=stats)
    relay.sounds = R.Sounds(lib, stats, logs.append)
    tr, _ = await loop.create_datagram_endpoint(lambda: relay, local_addr=('127.0.0.1', PORT))
    reports = R.Reports(os.path.join(tmp, 'reports'), logs.append, relay, relay.sounds)
    srv = await asyncio.start_server(reports.handle, '127.0.0.1', PORT)

    def tot(name):   # (the relay's own database connection belongs to its thread)
        import sqlite3
        look = sqlite3.connect(db)
        up = look.execute('SELECT COUNT(*) FROM sound_votes WHERE sound = ? AND vote > 0', (name,)).fetchone()[0]
        down = look.execute('SELECT COUNT(*) FROM sound_votes WHERE sound = ? AND vote < 0', (name,)).fetchone()[0]
        look.close()
        return up, down

    def blocking():
        c = catalog()
        check(c is not None, 'the catalog answers (PASND1 ... END)')
        names = sorted(c or {})
        check(names == ['Bounce__boing.wav'] + ['ShieldBreak__try_%d.wav' % i for i in range(5)],
              'only proper sounds are listed, five for an event at most, the oldest first (%s)' % names)
        check(any('Bounce__stereo.wav left out' in m for m in logs) and any('Nothing__made_up.wav left out' in m for m in logs),
              'the ones left out are logged, with why')
        got = fetch('Bounce__boing.wav')
        check(got == files['Bounce__boing.wav'] and c and hashlib.sha256(got).hexdigest() == c['Bounce__boing.wav'][1],
              'a sound downloads whole, its checksum as the catalog says')
        check(fetch('Nothing__here.wav') is None and fetch('../stats.db') is None, 'no other file can be fetched')
        # votes: one per player, changed and taken back; players see only their own
        a, b, nokey = Fake('ACE', 9), Fake('BEE', 9), Fake('NOKEY', 9)
        for f in (a, b, nokey):
            f.hello()
        a.raw(R.KEY, b'a' * 16)
        b.raw(R.KEY, b'b' * 16)
        time.sleep(0.1)
        vote(a, 'Bounce__boing.wav', 1)
        vote(b, 'Bounce__boing.wav', -1)
        vote(nokey, 'Bounce__boing.wav', 1)        # no key: not counted
        vote(a, 'Nothing__here.wav', 1)            # no such sound: nothing
        vote(a, 'ShieldBreak__try_1.wav', 5)       # not -1, 0 or 1: nothing
        time.sleep(0.15)
        ca, cb, cn = catalog(b'a' * 16), catalog(b'b' * 16), catalog()
        check(ca['Bounce__boing.wav'][2] == 1 and cb['Bounce__boing.wav'][2] == -1 and cn['Bounce__boing.wav'][2] == 0,
              'each player is told their own vote only')
        check(tot('Bounce__boing.wav') == (1, 1) and tot('ShieldBreak__try_1.wav') == (0, 0),
              'one vote each (a player without a key, a made-up sound and a bad vote count for nothing)')
        vote(a, 'Bounce__boing.wav', -1)
        time.sleep(0.1)
        check(tot('Bounce__boing.wav') == (0, 2) and catalog(b'a' * 16)['Bounce__boing.wav'][2] == -1,
              'a vote can be changed to the other')
        vote(a, 'Bounce__boing.wav', 0)
        time.sleep(0.1)
        check(tot('Bounce__boing.wav') == (0, 1) and catalog(b'a' * 16)['Bounce__boing.wav'][2] == 0,
              '... or taken back')
        vote(b, 'ShieldBreak__try_2.wav', 1)
        time.sleep(0.1)
        b.raw(R.DELETE)
        time.sleep(0.1)
        check(tot('ShieldBreak__try_2.wav') == (0, 0), 'DELETE MY STATS takes the player\'s votes too')
        vote(a, 'ShieldBreak__try_2.wav', 1)
        vote(b, 'ShieldBreak__try_3.wav', -1)
        time.sleep(0.1)
        for f in (a, b, nokey):
            f.s.close()

    await loop.run_in_executor(None, blocking)
    # the owner's commands (on the server, with the relay running)
    out = io.StringIO()
    args = argparse.Namespace(stats=db, sounds=lib, sound_totals=True, sound_remove='', reason='', adopted_in='')
    old = sys.stdout
    sys.stdout = out
    R.sound_admin(args, print)
    args = argparse.Namespace(stats=db, sounds=lib, sound_totals=False, sound_remove='ShieldBreak__try_2', reason='adopted', adopted_in='0.3.17')
    code = R.sound_admin(args, print)
    args = argparse.Namespace(stats=db, sounds=lib, sound_totals=False, sound_remove='ShieldBreak__try_3.wav', reason='removed', adopted_in='')
    R.sound_admin(args, print)
    args = argparse.Namespace(stats=db, sounds=lib, sound_totals=False, sound_remove='ShieldBreak__try_4', reason='maybe', adopted_in='')
    bad = R.sound_admin(args, print)
    sys.stdout = old
    text = out.getvalue()
    check('ShieldBreak__try_2.wav' in text and 'up    1' in text and '6 sounds in the library' in text, '--sound-totals lists every sound\'s votes')
    results = [json.loads(x) for x in open(os.path.join(lib, 'removed', 'results.log'), encoding='utf-8')]
    check(code == 0 and results[0] == {**results[0], 'sound': 'TRY 2', 'event': 'ShieldBreak', 'up': 1, 'down': 0, 'reason': 'adopted',
                                       'adopted_in': '0.3.17'}, '--sound-remove ... adopted: its result, for the Discord post')
    check(results[1]['reason'] == 'removed' and results[1]['down'] == 1 and bad == 1, '... removed by the community; a wrong reason is refused')
    check(os.path.isfile(os.path.join(lib, 'removed', 'ShieldBreak__try_2.wav')), 'the file is kept in removed/')

    def after():
        c = catalog()
        check('ShieldBreak__try_2.wav' not in c and 'ShieldBreak__try_5.wav' in c and 'ShieldBreak__try_6.wav' in c,
              'gone from the catalog at once; the next ones waiting take their places')
    await loop.run_in_executor(None, after)
    srv.close()
    tr.close()


if __name__ == '__main__':
    asyncio.run(run())
    print('\n%d passed, %d failed' % (T.passed, T.fails))
    sys.exit(1 if T.fails else 0)
