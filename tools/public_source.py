"""The public form of app/src, for the release builds (the owner, 2026-11-18: "clean it first"; the 0.3.21 store build
carried the private cross-play code: the other game's name in 28 of its internal names and three lines of text, and
this computer's folder path). Every file of app/src is copied to DEST but the cross-play files (make_public_repo's
EXCLUDE), its C and C++ files with their PRIVATE blocks left out and PUBLIC ones put in (make_public_repo.public_text):
the source the public repository has and builds. Run after tools/write_servers_header.py (its header is copied too).

Usage: python -I tools/public_source.py DEST
"""
import fnmatch
import os
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXCLUDE = []   # (this repository's source is its public form already: copied as it is)


def public_text(text, c='#'):
    return text

SRC = os.path.join(ROOT, 'app', 'src')
CODE = ('.c', '.cpp', '.h', '.hpp', '.inc')


def main():
    if len(sys.argv) != 2:
        raise SystemExit(__doc__)
    dest = os.path.abspath(sys.argv[1])
    if os.path.commonpath([dest, SRC]) == SRC:
        raise SystemExit('the public copy goes outside app/src')
    if os.path.exists(dest):
        shutil.rmtree(dest)
    files = left = changed = 0
    for dp, _, names in os.walk(SRC):
        for name in names:
            src = os.path.join(dp, name)
            rel = os.path.relpath(src, ROOT).replace('\\', '/')
            if any(fnmatch.fnmatch(rel, pat) for pat in EXCLUDE):
                left += 1
                continue
            out = os.path.join(dest, os.path.relpath(src, SRC))
            os.makedirs(os.path.dirname(out), exist_ok=True)
            if name.endswith(CODE):
                text = open(src, 'rb').read().decode('utf-8')
                if 'PRIVATE {' in text or 'PUBLIC {' in text:
                    text = public_text(text, '//')
                    changed += 1
                open(out, 'wb').write(text.encode('utf-8'))
            else:
                shutil.copyfile(src, out)
            files += 1
    print('public source: %d files in %s (%d with private parts taken out, %d left out)' % (files, dest, changed, left))


if __name__ == '__main__':
    main()
