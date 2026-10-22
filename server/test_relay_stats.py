"""Self-test for the relay's player statistics (KEY, TANK, MATCH, STATSREQ / STATS): fake games through a local relay.
Usage: python test_relay_stats.py"""
import asyncio
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import projectarena_relay as R  # noqa: E402
import socket  # noqa: E402

PORT = 47798
fails = passed = 0


def check(cond, msg):
    global fails
    global passed
    print(('  ok   ' if cond else '  FAIL ') + msg)
    fails += 0 if cond else 1
    passed += 1 if cond else 0


class Fake:   # (as in test_relay.py, which runs its own test when imported)
    def __init__(self, name, version=8):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind(('127.0.0.1', 0))
        self.s.settimeout(0.5)
        self.name, self.version = name, version
        self.token, self.vip = bytes(16), 0

    def raw(self, ptype, body=b''):
        self.s.sendto(struct.pack('<IB', R.MAGIC, ptype) + self.token + body, ('127.0.0.1', PORT))

    def recv(self):
        try:
            return self.s.recvfrom(4096)[0]
        except socket.timeout:
            return None

    def hello(self):
        self.raw(R.HELLO, bytes([self.version]) + self.name.encode().ljust(16, bytes(1)))
        d = self.recv()
        if d and struct.unpack_from('<IB', d)[1] == R.WELCOME:
            self.token, self.vip = d[5:21], struct.unpack_from('<I', d, 21)[0]
        return d


def stats_reply(f):
    while True:
        d = f.recv()
        if d is None:
            return None
        if struct.unpack_from('<IB', d)[1] == R.STATS:
            body = d[5:]
            hv = struct.unpack_from('<I', body)[0]
            out = []
            for k in range(2):
                known, name, *rest = struct.unpack_from('<B16s8IH', body, 4 + 51 * k)
                out.append(dict(known=known, name=name.split(b'\0')[0].decode(), matches=rest[0], wins=rest[1],
                                throws=rest[2], on_target=rest[3], hits=rest[4], blocks=rest[5], faced=rest[6],
                                catches=rest[7], top=rest[8]))
            return hv, out


async def run():
    loop = asyncio.get_running_loop()
    db = os.path.join(tempfile.mkdtemp(), 'stats.db')
    logs = []
    relay = R.Relay(8, log=logs.append, stats=R.Stats(db, log=lambda m: None))
    tr, _ = await loop.create_datagram_endpoint(lambda: relay, local_addr=('127.0.0.1', PORT))

    def blocking():
        import time
        host, guest, watcher, twin = Fake('MOJO', 8), Fake('FRIEND', 8), Fake('WATCHER', 8), Fake('TWIN', 8)
        for f in (host, guest, watcher, twin):
            f.hello()
        host.raw(R.KEY, b'H' * 16)
        guest.raw(R.KEY, b'G' * 16)
        host.raw(R.TANK, struct.pack('<II', host.vip, guest.vip))
        time.sleep(0.1)
        row0 = struct.pack('<7H', 20, 12, 7, 5, 9, 3, 2350)    # host: 20 throws, 12 on target, 7 hits, 5 of 9 blocked
        row1 = struct.pack('<7H', 18, 9, 4, 4, 11, 1, 2010)
        score = bytes([7, 4, 0, 0])
        confirm = lambda w, sc: guest.raw(R.CONFIRM, struct.pack('<IB', host.vip, w) + sc)

        def ask():
            time.sleep(0.6)   # (an asker may ask twice a second)
            watcher.raw(R.STATSREQ, struct.pack('<I', host.vip))
            return stats_reply(watcher)

        def rearm():
            for c in relay.by_addr.values():
                c.last_match = -1e9

        host.raw(R.MATCH, bytes([0]) + row0 + row1 + score)
        r = ask()
        check(r is not None and r[1][0]['matches'] == 0, 'the host\'s result alone counts for nobody (it waits for the other player)')
        confirm(0, score)
        r = ask()
        check(r is not None and r[0] == host.vip, 'a spectator asks for a host\'s players and gets an answer')
        if r:
            a, b = r[1]
            check(a['known'] and a['name'] == 'MOJO' and a['matches'] == 1 and a['wins'] == 1 and a['throws'] == 20 and
                  a['on_target'] == 12 and a['hits'] == 7 and a['blocks'] == 5 and a['faced'] == 9 and a['top'] == 2350,
                  'confirmed by the other player: the winner\'s career after one match')
            check(b['name'] == 'FRIEND' and b['matches'] == 1 and b['wins'] == 0 and b['top'] == 2010, 'the loser\'s career')
        host.raw(R.MATCH, bytes([1]) + row0 + row1 + score)   # straight after: refused (one result per 30 s per host)
        confirm(1, score)
        r = ask()
        check(r and r[1][0]['matches'] == 1, 'a second result straight after the first is not counted')
        rearm()
        host.raw(R.MATCH, bytes([0]) + row0 + row1 + score)   # the host says it won 7-4 ...
        confirm(1, bytes([4, 7, 0, 0]))                       # ... the other player saw the opposite
        r = ask()
        check(r and r[1][0]['matches'] == 1 and r[1][1]['matches'] == 1, 'when the two players disagree, the match counts for nobody')
        rearm()
        confirm(0, score)                                     # the confirmation may arrive first
        host.raw(R.MATCH, bytes([0]) + row0 + row1 + score)
        r = ask()
        check(r and r[1][0]['matches'] == 2, 'a confirmation that arrives before the host\'s result still counts')
        rearm()
        host.raw(R.MATCH, bytes([0]) + row0 + row1 + score)
        host.raw(R.CONFIRM, struct.pack('<IB', host.vip, 0) + score)   # the host can't confirm its own result
        r = ask()
        check(r and r[1][0]['matches'] == 2, 'the host can\'t confirm its own result')
        relay.pending.clear()   # (that real result would still count if the other player confirmed it in time)
        rearm()
        bad = struct.pack('<7H', 20, 25, 7, 5, 9, 3, 2350)   # more on target than thrown
        host.raw(R.MATCH, bytes([0]) + bad + row1 + score)
        confirm(0, score)
        r = ask()
        check(r and r[1][0]['matches'] == 2, 'an impossible result counts for nobody')
        rearm()
        host.raw(R.TANK, struct.pack('<II', host.vip, 0))   # against the bot: nobody to credit
        time.sleep(0.1)
        host.raw(R.MATCH, bytes([0]) + row0 + row1 + score)
        r = ask()
        check(r and r[1][0]['matches'] == 2 and r[1][1]['known'] == 0, 'a match against the bot is not counted')
        # privacy: the table holds only salted hashes of the keys; the salt and the table are private files
        raw = open(db, 'rb').read()
        check((b'H' * 16).hex().encode() not in raw and b'HHHHHHHHHHHHHHHH' not in raw, 'the table never stores a player\'s key')
        if os.name == 'posix':
            check(os.stat(db).st_mode & 0o077 == 0 and os.stat(db + '.salt').st_mode & 0o077 == 0,
                  'the table and its salt are readable by the server\'s account only')
        s2 = R.Stats(db, log=lambda m: None)
        check(s2.get(s2.ident(b'H' * 16))[1] == 2, 'the table survives a relay restart (the same salt)')
        # the same player from two places is logged
        logs.clear()
        twin.raw(R.KEY, b'G' * 16)
        time.sleep(0.2)
        check(any('connected twice' in m for m in logs), 'the same player connected from two places is logged')
        # DELETE: a player removes their own statistics
        guest.raw(R.DELETE)
        time.sleep(0.2)
        host.raw(R.TANK, struct.pack('<II', host.vip, guest.vip))
        r = ask()
        check(r and r[1][1]['matches'] == 0 and r[1][0]['matches'] == 2, 'DELETE removes that player\'s statistics, nobody else\'s')
        # the privacy policy's 24 months: a player unseen that long is removed, a current one stays
        s3 = R.Stats(db, log=lambda m: None)
        s3.db.execute('INSERT INTO players (key, name, matches, first_seen, last_seen) VALUES (?, ?, 5, 0, ?)',
                      ('old', 'GONE', int(time.time()) - 25 * 30 * 86400))
        s3.db.commit()
        s3.purge()
        check(s3.get('old') is None and s3.get(s3.ident(b'H' * 16)) is not None, 'statistics unseen for 24 months are removed')
        # 0.3.8: a HOTDOG match (no shields) counts in the career and in the hotdog mode; standard matches in theirs
        rearm()
        before = s3.get(s3.ident(b'H' * 16))[1]
        host.raw(R.MATCH, bytes([0]) + row0 + row1 + score + bytes([1]))
        confirm(0, score)
        time.sleep(0.3)
        s4 = R.Stats(db, log=lambda m: None)
        md = s4.modes(s4.ident(b'H' * 16))
        check(s4.get(s4.ident(b'H' * 16))[1] == before + 1 and md.get(1, (0, 0))[:2] == (1, 1),
              'a hotdog match counts in the career and in the hotdog mode (1 match, 1 win)')
        check(md.get(0, (0,))[0] == 2, 'results without a mode byte (0.3.7 hosts, which have no options) count as standard')
        guest.raw(R.DELETE)
        time.sleep(0.3)
        s5 = R.Stats(db, log=lambda m: None)
        check(s5.modes(s5.ident(b'G' * 16)) == {}, 'DELETE removes the player\'s modes too')

    await loop.run_in_executor(None, blocking)
    tr.close()


if __name__ == '__main__':
    asyncio.run(run())
    print('%d passed, %d failed' % (passed, fails))
    sys.exit(1 if fails else 0)
