# Data and security

Everything Project Arena and the official community server handle, where it is kept, who can see it, how long it is
kept, and what protects it. The player-facing version is `docs/PRIVACY.md`; keep the two in step (the release
checklist, `docs/RELEASE_CHECKLIST.md`, says when). Last reviewed: 29 October 2026 (version 0.3.11: the game's version in the server log; earlier, 0.3.10: match
recordings, MATCH MY LEVEL).

## 1. Inventory

| Data | Where | Who sees it | Kept | Notes |
|---|---|---|---|---|
| Player name | The device (`player_name.txt`); sent to everyone in a room or watching; the server log | Other players, spectators, the server operator | On the device until uninstalled; server log lines are rotated by the system journal | Chosen by the player; A-Z, 0-9, space, `-`, `.` only |
| Game version (0.3.11+) | Sent to the official server at each connection; its log | The operator | Server log lines are rotated by the system journal | The version number only (e.g. 0.3.11), written next to the player name when they join: to see who needs an update |
| Avatar (look, COUNTRY) | The device (`avatar.cfg`); sent to other players | Other players, spectators | On the device | Cosmetic; the country is only a choice of kit, not a location |
| Settings, camera positions | The device (`settings.cfg`, `warps.cfg`) | Nobody else | On the device | Includes FIELD OF VIEW (0.3.11) and the EXPERIMENTAL choice |
| Diagnostic log | The device (`projectarena_log.txt`) | Nobody else, unless in a report | On the device (rotated) | |
| Match recordings (off unless turned on: MY STATS > RECORD MY MATCHES) | The device (`recordings/match_*.par`, `app/src/record.h`) | Nobody else, unless the player sends one | The newest 20 (older ones deleted) | The player's own head and hand movement, both balls, the game's events, frame by frame; never another person's movement (a bot's, yes). For tuning the practice bot (`tools/analyze_play.py`, `docs/BOT_TUNING.md`) |
| MATCH MY LEVEL | The device (`settings.cfg`: botmatch) | Nobody else | On the device | One number: the practice bot's level |
| Player key (16 random bytes) | The device (`stats_key.bin`); sent to the official server at each connection | Nobody (the server keeps only a salted hash) | On the device until uninstalled | Ties statistics to a game install, not to a person or account |
| Career statistics | The official server (`player_stats.db`, SQLite) | **Public in the game**: other players and spectators (screen over the court, the PC VS screen); the player (MY STATS); the operator | Until the player deletes them (MY STATS > DELETE MY STATS), or 24 months after last seen (removed automatically) | Matches, wins, throws, throws on target, hits, blocks, balls faced, catches, top speed, the name last used, first and last seen; from 0.3.8 also per kind of match (`player_modes`: standard, HOTDOG, custom), deleted and purged with the player's row; from 0.3.9 also hits from returned balls (`return_hits`) |
| TRAINING bests | The device (`training.cfg`); each lesson's best sent to the official server (`player_stats.db`, table `training`, under the key's salted hash) | **Public in the game** (the leaderboards: the top ten and the asker's own place); the operator | On the device until uninstalled; on the server until the player deletes their statistics (MY STATS > DELETE MY STATS, which deletes these too), or 24 months after the last improvement | From 0.3.12. Per passed lesson: the best time, the best accuracy (successes / attempts), the name last used, when last improved. Implausible results are refused and logged with the name |
| Bug reports | The official server (`bug_reports/`); waits on the device (`bug_outbox.txt`) until the server can take it | The operator | Up to 12 months (the operator deletes older ones) | Only when the player sends one: note, settings, recent log, the sending address |
| Player reports | The official server (`bug_reports/player_report_*`) | The operator | Up to 12 months | Only when a player sends one: names in the room, reason, note, the reported player's current address |
| Ban list | The official server (`bans.txt`) | The operator | Until lifted | Addresses and names |
| Votes to remove (REPORT PLAYER > VOTE TO REMOVE) | The arena host's game, in memory only | The host's game; a notice to the host | Until the host leaves its arena (never written anywhere) | Who voted against whom; a removed player's session, virtual address and name, so the host can turn them away for the rest of that session |
| IP addresses | Seen by the server for every connection | The operator (never shown to players) | Not stored, except inside reports and the ban list | Players see only virtual addresses |
| Voice | Forwarded live between players in a room | The people in that room | **Never stored** | Push-to-talk by default; mutes for spectators, players, everyone, one person |
| Live match (The Crowd, 0.3.7+) | Sent by an online host to the official server every second; shown on the public web page crowd.aftersparc.com | **Anyone** with the page | In memory only, while the arena is open | The host's name, the tank players' names, countries or end colours, score, rules, clock |
| Fans' cheers, waved flags, fan count | The official server, in memory; counts sent to the arena host, which passes them to its room and spectators | The players and spectators of that arena (counts only) | Never stored | Fans are anonymous (no sign-in, name or account); the fan score stays on the phone (browser storage) |
| Fans' IP addresses | Seen by the web server (Caddy) and the relay for each connection | Nobody | Not stored or logged (no access log; used in memory to cap 10 connections per address) | |
| Passthrough camera images, room depth | The headset only | Nobody | Never stored or sent | The game never receives camera images |

Nothing else is collected: no accounts, no email, no real names, no Meta account data, no location, no analytics or
advertising identifiers.

## 2. Protections in place

**Network (relay)**
- Each connection gets a random 16-byte session token bound to its address; packets with the wrong token are dropped.
- Players never see each other's IP addresses (virtual addresses only).
- Only game packets up to 1200 bytes are forwarded; per-client rate limit (1000 packets/s); a client cap.
- Names are sanitised; versions are kept apart.
- Bans by address or name, re-read within seconds.

**Statistics (from 0.3.6)**
- The server never stores a player's key, only HMAC-SHA256(salt, key). The 32-byte salt is a separate file
  (`player_stats.db.salt`) made once by the server; without it the hashes can't be matched to anything.
- The table and the salt are created readable by the relay's own account only (mode 600); the relay runs as the
  unprivileged `projectarena` user (`server/deploy/setup.sh`).
- A match counts only when the host's result and the other player's confirmation agree on the winner and the score
  within 60 seconds; the host can't confirm its own result; disagreeing results count for nobody; numbers that can't
  be real (more on target than thrown, a speed over 216 km/h, ...) are refused; one result per host every 30 s;
  matches against the practice bot never count.
- Stats requests are limited to two a second per player.
- DELETE (MY STATS > DELETE MY STATS) erases the player's row at once. Rows unseen for 24 months are removed by the
  relay at start and once a day.
- Abuse checks: first and last seen are kept; the relay logs `the same player is connected twice` when one key is
  connected from two places at once.
- Tested by `server/test_relay_stats.py` (15 checks) and end to end in `app/tests/test_rooms.cpp`.

**In the game**
- A one-time notice before the first online arena room says statistics are kept, public, and deletable (MY STATS).
- VOTE TO REMOVE needs more than half of the other people in the room and at least two votes; the host can't be
  removed; it lasts for that host's session only (tested in `app/tests/test_rooms.cpp`).

**Server and operator**
- SSH by key only; the relay as an unprivileged service; the cloud firewall opens only UDP/TCP 47780 and SSH.
- Ubuntu's unattended security updates (check: `systemctl status unattended-upgrades`).
- Backups of the statistics table and its salt are taken together and encrypted (`docs/SERVER_SETUP.md`).
- `.gitignore` keeps statistics tables, salts, keystores and local server settings out of the repository.

**The Crowd (web)**
- HTTPS only (Caddy, automatic Let's Encrypt certificates, HSTS); the relay's WebSocket port listens on 127.0.0.1
  only, behind Caddy.
- The relay accepts the live connection only from The Crowd's own page (Origin check), at most 2000 fans and 10 per
  address, 20 messages a second per fan (cheers counted at most 8 a second), messages of at most 512 bytes, idle
  connections closed after 90 s.
- Strict Content-Security-Policy (the page loads only its own files; no third-party scripts, fonts or trackers),
  no cookies, no access log.
- Fans see a random match id, never a host's address (virtual or real).

## 3. Known gaps (accepted for now)

- **No transport encryption.** Game, voice and the player key travel over plain UDP. Someone on the network path
  could read them; with a captured key they could pose as that player for statistics only. The fix (each game signs
  with its own private key; the server keeps the public half) is planned before statistics matter competitively
  (leaderboards, rankings).
- **Host-authoritative matches.** The host simulates the match; a modified host could cheat inside a match. The
  confirmation stops a host from inventing results the opponent didn't see, not from playing unfairly.
- **A reinstall starts new statistics** (a new key); old rows stay until deleted on request or after 24 months.
- **Voice moderation** is reports, mutes and bans; there is no automatic filtering.

## 4. Operator procedures

- **Deletion request by email** (vrsportsfan@gmail.com): delete by name (`docs/SERVER_SETUP.md`); reply when done.
  Bug and player reports mentioning the person can be deleted too.
- **Reports:** check a few times a week; delete reports older than 12 months.
- **Suspected abuse:** look for `connected twice` and repeated joins in `journalctl -u projectarena-relay`; ban by
  address or name in `bans.txt`.
- **A leak or break-in:** take the relay down (`systemctl stop projectarena-relay`); replace the salt (all existing
  statistics become unmatched: delete the table too), rotate the server's SSH key, rebuild the server from
  `server/deploy/setup.sh`; tell players in the release notes and the store listing what happened and what was
  exposed (names and game statistics; no passwords, emails or payment data exist).
- **Changing what is collected:** update `docs/PRIVACY.md`, this file, the store data-use answers and the release
  notes in the same release (`docs/RELEASE_CHECKLIST.md`).
