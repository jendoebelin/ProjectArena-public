"""Release safety check: every asset inside the APK must be byte-identical to a file in the open asset set
(app/assets_open), so nothing extracted from the original game can slip into a shared build.
Usage: python -I tools/check_open_apk.py <apk> <asset_dir>"""
import hashlib
import os
import sys
import zipfile

apk, root = sys.argv[1], sys.argv[2]
bad, n = [], 0
with zipfile.ZipFile(apk) as z:
    for info in z.infolist():
        if not info.filename.startswith('assets/') or info.is_dir():
            continue
        n += 1
        rel = info.filename[len('assets/'):]
        src = os.path.join(root, rel.replace('/', os.sep))
        if not os.path.isfile(src):
            bad.append(rel + ' (not in the open set)')
        elif hashlib.sha256(z.read(info)).digest() != hashlib.sha256(open(src, 'rb').read()).digest():
            bad.append(rel + ' (differs from the open set)')
if bad:
    print('APK asset check FAILED:\n  ' + '\n  '.join(bad[:20]))
    sys.exit(1)
print('APK asset check: %d assets, all from the open set' % n)
