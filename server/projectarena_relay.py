#!/usr/bin/env python3
"""Project Arena community relay server (MIT). One UDP port; no dependencies beyond Python 3.8+.

The relay works like a virtual LAN for the game: every connected headset gets a private virtual address, and the
game's own LAN packets (beacons, join, inputs, snapshots, spectating) are forwarded between players by that address.
So hosting, the games list, joining and spectating work online exactly as on a home network, and players never see
each other's real IP address.

Protocol (little endian), client -> server: magic 'OARL' (u32), type (u8), token (16 bytes), body
  HELLO  1  body: game protocol version (u8), name (16 bytes)      (token = zeros), then from 0.3.11 the game's
             own version: length (u8) + ASCII (e.g. "0.3.11"), for the log; older games stop after the name
  SEND   3  body: destination virtual address (u32, 0xFFFFFFFF = everyone), game packet
  PING   4  keep-alive (every ~2 s; clients silent for 10 s are dropped); answered with a PING
  BYE    5
server -> client: magic, type, body
  WELCOME 2  body: token (16), virtual address (u32), then the current release: length (u8) + version (ASCII, e.g.
             "0.3.4"; --latest file, empty when there is none). Builds before 0.3.4 read only the first 20 bytes.
  DELIVER 6  body: source virtual address (u32), game packet
  ERROR   7  body: message (48 bytes, upper-case ASCII)
A minimum version (2026-11-15, --minimum FILE: one line, e.g. 0.3.20; none when the file is missing or empty): a game
  older than it (games before 0.3.11 don't say theirs: older than any) is answered at HELLO with ERROR UPDATE TO 0.3.20
  OR NEWER TO PLAY ONLINE, which every game version shows as a notice, and is not let on. The file is re-read while
  running; games already online and older than a new minimum get the same message and are disconnected. Raised only
  after players were told ahead of time (docs/BUILD_REVIEW_CHECKLIST.md, section 4).
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
                                the round (u8) and overtime (u8); then (games from 0.3.23, a four-player match:
                                FOUR CORNERS or FIREBALL BLITZ; 104 bytes, 186 in all) 4 (u8), the room (u8: 2 FOUR
                                CORNERS, 3 FIREBALL BLITZ), team mode (u8: 0 free-for-all, 1 2 v 2, 2 3 v 1), THE ONE
                                (u8); for players 0-3 their colour (RGB), team (u8), points (i16), health (u8), out
                                (u8); the four countries (4 x u8); players 2 and 3's names (16) and this match so far
                                (8 x u16, as above). With two players the ends are the players; with four in teams
                                the two teams (points 0 and 1: the teams' scores)
  CROWD   17  server -> host    body (twice a second while anyone watches): fans watching (u16), cheers for end 0
                                and end 1 in the last half second (2 x u16), hype 0 and 1 (2 x u8, 0..100), fans
                                waving each end's flag (2 x u16)
  Hosts from before 0.3.7 send no LIVE: their match is shown from the arena's own BEACON (forwarded to everyone
  anyway: host name, score, clock, rules) and the tank players' names from TANK; no match statistics or flags.
  Fans are anonymous: the web page sends no name or account; the relay keeps them in memory only (no file, no log
  line per fan) and counts at most 10 connections per address.
  Notifications (2026-11-21, --push FILE): a fan can turn on notifications (the page's bell). The hello then carries
  the server's VAPID public key ("push"). The page sends {"t": "push", "sub": the browser's push subscription,
  "prefs": {"when": "any" or "match", "gap": 30, 60 or 180 minutes, "quiet": [from, to] hours or null, "tz": minutes
  east of UTC}} (again each time it opens), {"t": "unpush", "endpoint"} to turn them off; the relay answers
  {"t": "push", "ok"} and sends a Web Push notification when someone hosts an arena or a match of two or more players
  starts (class Push, below). No following a player by name (the owner, 2026-11-21).
TRAINING leaderboards (games from 0.3.12; the player's KEY says whose, as for careers):
  TRAIN   18  player -> server  body: a lesson passed: the lesson (u8 id, LESSONS below), 1 (u8), its time (u32 ms; 0 for
                                a lesson without a clock), successes (u8), attempts (u8). Kept per player and lesson: the
                                best time and the best accuracy (successes / attempts), with the player's name. A result
                                that can't be real for that lesson is refused (logged). The game sends its bests again at
                                each connection: the server keeps only what is better.
  BOARDREQ 19 player -> server  body: lesson (u8; 0 = the overall board), board (u8: 0 TIME, 1 ACCURACY)
  BOARD   20  server -> asker   body: lesson (u8), board (u8), count (u8), then that many places: name (16), value (u32:
                                the time in ms; accuracy in tenths of a percent; overall: every lesson's best accuracy
                                added up), then the asker's own place (u16, 0 = none), value (u32), and how many are on
                                the board (u16)
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
Blocked names (2026-11-13): a name with a slur or hate word (NAME_WORDS_*, and the bans file's word:WORD lines) is
never shown to other players (they see PLAYER), a rename while online included; the log never has the name.
SOUND STUDIO (0.3.16): --sounds FOLDER (default ./sound_library) holds sounds players can try in the game and vote on,
named Event__name.wav (Event: one of SOUND_EVENTS; name: a-z, 0-9 and _; WAV mono 48 kHz 16-bit; at most SOUND_MAX
sounds, SOUND_PER_EVENT per event, the oldest first). Over TCP on the same port: b"PASND1\n", then u8 1 (the catalog;
then the game's 16-byte statistics key, or 16 zeros): answered b"PASND1\n" and one line per sound,
"file\tsize\tsha256\tmy vote (-1, 0, 1)\n", then b"END\n"; or u8 2 (a sound; then u8 length and the file name): answered
u32 size and the file (0xFFFFFFFF: no such sound). Votes come on the UDP connection (SOUNDVOTE), one per player key and
sound; players are only ever told their own. The owner, on the server: --sound-totals prints every sound's thumbs up and
down; --sound-remove FILE --reason removed|adopted [--adopted-in VERSION] takes a sound out of the library (into
FOLDER/removed, its totals kept in FOLDER/removed/results.log) and prints the result for the Discord post.
MUTED PLAYERS (0.3.23; a mute the player keeps on their own headset):
  MUTECODE 22  player -> server  body: virtual addresses (u32 each, at most 16, at most 5 requests a second)
               server -> player  body: for each, the address (u32) and an 8-byte code for that person made for this
                                 player only (HMAC-SHA256 of the two players' keys as the server knows them, the
                                 salted hashes): the same person keeps it under a new name, no one else gets the same
                                 code for them, and it says nothing about the key. Zeros: not online, either of
                                 the two without a KEY, or two who haven't met (no game packets between them, or
                                 with the same host, in the last 10 minutes: from 2026-11-21). Worked out when asked;
                                 nothing is stored.
Testing: --lag MS --jitter MS --loss PERCENT make the relay behave like a long internet path (forwarded game packets
are delayed by lag plus a smoothly drifting 0..jitter ms, kept in order, and a share is dropped). Never on a public server.
"""
import argparse
import asyncio
import re
import base64
import hashlib
import hmac
import json
import math
import random
import os
import secrets
import socket
import sqlite3
import struct
import sys
import time
import urllib.error
import urllib.parse
import urllib.request

MAGIC = 0x4C52414F          # 'OARL'
GAME_MAGIC = 0x43525053     # 'SPRC' (the game's LAN packets)
HELLO, WELCOME, SEND, PING, BYE, DELIVER, ERROR = 1, 2, 3, 4, 5, 6, 7
KEY, TANK, MATCH, STATSREQ, STATS, CONFIRM, DELETE, MYSTATS = 8, 9, 10, 11, 12, 13, 14, 15
LIVE, CROWD = 16, 17
TRAIN, BOARDREQ, BOARD = 18, 19, 20
SOUNDVOTE = 21              # player -> server (0.3.16)  body: u8 length, the sound's file name, i8 vote (-1, 0, 1)
MUTECODE = 22               # player <-> server (0.3.23): MUTED PLAYERS' codes (see the protocol above)
# SOUND STUDIO's library (the game's sound events, app/assets_open/audio/events.txt; a sound is made for one of them)
SOUND_EVENTS = ('Spawn', 'SpawnRise', 'Grab', 'Caught', 'ThrowSlow', 'ThrowMed', 'ThrowFast', 'ThrowFull', 'ThrowFail',
                'Bounce', 'ShieldBlock', 'ShieldBreak', 'KnuckleBlock', 'HitPlayerOpponent', 'HitPlayerSelf', 'Returned',
                'Foul', 'Countdown', 'RoundStart', 'RoundEndWin', 'RoundEndLose', 'MatchEndWin', 'MatchEndLose', 'Recharge',
                'Strike', 'Overtime', 'FistBump', 'Ambience', 'Crowd', 'Cheer', 'ClockTick', 'LeadChange', 'TargetHit',
                'KnucklePunch')   # (S34, 0.3.22)
SOUND_MAX, SOUND_PER_EVENT = 40, 5
SOUND_BYTES = 1300000       # (12 s of 48 kHz mono 16-bit: the longest a crowd loop may be)
SOUND_FILE = re.compile(r'^([A-Za-z]+)__([a-z0-9_]{1,24})\.wav$')
SOUND_RATE = 120            # catalog and sound requests per address per minute
# TRAINING's lessons (app/src/training.cpp; their ids never change): id -> (has a clock, the goal, throws or tries in it).
# The goal: THROW lessons every target (one throw each at least), the others successes to pass. A passed run can't be
# quicker than about 1.4 s a target (each takes your ball, which comes back in 1.7 s).
# Three tracks (owner, 2026-10-29): throwing 1-8, blocking 21-25, dodging 41-44.
LESSONS = {1: (1, 5, 15), 2: (1, 8, 22), 3: (1, 5, 15), 4: (1, 4, 16), 5: (1, 4, 16), 6: (1, 6, 18), 7: (1, 5, 15), 8: (0, 5, 8),
           21: (0, 7, 10), 22: (0, 7, 10), 23: (0, 7, 10), 24: (0, 9, 12), 25: (1, 3, 15),
           41: (0, 7, 10), 42: (0, 7, 10), 43: (0, 7, 10), 44: (0, 8, 12)}
DEFENSE_TRIES = (8, 21, 22, 23, 24, 41, 42, 43, 44)   # every try counts: attempts are the lesson's tries
MODE_NAMES = ('standard', 'hotdog', 'custom')   # MATCH's mode byte (0.3.8)
SELF = 0xFFFFFFFF           # STATS for MYSTATS: "host" SELF, slot 0 = the asker
CONFIRM_WINDOW = 60.0       # seconds: a host's result and the other player's confirmation must meet within this
STAT_FIELDS = ('matches', 'wins', 'throws', 'on_target', 'hits', 'blocks', 'faced', 'catches')
MATCH_GAP = 30.0            # seconds: at most one result per host this often
BROADCAST = 0xFFFFFFFF
MAX_PAYLOAD = 1200
RATE = 1000                 # packets / second per client
HOST_RATE = 2500            # ... an arena host (LIVE in the last HOST_HOLD s): a full room of four playing and four
HOST_HOLD = 5.0             #     waiting is ~500 game packets a second, and ~150 more for each person talking (0.3.13)
TIMEOUT = 10.0
NAME_REFUSED = 'NAME NOT ALLOWED: CHANGE NAME TO PLAY ONLINE'   # (ERROR: at most 48, the game shows it as a notice)


def tidy_name(raw):
    """A name as the game's keyboard can type it: upper case A-Z, 0-9, space, - and ., at most 16."""
    s = raw.split(b'\0')[0].decode('ascii', 'replace').upper()
    s = ''.join(c if ('A' <= c <= 'Z' or '0' <= c <= '9' or c in ' -.') else ' ' for c in s).strip()
    return s[:16] or 'PLAYER'


# Names nobody may use online (owner, 2026-11-13: a player renamed themselves to a racist slur mid-session). Slurs and
# hate words. A name is read with look-alike digits as letters (N1GG3R) and each letter allowed to repeat (NIIIGGER).
# NAME_WORDS_ANYWHERE are found inside a longer name too, with its spaces, dashes and dots left out (N I G G E R);
# NAME_WORDS_ALONE only as a whole word of the name, digits around it ignored and an S on the end allowed (so RACCOON,
# SPICY, KNIGHT and PAKISTAN stay fine; COON, SPICS and FAG69 do not). The owner adds more without a code change:
# word:WORD lines in the bans file, found anywhere like the first list (keep those words long enough to be safe).
# A game with such a name is told NAME NOT ALLOWED and disconnected (a rename while online too); the relay's own
# displays (the Crowd page, the boards) show PLAYER instead.
NAME_WORDS_ANYWHERE = ('NIGGER', 'NIGGA', 'NIGGUH', 'NIGLET', 'NIGNOG', 'FAGOT', 'FAGIT', 'WETBACK', 'JIGABOO',
                       'PORCHMONKEY', 'JUNGLEBUNNY', 'TOWELHEAD', 'RAGHEAD', 'CHINGCHONG', 'HITLER', 'SIEGHEIL',
                       'WHITEPOWER', 'GASTHEJEW', 'KKK')
NAME_WORDS_ALONE = ('NIG', 'KIKE', 'KYKE', 'CHINK', 'SPIC', 'SPICK', 'SPIK', 'GOOK', 'COON', 'BEANER', 'WOP', 'PAKI',
                    'DYKE', 'FAG', 'TRANNY', 'SHEMALE', 'RETARD', 'RETARDED', 'NAZI')
NAME_NUMBERS_ALONE = ('1488',)
LOOKALIKE = str.maketrans('013456789', 'OIEASGTBG')


def _name_pattern(word):
    return ''.join(re.escape(c) + '+' for c in word)


NAME_ANYWHERE_RE = re.compile('|'.join(_name_pattern(w) for w in NAME_WORDS_ANYWHERE))
NAME_ALONE_RE = re.compile('(?:%s)S*' % '|'.join(_name_pattern(w) for w in NAME_WORDS_ALONE))


def name_blocked(name, extra=()):
    """True for a name with a slur or hate word in it (and the bans file's word: lines, given as extra)."""
    parts = [p for p in re.split(r'[ .\-]+', name) if p]
    if any(p in NAME_NUMBERS_ALONE for p in parts):
        return True
    squashed = ''.join(parts).translate(LOOKALIKE)
    if NAME_ANYWHERE_RE.search(squashed) or any(w in squashed for w in extra):
        return True
    for p in parts:
        for form in (p.translate(LOOKALIKE), p.strip('0123456789').translate(LOOKALIKE)):
            if form and NAME_ALONE_RE.fullmatch(form):
                return True
    return False


def clean_name(raw):
    """tidy_name, and PLAYER for a blocked name: what the relay itself shows of a name (the Crowd page, the boards)."""
    s = tidy_name(raw)
    return 'PLAYER' if name_blocked(s) else s


def app_version(body):
    """The game's own version after HELLO's name ('' from games before 0.3.11): digits and dots only."""
    if len(body) < 18:
        return ''
    raw = body[18:18 + min(body[17], 15)]
    return ''.join(chr(b) for b in raw if chr(b).isdigit() or b == ord('.'))


def version_tuple(v):
    """'0.3.20' -> (0, 3, 20), compared part by part (0.3.9 < 0.3.20); '' (before 0.3.11) -> (), older than any."""
    if isinstance(v, bytes):
        v = v.decode('ascii', 'ignore')
    return tuple(int(p) for p in v.split('.') if p.isdigit()) if v else ()


def too_old(app, minimum):
    """whether a game on version app (HELLO's, '' before 0.3.11) is older than the minimum (b'' or '': none)"""
    m = version_tuple(minimum)
    return bool(m) and version_tuple(app) < m


def update_message(minimum):
    m = minimum.decode('ascii', 'ignore') if isinstance(minimum, bytes) else minimum
    msg = 'UPDATE TO %s OR NEWER TO PLAY ONLINE' % m
    return msg if len(msg) <= 48 else 'UPDATE TO %s TO PLAY ONLINE' % m   # (ERROR: at most 48; versions at most 15)


def ip_text(v):
    return '%d.%d.%d.%d' % (v >> 24 & 255, v >> 16 & 255, v >> 8 & 255, v & 255)


class Client:
    __slots__ = ('addr', 'token', 'vip', 'version', 'app', 'name', 'seen', 'bucket', 'bucket_t', 'host_until', 'joined', 'key', 'tank',
                 'last_match', 'last_req', 'trains', 'last_board', 'sound_votes', 'last_mute', 'peers')

    def __init__(self, addr, vip, version, name):
        self.addr, self.vip, self.version, self.name = addr, vip, version, name
        self.app = ''
        self.token = os.urandom(16)
        self.seen = self.bucket_t = self.joined = time.monotonic()
        self.bucket = RATE
        self.host_until = 0.0   # (hosting: the host's rate, while its LIVE keeps coming)
        self.key, self.tank, self.last_match, self.last_req = None, (0, 0), -1e9, -1e9
        self.trains, self.last_board = 0, -1e9
        self.sound_votes = 0    # (SOUND STUDIO: votes from this connection)
        self.last_mute = -1e9   # (MUTECODE: the last request answered)
        self.peers = {}         # (MUTECODE: the virtual addresses this one sent game packets to or had them from, and
                                #  when last; in memory only, gone with the connection)


class Stats:
    """Career statistics per player key (a SQLite file): matches, wins and the counts a host reports per match."""

    DELETIONS_DAYS = 30     # how long a deletion's note is kept (longer than any backup: tools/backup_server.py keeps 14)

    def __init__(self, path, log=print):
        self.log = log
        self.deletions_path = path + '.deletions'
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
        self.db.execute('CREATE TABLE IF NOT EXISTS training (key TEXT, lesson INTEGER, name TEXT, best_ms INTEGER DEFAULT 0, '
                        'succ INTEGER DEFAULT 0, att INTEGER DEFAULT 0, updated INTEGER, PRIMARY KEY (key, lesson))')
        for table in ('players', 'player_modes'):   # (0.3.9: the points from returns)
            if 'return_hits' not in [r[1] for r in self.db.execute('PRAGMA table_info(%s)' % table)]:
                self.db.execute('ALTER TABLE %s ADD COLUMN return_hits INTEGER DEFAULT 0' % table)
        self.db.commit()
        self.reapply_deletions()
        self.purge()
        n = self.db.execute('SELECT COUNT(*) FROM players').fetchone()[0]
        log('stats: %d players (%s)' % (n, os.path.abspath(path)))

    def purge(self, months=24):
        """The privacy policy's promise: statistics go after 24 months unseen (at start and once a day)."""
        n = self.db.execute('DELETE FROM players WHERE last_seen < ?', (int(time.time()) - months * 30 * 86400,)).rowcount
        self.db.execute('DELETE FROM player_modes WHERE key NOT IN (SELECT key FROM players)')
        self.db.execute('DELETE FROM training WHERE updated < ?', (int(time.time()) - months * 30 * 86400,))
        try:   # (SOUND STUDIO's votes, 0.3.16: 24 months after they were last changed)
            self.db.execute('DELETE FROM sound_votes WHERE updated < ?', (int(time.time()) - months * 30 * 86400,))
        except sqlite3.OperationalError:
            pass
        self.db.commit()
        if n:
            self.log('stats: %d players unseen for %d months removed' % (n, months))
        try:   # (the deletion notes: none kept past DELETIONS_DAYS)
            with open(self.deletions_path, encoding='ascii') as f:
                lines = len(f.read().splitlines())
            notes = self.deletions()
            if lines != len(notes):
                self.write_deletions(notes)
        except OSError:
            pass

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

    # DELETIONS (2026-11-20; the owner: if a backup is restored, "there won't be confusion why their stats are back"):
    # a DELETE also leaves a note beside the database (--stats + '.deletions': the time and the key's salted hash, no
    # name, nothing else; the server's account only), kept DELETIONS_DAYS. At every start the notes are applied again:
    # a restored copy of the database loses what was deleted after it was made. Only what is older than the deletion
    # goes (the career row first seen before it, training and votes last changed before it), so a player who plays on
    # after deleting keeps the new statistics.
    def deletions(self):
        cut, out = time.time() - self.DELETIONS_DAYS * 86400, []
        try:
            with open(self.deletions_path, encoding='ascii') as f:
                for line in f:
                    parts = line.split()
                    if len(parts) == 2 and parts[0].isdigit() and int(parts[0]) >= cut:
                        out.append((int(parts[0]), parts[1]))
        except OSError:
            pass
        return out

    def write_deletions(self, notes):
        tmp = self.deletions_path + '.tmp'
        fd = os.open(tmp, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o600)
        with os.fdopen(fd, 'w', encoding='ascii') as f:
            f.writelines('%d %s\n' % n for n in notes)
        os.replace(tmp, self.deletions_path)

    def note_deletion(self, key):
        self.write_deletions(self.deletions() + [(int(time.time()), key)])   # (the expired ones dropped as it goes)

    def reapply_deletions(self):
        notes = self.deletions()
        gone = 0
        for at, key in notes:
            # (strictly older: anything from the second of the deletion on is new, made after it)
            n = self.db.execute('DELETE FROM players WHERE key = ? AND first_seen < ?', (key, at)).rowcount
            self.db.execute('DELETE FROM player_modes WHERE key = ? AND key NOT IN (SELECT key FROM players)', (key,))
            n += self.db.execute('DELETE FROM training WHERE key = ? AND updated < ?', (key, at)).rowcount
            try:
                n += self.db.execute('DELETE FROM sound_votes WHERE key = ? AND updated < ?', (key, at)).rowcount
            except sqlite3.OperationalError:
                pass
            gone += 1 if n else 0
        self.db.commit()
        if gone:
            self.log('stats: %d deletions applied again (a restored copy had brought them back)' % gone)
        if notes or os.path.exists(self.deletions_path):
            self.log('stats: %d deletion notes (%d days)' % (len(notes), self.DELETIONS_DAYS))

    def delete(self, key):
        self.db.execute('DELETE FROM player_modes WHERE key = ?', (key,))
        self.db.execute('DELETE FROM training WHERE key = ?', (key,))
        try:   # (SOUND STUDIO's votes, 0.3.16)
            self.db.execute('DELETE FROM sound_votes WHERE key = ?', (key,))
        except sqlite3.OperationalError:
            pass
        n = self.db.execute('DELETE FROM players WHERE key = ?', (key,)).rowcount
        self.db.commit()
        return n

    def train(self, key, name, lesson, ms, succ, att):
        """A passed lesson: keeps the better time and the better accuracy (the name as the player is called now).
        True when something improved."""
        t = int(time.time())
        self.db.execute('INSERT OR IGNORE INTO training (key, lesson, name, updated) VALUES (?, ?, ?, ?)', (key, lesson, name, t))
        best_ms, s0, a0 = self.db.execute('SELECT best_ms, succ, att FROM training WHERE key = ? AND lesson = ?', (key, lesson)).fetchone()
        better = False
        if ms > 0 and (best_ms == 0 or ms < best_ms):
            best_ms, better = ms, True
        if a0 == 0 or succ * 1000 // att > s0 * 1000 // a0 or (succ * 1000 // att == s0 * 1000 // a0 and succ > s0):
            s0, a0, better = succ, att, True
        self.db.execute('UPDATE training SET best_ms = ?, succ = ?, att = ?, updated = ? WHERE key = ? AND lesson = ?',
                        (best_ms, s0, a0, t, key, lesson))
        self.db.execute('UPDATE training SET name = ? WHERE key = ?', (name, key))
        self.db.commit()
        return better

    def board(self, key, lesson, kind, n=10):
        """(the top n [(name, value)], the asker's place (0: none), its value, how many are on the board)"""
        if lesson == 0:   # overall: each player's lessons' best accuracies added up
            q = 'SELECT key, MAX(name), SUM(succ * 1000 / att) AS v FROM training WHERE att > 0 GROUP BY key'
            rows = self.db.execute(q + ' ORDER BY v DESC, MAX(updated) ASC LIMIT ?', (n,)).fetchall()
            mine = self.db.execute('SELECT SUM(succ * 1000 / att) FROM training WHERE key = ? AND att > 0', (key,)).fetchone()[0]
            count = self.db.execute('SELECT COUNT(DISTINCT key) FROM training WHERE att > 0').fetchone()[0]
            place = 0
            if mine is not None:
                place = 1 + self.db.execute('SELECT COUNT(*) FROM (' + q + ') WHERE v > ?', (mine,)).fetchone()[0]
            return [(r[1], r[2]) for r in rows], place, mine or 0, count
        if kind == 0:
            where = 'lesson = ? AND best_ms > 0'
            rows = self.db.execute('SELECT name, best_ms FROM training WHERE ' + where + ' ORDER BY best_ms ASC, updated ASC LIMIT ?',
                                   (lesson, n)).fetchall()
            mine = self.db.execute('SELECT best_ms FROM training WHERE key = ? AND ' + where, (key, lesson)).fetchone()
            count = self.db.execute('SELECT COUNT(*) FROM training WHERE ' + where, (lesson,)).fetchone()[0]
            place = 0 if mine is None else 1 + self.db.execute('SELECT COUNT(*) FROM training WHERE ' + where + ' AND best_ms < ?',
                                                               (lesson, mine[0])).fetchone()[0]
            return [(r[0], r[1]) for r in rows], place, mine[0] if mine else 0, count
        where = 'lesson = ? AND att > 0'
        rows = self.db.execute('SELECT name, succ * 1000 / att AS v FROM training WHERE ' + where +
                               ' ORDER BY v DESC, succ DESC, updated ASC LIMIT ?', (lesson, n)).fetchall()
        mine = self.db.execute('SELECT succ * 1000 / att FROM training WHERE key = ? AND ' + where, (key, lesson)).fetchone()
        count = self.db.execute('SELECT COUNT(*) FROM training WHERE ' + where, (lesson,)).fetchone()[0]
        place = 0 if mine is None else 1 + self.db.execute('SELECT COUNT(*) FROM training WHERE ' + where + ' AND succ * 1000 / att > ?',
                                                           (lesson, mine[0])).fetchone()[0]
        return [(r[0], r[1]) for r in rows], place, mine[0] if mine else 0, count

    def get(self, key):
        """(name, matches, wins, throws, on_target, hits, blocks, faced, catches, top_speed, return_hits) or None"""
        return self.db.execute('SELECT name, ' + ', '.join(STAT_FIELDS) + ', top_speed, return_hits FROM players WHERE key = ?',
                               (key,)).fetchone()


def train_ok(lesson, ms, succ, att):
    """Could a passed run of this lesson have gone so?"""
    if lesson not in LESSONS:
        return False
    timed, goal, tries = LESSONS[lesson]
    if att < 1 or att > tries or succ > att or succ < goal:
        return False
    if lesson in DEFENSE_TRIES and att != tries:   # (defense and CATCH: every try counts)
        return False
    if lesson not in DEFENSE_TRIES and lesson not in (7, 25) and succ != goal:   # (THROW: every target, once)
        return False
    if timed and (ms < 800 + 1400 * (goal - 1) or ms > 3600 * 1000):
        return False
    return True


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
    """--bans FILE: IP addresses, name:PLAYERNAME and word:WORD lines (no name containing WORD may be used, see
    name_blocked), re-read whenever the file changes."""

    def __init__(self, path, log):
        self.path, self.log = path, log
        self.ips, self.names, self.words, self.mtime = set(), set(), (), None
        self.reload()

    def reload(self):
        try:
            mtime = os.path.getmtime(self.path)
        except OSError:
            mtime = None
        if mtime == self.mtime:
            return False
        self.mtime = mtime
        ips, names, words = set(), set(), set()
        if mtime is not None:
            with open(self.path, encoding='utf-8', errors='replace') as f:
                for line in f:
                    line = line.split('#', 1)[0].strip()
                    if line.lower().startswith('name:'):
                        names.add(tidy_name(line[5:].encode('ascii', 'replace')))
                    elif line.lower().startswith('word:'):
                        w = ''.join(c for c in tidy_name(line[5:].encode('ascii', 'replace')) if c not in ' .-')
                        if len(w) >= 3:   # (shorter would block half the names)
                            words.add(w.translate(LOOKALIKE))
                    elif line:
                        ips.add(line)
        self.ips, self.names, self.words = ips, names, tuple(sorted(words))
        self.log('bans: %d addresses, %d names, %d words (%s)' % (len(ips), len(names), len(words), self.path))
        return True

    def banned(self, ip, name):
        return ip in self.ips or name in self.names


class Relay(asyncio.DatagramProtocol):
    def __init__(self, max_clients, log=print, lag=0.0, jitter=0.0, loss=0.0, bans=None, latest=None, stats=None,
                 minimum=None):
        self.latest = latest
        self.minimum = minimum                  # (a Latest on --minimum: games older than it are told to update)
        self.min_seen = b''
        self.too_old = {}                       # address -> when its refusal was last logged (it says hello every second)
        self.player_stats = stats
        self.max_clients = max_clients
        self.bans = bans
        self.sounds = None          # SOUND STUDIO's library (set by main)
        self.lag, self.jitter, self.loss = lag / 1000.0, jitter / 1000.0, loss / 100.0
        self.drift, self.last_out = {}, {}
        self.by_addr, self.by_vip = {}, {}
        self.next_vip = 0x0A000001   # 10.0.0.1 upward (virtual, never routed)
        self.log = log
        self.transport = None
        self.stats = {'forwarded': 0, 'dropped': 0}
        self.name_refused = {}                  # address -> when its blocked name was last logged
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

    def refuse_name(self, addr, now):
        """A blocked name (name_blocked): the game is told and, if it was online (a rename), disconnected. The log
        never has the name: a renamed player is logged under the name they had, a new one once in 10 minutes."""
        self.error(addr, NAME_REFUSED)
        old = self.by_addr.get(addr)
        if old is not None:
            self.drop(old, 'name not allowed (renamed to a blocked word)')
        elif now - self.name_refused.get(addr, -1e9) > 600:
            if len(self.name_refused) > 1000:
                self.name_refused.clear()
            self.name_refused[addr] = now
            self.log('refused a name with a blocked word (a new connection)')

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
            version, name = body[0], tidy_name(body[1:17])
            if self.bans is not None and self.bans.banned(addr[0], name):
                self.error(addr, 'BANNED')
                old = self.by_addr.get(addr)
                if old is not None:
                    self.drop(old, 'banned')
                return
            if name_blocked(name, self.bans.words if self.bans is not None else ()):
                self.refuse_name(addr, now)
                return
            c = self.by_addr.get(addr)
            if c is None:
                minimum = self.minimum.get() if self.minimum else b''
                if too_old(app_version(body), minimum):
                    self.error(addr, update_message(minimum))
                    if now - self.too_old.get(addr, -1e9) > 600:
                        if len(self.too_old) > 4096:
                            self.too_old.clear()
                        self.too_old[addr] = now
                        self.log('refused %s: older than the minimum %s' % (app_version(body) or 'pre-0.3.11',
                                                                          minimum.decode()))
                    return
                if len(self.by_addr) >= self.max_clients:
                    self.error(addr, 'SERVER FULL')
                    return
                c = Client(addr, self.alloc_vip(), version, name)
                c.app = app_version(body)
                self.by_addr[addr] = c
                self.by_vip[c.vip] = c
                self.log('join   %-16s %s v%d (%d online)' % (name, c.app or 'pre-0.3.11', version,
                                                             len(self.by_addr)))
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
        rate = HOST_RATE if c.host_until > now else RATE
        c.bucket = min(rate, c.bucket + (now - c.bucket_t) * rate)
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
            c.host_until = now + HOST_HOLD
            if self.crowd is not None:
                self.crowd.live(c, body, now)
            return
        if ptype == SOUNDVOTE:
            if self.sounds is not None:
                self.sounds.vote_message(c, body)
            return
        if ptype == MUTECODE:
            self.mute_codes(c, body, now)
            return
        if ptype in (KEY, TANK, MATCH, STATSREQ, CONFIRM, DELETE, MYSTATS, TRAIN, BOARDREQ):
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
                self.met(c, o, now)   # (MUTECODE: who plays or watches with whom)
                self.forward(out, o.addr)

    PEER_SECONDS = 600   # MUTECODE: how long after their last game packet two players still count as having met

    def met(self, c, o, now):
        """Two connections exchanged a game packet (one forwarded from c to o): each notes the other, for MUTECODE."""
        for x, y in ((c, o), (o, c)):
            if len(x.peers) >= 256:   # (a busy connection: only those met lately kept)
                x.peers = {v: t for v, t in x.peers.items() if now - t < self.PEER_SECONDS}
                if len(x.peers) >= 256:
                    x.peers.pop(min(x.peers, key=x.peers.get))
            x.peers[y.vip] = now

    def have_met(self, c, o, now):
        """Whether c may have o's code: they exchanged game packets lately, or both did with the same third (the host of
        the room they are in, or whose arena they watch). Review 2026-11-21: any address online was answered, so a game
        could collect a lasting code for everyone online and follow them through name changes"""
        s = self.PEER_SECONDS
        if now - c.peers.get(o.vip, -1e9) < s:
            return True
        return any(now - t < s and now - o.peers.get(v, -1e9) < s for v, t in c.peers.items())

    def mute_codes(self, c, body, now):
        """MUTED PLAYERS (0.3.23): for each virtual address asked, that person's code for this player only (see the
        protocol above), for people the player has met (have_met). Nothing is kept: the same two keys always give the
        same code."""
        if now - c.last_mute < 0.2 or len(body) < 4:
            return
        c.last_mute = now
        out = bytearray()
        for i in range(min(len(body) // 4, 16)):
            v = struct.unpack_from('<I', body, 4 * i)[0]
            o = self.by_vip.get(v)
            code = bytes(8)
            if o is not None and o is not c and c.key and o.key and self.have_met(c, o, now):
                code = hmac.new(c.key.encode(), o.key.encode(), hashlib.sha256).digest()[:8]
            out += struct.pack('<I', v) + code
        self.send(c.addr, MUTECODE, bytes(out))

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
        elif ptype == TRAIN and len(body) >= 8 and st is not None and c.key is not None:
            lesson, _, ms, succ, att = struct.unpack_from('<BBIBB', body)
            c.trains += 1
            if c.trains > 100 or not train_ok(lesson, ms, succ, att):   # (a game sends its dozen bests per connection)
                self.log('stats: training result from %s refused (lesson %d, %d ms, %d / %d)' % (c.name, lesson, ms, succ, att))
                return
            if st.train(c.key, c.name, lesson, ms if LESSONS[lesson][0] else 0, succ, att):
                self.log('stats: %s: a training best (lesson %d, %.1f s, %d / %d)' % (c.name, lesson, ms / 1000.0, succ, att))
        elif ptype == BOARDREQ and len(body) >= 2 and st is not None:
            if now - c.last_board < 0.25:
                return
            c.last_board = now
            lesson, kind = body[0], 1 if body[1] else 0
            if lesson != 0 and lesson not in LESSONS:
                return
            if lesson != 0 and kind == 0 and not LESSONS[lesson][0]:
                kind = 1
            rows, place, mine, count = st.board(c.key or '', lesson, kind)
            out = struct.pack('<BBB', lesson, kind, len(rows))
            for name, v in rows:
                out += struct.pack('<16sI', (name or 'PLAYER').encode('ascii', 'replace')[:16], int(v))
            self.send(c.addr, BOARD, out + struct.pack('<HIH', min(65535, place), int(mine), min(65535, count)))
        elif ptype == DELETE and st is not None and c.key is not None:
            n = st.delete(c.key)
            st.note_deletion(c.key)   # (so a restored backup can't bring them back)
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
        minimum = self.minimum.get() if self.minimum else b''
        if minimum != self.min_seen:   # (a new minimum: games already online and older than it are told, then let go)
            self.min_seen = minimum
            self.log('minimum version: %s' % (minimum.decode() or 'none'))
            for c in [c for c in self.by_addr.values() if too_old(c.app, minimum)]:
                self.error(c.addr, update_message(minimum))
                self.drop(c, 'older than the minimum %s' % minimum.decode())
        if self.bans is not None and self.bans.reload():
            for c in [c for c in self.by_addr.values() if self.bans.banned(c.addr[0], c.name)]:
                self.error(c.addr, 'BANNED')
                self.drop(c, 'banned')
            for c in [c for c in self.by_addr.values() if name_blocked(c.name, self.bans.words)]:
                self.error(c.addr, NAME_REFUSED)
                self.drop(c, 'name not allowed (a new word: line)')

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
    __slots__ = ('id', 'host', 'info', 'seen', 'cheers', 'hype', 'fans', 'career', 'career_t', 'live_t', 'pushed')

    def __init__(self, host):
        self.id = os.urandom(4).hex()          # what fans see (not the virtual address)
        self.host, self.info, self.seen = host, None, 0.0
        self.cheers, self.hype, self.fans = [0, 0], [0.0, 0.0], set()
        self.career, self.career_t = [None, None], -1e9
        self.live_t = -1e9                     # the last LIVE (0.3.7+); until then the beacon describes the match
        self.pushed = set()                    # the notifications already sent for it ('hosted', 'match')


class Fan:
    __slots__ = ('writer', 'addr', 'watch', 'bucket', 'bucket_t', 'cheer_bucket', 'flag', 'flag_t', 'seen', 'push_bucket',
                 'push_t')

    def __init__(self, writer, addr):
        self.writer, self.addr, self.watch = writer, addr, None
        self.bucket, self.cheer_bucket = CROWD_MSG_RATE, CROWD_CHEERS
        self.bucket_t = self.seen = time.monotonic()
        self.flag, self.flag_t = -1, 0.0
        self.push_bucket, self.push_t = PUSH_MSG_BURST, self.seen   # notification messages (each may write the table)


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
    def __init__(self, relay, log, origins=(), push=None):
        self.relay, self.log, self.push = relay, log, push
        self.recent_hosts = {}                 # host (its key, or name) -> when its last arena closed (notifications)
        self.push_new = {}                     # phone address -> [new devices today, since when] (memory only)
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
        if len(body) >= 82 + 104 and body[82] == 4:   # four players (games from 0.3.23: FOUR CORNERS, FIREBALL BLITZ)
            room, tm, one = body[83], body[84], body[85]
            m.info.update({'np': 4, 'room': room, 'tm': tm, 'one': one})
            ps = m.info['p']
            # (the layout: 82 the marker, room, team mode, THE ONE; 86 + 8k player k's colour, team, points, health, out;
            # 118 + k the countries; 122 + 32i players 2 and 3's names and 138 + 32i their counts: 186 bytes)
            ps.extend({'n': clean_name(body[122 + 32 * i:138 + 32 * i])} for i in range(2))   # (players 2 and 3)
            for k in range(4):
                q = 86 + 8 * k
                ps[k]['c'] = '#%02x%02x%02x' % tuple(body[q:q + 3])
                ps[k]['team'] = body[q + 3]
                ps[k]['pts'] = struct.unpack_from('<h', body, q + 4)[0]
                ps[k]['hp'] = body[q + 6]
                ps[k]['out'] = bool(body[q + 7])
                ps[k]['nat'] = body[118 + k]
            for i in range(2):
                t, ot, hi, bl, fa, ca, sk, top = struct.unpack_from('<8H', body, 138 + 32 * i)
                ps[2 + i]['m'] = {'throws': t, 'on': ot, 'hits': hi, 'blocks': bl, 'faced': fa, 'catches': ca,
                                  'strikes': sk, 'top': round(top * 0.036)}
        self.careers(c, m, now)
        self.announce(m)

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
        self.announce(m)

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
        if m.info and self.push is not None:
            self.recent_hosts[m.host.key or m.info['host']] = time.time()
            if len(self.recent_hosts) > 500:
                cut = time.time() - PUSH_REHOST
                self.recent_hosts = {k: v for k, v in self.recent_hosts.items() if v > cut}
        for f in list(m.fans):
            f.watch = None
            self.send(f, {'t': 'gone', 'id': m.id})

    def announce(self, m):
        """notifications: someone hosts an arena ('hosted', once per arena, not when the same host just reopened one)
        and a match of two or more players starts ('match', once per arena; FOUR CORNERS and FIREBALL BLITZ too). The
        names as The Crowd shows them (a blocked name is PLAYER already)."""
        if self.push is None or not m.info:
            return
        i = m.info
        shown = lambda n: n if n and not name_blocked(n) else 'A player'
        people = [p.get('n') for p in i['p'] if p.get('n') not in (None, '', 'OPEN', 'BOT')]
        if i.get('np') != 4 and i.get('bot'):
            people = people[:1]                # a one-on-one against the bot: one player
        try:
            if 'match' not in m.pushed and len(people) >= 2 and i.get('st') in (1, 2, 3):
                m.pushed.update(('match', 'hosted'))   # (a match already on when the arena appears: just the match)
                names = [shown(n) for n in people]
                if len(names) == 2:
                    title = '%s vs %s is live' % tuple(names)
                else:
                    title = '%s & %s are playing %s' % (', '.join(names[:-1]), names[-1],
                                                        'FIREBALL BLITZ' if i.get('room') == 3 else 'FOUR CORNERS')
                self.push.notify('match', title, 'Tap to watch and cheer on The Crowd.', m.id)
            elif 'hosted' not in m.pushed:
                m.pushed.add('hosted')
                if time.time() - self.recent_hosts.get(m.host.key or i['host'], -1e9) > PUSH_REHOST:
                    self.push.notify('hosted', '%s is hosting an arena' % shown(i['host']),
                                     'People are getting on in Project Arena. Tap to watch, or hop in.', m.id)
        except sqlite3.Error as e:
            self.log('push: %s' % e)

    def fan_push(self, fan, msg):
        """a page's 'push' (notifications on or changed) or 'unpush' (off): at most PUSH_MSG_BURST at once and one every
        PUSH_MSG_EVERY seconds per connection, and at most PUSH_NEW_PER_ADDR new devices a day from one address, so a
        page can't fill the table or keep the relay writing it"""
        now = time.monotonic()
        fan.push_bucket = min(PUSH_MSG_BURST, fan.push_bucket + (now - fan.push_t) / PUSH_MSG_EVERY)
        fan.push_t = now
        if fan.push_bucket < 1:
            return False
        fan.push_bucket -= 1
        if msg.get('t') == 'unpush':
            self.push.unsubscribe(msg.get('endpoint'))
            return True
        sub = msg.get('sub')
        ep = sub.get('endpoint') if isinstance(sub, dict) else None
        if not isinstance(ep, str):
            return False
        if self.push.known(ep):
            return self.push.subscribe(sub, msg.get('prefs'))
        n, since = self.push_new.get(fan.addr, (0, now))
        if now - since > 86400:
            n, since = 0, now
        if n >= PUSH_NEW_PER_ADDR or not self.push.subscribe(sub, msg.get('prefs')):
            return False
        self.push_new[fan.addr] = (n + 1, since)
        return True

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
            hello = {'t': 'hello', 'v': 1}
            if self.push is not None:
                hello['push'] = self.push.public_key   # (the page shows its bell)
            self.send(fan, hello)
            self.send(fan, self.list_msg())
            while True:
                b = await asyncio.wait_for(reader.readexactly(2), CROWD_IDLE)
                fin, op, masked, n = b[0] & 0x80, b[0] & 0x0F, b[1] & 0x80, b[1] & 0x7F
                if n == 126:
                    n = struct.unpack('>H', await reader.readexactly(2))[0]
                elif n == 127:
                    return
                if not masked or not fin or n > 2048:   # phones send short, masked, unfragmented messages (a push
                    return                                # subscription is the longest: up to about 1.3 kB)
                mask = await reader.readexactly(4)
                raw = await reader.readexactly(n)       # unmasked in one go (a byte at a time cost 0.1 ms a message)
                data = (int.from_bytes(raw, 'big') ^ int.from_bytes((mask * (n // 4 + 1))[:n], 'big')).to_bytes(n, 'big')
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
        elif t in ('push', 'unpush') and self.push is not None:   # notifications on / changed / off (the bell)
            try:
                ok = self.fan_push(fan, msg)
            except sqlite3.Error as e:
                self.log('push: %s' % e)
                ok = False
            self.send(fan, dict({'t': 'push', 'ok': bool(ok)}, **({'off': True} if t == 'unpush' else {})))

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
        if self.push is not None:
            try:
                self.push.flush()              # the push services' answers (gone, refused) in one transaction
            except sqlite3.Error as e:
                self.log('push: %s' % e)
            if k % 7200 == 0 and self.push_new:   # (an hour) the day's counts of new devices per address that are over
                self.push_new = {a: v for a, v in self.push_new.items() if now - v[1] <= 86400}


# ---------------------------------------------------------------- The Crowd: notifications (Web Push, 2026-11-21)
# A fan who turns on notifications on The Crowd (the bell) gets a phone or computer notification when people are
# playing: someone hosts an arena, or a match starts (the owner, 2026-11-21: no following a player by name). Standard
# Web Push (RFC 8030): VAPID (RFC 8292, an ES256 signature) and an encrypted message (RFC 8291, aes128gcm), with the
# `cryptography` package (Ubuntu: python3-cryptography; setup.sh installs it). Without it the relay runs as before,
# without notifications (the page's bell stays hidden).
# Kept (--push FILE, SQLite, the server's account only): each device's push address (the browser's anonymous endpoint
# and its two keys) and the fan's choices (what about, how often, quiet hours, the phone's time zone offset), when it
# was last notified and last seen; deleted when the fan turns notifications off, when the browser's push service says
# the address is gone (or refuses it three times running), or a year after the page was last opened. Fans' addresses
# are never kept: in memory only, a day's count of new devices per address.

try:
    from cryptography.hazmat.primitives import hashes as _c_hashes, serialization as _c_ser
    from cryptography.hazmat.primitives.asymmetric import ec as _c_ec, utils as _c_utils
    from cryptography.hazmat.primitives.ciphers.aead import AESGCM as _C_AESGCM
    HAVE_CRYPTOGRAPHY = True
except ImportError:
    HAVE_CRYPTOGRAPHY = False

P256_N = 0xffffffff00000000ffffffffffffffffbce6faada7179e84f3b9cac2fc632551   # (the curve's order: VAPID keys below it)


def b64u(data):
    return base64.urlsafe_b64encode(data).rstrip(b'=').decode('ascii')


def b64u_dec(text):
    return base64.urlsafe_b64decode(text + '=' * (-len(text) % 4))


def p256_point(raw):
    """a browser's public key: an uncompressed P-256 point, checked to be on the curve (ValueError otherwise)"""
    if not isinstance(raw, bytes) or len(raw) != 65 or raw[0] != 4:
        raise ValueError('not a P-256 point')
    return _c_ec.EllipticCurvePublicKey.from_encoded_point(_c_ec.SECP256R1(), raw)


def p256_public(d):
    return _c_ec.derive_private_key(d, _c_ec.SECP256R1()).public_key().public_bytes(
        _c_ser.Encoding.X962, _c_ser.PublicFormat.UncompressedPoint)


def webpush_keys(p256dh, auth, as_pub, shared, salt):
    """RFC 8291's key derivation (HKDF-SHA-256, one block each): the content key and nonce"""
    prk_key = hmac.new(auth, shared, hashlib.sha256).digest()
    ikm = hmac.new(prk_key, b'WebPush: info\0' + p256dh + as_pub + b'\1', hashlib.sha256).digest()
    prk = hmac.new(salt, ikm, hashlib.sha256).digest()
    cek = hmac.new(prk, b'Content-Encoding: aes128gcm\0\1', hashlib.sha256).digest()[:16]
    nonce = hmac.new(prk, b'Content-Encoding: nonce\0\1', hashlib.sha256).digest()[:12]
    return cek, nonce


def webpush_encrypt(p256dh, auth, payload, _d=None, _salt=None):
    """RFC 8291: the message body for one browser (its p256dh public key and auth secret), one record (_d, _salt: the
    RFC's example in the tests; otherwise a new key and salt for every message)"""
    ua = p256_point(p256dh)
    key = _c_ec.derive_private_key(_d, _c_ec.SECP256R1()) if _d else _c_ec.generate_private_key(_c_ec.SECP256R1())
    as_pub = key.public_key().public_bytes(_c_ser.Encoding.X962, _c_ser.PublicFormat.UncompressedPoint)
    salt = _salt or os.urandom(16)
    cek, nonce = webpush_keys(p256dh, auth, as_pub, key.exchange(_c_ec.ECDH(), ua), salt)
    return salt + struct.pack('>IB', 4096, 65) + as_pub + _C_AESGCM(cek).encrypt(nonce, payload + b'\2', None)


def es256(d, msg):
    """ECDSA P-256 / SHA-256, the raw 64-byte r || s that a JWT carries"""
    der = _c_ec.derive_private_key(d, _c_ec.SECP256R1()).sign(msg, _c_ec.ECDSA(_c_hashes.SHA256()))
    r, s = _c_utils.decode_dss_signature(der)
    return r.to_bytes(32, 'big') + s.to_bytes(32, 'big')


# the browsers' push services (a fan's push address must be on one of them: the relay never posts anywhere else)
PUSH_HOSTS = ('fcm.googleapis.com', 'updates.push.services.mozilla.com', 'web.push.apple.com')
PUSH_HOST_ENDS = ('.push.apple.com', '.notify.windows.com', '.push.services.mozilla.com')
PUSH_MAX = 5000              # push addresses kept at most
PUSH_NEW_PER_ADDR = 10       # new devices one address may turn notifications on for in a day (in memory only)
PUSH_MSG_BURST = 6           # notification messages ('push', 'unpush') one page connection may send at once ...
PUSH_MSG_EVERY = 10.0        # ... and one more every this many seconds
PUSH_GAPS = (30, 60, 180)    # minutes: "at most once every ..."
PUSH_REHOST = 15 * 60        # seconds: the same host hosting again this soon is not news
PUSH_KEEP_DAYS = 365         # a device whose page hasn't been opened for this long is forgotten
PUSH_FAILS = 3               # refusals in a row (an error other than gone, 404 / 410) before a device is forgotten
PUSH_TTL = 900               # seconds the push service may hold a notification (old "live now" is no use)
PUSH_QUEUE_MAX = 300         # notifications waiting for the notification process at most (more are dropped)
PUSH_PER_HOUR = 3000         # notifications sent an hour at most (more are dropped)
PUSH_PER_EVENT = 1000        # devices one event notifies at most (those notified longest ago first)


def push_endpoint_ok(url):
    if not isinstance(url, str) or len(url) > 1024:
        return False
    try:
        u = urllib.parse.urlsplit(url)
        port = u.port
    except ValueError:
        return False
    host = (u.hostname or '').lower()
    return (u.scheme == 'https' and port in (None, 443) and not u.username
            and (host in PUSH_HOSTS or any(host.endswith(e) for e in PUSH_HOST_ENDS)))


def push_prefs(raw):
    """a fan's choices, checked: when (any: someone hosts or a match starts; match: only when a match starts), gap
    (minutes between notifications), quiet ([from, to] local hours, or None) and tz (the phone's offset from UTC in
    minutes, for the quiet hours)"""
    raw = raw if isinstance(raw, dict) else {}
    when = raw.get('when') if raw.get('when') in ('any', 'match') else 'any'
    gap = raw.get('gap') if raw.get('gap') in PUSH_GAPS else 60
    q = raw.get('quiet')
    quiet = ([q[0], q[1]] if isinstance(q, list) and len(q) == 2 and all(type(h) is int and 0 <= h < 24 for h in q)
             and q[0] != q[1] else None)
    tz = raw.get('tz') if type(raw.get('tz')) is int and -900 <= raw.get('tz') <= 900 else 0
    return {'when': when, 'gap': gap, 'quiet': quiet, 'tz': tz}


class _NoRedirect(urllib.request.HTTPRedirectHandler):
    def redirect_request(self, *args, **kwargs):
        return None


# ---- the notification process. Encrypting, signing and posting run in a separate process at the lowest CPU priority
# (nice 19), never in the relay's own process: a busy evening of notifications can't delay a single game packet.
_JWTS, _OPENER = {}, []


def notify_worker_init():
    try:
        os.nice(19)
    except (AttributeError, OSError):
        pass


def vapid_token(d, contact, endpoint, now):
    u = urllib.parse.urlsplit(endpoint)
    aud = '%s://%s' % (u.scheme, u.netloc)
    tok = _JWTS.get((d, aud))
    if tok is None or tok[1] < now + 600:
        exp = int(now) + 12 * 3600
        head = b64u(b'{"typ":"JWT","alg":"ES256"}') + '.' + b64u(json.dumps(
            {'aud': aud, 'exp': exp, 'sub': contact}, separators=(',', ':')).encode())
        tok = _JWTS[(d, aud)] = (head + '.' + b64u(es256(d, head.encode())), exp)
    return tok[0]


def notify_request(job):
    """a job ('webpush', endpoint, p256dh, auth, message, VAPID key, contact, public key) -> (url, body, headers)"""
    _, endpoint, p256dh, auth, msg, d, contact, pub = job
    return endpoint, webpush_encrypt(p256dh, auth, msg), {
        'TTL': str(PUSH_TTL), 'Urgency': 'high', 'Topic': 'live', 'Content-Encoding': 'aes128gcm',
        'Content-Type': 'application/octet-stream',
        'Authorization': 'vapid t=%s, k=%s' % (vapid_token(d, contact, endpoint, time.time()), pub)}


def notify_job(job):
    """one notification, in the notification process: the HTTP status (0: not sent)"""
    try:
        url, body, headers = notify_request(job)
        if not _OPENER:
            _OPENER.append(urllib.request.build_opener(_NoRedirect))
        with _OPENER[0].open(urllib.request.Request(url, data=body, method='POST', headers=headers), timeout=10) as r:
            return r.status
    except urllib.error.HTTPError as e:
        return e.code
    except Exception:
        return 0


class Notifier:
    """hands notifications to the notification process, within limits: at most PUSH_QUEUE_MAX waiting and PUSH_PER_HOUR
    an hour. Anything over is dropped (and logged once an hour), never queued up."""
    def __init__(self, log=print):
        self.log, self.pool, self.waiting = log, None, 0
        self.hour, self.sent, self.dropped = 0.0, 0, 0
        self.run_inline = None                 # (the tests: a function(job) -> status, run at once)

    def room(self):
        now = time.time()
        if now - self.hour >= 3600:
            if self.dropped:
                self.log('notify: %d notifications dropped in the last hour (limits)' % self.dropped)
            self.hour, self.sent, self.dropped = now, 0, 0
        if self.waiting >= PUSH_QUEUE_MAX or self.sent >= PUSH_PER_HOUR:
            self.dropped += 1
            return False
        return True

    def submit(self, job, done):
        if not self.room():
            return False
        self.sent += 1
        if self.run_inline is not None:
            done(self.run_inline(job))
            return True
        try:
            loop = asyncio.get_running_loop()
        except RuntimeError:
            done(notify_job(job))
            return True
        try:
            if self.pool is None:
                import concurrent.futures
                import multiprocessing
                self.pool = concurrent.futures.ProcessPoolExecutor(
                    1, mp_context=multiprocessing.get_context('spawn'), initializer=notify_worker_init)
            fut = loop.run_in_executor(self.pool, notify_job, job)
        except Exception as e:                 # the process died: a new one next time
            self.pool = None
            self.log('notify: %s' % e)
            return False
        self.waiting += 1

        def fin(f):
            self.waiting -= 1
            try:
                st = f.result()
            except Exception:
                st, self.pool = 0, None
            done(st)
        fut.add_done_callback(fin)
        return True


class Push:
    """the devices that turned notifications on (SQLite), and the notifications to them. Which devices want an event is
    chosen by SQLite itself (one query, no per-row Python), and the push services' answers are written once a tick, in
    one transaction: a full table costs the relay's loop a few milliseconds per event."""
    def __init__(self, path, contact, notifier, log=print):
        self.log, self.contact, self.notifier, self.answers = log, contact, notifier, []
        key_path = path + '.vapid'
        if not os.path.exists(key_path):   # the server's VAPID key (made once; fans' subscriptions are tied to it)
            fd = os.open(key_path, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o600)
            with os.fdopen(fd, 'w') as f:
                f.write('%064x\n' % (secrets.randbelow(P256_N - 1) + 1))
        with open(key_path) as f:
            self.d = int(f.read().strip(), 16)
        self.public_key = b64u(p256_public(self.d))
        self.db = sqlite3.connect(path)
        for p in (path, key_path):
            try:
                os.chmod(p, 0o600)
            except OSError:
                pass
        self.db.execute('PRAGMA journal_mode=WAL')     # (a write is cheap: no wait for the disk at each one)
        self.db.execute('PRAGMA synchronous=NORMAL')
        self.db.execute('CREATE TABLE IF NOT EXISTS subs (endpoint TEXT PRIMARY KEY, p256dh TEXT, auth TEXT, '
                        'mode TEXT, gap INTEGER, qa INTEGER, qb INTEGER, tz INTEGER, last_sent REAL DEFAULT 0, '
                        'seen REAL, fails INTEGER DEFAULT 0)')
        self.db.commit()
        self.purge()
        log('push: %d devices (%s)' % (self.count(), os.path.abspath(path)))

    def count(self):
        return self.db.execute('SELECT COUNT(*) FROM subs').fetchone()[0]

    def known(self, endpoint):
        return self.db.execute('SELECT 1 FROM subs WHERE endpoint = ?', (str(endpoint),)).fetchone() is not None

    def purge(self):
        self.db.execute('DELETE FROM subs WHERE seen < ?', (time.time() - PUSH_KEEP_DAYS * 86400,))
        self.db.commit()

    def subscribe(self, sub, prefs):
        """a fan turned notifications on or changed them (the page sends this again each time it is opened)"""
        try:
            endpoint, keys = sub['endpoint'], sub['keys']
            p256dh, auth = b64u_dec(keys['p256dh']), b64u_dec(keys['auth'])
            p256_point(p256dh)
        except (TypeError, KeyError, ValueError, AttributeError):
            return False
        if not push_endpoint_ok(endpoint) or len(auth) != 16:
            return False
        p = push_prefs(prefs)
        row = (b64u(p256dh), b64u(auth), p['when'], p['gap']) + tuple(p['quiet'] or (None, None)) + (p['tz'],)
        now = time.time()
        old = self.db.execute('SELECT p256dh, auth, mode, gap, qa, qb, tz, seen FROM subs WHERE endpoint = ?',
                              (endpoint,)).fetchone()
        if old is not None and tuple(old[:7]) == row and now - (old[7] or 0) < 86400:
            return True                        # nothing new (the page opened again today): no write
        if old is None and self.count() >= PUSH_MAX:
            return False
        self.db.execute('INSERT INTO subs (endpoint, p256dh, auth, mode, gap, qa, qb, tz, seen) '
                        'VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?) ON CONFLICT(endpoint) DO UPDATE SET p256dh = excluded.p256dh, '
                        'auth = excluded.auth, mode = excluded.mode, gap = excluded.gap, qa = excluded.qa, '
                        'qb = excluded.qb, tz = excluded.tz, seen = excluded.seen, fails = 0', (endpoint,) + row + (now,))
        self.db.commit()
        return True

    def unsubscribe(self, endpoint):
        self.db.execute('DELETE FROM subs WHERE endpoint = ?', (str(endpoint),))
        self.db.commit()

    def notify(self, kind, title, body, match_id, now=None):
        """an event ('hosted' or 'match'): a notification to each device that wants it now (its kind of event, its gap
        since the last one, outside its quiet hours: the hour on the phone's clock from its time zone offset)"""
        now = time.time() if now is None else now
        msg = json.dumps({'title': title, 'body': body, 'id': match_id}, separators=(',', ':')).encode()
        rows = self.db.execute(
            'SELECT endpoint, p256dh, auth FROM (SELECT *, ((? + tz) / 60) % 24 AS h FROM subs) '
            "WHERE last_sent <= ? - gap * 60 AND (mode = 'any' OR ? = 'match') "
            'AND (qa IS NULL OR NOT (CASE WHEN qa < qb THEN h >= qa AND h < qb ELSE h >= qa OR h < qb END)) '
            'ORDER BY last_sent LIMIT ?', (int(now // 60), now, kind, PUSH_PER_EVENT)).fetchall()
        sent = []
        for endpoint, p256dh, auth in rows:
            job = ('webpush', endpoint, b64u_dec(p256dh), b64u_dec(auth), msg, self.d, self.contact, self.public_key)
            if not self.notifier.submit(job, lambda st, e=endpoint: self.answers.append((e, st))):
                break
            sent.append((now, endpoint))
        if sent:
            self.db.executemany('UPDATE subs SET last_sent = ? WHERE endpoint = ?', sent)
            self.db.commit()
            self.log('push: %s, %d device%s' % (kind, len(sent), '' if len(sent) == 1 else 's'))
        self.flush()

    def flush(self):
        """the push services' answers so far, in one transaction: gone (404, 410) forgets the device; another refusal
        (4xx, not 429 "too many") counts, PUSH_FAILS in a row forget it; a delivery clears the count"""
        if not self.answers:
            return
        answers, self.answers = self.answers, []
        gone = [(e,) for e, st in answers if st in (404, 410)]
        bad = [(e,) for e, st in answers if 400 <= st < 500 and st not in (404, 410, 429)]
        good = [(e,) for e, st in answers if 200 <= st < 300]
        self.db.executemany('DELETE FROM subs WHERE endpoint = ?', gone)
        self.db.executemany('UPDATE subs SET fails = fails + 1 WHERE endpoint = ?', bad)
        self.db.executemany('UPDATE subs SET fails = 0 WHERE endpoint = ? AND fails > 0', good)
        if bad:
            self.db.execute('DELETE FROM subs WHERE fails >= ?', (PUSH_FAILS,))
        self.db.commit()


REPORT_MAX = 200000
REPORT_RATE = 20             # per address per hour


def wav_ok(data):
    """A plain PCM WAV, mono, 48 kHz, 16-bit (what the game plays)."""
    if len(data) < 44 or data[:4] != b'RIFF' or data[8:12] != b'WAVE':
        return False
    at = 12
    while at + 8 <= len(data):
        cid, size = data[at:at + 4], struct.unpack_from('<I', data, at + 4)[0]
        if cid == b'fmt ' and size >= 16:
            fmt, ch, rate, _, _, bits = struct.unpack_from('<HHIIHH', data, at + 8)
            return fmt == 1 and ch == 1 and rate == 48000 and bits == 16
        at += 8 + size + (size & 1)
    return False


class Sounds:
    """SOUND STUDIO's library: the sounds in --sounds, their catalog and the players' votes (in the statistics file)."""

    def __init__(self, folder, stats, log):
        self.folder, self.stats, self.log = folder, stats, log
        os.makedirs(os.path.join(folder, 'removed'), exist_ok=True)
        self.seen = None
        self.items = []             # (file, size, sha256 hex, data), oldest first
        self.recent = {}            # address -> request times (in memory only)
        if stats is not None:
            stats.db.execute('CREATE TABLE IF NOT EXISTS sound_votes (sound TEXT, key TEXT, vote INTEGER, updated INTEGER, '
                             'PRIMARY KEY (sound, key))')
            stats.db.commit()
        self.scan()

    def scan(self):
        """Re-reads the folder when it has changed: the valid sounds, at most SOUND_PER_EVENT per event and SOUND_MAX."""
        try:
            names = sorted(n for n in os.listdir(self.folder) if os.path.isfile(os.path.join(self.folder, n)))
            seen = tuple((n, os.path.getmtime(os.path.join(self.folder, n)), os.path.getsize(os.path.join(self.folder, n))) for n in names)
        except OSError:
            return
        if seen == self.seen:
            return
        self.seen = seen
        found = []
        for n, mtime, size in seen:
            m = SOUND_FILE.match(n)
            if not m or m.group(1) not in SOUND_EVENTS or size > SOUND_BYTES:
                if not n.startswith('.'):
                    self.log('sounds: %s left out (not Event__name.wav, an unknown event, or too big)' % n)
                continue
            with open(os.path.join(self.folder, n), 'rb') as f:
                data = f.read()
            if not wav_ok(data):
                self.log('sounds: %s left out (not a mono 48 kHz 16-bit WAV)' % n)
                continue
            found.append((mtime, n, data))
        found.sort()
        items, per = [], {}
        for _, n, data in found:
            ev = n.split('__', 1)[0]
            if per.get(ev, 0) >= SOUND_PER_EVENT or len(items) >= SOUND_MAX:
                self.log('sounds: %s left out (the library is full: %d, %d per event)' % (n, SOUND_MAX, SOUND_PER_EVENT))
                continue
            per[ev] = per.get(ev, 0) + 1
            items.append((n, len(data), hashlib.sha256(data).hexdigest(), data))
        self.items = items
        self.log('sounds: %d in the library (%s)' % (len(items), os.path.abspath(self.folder)))

    def find(self, name):
        for it in self.items:
            if it[0] == name:
                return it
        return None

    def my_votes(self, key):
        if self.stats is None or key is None:
            return {}
        return dict(self.stats.db.execute('SELECT sound, vote FROM sound_votes WHERE key = ?', (key,)).fetchall())

    def vote_message(self, c, body):
        if self.stats is None or c.key is None or len(body) < 2:
            return
        n = body[0]
        if len(body) < 2 + n:
            return
        name, vote = body[1:1 + n].decode('ascii', 'replace'), struct.unpack_from('<b', body, 1 + n)[0]
        c.sound_votes += 1
        if c.sound_votes > 400 or vote not in (-1, 0, 1):
            return
        self.scan()
        if self.find(name) is None:
            return
        db = self.stats.db
        if vote == 0:
            db.execute('DELETE FROM sound_votes WHERE sound = ? AND key = ?', (name, c.key))
        else:
            db.execute('INSERT OR REPLACE INTO sound_votes (sound, key, vote, updated) VALUES (?, ?, ?, ?)',
                       (name, c.key, vote, int(time.time())))
        db.commit()

    def totals(self, name):
        if self.stats is None:
            return 0, 0
        up = self.stats.db.execute('SELECT COUNT(*) FROM sound_votes WHERE sound = ? AND vote > 0', (name,)).fetchone()[0]
        down = self.stats.db.execute('SELECT COUNT(*) FROM sound_votes WHERE sound = ? AND vote < 0', (name,)).fetchone()[0]
        return up, down

    async def handle(self, reader, writer, ip):
        now = time.time()
        times = [t for t in self.recent.get(ip, []) if now - t < 60]
        if len(times) >= SOUND_RATE:
            return
        times.append(now)
        self.recent[ip] = times
        if len(self.recent) > 5000:
            self.recent = {a: t for a, t in self.recent.items() if t and now - t[-1] < 60}
        cmd = (await asyncio.wait_for(reader.readexactly(1), 10))[0]
        self.scan()
        if cmd == 1:   # the catalog, with this player's own votes
            raw = await asyncio.wait_for(reader.readexactly(16), 10)
            key = self.stats.ident(raw) if self.stats is not None and raw != bytes(16) else None
            mine = self.my_votes(key)
            out = b'PASND1\n' + b''.join(('%s\t%d\t%s\t%d\n' % (n, size, sha, mine.get(n, 0))).encode('ascii')
                                         for n, size, sha, _ in self.items) + b'END\n'
            writer.write(out)
        elif cmd == 2:   # one sound
            n = (await asyncio.wait_for(reader.readexactly(1), 10))[0]
            name = (await asyncio.wait_for(reader.readexactly(n), 10)).decode('ascii', 'replace')
            it = self.find(name)
            writer.write(struct.pack('<I', 0xFFFFFFFF) if it is None else struct.pack('<I', it[1]) + it[3])


def sound_admin(args, log):
    """The owner's commands on the server: --sound-totals, --sound-remove FILE --reason removed|adopted."""
    stats = Stats(args.stats, lambda m: None)
    lib = Sounds(args.sounds, stats, lambda m: None)
    if args.sound_totals:
        for n, _, _, _ in lib.items:
            up, down = lib.totals(n)
            print('%-48s  up %4d  down %4d' % (n, up, down))
        print('%d sounds in the library' % len(lib.items))
        return 0
    name = args.sound_remove
    if not name.endswith('.wav'):
        name += '.wav'
    src = os.path.join(args.sounds, name)
    if not os.path.isfile(src) or args.reason not in ('removed', 'adopted'):
        print('no such sound (or --reason is not removed / adopted): %s' % name)
        return 1
    up, down = lib.totals(name)
    ev, _, short = name[:-4].partition('__')
    result = {'sound': short.replace('_', ' ').upper(), 'file': name, 'event': ev, 'up': up, 'down': down, 'reason': args.reason,
              'adopted_in': args.adopted_in or '', 'date': time.strftime('%Y-%m-%d')}
    os.replace(src, os.path.join(args.sounds, 'removed', name))
    with open(os.path.join(args.sounds, 'removed', 'results.log'), 'a', encoding='utf-8') as f:
        f.write(json.dumps(result) + '\n')
    print(json.dumps(result))
    return 0


class Reports:
    def __init__(self, folder, log, relay=None, sounds=None):
        self.folder, self.log, self.relay, self.sounds = folder, log, relay, sounds
        self.recent = {}
        os.makedirs(folder, exist_ok=True)

    async def handle(self, reader, writer):
        peer = writer.get_extra_info('peername')
        ip = peer[0] if peer else '?'
        try:
            magic = await asyncio.wait_for(reader.readexactly(7), 10)
            if magic == b'PASND1\n' and self.sounds is not None:   # SOUND STUDIO (0.3.16)
                await self.sounds.handle(reader, writer, ip)
                return
            if magic != b'PABUG1\n':
                return
            n = struct.unpack('<I', await asyncio.wait_for(reader.readexactly(4), 10))[0]
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
                    who = tidy_name(line[10:].split(' (session', 1)[0].encode('ascii', 'replace'))
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
    ap.add_argument('--minimum', default='minimum_version.txt', help='the oldest version allowed online (e.g. 0.3.20): '
                    'older games are told to update; none when the file is missing or empty')
    ap.add_argument('--stats', default='player_stats.db', help='player statistics (SQLite), keyed by each game\'s private key')
    ap.add_argument('--crowd', default='', help='The Crowd: local WebSocket address for the phone web app '
                    '(e.g. 127.0.0.1:47781, behind an HTTPS web server); off when empty')
    ap.add_argument('--crowd-origin', action='append', default=[], help='The Crowd: web page origin allowed to connect '
                    '(e.g. https://crowd.example.com; repeat for more; any when none)')
    ap.add_argument('--push', default='', help='The Crowd: notifications (Web Push); the SQLite file for the devices '
                    'that turned them on (its VAPID key is kept beside it, FILE.vapid)')
    ap.add_argument('--push-contact', default='https://github.com/jendoebelin/ProjectArena-public',
                    help='The Crowd: the contact the browsers\' push services see (a mailto: or https: address)')
    ap.add_argument('--sounds', default='sound_library', help='SOUND STUDIO: the folder of sounds players can try and vote on')
    ap.add_argument('--sound-totals', action='store_true', help='owner: print every library sound\'s votes and exit')
    ap.add_argument('--sound-remove', default='', help='owner: take this sound (its file name) out of the library and exit')
    ap.add_argument('--reason', default='', help='with --sound-remove: removed (the community didn\'t like it) or adopted')
    ap.add_argument('--adopted-in', default='', help='with --sound-remove --reason adopted: the version it is in')
    ap.add_argument('--lag', type=float, default=0.0, help='testing: extra one-way delay per forwarded packet (ms)')
    ap.add_argument('--jitter', type=float, default=0.0, help='testing: up to this much more delay at random (ms)')
    ap.add_argument('--loss', type=float, default=0.0, help='testing: percent of forwarded packets dropped')
    ap.add_argument('--udp-buffer', type=int, default=4 << 20, help='the UDP receive buffer asked for (bytes; the system '
                    'caps it at net.core.rmem_max)')
    args = ap.parse_args()
    loop = asyncio.get_running_loop()
    stamp = lambda m: print(time.strftime('%H:%M:%S ') + m, flush=True)
    if args.sound_totals or args.sound_remove:
        raise SystemExit(sound_admin(args, stamp))
    relay = Relay(args.max_clients, stamp, args.lag, args.jitter, args.loss, Bans(args.bans, stamp), Latest(args.latest),
                  Stats(args.stats, stamp), Latest(args.minimum))
    relay.sounds = Sounds(args.sounds, relay.player_stats, stamp)
    transport, _ = await loop.create_datagram_endpoint(lambda: relay, local_addr=(args.bind, args.port))
    # a bigger receive buffer (2026-11-19: the server's kernel had dropped 1.8 million packets in a month, 12%, for a
    # full one, the default 208 KB, while the relay was busy; none that day): packets wait for the relay, not dropped
    sock = transport.get_extra_info('socket')
    try:
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, args.udp_buffer)
    except OSError as e:
        stamp('UDP receive buffer not changed: %s' % e)
    stamp('Project Arena relay listening on UDP %s:%d (receive buffer %d KB)' %
          (args.bind, args.port, sock.getsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF) // 1024))
    reports = Reports(args.reports, stamp, relay, relay.sounds)
    await asyncio.start_server(reports.handle, args.bind, args.port)
    stamp('bug reports: TCP %s:%d -> %s' % (args.bind, args.port, os.path.abspath(args.reports)))
    crowd = None
    if args.crowd:
        h, _, p = args.crowd.rpartition(':')
        push = None
        if args.push and not HAVE_CRYPTOGRAPHY:   # (the relay runs on without them: the page shows no bell)
            stamp('push: OFF, python3-cryptography is not installed (sudo apt install python3-cryptography)')
        elif args.push:
            try:                                 # (never the game's relay down for the notifications' sake)
                push = Push(args.push, args.push_contact, Notifier(stamp), stamp)
            except (OSError, ValueError, sqlite3.Error) as e:
                stamp('push: OFF, %s' % e)
        crowd = Crowd(relay, stamp, args.crowd_origin, push)
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
        if k % 43200 == 0 and crowd is not None and crowd.push is not None:
            try:                                   # (devices not seen for a year; never the relay down for it)
                crowd.push.purge()
            except sqlite3.Error as e:
                stamp('push: %s' % e)
        if k % 15 == 0 and relay.by_addr:
            stamp('%d online, forwarded %d, dropped %d%s' % (len(relay.by_addr), relay.stats['forwarded'], relay.stats['dropped'],
                                                          ', %d fans' % len(crowd.fans) if crowd is not None and crowd.fans else ''))


if __name__ == '__main__':
    if sys.platform == 'win32':
        # (the tests' relay on Windows: there a game that has closed makes the next receive fail with "connection
        # reset", and Python 3.10's default (proactor) loop then stops receiving for good; the selector loop carries on.
        # The server runs on Linux, where this doesn't happen.)
        asyncio.set_event_loop_policy(asyncio.WindowsSelectorEventLoopPolicy())
    try:
        asyncio.run(main())
    except KeyboardInterrupt:
        pass
