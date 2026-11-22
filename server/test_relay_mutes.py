#!/usr/bin/env python3
"""Tests MUTED PLAYERS' codes (MUTECODE, 2026-11-20): a local relay on 127.0.0.1:47795 gives each player a code for
another player that stays the same when that player comes back under a new name, differs from viewer to viewer, and
is zeros for anyone without a statistics key, for yourself, and for nobody online.
Run: python -I server/test_relay_mutes.py"""
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

PORT = 47795
passed = failed = 0


def check(ok, msg):
    global passed, failed
    if ok:
        passed += 1
        print('  ok   ' + msg)
    else:
        failed += 1
        print('  FAIL ' + msg)


class Player:
    def __init__(self, name, key=None):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind(('127.0.0.1', 0))
        self.s.settimeout(1.0)
        self.token, self.vip = bytes(16), 0
        self.raw(R.HELLO, bytes([10]) + name.encode().ljust(16, b'\0') + bytes([6]) + b'0.3.23')
        while True:
            d = self.s.recvfrom(4096)[0]
            if d[4] == R.WELCOME:
                self.token, self.vip = d[5:21], struct.unpack_from('<I', d, 21)[0]
                break
        if key is not None:
            self.raw(R.KEY, key)
            time.sleep(0.1)

    def raw(self, ptype, body=b''):
        self.s.sendto(struct.pack('<IB', R.MAGIC, ptype) + self.token + body, ('127.0.0.1', PORT))

    def send(self, vip):
        """A game packet to that address (as a game in a room sends its host): the two have met."""
        self.raw(R.SEND, struct.pack('<I', vip) + struct.pack('<I', R.GAME_MAGIC) + bytes(12))
        time.sleep(0.05)

    def codes(self, *vips):
        """MUTECODE for these addresses: {vip: code}, or None without an answer."""
        time.sleep(0.25)   # (one request answered every 0.2 s)
        self.raw(R.MUTECODE, b''.join(struct.pack('<I', v) for v in vips))
        end = time.time() + 1.0
        while time.time() < end:
            try:
                d = self.s.recvfrom(4096)[0]
            except socket.timeout:
                return None
            if d[4] != R.MUTECODE:
                continue
            body = d[5:]
            return {struct.unpack_from('<I', body, 12 * i)[0]: body[12 * i + 4:12 * i + 12] for i in range(len(body) // 12)}
        return None

    def bye(self):
        self.raw(R.BYE)
        self.s.close()


def main():
    work = tempfile.mkdtemp(prefix='pa_relay_mutes_')
    relay = subprocess.Popen([sys.executable, '-I', os.path.join(HERE, 'projectarena_relay.py'), '--port', str(PORT),
                              '--bind', '127.0.0.1', '--reports', os.path.join(work, 'reports'),
                              '--bans', os.path.join(work, 'bans.txt'), '--latest', os.path.join(work, 'latest.txt'),
                              '--stats', os.path.join(work, 'stats.db')],
                             stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
    try:
        time.sleep(1.0)
        ka, kb, kc = os.urandom(16), os.urandom(16), os.urandom(16)
        a, b, nokey = Player('ALICE', ka), Player('BOB', kb), Player('OLDGAME')
        before = a.codes(b.vip)
        check(before is not None and before.get(b.vip) == bytes(8),
              'two who have never played or watched together: zeros (2026-11-21: not anyone online)')
        b.send(a.vip)       # (BOB in ALICE's room)
        nokey.send(a.vip)
        got = a.codes(b.vip, nokey.vip, a.vip, 0x0A00FFFE)
        check(got is not None and len(got) == 4, 'MUTECODE is answered, one code for each address asked')
        code_b = got[b.vip] if got else bytes(8)
        check(code_b != bytes(8), "a player with a key has a code")
        check(got is not None and got[nokey.vip] == bytes(8), 'a game without a key: zeros (its name is all there is)')
        check(got is not None and got[a.vip] == bytes(8), 'yourself: zeros')
        check(got is not None and got[0x0A00FFFE] == bytes(8), 'nobody at that address: zeros')
        check(code_b not in (kb[:8], ka[:8]), "the code isn't either player's key")

        b.bye()
        time.sleep(0.2)
        b2 = Player('ROBERT', kb)   # the same player, back under a new name (a new address)
        b2.send(a.vip)
        got2 = a.codes(b2.vip)
        check(got2 is not None and b2.vip != b.vip and got2.get(b2.vip) == code_b,
              'the same player under a new name and address: the same code')

        c = Player('CAROL', kc)
        c.send(a.vip)   # (CAROL in ALICE's room too: she and ROBERT have only ALICE, the host, in common)
        got3 = c.codes(b2.vip)
        check(got3 is not None and got3.get(b2.vip) not in (code_b, bytes(8)),
              "another player in the same room has a code for them, and it differs")
        eve = Player('EVE', os.urandom(16))
        got4 = eve.codes(b2.vip, a.vip)
        check(got4 is not None and got4.get(b2.vip) == bytes(8) and got4.get(a.vip) == bytes(8),
              'someone in no room with them: zeros')

        mute = Player('MALLORY', os.urandom(16))
        mute.send(a.vip)
        check(mute.codes(b2.vip) != mute.codes(a.vip), 'two people: two codes')
        time.sleep(0.25)
        mute.raw(R.MUTECODE, struct.pack('<I', b2.vip))
        mute.raw(R.MUTECODE, struct.pack('<I', b2.vip))   # (straight after: too soon, not answered)
        n = 0
        end = time.time() + 0.6
        while time.time() < end:
            try:
                if mute.s.recvfrom(4096)[0][4] == R.MUTECODE:
                    n += 1
            except socket.timeout:
                break
        check(n == 1, 'two requests within 0.2 s: one answer')
        many = a.codes(*range(1, 40))
        check(many is not None and len(many) == 16, 'at most 16 addresses a request')
        for p in (a, b2, c, nokey, mute, eve):
            p.bye()
    finally:
        relay.terminate()
        relay.communicate(timeout=5)
        shutil.rmtree(work, ignore_errors=True)
    print('%d passed, %d failed' % (passed, failed))
    return 1 if failed else 0


if __name__ == '__main__':
    sys.exit(main())
