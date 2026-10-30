"""Self-test for the relay's TRAINING leaderboards (TRAIN, BOARDREQ / BOARD): fake games through a local relay.
Usage: python test_relay_training.py"""
import asyncio
import os
import struct
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import projectarena_relay as R  # noqa: E402
from test_relay_stats import Fake, check  # noqa: E402
import test_relay_stats as T  # noqa: E402

PORT = T.PORT


def board_reply(f):
    while True:
        d = f.recv()
        if d is None:
            return None
        if struct.unpack_from('<IB', d)[1] == R.BOARD:
            b = d[5:]
            lesson, kind, n = struct.unpack_from('<BBB', b)
            rows = []
            for i in range(n):
                name, v = struct.unpack_from('<16sI', b, 3 + 20 * i)
                rows.append((name.split(b'\0')[0].decode(), v))
            place, mine, count = struct.unpack_from('<HIH', b, 3 + 20 * n)
            return dict(lesson=lesson, kind=kind, rows=rows, place=place, mine=mine, count=count)


def train(f, lesson, ms, succ, att):
    f.raw(R.TRAIN, struct.pack('<BBIBB', lesson, 1, ms, succ, att))


async def run():
    loop = asyncio.get_running_loop()
    db = os.path.join(tempfile.mkdtemp(), 'stats.db')
    logs = []
    stats = R.Stats(db, log=lambda m: None)
    relay = R.Relay(8, log=logs.append, stats=stats)
    tr, _ = await loop.create_datagram_endpoint(lambda: relay, local_addr=('127.0.0.1', PORT))

    def blocking():
        import time
        import sqlite3
        look = sqlite3.connect(db)   # (the relay's own connection belongs to its thread)
        a, b, c, nokey = Fake('ACE', 9), Fake('BEE', 9), Fake('CEE', 9), Fake('NOKEY', 9)
        for f in (a, b, c, nokey):
            f.hello()
        for f, k in ((a, b'a' * 16), (b, b'b' * 16), (c, b'c' * 16)):
            f.raw(R.KEY, k)
        time.sleep(0.1)
        # FIRST THROWS (5 targets, 15 throws): times and accuracies
        train(a, 1, 14000, 5, 5)
        train(b, 1, 12000, 5, 9)
        train(c, 1, 20000, 5, 6)
        train(a, 1, 16000, 5, 7)    # slower and less accurate: nothing changes
        train(nokey, 1, 9000, 5, 5)  # no key: not kept
        time.sleep(0.15)
        a.raw(R.BOARDREQ, bytes([1, 0]))
        r = board_reply(a)
        check(r is not None and [x[0] for x in r['rows']] == ['BEE', 'ACE', 'CEE'] and r['rows'][0][1] == 12000,
              'TIME: fastest first (%s)' % (r and r['rows']))
        check(r is not None and r['place'] == 2 and r['mine'] == 14000 and r['count'] == 3, 'TIME: the asker\'s own place and time')
        time.sleep(0.3)
        b.raw(R.BOARDREQ, bytes([1, 1]))
        r = board_reply(b)
        check(r is not None and [x for x in r['rows']] == [('ACE', 1000), ('CEE', 833), ('BEE', 555)] and r['place'] == 3,
              'ACCURACY: tenths of a percent, best first; the asker third (%s)' % (r and r['rows']))
        # impossible results are refused
        before = look.execute('SELECT COUNT(*) FROM training').fetchone()[0]
        train(c, 1, 2000, 5, 5)      # five targets in 2 s
        train(c, 1, 14000, 4, 5)     # passed with a target left
        train(c, 2, 20000, 8, 30)    # more throws than the lesson has
        train(c, 21, 0, 9, 9)        # a defense lesson without every try
        train(c, 99, 10000, 5, 5)    # no such lesson
        time.sleep(0.15)
        after = look.execute('SELECT COUNT(*) FROM training').fetchone()[0]
        refused = sum('training result from CEE refused' in m for m in logs)
        check(after == before and refused == 5, 'results that can\'t be real are refused (%d logged)' % refused)
        # a defense lesson (no clock): only ACCURACY; TIME asked for gives ACCURACY
        train(a, 21, 0, 9, 10)
        train(b, 21, 0, 7, 10)
        time.sleep(0.1)
        c.raw(R.BOARDREQ, bytes([21, 0]))
        r = board_reply(c)
        check(r is not None and r['kind'] == 1 and r['rows'] == [('ACE', 900), ('BEE', 700)] and r['place'] == 0,
              'SHIELD: accuracy only; the asker not on it')
        # overall: every lesson's best accuracy added up
        time.sleep(0.3)
        c.raw(R.BOARDREQ, bytes([0, 1]))
        r = board_reply(c)
        check(r is not None and r['rows'][0] == ('ACE', 1900) and r['rows'][1] == ('BEE', 1255) and r['place'] == 3 and r['count'] == 3,
              'OVERALL: the sum of each lesson\'s best accuracy (%s)' % (r and r['rows']))
        # a new name shows on every board; DELETE takes the training results too
        a2 = Fake('ACEY', 9)
        a2.hello()
        a2.raw(R.KEY, b'a' * 16)
        time.sleep(0.05)
        train(a2, 3, 13000, 5, 6)
        time.sleep(0.1)
        names = {r[0] for r in look.execute('SELECT name FROM training WHERE lesson IN (1, 21)')}
        check('ACEY' in names and 'ACE' not in names, 'the name as the player is called now, on all their lessons')
        a2.raw(R.DELETE)
        time.sleep(0.1)
        left = look.execute('SELECT COUNT(*) FROM training WHERE name = ?', ('ACEY',)).fetchone()[0]
        check(left == 0, 'DELETE MY STATS removes the training results as well')
        for f in (a, b, c, nokey, a2):
            f.s.close()

    await loop.run_in_executor(None, blocking)
    tr.close()


if __name__ == '__main__':
    asyncio.run(run())
    print('\n%d passed, %d failed' % (T.passed, T.fails))
    sys.exit(1 if T.fails else 0)
