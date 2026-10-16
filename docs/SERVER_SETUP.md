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

Updating: copy the new `projectarena_relay.py` up and run `sudo sh setup.sh` again.

## Any other Ubuntu server

Same steps 5 and 6 (Hetzner, DigitalOcean, Vultr, a Raspberry Pi at home with a port forward...). Open UDP and TCP
47780 in the provider's firewall if it has one.

## Testing locally

`python3 server/projectarena_relay.py` on any PC on your network; headsets use that PC's address. Add
`--lag 60 --jitter 20 --loss 1` to play over a simulated long-distance connection.
