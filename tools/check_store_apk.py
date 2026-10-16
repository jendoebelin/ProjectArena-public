"""Check an APK against Meta Horizon Store manifest rules (VRC.Quest.Packaging.1 / .4) before uploading.
Rules from https://developers.meta.com/horizon/resources/publish-mobile-manifest/ (checked October 2026).

    python -I tools/check_store_apk.py build/ProjectArena-release-open.apk [--release]
"""
import os
import re
import subprocess
import sys

AAPT2 = os.path.join(os.environ.get('ANDROID_HOME', r'C:\Android\sdk'), 'build-tools', '34.0.0', 'aapt2.exe')


def main():
    apk = sys.argv[1]
    release = '--release' in sys.argv
    badging = subprocess.run([AAPT2, 'dump', 'badging', apk], capture_output=True, text=True).stdout
    tree = subprocess.run([AAPT2, 'dump', 'xmltree', '--file', 'AndroidManifest.xml', apk], capture_output=True, text=True).stdout
    fails = []

    def need(ok, what):
        print('  %s %s' % ('ok  ' if ok else 'FAIL', what))
        if not ok:
            fails.append(what)

    m = re.search(r"sdkVersion:'(\d+)'", badging)
    t = re.search(r"targetSdkVersion:'(\d+)'", badging)
    c = re.search(r"compileSdkVersion='(\d+)'", badging)
    need(m and 29 <= int(m.group(1)) <= 34, 'minSdkVersion 29..34 (32 recommended): %s' % (m.group(1) if m else '?'))
    need(t and int(t.group(1)) == 34, 'targetSdkVersion 34 (required for apps created after 1 March 2026): %s' % (t.group(1) if t else '?'))
    need(not c or int(c.group(1)) >= int(t.group(1) if t else 0), 'compileSdkVersion absent or >= target: %s' % (c.group(1) if c else 'absent'))
    need(re.search(r'installLocation\(0x010102b7\)=0\b', tree) is not None, 'installLocation auto')
    need(re.search(r"uses-feature: name='android.hardware.vr.headtracking'", badging) is not None and
         re.search(r'android\.hardware\.vr\.headtracking[\s\S]{0,200}required\(0x0101028e\)=true', tree) is not None,
         'uses-feature android.hardware.vr.headtracking required=true version=1')
    need('com.oculus.supportedDevices' in tree, 'meta-data com.oculus.supportedDevices')
    # the store's upload checker: features that need a matching permission (rejected otherwise)
    for feature, perm in (('oculus.software.handtracking', 'com.oculus.permission.HAND_TRACKING'),):
        if feature in tree:
            need(perm in tree, 'uses-feature %s needs uses-permission %s' % (feature, perm))
    need('com.oculus.intent.category.VR' in tree and 'android.intent.action.MAIN' in tree, 'launcher intent: MAIN + com.oculus.intent.category.VR')
    need(re.search(r'excludeFromRecents\(0x01010017\)=true', tree) is not None, 'excludeFromRecents true')
    debuggable = re.search(r'debuggable\(0x0101000f\)=true', tree) is not None
    if release:
        need(not debuggable, 'not debuggable (release)')
    lab = re.search(r"application: label='([^']*)'", badging)
    need(lab is not None and lab.group(1).strip() != '', 'application label: %s' % (lab.group(1) if lab else '?'))
    size = os.path.getsize(apk)
    need(size < 1 << 30, 'APK under 1 GB: %.1f MB' % (size / 1e6))
    print('store manifest check: %s' % ('PASS' if not fails else 'FAIL (%d)' % len(fails)))
    return 1 if fails else 0


if __name__ == '__main__':
    sys.exit(main())
