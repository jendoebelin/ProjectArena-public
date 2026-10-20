# Data and security

Everything Project Arena and the official community server handle, where it is kept, who can see it, how long it is
kept, and what protects it. The player-facing version is `docs/PRIVACY.md`; keep the two in step (the release
checklist, `docs/RELEASE_CHECKLIST.md`, says when). Last reviewed: 20 October 2026 (version 0.3.6).

## 1. Inventory

| Data | Where | Who sees it | Kept | Notes |
|---|---|---|---|---|
| Player name | The device (`player_name.txt`); sent to everyone in a room or watching; the server log | Other players, spectators, the server operator | On the device until uninstalled; server log lines are rotated by the system journal | Chosen by the player; A-Z, 0-9, space, `-`, `.` only |
| Avatar (look, COUNTRY) | The device (`avatar.cfg`); sent to other players | Other players, spectators | On the device | Cosmetic; the country is only a choice of kit, not a location |
| Settings, camera positions | The device (`settings.cfg`, `warps.cfg`) | Nobody else | On the device | |
| Diagnostic log | The device (`projectarena_log.txt`) | Nobody else, unless in a report | On the device (rotated) | |
| Player key (16 random bytes) | The device (`stats_key.bin`); sent to the official server at each connection | Nobody (the server keeps only a salted hash) | On the device until uninstalled | Ties statistics to a game install, not to a person or account |
| Career statistics | The official server (`player_stats.db`, SQLite) | **Public in the game**: other players and spectators (screen over the court, the PC VS screen); the player (MY STATS); the operator | Until the player deletes them (MY STATS > DELETE MY STATS), or 24 months after last seen (removed automatically) | Matches, wins, throws, throws on target, hits, blocks, balls faced, catches, top speed, the name last used, first and last seen |
| Bug reports | The official server (`bug_reports/`); waits on the device (`bug_outbox.txt`) until the server can take it | The operator | Up to 12 months (the operator deletes older ones) | Only when the player sends one: note, settings, recent log, the sending address |
| Player reports | The official server (`bug_reports/player_report_*`) | The operator | Up to 12 months | Only when a player sends one: names in the room, reason, note, the reported player's current address |
| Ban list | The official server (`bans.txt`) | The operator | Until lifted | Addresses and names |
| Votes to remove (REPORT PLAYER > VOTE TO REMOVE) | The arena host's game, in memory only | The host's game; a notice to the host | Until the host leaves its arena (never written anywhere) | Who voted against whom; a removed player's session, virtual address and name, so the host can turn them away for the rest of that session |
| IP addresses | Seen by the server for every connection | The operator (never shown to players) | Not stored, except inside reports and the ban list | Players see only virtual addresses |
| Voice | Forwarded live between players in a room | The people in that room | **Never stored** | Push-to-talk by default; mutes for spectators, players, everyone, one person |
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
