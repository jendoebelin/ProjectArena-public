#!/bin/sh
# Installs the Project Arena community relay on an Ubuntu server (Oracle Cloud, Hetzner, DigitalOcean, ...).
# Run on the server, from the folder holding projectarena_relay.py:   sudo sh setup.sh [port]
# It installs Python if needed, a service user, a systemd service (starts on boot, restarts on failure), and opens
# the port (UDP for play, TCP for bug reports) in the server's own firewall. Your cloud provider's firewall
# (Oracle: the VCN security list) must allow the same port too: see docs/SERVER_SETUP.md.
set -e
PORT="${1:-47780}"
HERE="$(cd "$(dirname "$0")" && pwd)"
RELAY="$HERE/projectarena_relay.py"
[ -f "$RELAY" ] || RELAY="$HERE/../projectarena_relay.py"
[ -f "$RELAY" ] || { echo "projectarena_relay.py not found next to this script"; exit 1; }
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }

command -v python3 >/dev/null || { apt-get update && apt-get install -y python3; }
id projectarena >/dev/null 2>&1 || useradd --system --home /opt/projectarena --shell /usr/sbin/nologin projectarena
mkdir -p /opt/projectarena/bug_reports
install -m 0644 "$RELAY" /opt/projectarena/projectarena_relay.py
# moderation: one ban per line (an IP address, or name:PLAYERNAME); the relay re-reads it when it changes
[ -f /opt/projectarena/bans.txt ] || echo '# Project Arena bans: an IP address or name:PLAYERNAME per line' > /opt/projectarena/bans.txt
chown -R projectarena:projectarena /opt/projectarena

cat > /etc/systemd/system/projectarena-relay.service <<EOF
[Unit]
Description=Project Arena community relay
After=network-online.target
Wants=network-online.target

[Service]
User=projectarena
WorkingDirectory=/opt/projectarena
# The Crowd's web page address (setup_crowd.sh writes it)
EnvironmentFile=-/opt/projectarena/crowd.env
ExecStart=/usr/bin/python3 -I /opt/projectarena/projectarena_relay.py --port $PORT --reports /opt/projectarena/bug_reports --bans /opt/projectarena/bans.txt --stats /opt/projectarena/player_stats.db --crowd 127.0.0.1:47781 --crowd-origin=${CROWD_ORIGIN}
Restart=always
RestartSec=3
NoNewPrivileges=true
ProtectSystem=strict
ProtectHome=true
ReadWritePaths=/opt/projectarena
PrivateTmp=true

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable projectarena-relay
systemctl restart projectarena-relay   # also picks up a new projectarena_relay.py when updating

# the server's own firewall (Oracle's Ubuntu images ship iptables rules that block everything but SSH)
if command -v iptables >/dev/null; then
  for proto in udp tcp; do
    iptables -C INPUT -p $proto --dport "$PORT" -j ACCEPT 2>/dev/null || iptables -I INPUT 5 -p $proto --dport "$PORT" -j ACCEPT
  done
  if command -v netfilter-persistent >/dev/null; then netfilter-persistent save; else
    DEBIAN_FRONTEND=noninteractive apt-get install -y iptables-persistent && netfilter-persistent save; fi
fi
command -v ufw >/dev/null && ufw status | grep -q active && { ufw allow "$PORT"/udp; ufw allow "$PORT"/tcp; }

sleep 1
systemctl --no-pager status projectarena-relay | head -n 5
echo
echo "Relay running on port $PORT (UDP play, TCP bug reports). Reports: /opt/projectarena/bug_reports"
echo "Bans: sudo nano /opt/projectarena/bans.txt (applied within a few seconds, no restart needed)"
echo "Logs: journalctl -u projectarena-relay -f"
