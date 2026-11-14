"""Release safety check: every asset inside the APK must be byte-identical to a file in the open asset set
(app/assets_open), or to the licensed music (app/assets_music/assets: Suno, used under its terms), so nothing
extracted from the original game can slip into a shared build.
Usage: python -I tools/check_open_apk.py <apk> <asset_dir> [<music_dir>]"""
import hashlib
import os
import sys
import zipfile

apk, roots = sys.argv[1], sys.argv[2:]
bad, n = [], 0
with zipfile.ZipFile(apk) as z:
    for info in z.infolist():
        if not info.filename.startswith('assets/') or info.is_dir():
            continue
        n += 1
        rel = info.filename[len('assets/'):]
        srcs = [os.path.join(r, rel.replace('/', os.sep)) for r in roots]
        src = next((s for s in srcs if os.path.isfile(s)), srcs[0])
        if not os.path.isfile(src):
            bad.append(rel + ' (not in the open set)')
        elif hashlib.sha256(z.read(info)).digest() != hashlib.sha256(open(src, 'rb').read()).digest():
            bad.append(rel + ' (differs from the open set)')
if bad:
    print('APK asset check FAILED:\n  ' + '\n  '.join(bad[:20]))
    sys.exit(1)
print('APK asset check: %d assets, all from the open set or the licensed music' % n)
