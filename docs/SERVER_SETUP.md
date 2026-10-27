# Running the community server

Project Arena's online play goes through one small program, `server/projectarena_relay.py`. It connects players
(Quest and PC alike) without showing them each other's address, carries voice, and collects in-game bug reports.
It needs Python 3 and one port (47780 by default, UDP for play and TCP for bug reports). A tiny server is plenty.

## Oracle Cloud Always Free (no cost)

1. **Sign up** at cloud.oracle.com ("Start for free"). A card is needed for verification only. Choose a home region
   close to your players; free servers can only be created there.
2. **Optional but recommended:** in Billing, upgrade the account to *Pay As You Go*. Always Free resources stay free,
   and Oracle no longer reclaims a free server for looking idle (a relay with few players can look idle).
3. **Create the server:** Compute > Instances > Create instance.
   - Image: **Canonical Ubuntu 24.04**.
   - Shape: **VM.Standard.A1.Flex** (Ampere, 1 OCPU, 6 GB is plenty), or **VM.Standard.E2.1.Micro** if A1 is out of
     capacity in your region.
   - Networking: keep "Assign a public IPv4 address".
   - SSH keys: let it generate a key pair and **download the private key** (you need it to log in).
4. **Open the port in Oracle's firewall:** on the instance page, open its subnet > the *Default Security List* >
   *Add Ingress Rules*, twice:
   - Source CIDR `0.0.0.0/0`, IP Protocol **UDP**, Destination Port Range `47780`
   - Source CIDR `0.0.0.0/0`, IP Protocol **TCP**, Destination Port Range `47780`
5. **Copy the relay up and install it** (from the repository folder on your PC; `ubuntu` is the default user, use the
   instance's public IP and the key you downloaded):

   ```
   scp -i oracle.key server/projectarena_relay.py server/deploy/setup.sh ubuntu@PUBLIC_IP:~
   ssh -i oracle.key ubuntu@PUBLIC_IP "sudo sh setup.sh"
   ```

   The script installs it as a service (starts on boot, restarts if it stops) and opens the port in the server's own
   firewall too (Oracle's Ubuntu images block everything but SSH until then).
6. **Point builds at it:** put `PROJECTARENA_SERVER=PUBLIC_IP:47780` in `servers.local.cfg` (repo root, not in git)
   and rebuild. Players can also type it in SETTINGS > SERVER.

Check it: `ssh -i oracle.key ubuntu@PUBLIC_IP "journalctl -u projectarena-relay -n 20"` shows players joining and bug
reports arriving. Reports are saved in `/opt/projectarena/bug_reports`; fetch them with
`scp -i oracle.key "ubuntu@PUBLIC_IP:/opt/projectarena/bug_reports/*" .`

Updating: copy the new `projectarena_relay.py` (and `setup.sh`) up and run `sudo sh setup.sh` again; it restarts
the relay, which drops everyone for a few seconds (players reconnect by themselves).
- **Order: the relay first.** New relays accept older games; a new game may send what an older relay refuses (0.3.9's
  match statistics are refused by a 0.3.8 relay). Update the relay before (or as) the new game reaches players, and
  only then set `latest_version.txt`.
- Copy the file by its full path (e.g. `C:/Users/you/ProjectArena/server/projectarena_relay.py`), not a relative one:
  a copy run from another folder can fail and leave an old file in `/tmp` that then gets installed. Before
  installing, compare `sha256sum /tmp/projectarena_relay.py` with the same file on your computer; keep a backup
  (`sudo cp projectarena_relay.py projectarena_relay.py.bak-VERSION`) to put back if it doesn't start.

**Player reports and bans (moderation):**
- Player reports are the `player_report_*.txt` files in `/opt/projectarena/bug_reports`. Each one has the reported
  name, the reason and note, who else was in the room, the game log, and a line
  `reported player on the relay now: NAME address` (their address at the moment of the report).
- Ban someone: `ssh -t -i oracle.key ubuntu@PUBLIC_IP "sudo nano /opt/projectarena/bans.txt"`, add a line with their
  address (`203.0.113.7`) or `name:PLAYERNAME`, save. It takes effect within a few seconds, no restart. Delete the
  line to lift the ban. An address ban is stronger (a name can be changed), but a home address can change too.
- Check the folder regularly (a few times a week) once strangers play together; act on reports quickly.
- After each release, set the current version so older games (0.3.4 on) say UPDATE AVAILABLE:
  `ssh -i oracle.key ubuntu@PUBLIC_IP "echo 0.3.5 | sudo tee /opt/projectarena/latest_version.txt"` (the relay
  re-reads it on its own; no restart).

**Player statistics** (what is collected, the protections and the known gaps: `docs/DATA_AND_SECURITY.md`): the relay keeps every player's career (matches, wins, throws and throws on target, hits,
blocks and balls faced, catches, top throw speed) in `/opt/projectarena/player_stats.db` (SQLite), keyed by a private
random key each game makes for itself. Hosts report each finished match between two people; spectators and players
see the numbers on the screen over the court and the PC spectator's VS screen. A match counts only when the host's
result and the other player's confirmation agree. The table stores a salted hash of each key, never the key: the salt
is `/opt/projectarena/player_stats.db.salt`; both files are readable by the relay's account only (the relay sets
that). The relay logs `the same player is connected twice` when one key connects from two places.
- **Back up the table and its salt together, encrypted** (without the salt a backup can't be matched to players
  again, so keep them as a pair):
  `ssh -i oracle.key ubuntu@PUBLIC_IP "sudo tar czf - -C /opt/projectarena player_stats.db player_stats.db.salt" | gpg --symmetric --output stats-backup.tgz.gpg`
  (gpg asks for a passphrase; keep it with the server's key, never in the repository).
- Look inside: `ssh -t -i oracle.key ubuntu@PUBLIC_IP "sudo sqlite3 /opt/projectarena/player_stats.db 'select name, matches, wins, datetime(last_seen, \"unixepoch\") from players order by wins desc'"`.
- Remove someone's statistics on request (by name): `... sudo sqlite3 /opt/projectarena/player_stats.db "delete from players where name = 'NAME'"`.
- Statistics unseen for 24 months are removed by the relay itself (at start and once a day), as the privacy policy
  promises.
- Keep the server's security updates on (Ubuntu does this by itself with unattended-upgrades; check with
  `systemctl status unattended-upgrades`).

## The Crowd (the fans' web page, optional)

The relay can show live online matches on a phone web page where fans cheer (`server/crowd_web`). It needs a domain
name and HTTPS:
1. At your domain's DNS provider, add an **A record** for a name such as `crowd` pointing at the server's public IP.
2. Oracle: in the instance's subnet > Default Security List, add two **Ingress Rules**: source `0.0.0.0/0`, TCP,
   destination port `80`, and the same for `443`.
3. Copy the files and run the setup (it installs Caddy, which gets and renews the certificate by itself, opens 80
   and 443 in the server's firewall and restarts the relay with the page's address):
   `scp -i oracle.key -r server/deploy/setup_crowd.sh server/crowd_web ubuntu@PUBLIC_IP:` then
   `ssh -i oracle.key ubuntu@PUBLIC_IP "sudo sh setup_crowd.sh crowd.example.com"`.
   Run `setup.sh` from this release first (it adds `--crowd 127.0.0.1:47781` to the relay's service).
4. Open `https://crowd.example.com` on a phone. Updating the page later: run step 3 again.

Fans are anonymous and nothing about them is stored (`docs/DATA_AND_SECURITY.md`). Caddy writes no access log.

## Any other Ubuntu server

Same steps 5 and 6 (Hetzner, DigitalOcean, Vultr, a Raspberry Pi at home with a port forward...). Open UDP and TCP
47780 in the provider's firewall if it has one.

## Testing locally

`python3 server/projectarena_relay.py` on any PC on your network; headsets use that PC's address. Add
`--lag 60 --jitter 20 --loss 1` to play over a simulated long-distance connection.
