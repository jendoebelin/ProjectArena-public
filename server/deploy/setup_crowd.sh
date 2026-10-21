#!/bin/sh
# Installs The Crowd (the phone web app) next to the Project Arena relay on an Ubuntu server.
# Run on the server, from the folder holding this script and the crowd_web folder:   sudo sh setup_crowd.sh DOMAIN
# DOMAIN (for example crowd.example.com) must already point at this server (a DNS A record), and the cloud firewall
# (Oracle: the VCN security list) must allow TCP 80 and 443. Caddy gets and renews the HTTPS certificate by itself.
# Caddy serves the web app and passes /ws (the live connection) to the relay's local WebSocket port 47781; the relay
# must run with --crowd 127.0.0.1:47781 (setup.sh adds it). Caddy keeps no access log (none is configured).
set -e
DOMAIN="$1"
[ -n "$DOMAIN" ] || { echo "usage: sudo sh setup_crowd.sh crowd.example.com"; exit 1; }
HERE="$(cd "$(dirname "$0")" && pwd)"
WEB="$HERE/crowd_web"
[ -d "$WEB" ] || WEB="$HERE/../crowd_web"
[ -f "$WEB/index.html" ] || { echo "crowd_web/index.html not found next to this script"; exit 1; }
[ "$(id -u)" = 0 ] || { echo "run with sudo"; exit 1; }

command -v caddy >/dev/null || { apt-get update && apt-get install -y caddy; }
mkdir -p /var/www/projectarena-crowd
rm -rf /var/www/projectarena-crowd/*
cp -R "$WEB"/. /var/www/projectarena-crowd/
chmod -R a+rX /var/www/projectarena-crowd

cat > /etc/caddy/Caddyfile <<EOF
# The Crowd (Project Arena): written by setup_crowd.sh
$DOMAIN {
	encode gzip
	@live path /ws
	reverse_proxy @live 127.0.0.1:47781
	root * /var/www/projectarena-crowd
	file_server
	header {
		Strict-Transport-Security "max-age=31536000"
		X-Content-Type-Options nosniff
		Referrer-Policy no-referrer
		Permissions-Policy "camera=(), microphone=(), geolocation=()"
		Content-Security-Policy "default-src 'self'; connect-src 'self' wss://$DOMAIN; img-src 'self' data:; style-src 'self' 'unsafe-inline'; frame-ancestors 'none'"
		-Server
	}
	@html path / /index.html /sw.js
	header @html Cache-Control "no-cache"
}
EOF
# the relay accepts the live connection only from this page (setup.sh's service reads this file)
echo "CROWD_ORIGIN=https://$DOMAIN" > /opt/projectarena/crowd.env
systemctl restart projectarena-relay 2>/dev/null || echo "run setup.sh too (the relay)"
systemctl enable caddy
systemctl reload caddy 2>/dev/null || systemctl restart caddy

# the server's own firewall (Oracle's Ubuntu images block everything but SSH)
if command -v iptables >/dev/null; then
  for p in 80 443; do
    iptables -C INPUT -p tcp --dport $p -j ACCEPT 2>/dev/null || iptables -I INPUT 5 -p tcp --dport $p -j ACCEPT
  done
  if command -v netfilter-persistent >/dev/null; then netfilter-persistent save; else
    DEBIAN_FRONTEND=noninteractive apt-get install -y iptables-persistent && netfilter-persistent save; fi
fi
command -v ufw >/dev/null && ufw status | grep -q active && { ufw allow 80/tcp; ufw allow 443/tcp; }

sleep 2
systemctl --no-pager status caddy | head -n 5
echo
echo "The Crowd: https://$DOMAIN (the certificate can take a minute the first time; journalctl -u caddy -f)"
