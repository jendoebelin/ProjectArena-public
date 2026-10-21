"""Self-test for The Crowd (LIVE / CROWD and the phones' WebSocket): a fake host and fake phones through a local relay.
Usage: python test_relay_crowd.py"""
import asyncio
import base64
import json
import os
import socket
import struct
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import projectarena_relay as R  # noqa: E402

PORT, WS = 47796, 47797
ORIGIN = 'https://crowd.example.com'
fails = passed = 0


def check(cond, msg):
    global fails, passed
    print(('  ok   ' if cond else '  FAIL ') + msg)
    fails += 0 if cond else 1
    passed += 1 if cond else 0


class Host:
    def __init__(self, name):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind(('127.0.0.1', 0))
        self.s.settimeout(0.3)
        self.name, self.token, self.vip = name, bytes(16), 0

    def raw(self, ptype, body=b''):
        self.s.sendto(struct.pack('<IB', R.MAGIC, ptype) + self.token + body, ('127.0.0.1', PORT))

    def hello(self):
        self.raw(R.HELLO, bytes([8]) + self.name.encode().ljust(16, bytes(1)))
        d = self.s.recvfrom(4096)[0]
        self.token, self.vip = d[5:21], struct.unpack_from('<I', d, 21)[0]

    def live(self, score=(3, 1, 0, 0), names=('MOJO', 'FRIEND'), nations=(185, 61), stats=False):
        body = struct.pack('<7B', 2, *score, *nations) + bytes([0, 80, 255, 255, 120, 20]) + struct.pack('<BH', 1, 1234)
        body += names[0].encode().ljust(16, bytes(1)) + names[1].encode().ljust(16, bytes(1))
        if stats:   # 0.3.7: this match so far, the round, overtime
            body += struct.pack('<8H', 20, 12, 3, 5, 9, 1, 2, 2500) + struct.pack('<8H', 18, 9, 1, 4, 11, 0, 1, 2010) + bytes([2, 0])
        self.raw(R.LIVE, body)

    def crowd(self, wait=1.5):
        """the latest CROWD within `wait` seconds"""
        end, got = time.time() + wait, None
        while time.time() < end:
            try:
                d = self.s.recvfrom(4096)[0]
            except socket.timeout:
                continue
            if d[4] == R.CROWD:
                f = struct.unpack_from('<3H2B2H', d, 5)
                got = dict(fans=f[0], cheers=(f[1], f[2]), hype=(f[3], f[4]), waves=(f[5], f[6]))
        return got


class Phone:
    def __init__(self, origin=ORIGIN, path='/ws', xff='203.0.113.7'):
        self.s = socket.create_connection(('127.0.0.1', WS), timeout=2)
        key = base64.b64encode(os.urandom(16)).decode()
        self.s.sendall(('GET %s HTTP/1.1\r\nHost: crowd.example.com\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                        'Sec-WebSocket-Key: %s\r\nSec-WebSocket-Version: 13\r\nOrigin: %s\r\nX-Forwarded-For: %s\r\n\r\n'
                        % (path, key, origin, xff)).encode())
        head = b''
        while b'\r\n\r\n' not in head:
            c = self.s.recv(1)
            if not c:
                break
            head += c
        self.status = head.split(b' ')[1].decode() if b' ' in head else ''
        self.buf = b''

    def send(self, obj, masked=True, op=0x81):
        data = json.dumps(obj).encode() if not isinstance(obj, bytes) else obj
        mask = os.urandom(4)
        n = len(data)
        head = bytes([op, (0x80 if masked else 0) | (n if n < 126 else 126)]) + (struct.pack('>H', n) if n >= 126 else b'')
        body = bytes(b ^ mask[i & 3] for i, b in enumerate(data)) if masked else data
        self.s.sendall(head + (mask if masked else b'') + body)

    def recv(self, wait=1.5):
        """the next text message (dict), None on timeout, 'closed' when the server hung up"""
        self.s.settimeout(wait)
        try:
            while True:
                if len(self.buf) >= 2:
                    n, off = self.buf[1] & 0x7F, 2
                    if n == 126 and len(self.buf) >= 4:
                        n, off = struct.unpack_from('>H', self.buf, 2)[0], 4
                    if n != 126 and len(self.buf) >= off + n:
                        op, data = self.buf[0] & 0x0F, self.buf[off:off + n]
                        self.buf = self.buf[off + n:]
                        if op == 1:
                            return json.loads(data)
                        if op == 8:
                            return 'closed'
                        continue
                c = self.s.recv(65536)
                if not c:
                    return 'closed'
                self.buf += c
        except socket.timeout:
            return None
        except OSError:
            return 'closed'

    def until(self, pred, wait=2.0):
        end = time.time() + wait
        while time.time() < end:
            m = self.recv(max(0.05, end - time.time()))
            if m == 'closed':
                return m
            if m is not None and pred(m):
                return m
        return None


async def run():
    loop = asyncio.get_running_loop()
    logs = []
    relay = R.Relay(8, log=logs.append)
    tr, _ = await loop.create_datagram_endpoint(lambda: relay, local_addr=('127.0.0.1', PORT))
    crowd = R.Crowd(relay, logs.append, [ORIGIN])
    srv = await asyncio.start_server(crowd.handle, '127.0.0.1', WS, limit=16384)

    async def ticker():
        k = 0
        while True:
            await asyncio.sleep(R.CROWD_TICK)
            k += 1
            crowd.tick(k)
    tick = asyncio.ensure_future(ticker())

    def blocking():
        host = Host('MOJO')
        host.hello()
        host.live()
        time.sleep(0.2)
        p = Phone()
        check(p.status == '101', 'a phone on the web page connects (WebSocket upgrade)')
        hello = p.recv()
        check(isinstance(hello, dict) and hello.get('t') == 'hello', 'the server greets the phone')
        lst = p.until(lambda m: m.get('t') == 'list' and m['m'])
        ok = lst and lst != 'closed'
        check(ok and lst['m'][0]['host'] == 'MOJO' and lst['m'][0]['s'] == [3, 1, 0, 0], 'the live list shows the host\'s match and score')
        if ok:
            m0 = lst['m'][0]
            check(m0['p'][0]['n'] == 'MOJO' and m0['p'][1]['nat'] == 61 and m0['p'][1]['c'] == '#ff7814' and m0['adv']
                  and abs(m0['clock'] - 123.4) < 0.01, 'names, countries, end colours, rules and the clock reach the phone')
            check(len(m0['id']) == 8 and str(host.vip) not in json.dumps(lst), 'fans see a random match id, not the host\'s address')
        mid = lst['m'][0]['id'] if ok else ''
        p.send({'t': 'watch', 'id': mid})
        m = p.until(lambda m: m.get('t') == 'm')
        check(m and m != 'closed' and m['fans'] == 1, 'watching a match: its updates come to the phone, one fan')
        host.live(stats=True)
        m = p.until(lambda m: m.get('t') == 'm' and 'm' in m['p'][0])
        ok = m and m != 'closed'
        check(ok and m['p'][0]['m'] == {'throws': 20, 'on': 12, 'hits': 3, 'blocks': 5, 'faced': 9, 'catches': 1, 'strikes': 2, 'top': 90}
              and m['p'][1]['m']['strikes'] == 1 and m['rnd'] == 2 and m['ot'] is False,
              'the match so far reaches the phone: throws, on target, hits, blocks, catches, strikes, top speed, round')
        check(ok and 'car' not in m['p'][0], 'no career without a statistics table (and none for players without one)')
        c = host.crowd()
        check(c is not None and c['fans'] == 1, 'the host hears how many fans watch')
        for _ in range(6):
            p.send({'t': 'cheer', 'side': 0, 'n': 2})
        p.send({'t': 'flag', 'side': 1})
        host.live()
        c = host.crowd(0.8)
        check(c is not None and c['hype'][0] > 0 and c['hype'][1] == 0 and c['waves'] == (0, 1),
              'cheers raise that end\'s hype; a waved flag counts for its end')
        m = p.until(lambda m: m.get('t') == 'm' and m['h'][0] > 0)
        check(m and m != 'closed' and m['w'] == [0, 1], 'the phone sees the hype and the flags too')
        # a flood of cheers is capped (8 a second per phone)
        before = sum(mm.cheers[0] for mm in crowd.matches.values())
        for _ in range(15):
            p.send({'t': 'cheer', 'side': 0, 'n': 50})
        time.sleep(0.05)
        after = sum(mm.cheers[0] for mm in crowd.matches.values())
        check(after - before <= R.CROWD_CHEERS + 1, 'one phone can\'t flood the cheers (%d counted)' % (after - before))
        p.send({'t': 'cheer', 'side': 5, 'n': 1})
        p.send(b'not json')
        p.send({'t': 'watch', 'id': {'x': 1}})
        check(p.until(lambda m: m.get('t') in ('m', 'list')) not in (None, 'closed'), 'nonsense messages are ignored, the phone stays connected')
        # the host leaves: the phone is told (the bad 'watch' above stopped watching: watch again)
        p.send({'t': 'watch', 'id': mid})
        p.until(lambda m: m.get('t') == 'm')
        host.raw(R.BYE)
        g = p.until(lambda m: m.get('t') == 'gone')
        check(g and g != 'closed' and g['id'] == mid, 'when the host leaves, its fans are told the match is over')
        lst = p.until(lambda m: m.get('t') == 'list')
        check(lst and lst != 'closed' and lst['m'] == [], 'and the list is empty again')
        # wrong origin, wrong path, unmasked frames, too many connections from one address
        check(Phone(origin='https://evil.example.org').status == '403', 'another web site\'s page is refused')
        check(Phone(path='/admin').status == '403', 'only /ws upgrades')
        q = Phone(xff='198.51.100.9')
        q.recv()
        q.send({'t': 'watch', 'id': 'x'}, masked=False)
        check(q.until(lambda m: False, 1.0) == 'closed', 'an unmasked (non-browser) frame closes the connection')
        many = [Phone(xff='198.51.100.20') for _ in range(R.CROWD_PER_ADDR)]
        check(all(x.status == '101' for x in many) and Phone(xff='198.51.100.20').status == '503',
              'at most %d connections from one address' % R.CROWD_PER_ADDR)
        big = Phone(xff='198.51.100.30')
        big.recv()
        big.send({'t': 'cheer', 'pad': 'x' * 600})
        check(big.until(lambda m: False, 1.0) == 'closed', 'an oversized message closes the connection')
        check(not any('203.0.113' in l or '198.51.100' in l for l in logs), 'no phone address is ever logged')
        # a second host: the old match is not resurrected under the same id
        h2 = Host('ASTRO')
        h2.hello()
        h2.live(names=('ASTRO', 'BOT'))
        time.sleep(0.2)
        p2 = Phone(xff='198.51.100.40')
        lst = p2.until(lambda m: m.get('t') == 'list' and m['m'])
        check(lst and lst != 'closed' and [x['host'] for x in lst['m']] == ['ASTRO'] and lst['m'][0]['id'] != mid,
              'a new host gets a new match id')
        # a 0.3.6 host (no LIVE): its arena's beacon and TANK put its match on the list
        old, guest = Host('OLDHOST'), Host('GUEST')
        old.hello()
        guest.hello()
        old.raw(R.TANK, struct.pack('<II', old.vip, guest.vip))
        beacon = struct.pack('<IBBII', R.GAME_MAGIC, 8, 1, 77, 0) + b'OLDHOST'.ljust(16, bytes(1))
        beacon += struct.pack('<BBhhfBBBB', 0, 0, 2, 5, 61.5, 0, 0, 0, 2)
        for _ in range(3):
            old.raw(R.SEND, struct.pack('<I', R.BROADCAST) + beacon)
            time.sleep(0.2)
        lst = p2.until(lambda m: m.get('t') == 'list' and any(x['host'] == 'OLDHOST' for x in m['m']))
        o = next((x for x in lst['m'] if x['host'] == 'OLDHOST'), None) if lst and lst != 'closed' else None
        check(o is not None and o['s'][:2] == [2, 5] and abs(o['clock'] - 61.5) < 0.01 and o['old']
              and [o['p'][0]['n'], o['p'][1]['n']] == ['OLDHOST', 'GUEST'] and not o['adv'],
              "an older host's match shows from its beacon: score, clock, rules, the tank players' names")
        old.live(names=('OLDHOST', 'GUEST'))   # (updated to 0.3.7: LIVE takes over)
        time.sleep(0.1)
        old.raw(R.SEND, struct.pack('<I', R.BROADCAST) + beacon)
        time.sleep(0.1)
        lst = p2.until(lambda m: m.get('t') == 'list' and any(x['host'] == 'OLDHOST' and 'old' not in x for x in m['m']))
        check(lst and lst != 'closed', 'once a host sends LIVE, its beacon no longer overrides it')
        old.raw(R.BYE)
        guest.raw(R.BYE)
        # a host that stops sending LIVE disappears from the list after 5 s
        time.sleep(5.6)
        lst = p2.until(lambda m: m.get('t') == 'list' and m['m'] == [], 1.5)
        check(lst and lst != 'closed', 'a host that stops sending LIVE leaves the list')

    await loop.run_in_executor(None, blocking)
    tick.cancel()
    srv.close()
    tr.close()


if __name__ == '__main__':
    asyncio.run(run())
    print('%d passed, %d failed' % (passed, fails))
    sys.exit(1 if fails else 0)
