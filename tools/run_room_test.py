"""Builds and runs the arena room test against a private relay on 127.0.0.1:47781 (always stopped afterwards).
Usage: python -I tools/run_room_test.py"""
import os
import subprocess
import sys
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
b = subprocess.run(['cmd.exe', '/c', os.path.join(ROOT, 'tools', 'run_room_test.bat')], capture_output=True, text=True)
if b.returncode:
    print(b.stdout[-3000:], b.stderr[-2000:])
    sys.exit('build failed')
stats_db = os.path.join(ROOT, 'build', 'tests', 'room_test_stats.db')   # a fresh statistics table each run
if os.path.exists(stats_db):
    os.remove(stats_db)
latest = os.path.join(ROOT, 'build', 'tests', 'latest_version.txt')   # the relay announces a current release
with open(latest, 'w') as f:
    f.write('9.9.9')
relay = subprocess.Popen([sys.executable, '-I', os.path.join(ROOT, 'server', 'projectarena_relay.py'), '--port', '47781',
                          '--bind', '127.0.0.1', '--latest', latest,
                          '--stats', os.path.join(ROOT, 'build', 'tests', 'room_test_stats.db'),
                          '--crowd', '127.0.0.1:47782'], stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
try:
    time.sleep(1.0)
    env = dict(os.environ)
    if '-v' not in sys.argv:
        env['PROJECTARENA_QUIET'] = '1'
    r = subprocess.run([os.path.join(ROOT, 'build', 'tests', 'test_rooms.exe')], env=env, timeout=120)
    code = r.returncode
finally:
    relay.kill()
sys.exit(code)
