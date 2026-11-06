"""Builds app/tests/studio_test.cpp and runs it against a private relay (127.0.0.1:47785) serving a copy of
build/sound_library (made with tools/add_sound.py). Usage: python -I tools/run_studio_test.py"""
import os
import shutil
import subprocess
import sys
import tempfile
import time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
work = tempfile.mkdtemp()
lib = os.path.join(work, 'sound_library')
os.makedirs(os.path.join(lib, 'removed'))
src = os.path.join(ROOT, 'build', 'sound_library')
names = sorted(f for f in os.listdir(src) if f.endswith('_test.wav')) if os.path.isdir(src) else []   # (the four test sounds only)
if len(names) != 4:
    sys.exit('needs build/sound_library with the four test sounds (tools/add_sound.py)')
for n in names:
    shutil.copy(os.path.join(src, n), lib)
# the sound slots: the game's table (app/src/sound_studio.cpp) says what the asset catalog does (tools/gen_catalog.py)
import re
cat = re.findall(r"\('(S\d+)', '([A-Za-z]+)'", open(os.path.join(ROOT, 'tools', 'gen_catalog.py'), encoding='utf-8').read())
game = re.findall(r'\{"([A-Za-z]+)", "(S\d+)", "[^"]*"\}', open(os.path.join(ROOT, 'app', 'src', 'sound_studio.cpp'), encoding='utf-8').read())
events = [ln.split()[0] for ln in open(os.path.join(ROOT, 'app', 'assets_open', 'audio', 'events.txt'), encoding='utf-8') if ln.strip()]
ok = sorted(cat) == sorted((c, e) for e, c in game) and sorted(e for _, e in cat) == sorted(events)
print('  %s   the sound slots S01-S%02d agree: the asset catalog, the game and events.txt' % ('ok' if ok else 'FAIL', len(cat)))
if not ok:
    sys.exit(1)
exe = os.path.join(ROOT, 'build', 'tests', 'studio_test.exe')
bat = os.path.join(work, 'b.bat')
srcs = ' '.join('"%s"' % os.path.join(ROOT, 'app', *p) for p in (('tests', 'studio_test.cpp'), ('src', 'sound_studio.cpp'), ('src', 'net.cpp'),
                                                                   ('src', 'game.cpp'), ('src', 'avatar.cpp'), ('src', 'log.cpp')))
with open(bat, 'w') as f:
    f.write('@call "C:\\Program Files\\Microsoft Visual Studio\\2022\\Community\\VC\\Auxiliary\\Build\\vcvars64.bat" >nul\n'
            '@cd /d "%s"\n@cl /nologo /std:c++17 /O2 /EHsc /W3 /D_WINSOCK_DEPRECATED_NO_WARNINGS /D_CRT_SECURE_NO_WARNINGS /Fe"%s" %s ws2_32.lib || exit /b 1\n'
            % (work, exe, srcs))
b = subprocess.run(['cmd.exe', '/c', bat], capture_output=True, text=True)
if b.returncode:
    print(b.stdout[-3000:], b.stderr[-1000:])
    sys.exit('build failed')
relay = subprocess.Popen([sys.executable, '-I', os.path.join(ROOT, 'server', 'projectarena_relay.py'), '--port', '47785', '--bind', '127.0.0.1',
                          '--stats', os.path.join(work, 'stats.db'), '--latest', os.path.join(work, 'latest.txt'),
                          '--bans', os.path.join(work, 'bans.txt'), '--reports', os.path.join(work, 'reports'), '--sounds', lib],
                         stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
try:
    time.sleep(1.0)
    r = subprocess.run([exe, '127.0.0.1:47785', os.path.join(work, 'headset_library'), lib], timeout=120)
finally:
    relay.kill()
    relay.wait()
sys.exit(r.returncode)
