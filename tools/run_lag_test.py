"""Long-distance play, measured: builds app/tests/lag_test.cpp and runs it through a private relay on 127.0.0.1:47783
(always stopped afterwards; never the community server), once without delay and once like a far-away connection.
Usage: python -I tools/run_lag_test.py [seconds] [one-way lag ms] [jitter ms]
(default: 90 s; 105 ms each way + 0..15 ms, with this PC's own ~30 ms: the 245-260 ms round trip a headset
measured in a match across the world, 2026-11-04)"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
secs = sys.argv[1] if len(sys.argv) > 1 else '90'
lag = sys.argv[2] if len(sys.argv) > 2 else '105'
jitter = sys.argv[3] if len(sys.argv) > 3 else '15'
b = subprocess.run(['cmd.exe', '/c', os.path.join(ROOT, 'tools', 'run_lag_test.bat')], capture_output=True, text=True)
if b.returncode:
    print(b.stdout[-3000:], b.stderr[-2000:])
    sys.exit('build failed')
work = os.path.join(ROOT, 'build', 'tests', 'lag')
os.makedirs(work, exist_ok=True)


def run(label, lag_ms, jitter_ms, ahead):
    log = os.path.join(work, 'relay_%s.log' % label)
    relay = subprocess.Popen([sys.executable, '-I', os.path.join(ROOT, 'server', 'projectarena_relay.py'), '--port', '47783',
                              '--bind', '127.0.0.1', '--lag', str(lag_ms), '--jitter', str(jitter_ms),
                              '--stats', os.path.join(work, 'stats.db'), '--latest', os.path.join(work, 'latest.txt'),
                              '--bans', os.path.join(work, 'bans.txt'), '--reports', os.path.join(work, 'reports')],
                             stdout=open(log, 'w'), stderr=subprocess.STDOUT)
    try:
        time.sleep(1.0)
        env = dict(os.environ, PROJECTARENA_QUIET='1')
        print('[%s: %s ms each way + 0..%s ms, BALL LOOK-AHEAD %s]' % (label, lag_ms, jitter_ms, 'on' if ahead else 'off'))
        r = subprocess.run([os.path.join(ROOT, 'build', 'tests', 'lag_test.exe'), secs, '127.0.0.1:47783', '1' if ahead else '0'],
                           cwd=ROOT, env=env,
                           capture_output=True, text=True, timeout=float(secs) + 120)
    finally:
        relay.kill()
        relay.wait()
    out = {}
    for line in r.stdout.splitlines():
        if line.startswith('RESULT '):
            k, v = line[7:].split(' ', 1)
            out[k] = v
        else:
            print(line)
    if r.returncode:
        print(r.stderr[-2000:])
        sys.exit('%s: the test failed' % label)
    return out


runs = [('no delay', run('nolag', 0, 0, False)), ('far, as today', run('far_off', lag, jitter, False)),
        ('far, look-ahead', run('far_on', lag, jitter, True))]
rows = [('round trip (ping), ms', 'ping_ms'),
        ('host: trigger -> ball in my hand, ms', 'host_grab_ms'),
        ('host: let go -> my ball flies, ms', 'host_leave_ms'),
        ('player: trigger -> ball in my hand, ms', 'player_grab_ms'),
        ('player: let go -> my ball flies, ms', 'player_leave_ms'),
        ('player: ... 90th percentile, ms', 'player_leave_p90_ms'),
        ('player: host\'s ball shown late by, ms', 'incoming_late_ms'),
        ('player: ... 90th percentile, ms', 'incoming_late_p90_ms'),
        ('player: a block (at your end) lands off by, ms', 'block_error_ms'),
        ('player: own throw, biggest glide, cm', 'own_glide_cm'),
        ('player: ... 90th percentile, cm', 'own_glide_p90_cm'),
        ('player: network + look-ahead per frame, us', 'exchange_us'),
        ('player sees host\'s ball: off its path, 99%, cm', 'wobble0_p99_cm'),
        ('... jumps over 25 cm', 'jumps0'),
        ('player sees own ball: off its path, 99%, cm', 'wobble1_p99_cm'),
        ('... jumps over 25 cm', 'jumps1'),
        ('player' + chr(39) + 's blocks the host counted', 'player_blocks'),
        ('ball speed, m/s', 'ball_speed'),
        ('throws (host, player)', 'throws'),
        ('throws that never showed (host, player)', 'missed')]
print()
print('%-42s' % '' + ''.join('%17s' % n for n, _ in runs))
for name, k in rows:
    print('%-42s' % name + ''.join('%17s' % r.get(k, '-') for _, r in runs))
