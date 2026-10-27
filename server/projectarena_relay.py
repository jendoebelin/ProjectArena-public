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
  WELCOME 2  body: token (16), virtual address (u32), then the current release: length (u8) + version (ASCII, e.g.
             "0.3.4"; --latest file, empty when there is none). Builds before 0.3.4 read only the first 20 bytes.
  DELIVER 6  body: source virtual address (u32), game packet
  ERROR   7  body: message (48 bytes, upper-case ASCII)
Player statistics (2026-10-20; relays and games from 0.3.6 on, older ones never send these):
  KEY      8  client -> server  body: the player's private key (16 random bytes, kept by the game; the stats' owner)
  TANK     9  host -> server    body: virtual addresses in tank slot 0 and 1 (u32 each, 0 = empty or the bot)
  MATCH   10  host -> server    body: winner slot (u8), then for slot 0 and 1: throws, throws on target, hits,
                                blocks, balls faced on target, catches, top throw speed (cm/s) (7 x u16), then the
                                final score: points 0, points 1, rounds 0, rounds 1 (4 x u8). For the two players the
                                host last named in TANK (both with a KEY, both human); counted only once a tank
                                player who is not the host CONFIRMs the same winner and score (within 60 s)
  CONFIRM 13  player -> server  body: the host's virtual address (u32), winner slot (u8), the final score (4 x u8)
  DELETE  14  player -> server  (no body) delete this player's statistics (the player's KEY says whose)
  MYSTATS 15  player -> server  (no body) the player's own career: answered with STATS for host 0xFFFFFFFF, the
                                player in slot 0 (the game's MY STATS page)
  MATCH from 0.3.8 hosts has one more byte (34 in all): the match's mode, 0 standard rules, 1 HOTDOG (no shields),
  2 other custom MATCH OPTIONS. Careers count every mode; the table player_modes keeps each mode apart as well.
  MATCH from 0.3.9 hosts has four more (38 in all): each slot's points from returns (2 x u16), the hits scored by
  balls the player blocked or punched back. Throws on target are then literal (each throw at most once, returns never:
  GitHub #14), so the checks are: on target <= throws, returns' points <= hits and <= blocks, the other hits <= on
  target. Older hosts (0.3.6-0.3.8) counted a returned ball's hit, strike or block as a throw on target too, so their
  on target can exceed their throws in a real match: it is capped at the throws (accuracy never above 100 %), not
  refused; their hits must still be <= on target. STATS then ends with each slot's points from returns (2 x u32).
  STATSREQ 11 any -> server     body: a host's virtual address
  STATS   12  server -> asker   body: that host's virtual address (u32), then for slot 0 and 1: known (u8), name
                                (16), matches, wins, throws, on target, hits, blocks, faced, catches (8 x u32),
                                top speed (u16 cm/s). Career totals from --stats (SQLite).
The Crowd (2026-10-21; games from 0.3.7 on): fans follow live matches on their phones (a web page; --crowd) and cheer.
  LIVE    16  host -> server    body (every second while hosting): match state (u8, Game's MatchState), points 0,
                                points 1, rounds 0, rounds 1 (4 x u8), nation 0, nation 1 (u8 each, 0 = none), the two
                                ends' colours (2 x RGB u8), flags (u8: 1 advanced rules, 2 slot 1 is the bot), the
                                round clock (u16, tenths of a second), the tank players' names (2 x 16); then
                                (games from 0.3.7, 82 bytes) this match so far for slot 0 and 1: throws, throws on
                                target, hits, blocks, balls faced, catches, strikes, top throw speed (cm/s) (8 x u16),
                                the round (u8) and overtime (u8)
  CROWD   17  server -> host    body (twice a second while anyone watches): fans watching (u16), cheers for end 0
                                and end 1 in the last half second (2 x u16), hype 0 and 1 (2 x u8, 0..100), fans
                                waving each end's flag (2 x u16)
  Hosts from before 0.3.7 send no LIVE: their match is shown from the arena's own BEACON (forwarded to everyone
  anyway: host name, score, clock, rules) and the tank players' names from TANK; no match statistics or flags.
  Fans are anonymous: the web page sends no name or account; the relay keeps them in memory only (no file, no log
  line per fan) and counts at most 10 connections per address.
Privacy: the server never stores a player's key, only a salted hash of it (HMAC-SHA256 with a secret salt kept beside
the table, --stats + '.salt'); both files are readable by the server's own account only. first_seen / last_seen are
kept for abuse checks (spam, the same player connected from two places, which is logged).

Safety: tokens bind each client to its source address; only game packets (magic 'SPRC') are forwarded, at most
1200 bytes; per-client rate limit; broadcasts only reach clients on the same game version; a client cap; names are
sanitised; nothing is logged except connects / disconnects (no packet contents).

Run:  python3 projectarena_relay.py [--port 47780] [--max-clients 200] [--crowd 127.0.0.1:47781]
The Crowd's WebSocket (--crowd) is plain ws:// on a local port: a web server in front (Caddy, see
deploy/setup_crowd.sh) adds HTTPS and passes /ws to it. --crowd-origin limits which web pages may connect.
Bug reports: the game sends them over TCP to the same port: b"PABUG1\\n", u32 length (<= 200 kB), UTF-8 text;
answered "OK\\n". Saved as text files in --reports (default ./bug_reports), at most 20 per address per hour.
Player reports (REPORT PLAYER in the game) arrive the same way; the relay adds the reported name's current address(es)
to the saved file, for the moderators only (players never see addresses). Files whose names start with
"player_report" are those.
Moderation: --bans FILE (default ./bans.txt, optional) lists one ban per line, an IP address or name:PLAYERNAME;
"#" starts a comment. The file is re-read when it changes; banned players are disconnected and refused (BANNED).
Testing: --lag MS --jitter MS --loss PERCENT make the relay behave like a long internet path (forwarded game packets
are delayed by lag plus a smoothly drifting 0..jitter ms, kept in order, and a share is dropped). Never on a public server.
"""
import argparse
import asyncio
import base64
import hashlib
import hmac
import json
import math
import random
import os
import sqlite3
import struct
import time

MAGIC = 0x4C52414F          # 'OARL'
GAME_MAGIC = 0x43525053     # 'SPRC' (the game's LAN packets)
HELLO, WELCOME, SEND, PING, BYE, DELIVER, ERROR = 1, 2, 3, 4, 5, 6, 7
KEY, TANK, MATCH, STATSREQ, STATS, CONFIRM, DELETE, MYSTATS = 8, 9, 10, 11, 12, 13, 14, 15
LIVE, CROWD = 16, 17
MODE_NAMES = ('standard', 'hotdog', 'custom')   # MATCH's mode byte (0.3.8)
SELF = 0xFFFFFFFF           # STATS for MYSTATS: "host" SELF, slot 0 = the asker
CONFIRM_WINDOW = 60.0       # seconds: a host's result and the other player's confirmation must meet within this
STAT_FIELDS = ('matches', 'wins', 'throws', 'on_target', 'hits', 'blocks', 'faced', 'catches')
MATCH_GAP = 30.0            # seconds: at most one result per host this often
BROADCAST = 0xFFFFFFFF
MAX_PAYLOAD = 1200
RATE = 1000                 # packets / second per client (an arena host: ~450 game + forwarded voice)
TIMEOUT = 10.0


def clean_name(raw):
    s = raw.split(b'\0')[0].decode('ascii', 'replace').upper()
    s = ''.join(c if ('A' <= c <= 'Z' or '0' <= c <= '9' or c in ' -.') else ' ' for c in s).strip()
    return s[:16] or 'PLAYER'


def ip_text(v):
    return '%d.%d.%d.%d' % (v >> 24 & 255, v >> 16 & 255, v >> 8 & 255, v & 255)


class Client:
    __slots__ = ('addr', 'token', 'vip', 'version', 'name', 'seen', 'bucket', 'bucket_t', 'joined', 'key', 'tank',
                 'last_match', 'last_req')

    def __init__(self, addr, vip, version, name):
        self.addr, self.vip, self.version, self.name = addr, vip, version, name
        self.token = os.urandom(16)
        self.seen = self.bucket_t = self.joined = time.monotonic()
        self.bucket = RATE
        self.key, self.tank, self.last_match, self.last_req = None, (0, 0), -1e9, -1e9


class Stats:
    """Career statistics per player key (a SQLite file): matches, wins and the counts a host reports per match."""

    def __init__(self, path, log=print):
        self.log = log
        salt_path = path + '.salt'
        if not os.path.exists(salt_path):
            fd = os.open(salt_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, 'wb') as f:
                f.write(os.urandom(32))
        with open(salt_path, 'rb') as f:
            self.salt = f.read()
        self.db = sqlite3.connect(path)
        for p in (path, salt_path):   # the server's own account only
            try:
                os.chmod(p, 0o600)
            except OSError:
                pass
        self.db.execute('CREATE TABLE IF NOT EXISTS players (key TEXT PRIMARY KEY, name TEXT, ' +
                        ', '.join('%s INTEGER DEFAULT 0' % f for f in STAT_FIELDS) +
                        ', top_speed INTEGER DEFAULT 0, first_seen INTEGER, last_seen INTEGER)')
        self.db.execute('CREATE TABLE IF NOT EXISTS player_modes (key TEXT, mode INTEGER, ' +
                        ', '.join('%s INTEGER DEFAULT 0' % f for f in STAT_FIELDS) +
                        ', top_speed INTEGER DEFAULT 0, PRIMARY KEY (key, mode))')
        for table in ('players', 'player_modes'):   # (0.3.9: the points from returns)
            if 'return_hits' not in [r[1] for r in self.db.execute('PRAGMA table_info(%s)' % table)]:
                self.db.execute('ALTER TABLE %s ADD COLUMN return_hits INTEGER DEFAULT 0' % table)
        self.db.commit()
        self.purge()
        n = self.db.execute('SELECT COUNT(*) FROM players').fetchone()[0]
        log('stats: %d players (%s)' % (n, os.path.abspath(path)))

    def purge(self, months=24):
        """The privacy policy's promise: statistics go after 24 months unseen (at start and once a day)."""
        n = self.db.execute('DELETE FROM players WHERE last_seen < ?', (int(time.time()) - months * 30 * 86400,)).rowcount
        self.db.execute('DELETE FROM player_modes WHERE key NOT IN (SELECT key FROM players)')
        self.db.commit()
        if n:
            self.log('stats: %d players unseen for %d months removed' % (n, months))

    def add(self, key, name, won, counts, top, mode=0, returns=0):
        t = int(time.time())
        self.db.execute('INSERT OR IGNORE INTO players (key, name, first_seen, last_seen) VALUES (?, ?, ?, ?)', (key, name, t, t))
        sets = ', '.join('%s = %s + ?' % (f, f) for f in STAT_FIELDS + ('return_hits',))
        vals = (1, 1 if won else 0) + tuple(counts) + (returns,)
        self.db.execute('UPDATE players SET name = ?, last_seen = ?, top_speed = MAX(top_speed, ?), ' + sets + ' WHERE key = ?',
                        (name, t, top) + vals + (key,))
        self.db.execute('INSERT OR IGNORE INTO player_modes (key, mode) VALUES (?, ?)', (key, mode))
        self.db.execute('UPDATE player_modes SET top_speed = MAX(top_speed, ?), ' + sets + ' WHERE key = ? AND mode = ?',
                        (top,) + vals + (key, mode))
        self.db.commit()

    def modes(self, key):
        """a player's statistics by mode: {mode: (matches, wins, ..., top_speed)}"""
        return {r[0]: r[1:] for r in self.db.execute('SELECT mode, ' + ', '.join(STAT_FIELDS) + ', top_speed FROM player_modes '
                                                       'WHERE key = ?', (key,))}

    def ident(self, raw):
        """The stored id for a player's key: a salted hash, so a copy of the table can't be used to act as anyone."""
        return hmac.new(self.salt, raw, hashlib.sha256).hexdigest()

    def delete(self, key):
        self.db.execute('DELETE FROM player_modes WHERE key = ?', (key,))
        n = self.db.execute('DELETE FROM players WHERE key = ?', (key,)).rowcount
        self.db.commit()
        return n

    def get(self, key):
        """(name, matches, wins, throws, on_target, hits, blocks, faced, catches, top_speed, return_hits) or None"""
        return self.db.execute('SELECT name, ' + ', '.join(STAT_FIELDS) + ', top_speed, return_hits FROM players WHERE key = ?',
                               (key,)).fetchone()


class Latest:
    """The current release's version (one line, e.g. 0.3.4) from a text file, re-read when it changes; games older
    than it say UPDATE AVAILABLE."""

    def __init__(self, path):
        self.path, self.mtime, self.version = path, None, b''

    def get(self):
        try:
            m = os.path.getmtime(self.path)
        except OSError:
            return b''
        if m != self.mtime:
            self.mtime = m
            with open(self.path, 'rb') as f:
                v = f.read(64).decode('ascii', 'ignore').strip()
            self.version = ''.join(c for c in v if c.isdigit() or c == '.')[:15].encode()
        return self.version


class Bans:
    """--bans FILE: IP addresses and name:PLAYERNAME lines, re-read whenever the file changes."""

    def __init__(self, path, log):
        self.path, self.log = path, log
        self.ips, self.names, self.mtime = set(), set(), None
        self.reload()

    def reload(self):
        try:
            mtime = os.path.getmtime(self.path)
        except OSError:
            mtime = None
        if mtime == self.mtime:
            return False
        self.mtime = mtime
        ips, names = set(), set()
        if mtime is not None:
            with open(self.path, encoding='utf-8', errors='replace') as f:
                for line in f:
                    line = line.split('#', 1)[0].strip()
                    if line.lower().startswith('name:'):
                        names.add(clean_name(line[5:].encode('ascii', 'replace')))
                    elif line:
                        ips.add(line)
        self.ips, self.names = ips, names
        self.log('bans: %d addresses, %d names (%s)' % (len(ips), len(names), self.path))
        return True

    def banned(self, ip, name):
        return ip in self.ips or name in self.names


class Relay(asyncio.DatagramProtocol):
    def __init__(self, max_clients, log=print, lag=0.0, jitter=0.0, loss=0.0, bans=None, latest=None, stats=None):
        self.latest = latest
        self.player_stats = stats
        self.max_clients = max_clients
        self.bans = bans
        self.lag, self.jitter, self.loss = lag / 1000.0, jitter / 1000.0, loss / 100.0
        self.drift, self.last_out = {}, {}
        self.by_addr, self.by_vip = {}, {}
        self.next_vip = 0x0A000001   # 10.0.0.1 upward (virtual, never routed)
        self.log = log
        self.transport = None
        self.stats = {'forwarded': 0, 'dropped': 0}
        self.pending, self.confirms = {}, {}   # host vip -> its result waiting for a confirmation (and the reverse)
        self.crowd = None                       # The Crowd (fans on phones), when --crowd is on

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
        if self.crowd is not None:
            self.crowd.host_gone(c)
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
            if self.bans is not None and self.bans.banned(addr[0], name):
                self.error(addr, 'BANNED')
                old = self.by_addr.get(addr)
                if old is not None:
                    self.drop(old, 'banned')
                return
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
            v = self.latest.get() if self.latest else b''
            self.send(addr, WELCOME, c.token + struct.pack('<I', c.vip) + bytes([len(v)]) + v)
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
        if ptype == LIVE:
            if self.crowd is not None:
                self.crowd.live(c, body, now)
            return
        if ptype in (KEY, TANK, MATCH, STATSREQ, CONFIRM, DELETE, MYSTATS):
            self.stats_message(c, ptype, body, now)
            return
        if ptype != SEND or len(body) < 4 + 5:
            return
        dst = struct.unpack_from('<I', body)[0]
        payload = body[4:]
        if len(payload) > MAX_PAYLOAD or struct.unpack_from('<I', payload)[0] != GAME_MAGIC:
            self.stats['dropped'] += 1
            return
        out = struct.pack('<IBI', MAGIC, DELIVER, c.vip) + payload
        if dst == BROADCAST and self.crowd is not None and len(payload) > 5 and payload[5] == 1:
            self.crowd.beacon(c, payload, now)   # The Crowd: older hosts' matches from their beacons
        if dst == BROADCAST:
            for o in self.by_addr.values():
                if o is not c and o.version == c.version:
                    self.forward(out, o.addr)
        else:
            o = self.by_vip.get(dst)
            if o is not None:
                self.forward(out, o.addr)

    def stats_message(self, c, ptype, body, now):
        st = self.player_stats
        if ptype == KEY and len(body) >= 16:
            c.key = st.ident(body[:16]) if st is not None else hashlib.sha256(body[:16]).hexdigest()
            for o in self.by_addr.values():   # abuse check: one player connected from two places at once
                if o is not c and o.key == c.key:
                    self.log('stats: the same player is connected twice: %s (%s) and %s (%s)' %
                             (c.name, ip_text(c.vip), o.name, ip_text(o.vip)))
        elif ptype == TANK and len(body) >= 8:
            c.tank = struct.unpack_from('<II', body)
        elif ptype == MATCH and len(body) >= 33 and st is not None:
            if now - c.last_match < MATCH_GAP:
                return
            winner = body[0]
            rows = [struct.unpack_from('<7H', body, 1 + 14 * k) for k in range(2)]
            score = tuple(body[29:33])
            mode = body[33] if len(body) >= 34 and body[33] <= 2 else 0   # (0.3.8: the match's mode)
            returns = struct.unpack_from('<2H', body, 34) if len(body) >= 38 else None   # (0.3.9: points from returns)
            players = [self.by_vip.get(v) for v in c.tank]
            if winner > 1 or any(p is None or p.key is None for p in players) or players[0].key == players[1].key:
                return
            for k, r in enumerate(rows):   # a result that can't be real counts for nobody
                throws, on_target, hits, blocks, faced, catches, top = r
                if returns is None:   # (older hosts: on target may include returns; capped below)
                    bad = hits > on_target or on_target > 3000
                else:
                    rh = returns[k]
                    bad = on_target > throws or rh > hits or rh > blocks or hits - rh > on_target
                if bad or throws > 3000 or blocks > faced or faced > 3000 or top > 6000:
                    self.log('stats: result from %s refused (%s%s)' % (c.name, r, '' if returns is None else ', returns %d' % returns[k]))
                    return
            if returns is None:
                rows = [(r[0], min(r[1], r[0])) + tuple(r[2:]) for r in rows]
            c.last_match = now
            self.pending[c.vip] = dict(t=now, host=c, winner=winner, score=score, rows=rows, mode=mode, returns=returns or (0, 0),
                                       keys=[p.key for p in players], names=[p.name for p in players])
            self.settle(c.vip, now)
        elif ptype == CONFIRM and len(body) >= 9:
            hv, winner = struct.unpack_from('<IB', body)
            if c.key is None:
                return
            self.confirms[hv] = dict(t=now, key=c.key, vip=c.vip, winner=winner, score=tuple(body[5:9]))
            self.settle(hv, now)
        elif ptype == DELETE and st is not None and c.key is not None:
            n = st.delete(c.key)
            self.log('stats: %s deleted their statistics (%d row)' % (c.name, n))
        elif (ptype == STATSREQ and len(body) >= 4) or ptype == MYSTATS:
            if now - c.last_req < 0.5:
                return
            c.last_req = now
            hv = SELF if ptype == MYSTATS else struct.unpack_from('<I', body)[0]
            h = self.by_vip.get(hv)
            out, rets = struct.pack('<I', hv), []
            for v in ((c.vip, 0) if ptype == MYSTATS else h.tank if h is not None else (0, 0)):
                p = self.by_vip.get(v)
                r = st.get(p.key) if (st is not None and p is not None and p.key) else None
                if r is None:
                    name = (p.name if p is not None else '').encode('ascii', 'replace')[:16]
                    out += struct.pack('<B16s8IH', 1 if p is not None and p.key else 0, name, *([0] * 8), 0)
                    rets.append(0)
                else:
                    out += struct.pack('<B16s8IH', 1, (p.name or r[0]).encode('ascii', 'replace')[:16], *r[1:9], min(65535, r[9]))
                    rets.append(r[10] or 0)
            self.send(c.addr, STATS, out + struct.pack('<2I', *rets))   # (0.3.9: points from returns; older games stop at 106)

    def settle(self, hv, now):
        """Count a host's result once a tank player who is not the host confirms the same winner and score."""
        for d in (self.pending, self.confirms):
            for k in [k for k, v in d.items() if now - v['t'] > CONFIRM_WINDOW]:
                del d[k]
        p, cf = self.pending.get(hv), self.confirms.get(hv)
        if p is None or cf is None:
            return
        if cf['vip'] == hv or cf['key'] not in p['keys'] or cf['winner'] != p['winner'] or cf['score'] != p['score']:
            if cf['vip'] != hv and cf['key'] in p['keys']:
                self.log('stats: %s\'s result and the other player\'s disagree: not counted' % p['host'].name)
                del self.pending[hv]
            del self.confirms[hv]
            return
        del self.pending[hv], self.confirms[hv]
        st = self.player_stats
        for k in range(2):
            throws, on_target, hits, blocks, faced, catches, top = p['rows'][k]
            st.add(p['keys'][k], p['names'][k], k == p['winner'], (throws, on_target, hits, blocks, faced, catches), top,
                   p.get('mode', 0), p.get('returns', (0, 0))[k])
        self.log('stats: %s beat %s (host %s, confirmed, %s)' % (p['names'][p['winner']], p['names'][1 - p['winner']],
                                                                 p['host'].name, MODE_NAMES[p.get('mode', 0)]))

    def sweep(self):
        now = time.monotonic()
        for c in [c for c in self.by_addr.values() if now - c.seen > TIMEOUT]:
            self.drop(c, 'timeout')
        if self.bans is not None and self.bans.reload():
            for c in [c for c in self.by_addr.values() if self.bans.banned(c.addr[0], c.name)]:
                self.error(c.addr, 'BANNED')
                self.drop(c, 'banned')

    def addresses_of(self, name):
        """moderators: where a reported player name is connected from right now"""
        return sorted({'%s:%d' % c.addr for c in self.by_addr.values() if c.name == name})


# ------------------------------------------------------------------ The Crowd: fans on phones (WebSocket, JSON)
CROWD_TICK = 0.5             # seconds: CROWD to hosts, the watched match to its fans
CROWD_MAX_FANS = 2000
CROWD_PER_ADDR = 10          # connections per address
CROWD_MSG_RATE = 20          # messages / second per fan
CROWD_CHEERS = 8             # counted cheers / second per fan
CROWD_IDLE = 90.0            # seconds without a message (the page pings every 20 s)
WS_GUID = b'258EAFA5-E914-47DA-95CA-C5AB0DC85B11'


class LiveMatch:
    __slots__ = ('id', 'host', 'info', 'seen', 'cheers', 'hype', 'fans', 'career', 'career_t', 'live_t')

    def __init__(self, host):
        self.id = os.urandom(4).hex()          # what fans see (not the virtual address)
        self.host, self.info, self.seen = host, None, 0.0
        self.cheers, self.hype, self.fans = [0, 0], [0.0, 0.0], set()
        self.career, self.career_t = [None, None], -1e9
        self.live_t = -1e9                     # the last LIVE (0.3.7+); until then the beacon describes the match


class Fan:
    __slots__ = ('writer', 'addr', 'watch', 'bucket', 'bucket_t', 'cheer_bucket', 'flag', 'flag_t', 'seen')

    def __init__(self, writer, addr):
        self.writer, self.addr, self.watch = writer, addr, None
        self.bucket, self.cheer_bucket = CROWD_MSG_RATE, CROWD_CHEERS
        self.bucket_t = self.seen = time.monotonic()
        self.flag, self.flag_t = -1, 0.0


def ws_frame(text, op=0x81):
    data = text.encode('utf-8') if isinstance(text, str) else text
    n = len(data)
    if n < 126:
        head = bytes([op, n])
    elif n < 65536:
        head = bytes([op, 126]) + struct.pack('>H', n)
    else:
        head = bytes([op, 127]) + struct.pack('>Q', n)
    return head + data


class Crowd:
    def __init__(self, relay, log, origins=()):
        self.relay, self.log = relay, log
        self.origins = tuple(o.rstrip('/').lower() for o in origins if o)
        self.matches = {}                      # host vip -> LiveMatch
        self.fans = set()
        relay.crowd = self

    # --- hosts (UDP)
    def live(self, c, body, now):
        if len(body) < 48:
            return
        st, p0, p1, r0, r1, n0, n1 = struct.unpack_from('<7B', body)
        rgb = body[7:13]
        flags, clock = struct.unpack_from('<BH', body, 13)
        names = (clean_name(body[16:32]), clean_name(body[32:48]))
        m = self.matches.get(c.vip)
        if m is None:
            m = self.matches[c.vip] = LiveMatch(c)
        m.seen = m.live_t = now
        m.info = {'id': m.id, 'host': c.name, 'st': st, 's': [p0, p1, r0, r1], 'adv': bool(flags & 1),
                  'bot': bool(flags & 2), 'clock': clock / 10.0, 'hotdog': bool(flags & 4), 'free': bool(flags & 8),
                  'kg': bool(flags & 16), 'rt': bool(flags & 32),
                  'p': [{'n': names[k], 'nat': (n0, n1)[k], 'c': '#%02x%02x%02x' % tuple(rgb[3 * k:3 * k + 3])}
                        for k in range(2)]}
        if len(body) >= 82:   # this match so far (0.3.7)
            v = struct.unpack_from('<16H2B', body, 48)
            for k in range(2):
                t, ot, hi, bl, fa, ca, sk, top = v[8 * k:8 * k + 8]
                m.info['p'][k]['m'] = {'throws': t, 'on': ot, 'hits': hi, 'blocks': bl, 'faced': fa, 'catches': ca,
                                       'strikes': sk, 'top': round(top * 0.036)}   # cm/s -> km/h
            m.info['rnd'], m.info['ot'] = v[16], bool(v[17])
        self.careers(c, m, now)

    def beacon(self, c, payload, now):
        """a host from before 0.3.7 (no LIVE): its arena's beacon, which every host broadcasts twice a second"""
        if len(payload) < 14 + 30 or payload[4] != 8 or payload[5] != 1:   # protocol 8, MSG_BEACON
            return
        m = self.matches.get(c.vip)
        if m is not None and now - m.live_t < 3.0:   # it sends LIVE: that says more
            return
        adv, full, s0, s1, clock, specs, waiting, queued, humans = struct.unpack_from('<BBhhfBBBB', payload, 30)
        if m is None:
            m = self.matches[c.vip] = LiveMatch(c)
        m.seen = now
        names = []
        for k, v in enumerate(c.tank):   # the tank as the host last named it (TANK, every 3 s since 0.3.6)
            p = self.relay.by_vip.get(v) if v else None
            names.append(p.name if p is not None else 'BOT' if k else c.name if c.tank == (0, 0) else 'OPEN')
        cl = clock if clock == clock and 0 <= clock < 100000 else 0.0   # (not NaN)
        m.info = {'id': m.id, 'host': c.name, 'st': 2, 's': [max(0, s0), max(0, s1), 0, 0], 'adv': bool(adv),
                  'bot': names[1] == 'BOT', 'clock': round(cl, 1), 'old': True,
                  'p': [{'n': names[0], 'nat': 0, 'c': '#1ab3ff'}, {'n': names[1], 'nat': 0, 'c': '#ff6a1a'}]}
        self.careers(c, m, now)

    def careers(self, c, m, now):
        # the two players' careers (the statistics the game shows over the court), refreshed every 10 s
        st_db = self.relay.player_stats
        if st_db is not None and now - m.career_t > 10.0:
            m.career_t = now
            for k, v in enumerate(c.tank):
                p = self.relay.by_vip.get(v) if v else None
                r = st_db.get(p.key) if p is not None and p.key else None
                m.career[k] = None if r is None else {'matches': r[1], 'wins': r[2], 'throws': r[3], 'on': r[4],
                                                      'blocks': r[6], 'faced': r[7], 'top': round(r[9] * 0.036)}
                if r is not None:
                    m.career[k]['modes'] = {MODE_NAMES[md]: {'matches': v[0], 'wins': v[1]}
                                            for md, v in st_db.modes(p.key).items() if md < len(MODE_NAMES)}
        for k in range(2):
            if m.career[k] is not None:
                m.info['p'][k]['car'] = m.career[k]

    def host_gone(self, c):
        m = self.matches.get(c.vip)
        if m is None or m.host is not c:
            return
        del self.matches[c.vip]
        for f in list(m.fans):
            f.watch = None
            self.send(f, {'t': 'gone', 'id': m.id})

    # --- fans (WebSocket)
    async def handle(self, reader, writer):
        fan = None
        try:
            head = await asyncio.wait_for(reader.readuntil(b'\r\n\r\n'), 10)
            lines = head.decode('latin1').split('\r\n')
            req = lines[0].split(' ')
            hdr = {}
            for ln in lines[1:]:
                if ':' in ln:
                    k, v = ln.split(':', 1)
                    hdr[k.strip().lower()] = v.strip()
            key = hdr.get('sec-websocket-key', '')
            origin = hdr.get('origin', '').rstrip('/').lower()
            if (len(req) < 2 or req[0] != 'GET' or req[1].split('?')[0] != '/ws'
                    or 'websocket' not in hdr.get('upgrade', '').lower() or not key
                    or (self.origins and origin not in self.origins)):
                writer.write(b'HTTP/1.1 403 Forbidden\r\nContent-Length: 0\r\nConnection: close\r\n\r\n')
                return
            # the phone's address (the web server in front sets X-Forwarded-For): only to cap connections per address
            addr = hdr.get('x-forwarded-for', '').split(',')[0].strip() or (writer.get_extra_info('peername') or ('?',))[0]
            if len(self.fans) >= CROWD_MAX_FANS or sum(1 for f in self.fans if f.addr == addr) >= CROWD_PER_ADDR:
                writer.write(b'HTTP/1.1 503 Busy\r\nContent-Length: 0\r\nConnection: close\r\n\r\n')
                return
            accept = base64.b64encode(hashlib.sha1(key.encode('latin1') + WS_GUID).digest()).decode()
            writer.write(('HTTP/1.1 101 Switching Protocols\r\nUpgrade: websocket\r\nConnection: Upgrade\r\n'
                          'Sec-WebSocket-Accept: %s\r\n\r\n' % accept).encode())
            fan = Fan(writer, addr)
            self.fans.add(fan)
            self.send(fan, {'t': 'hello', 'v': 1})
            self.send(fan, self.list_msg())
            while True:
                b = await asyncio.wait_for(reader.readexactly(2), CROWD_IDLE)
                fin, op, masked, n = b[0] & 0x80, b[0] & 0x0F, b[1] & 0x80, b[1] & 0x7F
                if n == 126:
                    n = struct.unpack('>H', await reader.readexactly(2))[0]
                elif n == 127:
                    return
                if not masked or not fin or n > 512:   # phones send short, masked, unfragmented messages
                    return
                mask = await reader.readexactly(4)
                data = bytes(x ^ mask[i & 3] for i, x in enumerate(await reader.readexactly(n)))
                if op == 8:
                    writer.write(b'\x88\x00')
                    return
                if op == 9:
                    writer.write(ws_frame(data, 0x8A))
                    continue
                if op == 1:
                    self.fan_message(fan, data)
        except (asyncio.TimeoutError, asyncio.IncompleteReadError, asyncio.LimitOverrunError, ConnectionError,
                UnicodeDecodeError, ValueError):
            pass
        finally:
            if fan is not None:
                self.fans.discard(fan)
                if fan.watch is not None:
                    fan.watch.fans.discard(fan)
            try:
                writer.close()
            except Exception:
                pass

    def fan_message(self, fan, data):
        now = time.monotonic()
        fan.seen = now
        dt = now - fan.bucket_t
        fan.bucket_t = now
        fan.bucket = min(CROWD_MSG_RATE, fan.bucket + dt * CROWD_MSG_RATE)
        fan.cheer_bucket = min(CROWD_CHEERS, fan.cheer_bucket + dt * CROWD_CHEERS)
        if fan.bucket < 1:
            return
        fan.bucket -= 1
        try:
            msg = json.loads(data.decode('utf-8'))
        except ValueError:
            return
        if not isinstance(msg, dict):
            return
        t = msg.get('t')
        if t == 'watch':
            if fan.watch is not None:
                fan.watch.fans.discard(fan)
                fan.watch = None
            m = next((m for m in self.matches.values() if m.id == msg.get('id')), None)
            if m is not None and m.info:
                fan.watch = m
                m.fans.add(fan)
                self.send(fan, self.match_msg(m))
            else:
                self.send(fan, self.list_msg())
        elif t == 'cheer' and fan.watch is not None:
            side, n = msg.get('side'), msg.get('n', 1)
            if side in (0, 1) and type(n) is int and n > 0:
                n = min(n, int(fan.cheer_bucket))
                fan.cheer_bucket -= n
                fan.watch.cheers[side] += n
        elif t == 'flag':
            side = msg.get('side')
            fan.flag, fan.flag_t = (side, now) if side in (0, 1) and type(side) is int else (-1, 0.0)

    def send(self, fan, obj):
        try:
            if fan.writer.transport.get_write_buffer_size() > 65536:   # a stalled phone: skip it
                return
            fan.writer.write(ws_frame(json.dumps(obj, separators=(',', ':'))))
        except Exception:
            pass

    def list_msg(self):
        return {'t': 'list', 'm': [dict(m.info, fans=len(m.fans)) for m in self.matches.values() if m.info]}

    def match_msg(self, m):
        now = time.monotonic()
        waves = [sum(1 for f in m.fans if f.flag == k and now - f.flag_t < 4.0) for k in range(2)]
        return dict(m.info, t='m', fans=len(m.fans), h=[round(h) for h in m.hype], w=waves)

    def tick(self, k):
        """every CROWD_TICK: hype from the cheers, CROWD to each host, the match to its fans, the list every second"""
        now = time.monotonic()
        for m in list(self.matches.values()):
            if now - m.seen > 5.0:            # the host stopped sending LIVE (left the arena)
                self.host_gone(m.host)
                continue
            n = max(1, len(m.fans))
            for s in range(2):
                rate = m.cheers[s] / CROWD_TICK                         # cheers per second
                target = 100.0 * (1.0 - math.exp(-rate / (3.0 + 1.5 * n)))
                m.hype[s] += (target - m.hype[s]) * (0.5 if target > m.hype[s] else 0.1)
                if m.hype[s] < 0.5:
                    m.hype[s] = 0.0
            mm = self.match_msg(m)
            if m.fans or m.hype[0] or m.hype[1]:
                self.relay.send(m.host.addr, CROWD, struct.pack('<3H2B2H', min(len(m.fans), 65535), min(m.cheers[0], 65535),
                                                                min(m.cheers[1], 65535), round(m.hype[0]),
                                                                round(m.hype[1]), mm['w'][0], mm['w'][1]))
            m.cheers = [0, 0]
            for f in list(m.fans):
                self.send(f, mm)
        for f in [f for f in self.fans if now - f.seen > CROWD_IDLE]:
            try:
                f.writer.close()
            except Exception:
                pass
        if k % 2 == 0:
            lst = self.list_msg()
            for f in list(self.fans):
                if f.watch is None:
                    self.send(f, lst)


REPORT_MAX = 200000
REPORT_RATE = 20             # per address per hour


class Reports:
    def __init__(self, folder, log, relay=None):
        self.folder, self.log, self.relay = folder, log, relay
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
            name, kind, extra = 'player', 'bug report', ''
            lines = text.splitlines()
            if lines and lines[0].strip() == 'Project Arena player report':
                kind = 'player report'
            for line in lines[:20]:
                if line.startswith('player: '):
                    name = ''.join(c for c in line[8:] if c.isalnum() or c in '-_')[:16] or 'player'
                if kind == 'player report' and line.startswith('reported: ') and self.relay is not None:
                    who = clean_name(line[10:].split(' (session', 1)[0].encode('ascii', 'replace'))
                    where = self.relay.addresses_of(who)
                    extra = 'reported player on the relay now: %s %s\n' % (who, ', '.join(where) if where else '(not connected)')
            prefix = 'player_report_' if kind == 'player report' else ''
            fn = os.path.join(self.folder, '%s%s_%s_%03d.txt' % (prefix, time.strftime('%Y%m%d_%H%M%S'), name, int(now * 1000) % 1000))
            with open(fn, 'w', encoding='utf-8', newline='\n') as f:
                f.write('received: %s from %s\n' % (time.strftime('%Y-%m-%d %H:%M:%S'), ip))
                f.write(extra)
                f.write(text)
            writer.write(b'OK\n')
            self.log('%s from %-16s saved %s (%d bytes)' % (kind, name, os.path.basename(fn), n))
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
    ap.add_argument('--bans', default='bans.txt', help='moderation: banned IP addresses and name:PLAYERNAME lines')
    ap.add_argument('--latest', default='latest_version.txt', help='the current release (e.g. 0.3.4): older games say UPDATE AVAILABLE')
    ap.add_argument('--stats', default='player_stats.db', help='player statistics (SQLite), keyed by each game\'s private key')
    ap.add_argument('--crowd', default='', help='The Crowd: local WebSocket address for the phone web app '
                    '(e.g. 127.0.0.1:47781, behind an HTTPS web server); off when empty')
    ap.add_argument('--crowd-origin', action='append', default=[], help='The Crowd: web page origin allowed to connect '
                    '(e.g. https://crowd.example.com; repeat for more; any when none)')
    ap.add_argument('--lag', type=float, default=0.0, help='testing: extra one-way delay per forwarded packet (ms)')
    ap.add_argument('--jitter', type=float, default=0.0, help='testing: up to this much more delay at random (ms)')
    ap.add_argument('--loss', type=float, default=0.0, help='testing: percent of forwarded packets dropped')
    args = ap.parse_args()
    loop = asyncio.get_running_loop()
    stamp = lambda m: print(time.strftime('%H:%M:%S ') + m, flush=True)
    relay = Relay(args.max_clients, stamp, args.lag, args.jitter, args.loss, Bans(args.bans, stamp), Latest(args.latest),
                  Stats(args.stats, stamp))
    await loop.create_datagram_endpoint(lambda: relay, local_addr=(args.bind, args.port))
    stamp('Project Arena relay listening on UDP %s:%d' % (args.bind, args.port))
    reports = Reports(args.reports, stamp, relay)
    await asyncio.start_server(reports.handle, args.bind, args.port)
    stamp('bug reports: TCP %s:%d -> %s' % (args.bind, args.port, os.path.abspath(args.reports)))
    crowd = None
    if args.crowd:
        h, _, p = args.crowd.rpartition(':')
        crowd = Crowd(relay, stamp, args.crowd_origin)
        await asyncio.start_server(crowd.handle, h or '127.0.0.1', int(p), limit=16384)
        stamp('the crowd: WebSocket %s (origins: %s)' % (args.crowd, ', '.join(args.crowd_origin) or 'any'))
    if args.lag or args.jitter or args.loss:
        stamp('TEST MODE: +%g ms lag, +0..%g ms jitter, %g%% loss on forwarded game packets' % (args.lag, args.jitter, args.loss))
    k, half = 0, 0
    while True:
        await asyncio.sleep(CROWD_TICK)
        half += 1
        if crowd is not None:
            crowd.tick(half)
        if half % 4:
            continue
        relay.sweep()
        k += 1
        if k % 43200 == 0 and relay.player_stats is not None:   # once a day (k counts 2 s steps)
            relay.player_stats.purge()
        if k % 15 == 0 and relay.by_addr:
            stamp('%d online, forwarded %d, dropped %d%s' % (len(relay.by_addr), relay.stats['forwarded'], relay.stats['dropped'],
                                                          ', %d fans' % len(crowd.fans) if crowd is not None and crowd.fans else ''))


if __name__ == '__main__':
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
