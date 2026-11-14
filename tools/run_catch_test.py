"""CATCHES ONLINE, measured: how many of a player's snatches at their own returning ball the host counts as catches,
for the player who isn't hosting, at several round trips, with the host's CATCHES ONLINE rule on and off (the host's
own snatches, with no delay, alongside). Builds app/tests/lag_test.cpp (tools/run_lag_test.bat) and runs it in catch
mode through a private relay on 127.0.0.1:47783 (always stopped afterwards; never the community server).
Usage: python -I tools/run_catch_test.py [seconds per run]   (default 45; PA_SNATCH_AT=1.5: grab earlier, metres from the head;
PA_CASES="20,5 45,5": only those delays, ms each way and jitter)"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
secs = sys.argv[1] if len(sys.argv) > 1 else '45'
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
                             stdout=open(os.path.join(work, 'relay_catch.log'), 'w'), stderr=subprocess.STDOUT)
    try:
        time.sleep(1.0)
        env = dict(os.environ, PROJECTARENA_QUIET='1', PA_LATE_CATCH='1' if rule else '0')
        r = subprocess.run([os.path.join(ROOT, 'build', 'tests', 'lag_test.exe'), secs, '127.0.0.1:47783', '0', '1'],
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
print('%-34s %10s %16s %16s %12s' % ('', 'ping, ms', 'player caught', 'host caught', 'self-hits'))
for lag, jit in cases:
    for rule in (False, True):
        o = run(lag, jit, rule)
        name = '%d ms each way, CATCHES ONLINE %s' % (lag, 'on' if rule else 'off')
        print('%-34s %10s %16s %16s %12s' % (name, o.get('ping_ms', '-'), o.get('catch_player', '-'),
                                              o.get('catch_host', '-'), o.get('selfhit_player', '-')), flush=True)
