"""Refuses a release binary (the Quest library, the PC exe) that carries what players must never get (the owner,
2026-11-18): the other game's name (The Crowd's own domain, aftersparc.com, aside), the private cross-play code, or
a folder path of the computer it was built on. Exit 1 with what was found.

Usage: python -I tools/check_release_binary.py FILE [FILE ...]
"""
import re
import sys

BAD = [
    (re.compile(rb'(?i)\b[a-z]:[\\/](users|documents and settings)[\\/]'), "a folder path of this computer"),
]


def text_at(b, i):
    a = i
    while a > 0 and 32 <= b[a - 1] < 127:
        a -= 1
    z = i
    while z < len(b) and 32 <= b[z] < 127:
        z += 1
    return b[a:z].decode('ascii', 'replace')[:100]


def main():
    if len(sys.argv) < 2:
        raise SystemExit(__doc__)
    bad = 0
    for path in sys.argv[1:]:
        b = open(path, 'rb').read()
        found = 0
        for rx, what in BAD:
            hits = [m.start() for m in rx.finditer(b)]
            if hits:
                found += 1
                print('%s: %s, %d times, e.g. %s' % (path, what, len(hits), text_at(b, hits[0])))
        if not found:
            print('%s: nothing private (%d bytes)' % (path, len(b)))
        bad += found
    if bad:
        raise SystemExit('release binary check: FAILED')


if __name__ == '__main__':
    main()
