"""BOT ROOM (owner, 2026-11-12): a private relay on this PC and a room where two bot-moved players play, for testing
the queue from a headset. Builds app/tests/bot_room.cpp, starts server/projectarena_relay.py on this PC (port 47790,
reachable on the local network; its files in build/tests/bot_room_relay; the SOUND STUDIO library from
build/sound_library), then the two players. Never the community server. Stops the relay when the players stop.
On the headset: SETTINGS > SERVER this PC's address and port 47790, then MULTIPLAYER: BOT ONE's room.
Usage: python -I tools/run_bot_room.py [--minutes 60] [--level 3] [--port 47790]"""
import argparse
import os
import socket
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ap = argparse.ArgumentParser()
ap.add_argument('--minutes', type=float, default=60)
ap.add_argument('--level', type=int, default=3, help='the bots\' level, 0 (ROOKIE) to 5')
ap.add_argument('--port', type=int, default=47790)
args = ap.parse_args()

b = subprocess.run(['cmd.exe', '/c', os.path.join(ROOT, 'tools', 'run_bot_room.bat')], capture_output=True, text=True)
if b.returncode:
    print(b.stdout[-3000:], b.stderr[-2000:])
    sys.exit('build failed')
work = os.path.join(ROOT, 'build', 'tests', 'bot_room_relay')
os.makedirs(work, exist_ok=True)
with open(os.path.join(work, 'latest_version.txt'), 'w') as f:
    f.write('0.0.0')   # (no UPDATE AVAILABLE notices)
probe = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
try:
    probe.bind(('0.0.0.0', args.port)); probe.close(); busy = False
except OSError:
    busy = True   # (a private relay already runs on this port: the players use it)
relay = None if busy else subprocess.Popen(
    [sys.executable, '-I', os.path.join(ROOT, 'server', 'projectarena_relay.py'), '--port', str(args.port),
     '--bind', '0.0.0.0', '--latest', os.path.join(work, 'latest_version.txt'),
     '--stats', os.path.join(work, 'stats.db'), '--reports', os.path.join(work, 'reports'),
     '--bans', os.path.join(work, 'bans.txt'), '--sounds', os.path.join(ROOT, 'build', 'sound_library')],
    stdout=open(os.path.join(work, 'relay.log'), 'w'), stderr=subprocess.STDOUT, cwd=work)
print('relay: %s, port %d' % ('the one already running on this PC' if busy else 'started on this PC', args.port), flush=True)
try:
    r = subprocess.run([os.path.join(ROOT, 'build', 'tests', 'bot_room.exe'), '127.0.0.1:%d' % args.port,
                        str(args.minutes * 60), str(args.level),
                        os.path.join(ROOT, 'app', 'assets_open', 'arena.col')])
    code = r.returncode
except KeyboardInterrupt:
    code = 0
finally:
    if relay:
        relay.kill()
        relay.wait()
        print('relay: stopped', flush=True)
sys.exit(code)
