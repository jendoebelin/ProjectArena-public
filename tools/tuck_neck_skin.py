"""THE NECK UNDER THE CHIN (0.3.27; the owner's headset screenshots, 2026-11-27: feminine FACE 3's GOATEE and SHORT
BEARD "still jacked up": bare skin through them under the chin, in pieces).

Every top carries its own neck (its av_skin), rising a little way up inside the head, where the head hides it: the
standard tops' to 7.9 cm in the feminine head's bind space (masculine 9.9), and no further forward than 6.1 cm in
front of the neck's base (masculine 7.6). Three tops' skin also carries a chin's underside from the head they were
modelled on, forward to 12.4 cm (masculine 13.3): RONIN WRAP (16), DRAGON SILK QIPAO (19) and CHERRY POODLE SKIRT (20),
and their country versions (19c, 20c). On a face whose jaw differs (FACE 3's, long and flat) it stood out of the head
under the chin, in the band where the beards lie, and showed through them in patches.

This moves those tops' skin above NECK_FROM (4 cm; below it, their chest) back to the standard neck's front: no point
further forward than the standard tops' necks reach. The rest of each top doesn't move. Writes the models in place
(then their .lod: build/tools/make_lods.exe). Run it again: nothing moves.

Usage: python -I tools/tuck_neck_skin.py
"""
import os
import struct

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, 'app', 'assets_open')
DT = [('p', '<f4', 3), ('n', '<f4', 3), ('u0', '<f4', 2), ('u1', '<f4', 2), ('c', 'u1', 4), ('bi', 'u1', 4), ('bw', 'u1', 4)]
TOPS = ('16', '19', '19c', '20', '20c')
NECK_FROM = 0.040
SKIN = 4   # (the skin's MAT channel)


def read(name):
    d = bytearray(open(os.path.join(ASSETS, name + '.mdl'), 'rb').read())
    ver, nv, ni, ns, nm = struct.unpack_from('<5I', d, 4)
    V = np.frombuffer(bytes(d), dtype=DT, count=nv, offset=24).copy()
    o = 24 + nv * 52
    I = np.frombuffer(bytes(d), '<u4', count=ni, offset=o)
    o += ni * 4
    S = [struct.unpack_from('<3I', d, o + 12 * k) for k in range(ns)]
    o += ns * 12
    chan = [(struct.unpack_from('<I', d, o + 172 * i + 40)[0] >> 8) & 15 for i in range(nm)]
    return d, V, I, S, chan


def skin_of(V, I, S, chan):
    parts = [I[f:f + c] for f, c, m in S if chan[m] == SKIN]
    return np.unique(np.concatenate(parts)) if parts else np.zeros(0, np.int64)


def neck_front(bk):
    """how far forward the standard tops' necks reach (above NECK_FROM): the most forward of tops 0-15 and 17-18"""
    front = 0.0
    for t in [k for k in range(19) if k != 16]:
        name = 'av_top_%d_%s' % (t, bk)
        if not os.path.exists(os.path.join(ASSETS, name + '.mdl')):
            continue
        _, V, I, S, chan = read(name)
        P = V['p'][skin_of(V, I, S, chan)]
        up = P[:, 1] > NECK_FROM
        if up.any():
            front = min(front, float(P[up, 2].min()))
    return front


def main():
    for bk in 'mf':
        zf = neck_front(bk)
        for t in TOPS:
            name = 'av_top_%s_%s' % (t, bk)
            d, V, I, S, chan = read(name)
            skin = skin_of(V, I, S, chan)
            P = V['p'][skin]
            fwd = (P[:, 1] > NECK_FROM) & (P[:, 2] < zf - 1e-5)
            if not fwd.any():
                print('%-14s nothing forward of the standard neck (%.3f)' % (name, zf))
                continue
            back = zf - P[fwd, 2]
            V['p'][skin[fwd], 2] = np.float32(zf)
            d[24:24 + len(V) * 52] = V.tobytes()
            open(os.path.join(ASSETS, name + '.mdl'), 'wb').write(bytes(d))
            print('%-14s %3d skin points moved back to the standard neck\'s front (%.3f), by up to %.1f cm' % (
                name, int(fwd.sum()), zf, back.max() * 100))


if __name__ == '__main__':
    main()
