"""Copy an aapt2-linked APK and add lib/<abi>/*.so entries STORED (uncompressed), as required when
android:extractNativeLibs="false". Usage: python -I add_libs.py <in.apk> <dir with lib/> <out.apk>"""
import os, sys, zipfile

src, libroot, dst = sys.argv[1:4]
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(dst, 'w') as zout:
    for info in zin.infolist():
        zout.writestr(info, zin.read(info.filename))
    for dp, _, files in os.walk(os.path.join(libroot, 'lib')):
        for f in files:
            full = os.path.join(dp, f)
            arc = os.path.relpath(full, libroot).replace('\\', '/')
            zi = zipfile.ZipInfo(arc, date_time=(2026, 1, 1, 0, 0, 0))
            zi.compress_type = zipfile.ZIP_STORED
            zout.writestr(zi, open(full, 'rb').read())
            print('added', arc, os.path.getsize(full))
