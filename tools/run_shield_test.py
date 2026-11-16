"""A SHIELD HELD LOW, online (a player's report, 11-11: "a shield held low in front didn't deflect the ball; always got
hit"): the player who isn't hosting holds their charged ball low with the shield out to the side and swings it into
the host's low throws where their own game shows the ball, PA_REACT seconds before it arrives (default 0.15: a late
block); the host counts blocks and hits. At several round trips. Builds app/tests/lag_test.cpp (tools/run_lag_test.bat)
and runs it in shield mode through a private relay on 127.0.0.1:47783 (always stopped afterwards; never the community
server). Offline the same hold blocks every ball (app/tests/test_game.cpp's low-shield probe).
Each case runs with the host's BLOCKS ONLINE rule off and on (Game::lateBlock).
Usage: python -I tools/run_shield_test.py [seconds per run]   (default 60; PA_CASES="20,5 45,5": ms each way and jitter;
PA_REACT=0.08: how late the block is, s)"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
secs = sys.argv[1] if len(sys.argv) > 1 else '60'
b = subprocess.run(['cmd.exe', '/c', os.path.join(ROOT, 'tools', 'run_lag_test.bat')], capture_output=True, text=True)
if b.returncode:
    print(b.stdout[-3000:], b.stderr[-2000:])
    sys.exit('build failed')
work = os.path.join(ROOT, 'build', 'tests', 'lag')
os.makedirs(work, exist_ok=True)


def run(lag_ms, jitter_ms, rule):
    relay = subprocess.Popen([sys.executable, '-I', os.path.join(ROOT, 'server', 'projectarena_relay.py'), '--port', '47783',
                              '--bind', '127.0.0.1', '--lag', str(lag_ms), '--jitter', str(jitter_ms),
                              '--stats', os.path.join(work, 'stats.db'), '--latest', os.path.join(work, 'latest.txt'),
                              '--bans', os.path.join(work, 'bans.txt'), '--reports', os.path.join(work, 'reports')],
                             stdout=open(os.path.join(work, 'relay_shield.log'), 'w'), stderr=subprocess.STDOUT)
    try:
        time.sleep(1.0)
        env = dict(os.environ, PROJECTARENA_QUIET='1', PA_LATE_BLOCK='1' if rule else '0')
        r = subprocess.run([os.path.join(ROOT, 'build', 'tests', 'lag_test.exe'), secs, '127.0.0.1:47783', '0', '2'],
                           cwd=ROOT, env=env, capture_output=True, text=True, timeout=float(secs) + 120)
    finally:
        relay.kill()
        relay.wait()
    out = {}
    for line in r.stdout.splitlines():
        if line.startswith('RESULT '):
            k, v = line[7:].split(' ', 1)
            out[k] = v
    if r.returncode:
        print(r.stdout[-1500:], r.stderr[-1500:])
        sys.exit('the test failed')
    return out


cases = [(0, 0), (20, 5), (45, 5), (105, 15)]   # each way (+ jitter), with this PC's own ~30 ms round trip on top
if os.environ.get('PA_CASES'):
    cases = [tuple(int(x) for x in c.split(',')) for c in os.environ['PA_CASES'].split()]
print('%-40s %10s %10s %10s %10s' % ('', 'ping, ms', 'incoming', 'blocked', 'hit'))
for lag, jit in cases:
    for rule in (False, True):
        o = run(lag, jit, rule)
        name = '%d ms each way, BLOCKS ONLINE %s' % (lag, 'on' if rule else 'off')
        print('%-40s %10s %10s %10s %10s' % (name, o.get('ping_ms', '-'), o.get('shield_incoming', '-'),
                                             o.get('shield_blocks', '-'), o.get('shield_hits', '-')), flush=True)
    if os.environ.get('PA_VERBOSE'):
        print(o)
