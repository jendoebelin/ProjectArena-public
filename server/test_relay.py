"""Self-test for projectarena_relay.py: starts the relay on a local port and drives fake headsets through it.
Usage: python test_relay.py"""
import asyncio
import os
import socket
import struct
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import projectarena_relay as R  # noqa: E402

PORT = 47799
fails = 0


def check(cond, msg):
    global fails
    print(('  ok   ' if cond else '  FAIL ') + msg)
    fails += 0 if cond else 1


class Fake:
    def __init__(self, name, version=7, app=None):
        self.s = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        self.s.bind(('127.0.0.1', 0))
        self.s.settimeout(0.5)
        self.name, self.version, self.app = name, version, app
        self.token, self.vip = b'\0' * 16, 0

    def raw(self, ptype, body=b'', token=None):
        self.s.sendto(struct.pack('<IB', R.MAGIC, ptype) + (token or self.token) + body, ('127.0.0.1', PORT))

    def recv(self):
        try:
            d, _ = self.s.recvfrom(4096)
            return d
        except socket.timeout:
            return None

    def hello(self):
        tail = b'' if self.app is None else bytes([len(self.app)]) + self.app
        self.raw(R.HELLO, bytes([self.version]) + self.name.encode().ljust(16, b'\0') + tail)
        d = self.recv()
        if d and struct.unpack_from('<IB', d)[1] == R.WELCOME:
            self.token, self.vip = d[5:21], struct.unpack_from('<I', d, 21)[0]
        return d

    def send(self, dst, payload):
        self.raw(R.SEND, struct.pack('<I', dst) + payload)


def game_packet(tag):
    return struct.pack('<I', R.GAME_MAGIC) + tag


async def run():
    loop = asyncio.get_running_loop()
    logged = []
    relay = R.Relay(3, log=logged.append)
    tr, _ = await loop.create_datagram_endpoint(lambda: relay, local_addr=('127.0.0.1', PORT))

    def blocking():
        a, b, c, old = Fake('HOST'), Fake('JOINER'), Fake('WATCHER', app=b'0.3.11'), Fake('OLDVER', version=6)
        check(a.hello() is not None and a.vip != 0, 'hello gets a token and a virtual address')
        b.hello(); c.hello()
        check(len({a.vip, b.vip, c.vip}) == 3, 'virtual addresses are unique')
        joins = [m for m in logged if m.startswith('join')]
        check(any('WATCHER' in m and ' 0.3.11 v7' in m for m in joins), 'the log names a game\'s own version')
        check(any('HOST' in m and 'pre-0.3.11 v7' in m for m in joins), 'older games log as pre-0.3.11')
        check(R.app_version(bytes(17) + bytes([9]) + b'0.3<x>.1;') == '0.3.1', 'the version keeps digits and dots')
        check(R.app_version(bytes(17) + bytes([200]) + b'1' * 40) == '1' * 15, 'the version is capped at 15')
        a.send(R.BROADCAST, game_packet(b'beacon'))
        rb, rc = b.recv(), c.recv()
        check(rb is not None and rb.endswith(b'beacon') and struct.unpack_from('<I', rb, 5)[0] == a.vip,
              'broadcast reaches others with the sender\'s virtual address')
        check(rc is not None, 'broadcast reaches every client on the version')
        check(a.recv() is None, 'no echo to the sender')
        b.send(a.vip, game_packet(b'join'))
        ra = a.recv()
        check(ra is not None and ra.endswith(b'join'), 'unicast by virtual address')
        check(c.recv() is None, 'unicast is private')
        b.raw(R.SEND, struct.pack('<I', a.vip) + game_packet(b'forged'), token=b'x' * 16)
        check(a.recv() is None, 'a wrong token is dropped')
        b.send(a.vip, b'NOTAGAMEPACKET')
        check(a.recv() is None, 'non-game payloads are not forwarded')
        b.send(a.vip, game_packet(b'x' * 1300))
        check(a.recv() is None, 'oversized payloads are dropped')
        r = old.hello()
        check(r is not None and struct.unpack_from('<IB', r)[1] == R.ERROR, 'the client cap answers SERVER FULL')
        c.raw(R.BYE); import time; time.sleep(0.1)
        check(old.hello() is not None and old.vip != 0, 'a slot frees after BYE')
        a.send(R.BROADCAST, game_packet(b'beacon2'))
        check(old.recv() is None, 'broadcasts skip clients on another version')
        sent = R.RATE * 3
        for _ in range(sent):
            b.send(a.vip, game_packet(b'f'))
        got = 0
        while a.recv() is not None:
            got += 1
        check(got < sent * 0.6, 'rate limit holds a flood (%d of %d delivered)' % (got, sent))

    await loop.run_in_executor(None, blocking)
    tr.close()


asyncio.run(run())
print('relay self-test:', 'PASS' if fails == 0 else '%d FAILED' % fails)
sys.exit(1 if fails else 0)
