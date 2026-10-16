#!/usr/bin/env python3
"""Project Arena community relay server (MIT). One UDP port; no dependencies beyond Python 3.8+.

The relay works like a virtual LAN for the game: every connected headset gets a private virtual address, and the
game's own LAN packets (beacons, join, inputs, snapshots, spectating) are forwarded between players by that address.
So hosting, the games list, joining and spectating work online exactly as on a home network, and players never see
each other's real IP address.

Protocol (little endian), client -> server: magic 'OARL' (u32), type (u8), token (16 bytes), body
  HELLO  1  body: game protocol version (u8), name (16 bytes)      (token = zeros)
  SEND   3  body: destination virtual address (u32, 0xFFFFFFFF = everyone), game packet
  PING   4  keep-alive (every ~2 s; clients silent for 10 s are dropped); answered with a PING
  BYE    5
server -> client: magic, type, body
  WELCOME 2  body: token (16), virtual address (u32)
  DELIVER 6  body: source virtual address (u32), game packet
  ERROR   7  body: message (48 bytes, upper-case ASCII)

Safety: tokens bind each client to its source address; only game packets (magic 'SPRC') are forwarded, at most
1200 bytes; per-client rate limit; broadcasts only reach clients on the same game version; a client cap; names are
sanitised; nothing is logged except connects / disconnects (no packet contents).

Run:  python3 projectarena_relay.py [--port 47780] [--max-clients 200]
Bug reports: the game sends them over TCP to the same port: b"PABUG1\\n", u32 length (<= 200 kB), UTF-8 text;
answered "OK\\n". Saved as text files in --reports (default ./bug_reports), at most 20 per address per hour.
Testing: --lag MS --jitter MS --loss PERCENT make the relay behave like a long internet path (forwarded game packets
are delayed by lag plus a smoothly drifting 0..jitter ms, kept in order, and a share is dropped). Never on a public server.
"""
import argparse
import asyncio
import random
import os
import struct
import time

MAGIC = 0x4C52414F          # 'OARL'
GAME_MAGIC = 0x43525053     # 'SPRC' (the game's LAN packets)
HELLO, WELCOME, SEND, PING, BYE, DELIVER, ERROR = 1, 2, 3, 4, 5, 6, 7
BROADCAST = 0xFFFFFFFF
MAX_PAYLOAD = 1200
RATE = 1000                 # packets / second per client (an arena host: ~450 game + forwarded voice)
TIMEOUT = 10.0


def clean_name(raw):
    s = raw.split(b'\0')[0].decode('ascii', 'replace').upper()
    s = ''.join(c if ('A' <= c <= 'Z' or '0' <= c <= '9' or c in ' -.') else ' ' for c in s).strip()
    return s[:16] or 'PLAYER'


class Client:
    __slots__ = ('addr', 'token', 'vip', 'version', 'name', 'seen', 'bucket', 'bucket_t', 'joined')

    def __init__(self, addr, vip, version, name):
        self.addr, self.vip, self.version, self.name = addr, vip, version, name
        self.token = os.urandom(16)
        self.seen = self.bucket_t = self.joined = time.monotonic()
        self.bucket = RATE


class Relay(asyncio.DatagramProtocol):
    def __init__(self, max_clients, log=print, lag=0.0, jitter=0.0, loss=0.0):
        self.max_clients = max_clients
        self.lag, self.jitter, self.loss = lag / 1000.0, jitter / 1000.0, loss / 100.0
        self.drift, self.last_out = {}, {}
        self.by_addr, self.by_vip = {}, {}
        self.next_vip = 0x0A000001   # 10.0.0.1 upward (virtual, never routed)
        self.log = log
        self.transport = None
        self.stats = {'forwarded': 0, 'dropped': 0}

    def connection_made(self, transport):
        self.transport = transport

    def send(self, addr, ptype, body=b''):
        self.transport.sendto(struct.pack('<IB', MAGIC, ptype) + body, addr)

    def forward(self, out, addr):
        if self.loss and random.random() < self.loss:
            self.stats['dropped'] += 1
            return
        self.stats['forwarded'] += 1
        if self.lag or self.jitter:
            # like a real path: the extra delay drifts smoothly (not per packet), and packets stay in order
            now = time.monotonic()
            j = self.drift.get(addr, 0.5)
            j = min(1.0, max(0.0, j + random.uniform(-0.08, 0.08)))
            self.drift[addr] = j
            at = max(now + self.lag + j * self.jitter, self.last_out.get(addr, 0.0) + 0.0002)
            self.last_out[addr] = at
            asyncio.get_running_loop().call_later(at - now, self.transport.sendto, out, addr)
        else:
            self.transport.sendto(out, addr)

    def error(self, addr, msg):
        self.send(addr, ERROR, msg.upper().encode('ascii')[:48].ljust(48, b'\0'))

    def alloc_vip(self):
        while True:
            vip = self.next_vip
            self.next_vip = 0x0A000001 if self.next_vip >= 0x0AFFFFFE else self.next_vip + 1
            if vip not in self.by_vip:
                return vip

    def drop(self, c, why):
        self.by_addr.pop(c.addr, None)
        self.by_vip.pop(c.vip, None)
        self.log('leave  %-16s %s (%d online)' % (c.name, why, len(self.by_addr)))

    def datagram_received(self, data, addr):
        if len(data) < 21:
            return
        magic, ptype = struct.unpack_from('<IB', data)
        if magic != MAGIC:
            return
        token, body = data[5:21], data[21:]
        now = time.monotonic()
        if ptype == HELLO:
            if len(body) < 17:
                return
            version, name = body[0], clean_name(body[1:17])
            c = self.by_addr.get(addr)
            if c is None:
                if len(self.by_addr) >= self.max_clients:
                    self.error(addr, 'SERVER FULL')
                    return
                c = Client(addr, self.alloc_vip(), version, name)
                self.by_addr[addr] = c
                self.by_vip[c.vip] = c
                self.log('join   %-16s v%d (%d online)' % (name, version, len(self.by_addr)))
            c.version, c.name, c.seen = version, name, now
            self.send(addr, WELCOME, c.token + struct.pack('<I', c.vip))
            return
        c = self.by_addr.get(addr)
        if c is None or token != c.token:      # unknown sender or forged token: ask it to say hello again
            self.stats['dropped'] += 1
            return
        c.seen = now
        # token bucket rate limit
        c.bucket = min(RATE, c.bucket + (now - c.bucket_t) * RATE)
        c.bucket_t = now
        if c.bucket < 1:
            self.stats['dropped'] += 1
            return
        c.bucket -= 1
        if ptype == PING:              # answer, so a client alone on the server still knows it is connected
            self.send(addr, PING)
            return
        if ptype == BYE:
            self.drop(c, 'bye')
            return
        if ptype != SEND or len(body) < 4 + 5:
            return
        dst = struct.unpack_from('<I', body)[0]
        payload = body[4:]
        if len(payload) > MAX_PAYLOAD or struct.unpack_from('<I', payload)[0] != GAME_MAGIC:
            self.stats['dropped'] += 1
            return
        out = struct.pack('<IBI', MAGIC, DELIVER, c.vip) + payload
        if dst == BROADCAST:
            for o in self.by_addr.values():
                if o is not c and o.version == c.version:
                    self.forward(out, o.addr)
        else:
            o = self.by_vip.get(dst)
            if o is not None:
                self.forward(out, o.addr)

    def sweep(self):
        now = time.monotonic()
        for c in [c for c in self.by_addr.values() if now - c.seen > TIMEOUT]:
            self.drop(c, 'timeout')


REPORT_MAX = 200000
REPORT_RATE = 20             # per address per hour


class Reports:
    def __init__(self, folder, log):
        self.folder, self.log = folder, log
        self.recent = {}
        os.makedirs(folder, exist_ok=True)

    async def handle(self, reader, writer):
        peer = writer.get_extra_info('peername')
        ip = peer[0] if peer else '?'
        try:
            head = await asyncio.wait_for(reader.readexactly(11), 10)
            if head[:7] != b'PABUG1\n':
                return
            n = struct.unpack('<I', head[7:])[0]
            if n > REPORT_MAX:
                writer.write(b'TOO LARGE\n')
                return
            now = time.time()
            times = [t for t in self.recent.get(ip, []) if now - t < 3600]
            if len(times) >= REPORT_RATE:
                writer.write(b'TOO MANY REPORTS\n')
                return
            body = await asyncio.wait_for(reader.readexactly(n), 20)
            times.append(now)
            self.recent[ip] = times
            text = body.decode('utf-8', 'replace')
            name = 'player'
            for line in text.splitlines()[:20]:
                if line.startswith('player: '):
                    name = ''.join(c for c in line[8:] if c.isalnum() or c in '-_')[:16] or 'player'
            fn = os.path.join(self.folder, '%s_%s_%03d.txt' % (time.strftime('%Y%m%d_%H%M%S'), name, int(now * 1000) % 1000))
            with open(fn, 'w', encoding='utf-8', newline='\n') as f:
                f.write('received: %s from %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), ip))
                f.write(text)
            writer.write(b'OK\n')
            self.log('bug report from %-16s saved %s (%d bytes)' % (name, os.path.basename(fn), n))
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, ConnectionError):
            pass
        finally:
            try:
                await writer.drain()
                writer.close()
            except Exception:
                pass


async def main():
    ap = argparse.ArgumentParser(description='Project Arena community relay')
    ap.add_argument('--port', type=int, default=47780)
    ap.add_argument('--bind', default='0.0.0.0')
    ap.add_argument('--max-clients', type=int, default=200)
    ap.add_argument('--reports', default='bug_reports', help='folder for bug reports sent from the game')
    ap.add_argument('--lag', type=float, default=0.0, help='testing: extra one-way delay per forwarded packet (ms)')
    ap.add_argument('--jitter', type=float, default=0.0, help='testing: up to this much more delay at random (ms)')
    ap.add_argument('--loss', type=float, default=0.0, help='testing: percent of forwarded packets dropped')
    args = ap.parse_args()
    loop = asyncio.get_running_loop()
    stamp = lambda m: print(time.strftime('%H:%M:%S ') + m, flush=True)
    relay = Relay(args.max_clients, stamp, args.lag, args.jitter, args.loss)
    await loop.create_datagram_endpoint(lambda: relay, local_addr=(args.bind, args.port))
    stamp('Project Arena relay listening on UDP %s:%d' % (args.bind, args.port))
    reports = Reports(args.reports, stamp)
    await asyncio.start_server(reports.handle, args.bind, args.port)
    stamp('bug reports: TCP %s:%d -> %s' % (args.bind, args.port, os.path.abspath(args.reports)))
    if args.lag or args.jitter or args.loss:
        stamp('TEST MODE: +%g ms lag, +0..%g ms jitter, %g%% loss on forwarded game packets' % (args.lag, args.jitter, args.loss))
    k = 0
    while True:
        await asyncio.sleep(2.0)
        relay.sweep()
        k += 1
        if k % 15 == 0 and relay.by_addr:
            stamp('%d online, forwarded %d, dropped %d' % (len(relay.by_addr), relay.stats['forwarded'], relay.stats['dropped']))


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
