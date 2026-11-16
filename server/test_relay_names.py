#!/usr/bin/env python3
"""Tests the relay's name filter (2026-11-13): names with slurs or hate words are found and never shown to other
players, ordinary names are not touched, and the bans file's word: lines add words while the relay runs.
The first part checks name_blocked itself; the second a local relay on 127.0.0.1:47792.
Run: python -I server/test_relay_names.py"""
import os
import shutil
import socket
import struct
import subprocess
import sys
import tempfile
import time

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import projectarena_relay as R  # noqa: E402

PORT = 47792
MAGIC = 0x4C52414F
passed = failed = 0


def check(ok, msg):
    global passed, failed
    if ok:
        passed += 1
        print('  ok   ' + msg)
    else:
        failed += 1
        print('  FAIL ' + msg)


# Must be refused: the words as typed, spaced out, with look-alike digits, letters repeated, inside a longer name,
# and the whole-word ones with digits or an S added.
BLOCKED = ['NIGGER BITCH', 'N I G G E R', 'N1GG3R', 'NIIIGGGA', 'XXNIGGAXX', 'BIG-NIGGUH', 'FAGGOT', 'FA6GOT',
           'HITLER 88', 'H1TLER', 'SIEG HEIL', 'WHITE POWER', 'COON', 'COONS', 'COON88', 'FAG69', 'SPICS', 'THE KIKE',
           'K K K', 'KKK', 'NAZI', 'NAZI GAMER', '1488', 'TOWEL HEAD', 'RETARD', 'CHINK', 'PAKI 1', 'TRANNY']
# Must be fine: ordinary names, including ones with a blocked word's letters inside a longer word.
FINE = ['PLAYER', 'PLAYER 259', 'MOJO', 'QUEST', 'LARRY VR', 'THE-INSANIAC', 'D00D00BUTT3R', 'HOST050', 'RICK 05',
        'RACCOON', 'TYCOON', 'SPICY', 'SPICE GIRL', 'KNIGHT', 'NIGHTHAWK', 'NIGEL', 'NIGERIA', 'NIGER', 'PAKISTAN',
        'SNAKE', 'MIKE', 'BIG GAMER', 'KING GARY', 'DOS', 'GOOKIE', 'COONHOUND', 'FAGAN', 'NAZIR', 'WOPPER',
        'BEANERY', 'HIT LIST', 'SKANDI', 'JEEPMAN', 'A.B-C D', '88', '14', 'AGENT 47']


def unit():
    print('name_blocked:')
    for n in BLOCKED:
        check(R.name_blocked(n), 'refused: %s' % n)
    for n in FINE:
        check(not R.name_blocked(n), 'fine:    %s' % n)
    check(R.name_blocked('ZZTOPXX', ('ZZTOP',)) and not R.name_blocked('ZZTOPXX'), "a bans file word: counts")
    check(R.clean_name(b'NIGGER BITCH\0\0\0\0') == 'PLAYER' and R.clean_name(b'MOJO') == 'MOJO',
          "the relay's own displays show PLAYER for a blocked name")
    check(len(R.NAME_REFUSED) <= 48, 'the message fits the ERROR packet (48)')


def hello(sock, name, wait=True):
    sock.sendto(struct.pack('<IB', MAGIC, 1) + bytes(16) + bytes([10]) + name.encode().ljust(16, b'\0')[:16],
                ('127.0.0.1', PORT))
    return answer(sock) if wait else None


def answer(sock):
    try:
        data, _ = sock.recvfrom(2048)
    except socket.timeout:
        return 'NO ANSWER'
    if data[4] == 2:
        return 'WELCOME'
    if data[4] == 7:
        return data[5:].split(b'\0')[0].decode()
    return 'TYPE %d' % data[4]


def live():
    print('a relay:')
    work = tempfile.mkdtemp(prefix='pa_relay_names_')
    bans = os.path.join(work, 'bans.txt')
    with open(bans, 'w') as f:
        f.write('# test\n')
    relay = subprocess.Popen([sys.executable, '-I', os.path.join(HERE, 'projectarena_relay.py'), '--port', str(PORT),
                              '--bind', '127.0.0.1', '--reports', os.path.join(work, 'reports'), '--bans', bans,
                              '--stats', os.path.join(work, 'stats.db'), '--sounds', os.path.join(work, 'sounds')],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    out = ''
    try:
        time.sleep(1.5)
        socks = []
        for _ in range(3):
            s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
            s.settimeout(2)
            s.bind(('127.0.0.1', 0))
            socks.append(s)
        a, b, c = socks
        check(hello(a, 'MOJO') == 'WELCOME', 'an ordinary name connects')
        check(hello(b, 'N1GG3R') == R.NAME_REFUSED, 'a blocked name is refused: ' + R.NAME_REFUSED)
        check(hello(b, 'N1GG3R') == R.NAME_REFUSED, '... every time it tries')
        check(hello(b, 'PLAYER 259') == 'WELCOME', '... and connects once the name is changed')
        check(hello(b, 'COON88') == R.NAME_REFUSED, 'a rename to a blocked name while online is refused')
        b.sendto(struct.pack('<IB', MAGIC, 4) + bytes(16), ('127.0.0.1', PORT))   # (a ping: no session any more)
        check(answer(b) == 'NO ANSWER', '... and that player is disconnected')
        check(hello(c, 'ZZTOP') == 'WELCOME', 'a name connects before the owner adds a word')
        time.sleep(1.1)   # (a newer modification time)
        with open(bans, 'a') as f:
            f.write('word:ZZTOP\nword:ab\n')
        c.settimeout(5)
        check(answer(c) == R.NAME_REFUSED, 'a word: line added while it runs disconnects a name with it')
        c.settimeout(2)
        check(hello(c, 'THE ZZ TOP FAN') == R.NAME_REFUSED, '... and refuses it, spaces or not')
        check(hello(c, 'ABBA') == 'WELCOME', 'a word: shorter than 3 letters is ignored')
        check(hello(a, 'MOJO') == 'WELCOME', 'the ordinary player stayed on')
    finally:
        relay.terminate()
        out = relay.communicate(timeout=5)[0].decode(errors='replace')
        shutil.rmtree(work, ignore_errors=True)
    check('N1GG' not in out.upper() and 'NIGG' not in out.upper() and 'COON' not in out.upper(),
          "the relay's log never has a blocked name")
    check('name not allowed' in out and 'refused a name' in out and '1 words' in out,
          'the log says a name was refused and counts the word: lines')
    print('\n'.join('  relay: ' + l for l in out.splitlines() if 'name' in l or 'bans' in l))


if __name__ == '__main__':
    unit()
    live()
    print('\n%d passed, %d failed' % (passed, failed))
    sys.exit(1 if failed else 0)
