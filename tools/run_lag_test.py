"""Long-distance play, measured (and BALL LOOK-AHEAD near and far): builds app/tests/lag_test.cpp and runs it through a private relay on 127.0.0.1:47783
(always stopped afterwards; never the community server), once without delay and once like a far-away connection.
Usage: python -I tools/run_lag_test.py [--four] [seconds] [one-way lag ms] [jitter ms]
(default: 90 s; 105 ms each way + 0..15 ms, with this PC's own ~30 ms: the 245-260 ms round trip a headset
measured in a match across the world, 2026-11-04)
--four: FOUR CORNERS, free-for-all: the host and three players, all of them far from it, each throwing at the next
corner round; measured from the first player's game (its grab and let-go times from all three)
--blitz: the same in FIREBALL BLITZ, each throwing at the next one round still in; the fireballs measured"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
blitz = '--blitz' in sys.argv[1:]   # (FIREBALL BLITZ: the same four, in the octagon; the fireballs measured)
four = '--four' in sys.argv[1:] or blitz
args = [a for a in sys.argv[1:] if a not in ('--four', '--blitz')]
secs = args[0] if len(args) > 0 else '90'
lag = args[1] if len(args) > 1 else '105'
jitter = args[2] if len(args) > 2 else '15'
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
        r = subprocess.run([os.path.join(ROOT, 'build', 'tests', 'lag_test.exe'), secs, '127.0.0.1:47783', '1' if ahead else '0']
                           + (['4'] if blitz else ['3'] if four else []),
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


runs = [('no delay', run('nolag', 0, 0, False)), ('no delay, ahead', run('nolag_on', 0, 0, True)),
        ('far, as today', run('far_off', lag, jitter, False)), ('far, look-ahead', run('far_on', lag, jitter, True))]
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
        ('own throw: the host launches it later, ms', 'own_host_later_ms'),
        ('own throw: host' + chr(39) + 's start off by, cm', 'own_start_cm'),
        ('own throw: host' + chr(39) + 's speed minus mine, m/s', 'own_speed_ms'),
        ('own throw: host' + chr(39) + 's way off by, deg', 'own_dir_deg'),
        ('own throw: the two apart, 0.5 s into each flight, cm', 'own_apart_cm'),
        ('player: network + look-ahead per frame, us', 'exchange_us'),
        ('player sees host\'s ball: off its real path, 99%, cm', 'offpath0_p99_cm'),
        ('... the most, cm', 'offpath0_max_cm'),
        ('player sees own ball: off its real path, 99%, cm', 'offpath1_p99_cm'),
        ('... the most, cm', 'offpath1_max_cm'),
        ('player sees host\'s ball: off its path, 99%, cm', 'wobble0_p99_cm'),
        ('... jumps over 25 cm', 'jumps0'),
        ('player sees own ball: off its path, 99%, cm', 'wobble1_p99_cm'),
        ('... jumps over 25 cm', 'jumps1'),
        ('player' + chr(39) + 's blocks the host counted', 'player_blocks'),
        ('ball speed, m/s', 'ball_speed'),
        ('throws (host, player)', 'throws'),
        ('throws that never showed (host, player)', 'missed')]
if four:   # (the others' balls, not only the host's; the three players' blocks and the balls that hit them)
    rows = [(n.replace('host\'s ball', 'others\' balls').replace('(host, player)', '(host, the three)')
             .replace('player' + chr(39) + 's blocks', 'the players' + chr(39) + ' blocks'), k) for n, k in rows
            if not k.startswith('own_') or k.startswith('own_glide')]
    rows.insert(rows.index(('the players' + chr(39) + ' blocks the host counted', 'player_blocks')) + 1,
                ('balls that hit the players', 'player_hit'))
if blitz:
    rows = [(n.replace('balls', 'fireballs').replace('own ball', 'own fireballs'), k) for n, k in rows]
    rows += [('knockouts', 'knockouts'), ('matches played', 'matches')]
print()
print('%-42s' % '' + ''.join('%17s' % n for n, _ in runs))
for name, k in rows:
    print('%-42s' % name + ''.join('%17s' % r.get(k, '-') for _, r in runs))
