"""Watches the community server from this PC and says when something happens to it (2026-11-20).

Run every 10 minutes by Windows Task Scheduler (while the PC is on and you're signed in), it checks:
  - over SSH, read-only: whether the relay runs, when it last started, why it stopped (the service's journal since the
    last check), when the server itself last started, and the relay file's hash;
  - from the internet, as a game would: the game port answers (SOUND STUDIO's catalog over TCP; the server logs
    nothing for it).
and tells you, once per change:
  RESTARTED   the relay started again since the last check: after a crash (its error, if it left one), an update to
              the relay, a stop and start by hand, or the server itself restarting
  DOWN        the relay isn't running (and BACK UP when it is again)
  UNREACHABLE neither SSH nor the game port answers, twice in a row (about 20 minutes)
  NOT ANSWERING the relay runs, but the game port doesn't answer from outside, twice in a row (a firewall or network)
by a Windows notification, a line in <out>/watch_log.txt, and a message to the Discord channel whose webhook is the
`server-alerts=` line of discord.local.cfg, if there is one (a private channel: never a public one). No message ever
holds the server's address. A restart while the PC was off is reported at the next check.

  python -I tools/watch_server.py [--out DIR] [--cfg FILE] [--servers FILE] [--discord FILE]
  python -I tools/watch_server.py --test     a test notification (and Discord message), nothing checked
The server and key come from sounds.local.cfg (ssh=, key=), the game port's address from servers.local.cfg
(PROJECTARENA_SERVER=host:port); none of the three files is in git. Windows (notifications); Python 3.7 or newer."""
import argparse
import datetime
import json
import os
import re
import shutil
import socket
import subprocess
import sys
import time
import urllib.request
from xml.sax.saxutils import escape

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
DEFAULT_OUT = os.path.join(os.path.expanduser('~'), 'ProjectArena-server-watch')
NO_WINDOW = 0x08000000   # (CREATE_NO_WINDOW: pythonw from Task Scheduler, no console windows)
IP = re.compile(r'\b\d{1,3}(?:\.\d{1,3}){3}\b')

# on the server (as the SSH user; journalctl reads the service's journal): key=value lines, then the journal's lines
# about the service's life since the last check
REMOTE = r'''systemctl show projectarena-relay -p ActiveState -p SubState -p NRestarts -p ExecMainCode -p ExecMainStatus
echo START=$(date -d "$(systemctl show projectarena-relay -p ActiveEnterTimestamp --value)" +%s 2>/dev/null)
echo BOOT=$(date -d "$(uptime -s)" +%s)
echo RELAY=$(sha256sum /opt/projectarena/projectarena_relay.py | cut -c1-16)
echo NOW=$(date +%s)
echo JOURNAL
journalctl -u projectarena-relay --since @SINCE -o cat --no-pager 2>/dev/null | grep -E "Main process exited|Stopping|Stopped|Started|Failed|Traceback|Error|error|Killed|oom|Out of memory|listening" | tail -40
'''

TOAST = r'''
$ErrorActionPreference = 'Stop'
[Windows.UI.Notifications.ToastNotificationManager, Windows.UI.Notifications, ContentType = WindowsRuntime] | Out-Null
[Windows.Data.Xml.Dom.XmlDocument, Windows.Data.Xml.Dom.XmlDocument, ContentType = WindowsRuntime] | Out-Null
$x = New-Object Windows.Data.Xml.Dom.XmlDocument
$x.LoadXml($env:PA_TOAST_XML)
$n = [Windows.UI.Notifications.ToastNotification]::new($x)
[Windows.UI.Notifications.ToastNotificationManager]::CreateToastNotifier('{1AC14E77-02E7-4E5D-B744-2EB1AE5198B7}\WindowsPowerShell\v1.0\powershell.exe').Show($n)
'''


def read_cfg(path):
    cfg = {}
    if path and os.path.isfile(path):
        with open(path, encoding='utf-8') as f:
            for line in f:
                if '=' in line and not line.lstrip().startswith('#'):
                    k, v = line.split('=', 1)
                    cfg[k.strip()] = v.strip()
    return cfg


def when(epoch):
    return datetime.datetime.fromtimestamp(epoch).strftime('%H:%M on %b %d').replace(' 0', ' ')


# ---------------------------------------------------------------- the checks
def ssh_status(cfg, since):
    """(True, {key: value, 'journal': [lines]}) or (False, reason)."""
    ssh = shutil.which('ssh') or r'C:\Windows\System32\OpenSSH\ssh.exe'
    try:
        r = subprocess.run([ssh, '-o', 'BatchMode=yes', '-o', 'ConnectTimeout=20', '-i', cfg['key'], cfg['ssh'],
                            'sh -s'], input=REMOTE.replace('SINCE', str(int(since))).encode(), stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE, timeout=90, creationflags=NO_WINDOW)
    except (OSError, subprocess.TimeoutExpired) as e:
        return False, type(e).__name__
    if r.returncode != 0 and b'ActiveState=' not in r.stdout:
        return False, 'ssh exit %d' % r.returncode
    info, journal, in_journal = {}, [], False
    for line in r.stdout.decode(errors='replace').splitlines():
        if line == 'JOURNAL':
            in_journal = True
        elif in_journal:
            journal.append(IP.sub('x.x.x.x', line.strip()))
        elif '=' in line:
            k, v = line.split('=', 1)
            info[k] = v.strip()
    info['journal'] = journal
    return True, info


def port_answers(hostport):
    """Asks the relay for SOUND STUDIO's catalog over TCP, as a game does; True when it answers."""
    try:
        host, port = hostport.rsplit(':', 1)
        with socket.create_connection((host, int(port)), timeout=10) as s:
            s.settimeout(10)
            s.sendall(b'PASND1\n' + bytes([1]) + bytes(16))
            got = b''
            while len(got) < 7:
                b = s.recv(7 - len(got))
                if not b:
                    break
                got += b
            return got == b'PASND1\n'
    except (OSError, ValueError):
        return False


def int_or(v, d=0):
    try:
        return int(v)
    except (TypeError, ValueError):
        return d


def why(info, st):
    """Why the relay started again, from its journal and the server's own start time."""
    j = info.get('journal', [])
    text = '\n'.join(j)
    boot, relay = int_or(info.get('BOOT')), info.get('RELAY', '')
    if st.get('boot') and boot and boot != st['boot']:
        return 'the server itself restarted (at %s)' % when(boot)
    exits = [l for l in j if 'Main process exited' in l]
    bad = [l for l in exits if 'status=0/SUCCESS' not in l and 'code=exited, status=0' not in l]
    if bad or re.search(r'oom|Out of memory|Killed', text):
        err = [l for l in j if l.startswith('Traceback') or 'Error' in l or 'error' in l]
        what = 'ran out of memory' if re.search(r'oom|Out of memory', text) else bad[-1].split('Main process exited, ')[-1] if bad else 'killed'
        return 'after a crash (%s)%s' % (what, ('; its last error: %s' % err[-1][:160]) if err else '')
    if st.get('relay') and relay and relay != st['relay']:
        return 'after an update to the relay'
    if re.search(r'Stopping|Stopped', text):
        return 'a stop and start (by hand or by an update)'
    return 'the reason is not in its journal'


# ---------------------------------------------------------------- telling the owner
def toast(msg):
    xml = ('<toast><visual><binding template="ToastGeneric"><text>Project Arena server</text><text>%s</text>'
           '</binding></visual></toast>' % escape(msg))
    try:
        subprocess.run(['powershell', '-NoProfile', '-NonInteractive', '-WindowStyle', 'Hidden', '-Command', TOAST],
                       env=dict(os.environ, PA_TOAST_XML=xml), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL,
                       timeout=30, creationflags=NO_WINDOW)
        return True
    except (OSError, subprocess.TimeoutExpired):
        return False


def discord(url, msg):
    if not url:
        return None
    body = json.dumps({'content': '**Project Arena server:** ' + msg, 'username': 'Server watch',
                       'allowed_mentions': {'parse': []}}).encode()
    req = urllib.request.Request(url, data=body, method='POST',
                                 headers={'Content-Type': 'application/json', 'User-Agent': 'ProjectArenaServerWatch/1.0'})
    try:
        with urllib.request.urlopen(req, timeout=15) as r:
            return 200 <= r.status < 300
    except OSError:
        return False


def log(out, line):
    with open(os.path.join(out, 'watch_log.txt'), 'a', encoding='utf-8') as f:
        f.write('%s %s\n' % (datetime.datetime.now().strftime('%Y-%m-%d %H:%M'), line))


def tell(args, msg):
    if args.quiet:   # (tests: the log only)
        log(args.out, 'ALERT %s [quiet: not shown or sent]' % msg)
        return
    t = toast(msg)
    d = discord(read_cfg(args.discord).get('server-alerts', ''), msg)
    log(args.out, 'ALERT %s [notification %s, Discord %s]' % (msg, 'shown' if t else 'FAILED',
                                                              'not set up' if d is None else 'sent' if d else 'FAILED'))


# ---------------------------------------------------------------- one check
def check(args):
    path = os.path.join(args.out, 'state.json')
    try:
        with open(path, encoding='utf-8') as f:
            st = json.load(f)
    except (OSError, ValueError):
        st = {}
    now = time.time()
    cfg = read_cfg(args.cfg)
    if not cfg.get('ssh') or not cfg.get('key'):
        log(args.out, 'FAILED: %s needs ssh= and key= lines' % args.cfg)
        return 1
    since = st.get('last_check', now - 900) - 60
    ok, info = ssh_status(cfg, since)
    hostport = read_cfg(args.servers).get('PROJECTARENA_SERVER', '')
    answers = port_answers(hostport) if hostport else None
    alerts = []
    if ok:
        st['ssh_fail'] = 0
        state = '%s/%s' % (info.get('ActiveState', '?'), info.get('SubState', '?'))
        if info.get('ActiveState') != 'active':
            if not st.get('down'):
                alerts.append('DOWN: the relay is not running (%s)' % state)
                st['down'] = True
        else:
            start = int_or(info.get('START'))
            if st.get('down'):
                alerts.append('BACK UP: the relay is running again (since %s)' % when(start))
                st['down'] = False
            elif st.get('start') and start and start != st['start']:
                alerts.append('RESTARTED at %s: %s' % (when(start), why(info, st)))
            st['start'] = start or st.get('start')
        st['boot'] = int_or(info.get('BOOT')) or st.get('boot')
        st['relay'] = info.get('RELAY') or st.get('relay')
    else:
        st['ssh_fail'] = st.get('ssh_fail', 0) + 1
    if answers is False:
        st['port_fail'] = st.get('port_fail', 0) + 1
    elif answers:
        st['port_fail'] = 0
    if st.get('port_fail', 0) >= 2 and not st.get('unreachable'):
        alerts.append('UNREACHABLE: the server answers neither SSH nor the game port (for about 20 minutes)'
                      if not ok else 'NOT ANSWERING: the relay runs, but the game port does not answer from the '
                      'internet (for about 20 minutes): a firewall or network problem?')
        st['unreachable'] = True
    elif st.get('unreachable') and answers:
        alerts.append('BACK UP: the game port answers again')
        st['unreachable'] = False
    for a in alerts:
        tell(args, a)
    if not alerts and now - st.get('last_ok_log', 0) > 6 * 3600:   # (a line every six hours, so the log shows it runs)
        log(args.out, 'ok: relay %s, game port %s' % (
            'running since %s' % when(st['start']) if ok and not st.get('down') and st.get('start') else
            'NOT RUNNING' if ok else 'not checked (SSH failed %d times)' % st.get('ssh_fail', 0),
            'answers' if answers else 'not checked' if answers is None else 'NO ANSWER'))
        st['last_ok_log'] = now
    st['last_check'] = now
    with open(path + '.tmp', 'w', encoding='utf-8') as f:
        json.dump(st, f, indent=1)
    os.replace(path + '.tmp', path)
    return 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--out', default=DEFAULT_OUT)
    ap.add_argument('--cfg', default=os.path.join(ROOT, 'sounds.local.cfg'))
    ap.add_argument('--servers', default=os.path.join(ROOT, 'servers.local.cfg'))
    ap.add_argument('--discord', default=os.path.join(ROOT, 'discord.local.cfg'))
    ap.add_argument('--test', action='store_true')
    ap.add_argument('--quiet', action='store_true', help='alerts to the log only (for trying it out)')
    args = ap.parse_args()
    os.makedirs(args.out, exist_ok=True)
    if args.test:
        tell(args, 'TEST: this is how the server watch tells you something happened (nothing is wrong)')
        return 0
    return check(args)


if __name__ == '__main__':
    sys.exit(main())
