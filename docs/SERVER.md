# Running an Project Arena community server

The community server is one small Python program, `server/projectarena_relay.py` (MIT, no dependencies). It works like
a shared LAN for everyone connected to it: players see each other's hosted games in the menu, join, and spectate,
exactly as on home Wi-Fi. Players never see each other's IP addresses; the server only forwards game packets.

## What you need
- Any machine with Python 3.8+ and a public IP: a small VPS (1 CPU, 512 MB RAM is plenty) or a home PC with a
  forwarded port.
- UDP port **47780** open to the internet.
- Bandwidth: about 90 packets/s each way per player (~40 KB/s per player in a match).

## Quick start

    python3 server/projectarena_relay.py --port 47780 --stats player_stats.db

Self-tests (run a local copy and fake players): `python3 server/test_relay.py`, `python3 server/test_relay_stats.py`,
`python3 server/test_relay_moderation.py`.

## Run it as a service (Linux, systemd)

    sudo useradd --system --no-create-home projectarena
    sudo cp server/projectarena_relay.py /opt/projectarena_relay.py
    sudo install -d -m 700 -o projectarena -g projectarena /opt/projectarena   # its statistics (private)
    sudo tee /etc/systemd/system/projectarena-relay.service >/dev/null <<'UNIT'
    [Unit]
    Description=Project Arena community relay
    After=network-online.target
    [Service]
    User=projectarena
    ExecStart=/usr/bin/python3 /opt/projectarena_relay.py --port 47780 --max-clients 200 --stats /opt/projectarena/player_stats.db
    Restart=always
    [Install]
    WantedBy=multi-user.target
    UNIT
    sudo systemctl enable --now projectarena-relay
    sudo ufw allow 47780/udp        # if you use ufw

Give players the address, e.g. `play.yourdomain.net:47780` (a DNS name is easier to type than an IP).

## How players connect
In the headset: **MENU → SETTINGS → SERVER**, type the address on the keyboard, **DONE**. Then **MENU →
ONLINE: OFF (LAN)** switches to **ONLINE: ON**; games hosted on the server appear in the list. **HOST GAME**, joining
and **SPECTATE GAMES** work as on a LAN. Switch back to LAN play with the same button.

## Safety and limits
- Each player gets a random session token bound to their address; forged or foreign packets are dropped.
- Only game packets (max 1200 bytes) are forwarded; each player is rate-limited (1000 packets/s).
- Players on a different game version do not see each other's games (no confusing half-working matches).
- Names are limited to A-Z, 0-9, space, '-' and '.'. The server logs only joins and leaves (no packet contents).
- `--max-clients` caps load. There are no accounts; matches are host-authoritative, so a modified host could cheat.
  Voice is forwarded live inside a room and never stored.

## Player statistics (from 0.3.6)

With `--stats FILE` the server keeps each player's career (matches, wins, throws and throws on target, hits, blocks,
balls faced, catches, top speed) in a SQLite file, and the game shows them on the screen over the court, in the PC
spectator's VS screen and in MENU > MY STATS.
- The game sends a private random key once per connection; the server stores only HMAC-SHA256(salt, key). The salt
  is `FILE.salt`, made by the server; the table and the salt are made readable by the server's account only.
- The host reports a finished match between two people; it counts only when the other player's game confirms the
  same winner and score within 60 s. Impossible numbers are refused; one result per host every 30 s.
- Players can delete their own row (MY STATS > DELETE MY STATS); rows unseen for 24 months are removed at start and
  once a day. One key connected from two places is logged.
- Back up the table and the salt together, encrypted (`docs/SERVER_SETUP.md`). If you run a public server, publish a
  privacy policy that says this (the official one: `docs/PRIVACY.md`).
- The full data list and the known gaps: `docs/DATA_AND_SECURITY.md`.

## Reports and moderation
- **REPORT BUG** and **REPORT PLAYER** in the game send text reports over TCP to the same port; they are saved in
  `--reports` (player reports as `player_report_*.txt`). A player report names the reported player, the reason, the
  reporter's note, everyone who was in the room and the game's recent log; the server adds the address the reported
  name is connected from, which only you (the operator) see.
- **Bans:** `--bans bans.txt` (one per line: an IP address, or `name:PLAYERNAME`; `#` starts a comment). The file is
  re-read within a few seconds of a change: banned players are disconnected and see BANNED when they try again.
- Players can also mute one person for themselves (REPORT PLAYER > MUTE) or whole groups (SETTINGS > VOICE AND MUTES).
