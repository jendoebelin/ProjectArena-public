"""The CHERRY POODLE SKIRT's belt fitted to the waist (0.3.27; the owner's screenshots, 2026-11-27: "the waist is not
fitting the torso of the masculine or feminine form").

Usage: python -I tools/fit_poodle_waist.py      (then tools/build_country_kits.py, and the .lod files: tools/make_lods)

As imported from the owner's outfit kit (contrib/outfit/cherry-poodle-skirt), the belt was a cone standing off the
waist, wider at its lower edge: at the masculine front 130-148 mm from the centre line over a blouse at 110-122 and
the skirt's top at 118-121; at the feminine back 26 mm clear of the blouse. A hoop round a narrower waist. Now each
part of it, by direction about the waist (24) and height (5 mm steps), sits 4 mm outside what is under it there (the
blouse or the skirt, whichever is further out), its own thickness kept; its buckle and buttons move with it. The
blouse and the skirt don't move. Run it again and nothing moves (already fitted). Writes av_top_20_m / _f in place.
"""
import os
import struct

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, 'app', 'assets_open')
DT = [('p', '<f4', 3), ('n', '<f4', 3), ('u0', '<f4', 2), ('u1', '<f4', 2), ('c', 'u1', 4), ('bi', 'u1', 4), ('bw', 'u1', 4)]
BINS, STEP, GAP = 24, 0.005, 0.004
BELT_BELOW = -0.55   # (the belt: the dark cloth above this, in the head's metres under the neck; below it the petticoat)


def load(name):
    d = bytearray(open(os.path.join(ASSETS, name + '.mdl'), 'rb').read())
    ver, nv, ni, ns, nm = struct.unpack_from('<5I', d, 4)
    V = np.frombuffer(bytes(d), dtype=DT, count=nv, offset=24).copy()
    I = np.frombuffer(bytes(d), '<u4', count=ni, offset=24 + nv * 52)
    S = [struct.unpack_from('<3I', d, 24 + nv * 52 + ni * 4 + 12 * k) for k in range(ns)]
    mstart = len(d) - nm * 172
    tints = [struct.unpack_from('<4f', d, mstart + 172 * i + 4) for i in range(nm)]
    chan = [(struct.unpack_from('<I', d, mstart + 172 * i + 40)[0] >> 8) & 15 for i in range(nm)]
    verts = {}
    for f, c, m in S:
        verts.setdefault(m, set()).update(I[f:f + c].tolist())
    return d, V, {m: np.array(sorted(v)) for m, v in verts.items()}, tints, chan


def fit(sex):
    d, V, M, tints, chan = load('av_top_20_%s' % sex)
    P = V['p'].astype(np.float64)
    dark = [m for m in M if max(tints[m][:3]) < 0.06 and (P[M[m], 1] > BELT_BELOW).any()]
    assert len(dark) == 1, dark
    belt = M[dark[0]][P[M[dark[0]], 1] > BELT_BELOW]
    lo, hi = P[belt, 1].min(), P[belt, 1].max()
    # what lies under it: the blouse and the skirt (not the skin, the belt, or the small light parts on it)
    small = [m for m in M if m != dark[0] and chan[m] != 4 and len(M[m]) < 200]   # (the buckle and buttons)
    under = np.concatenate([P[M[m]] for m in M if m != dark[0] and chan[m] != 4 and m not in small], 0)
    under = under[(under[:, 1] > lo - STEP) & (under[:, 1] < hi + STEP)]
    cx = 0.5 * (under[:, 0].min() + under[:, 0].max())
    cz = 0.5 * (under[:, 2].min() + under[:, 2].max())
    ny = int(np.ceil((hi - lo) / STEP)) + 1

    def cell(Q):
        a = ((np.arctan2(Q[:, 0] - cx, Q[:, 2] - cz) / (2 * np.pi) + 0.5) * BINS).astype(int) % BINS
        h = np.clip(((Q[:, 1] - lo) / STEP).astype(int), 0, ny - 1)
        return a, h, np.hypot(Q[:, 0] - cx, Q[:, 2] - cz)

    ua, uh, ur = cell(under)
    ba, bh, br = cell(P[belt])
    floor_ = np.full((BINS, ny), np.nan)
    inner = np.full((BINS, ny), np.nan)
    for a in range(BINS):
        for h in range(ny):
            u = ur[(ua == a) & (np.abs(uh - h) <= 1)]   # (its own step and the ones beside it)
            if len(u):
                floor_[a, h] = u.max()
            b = br[(ba == a) & (bh == h)]
            if len(b):
                inner[a, h] = b.min()
    delta = (floor_ + GAP) - inner
    # (a cell with nothing measured: its neighbours' round the waist, then up and down)
    for _ in range(4):
        miss = ~np.isfinite(delta)
        if not miss.any():
            break
        nb = np.stack([np.roll(delta, 1, 0), np.roll(delta, -1, 0), np.roll(delta, 1, 1), np.roll(delta, -1, 1)])
        fill = np.nanmean(np.where(np.isfinite(nb), nb, np.nan), 0)
        delta = np.where(miss, fill, delta)
    delta = np.where(np.isfinite(delta), delta, 0.0)
    moved = 0
    movers = [belt] + [M[m] for m in small if (P[M[m], 1] > lo - STEP).all() and (P[M[m], 1] < hi + STEP).all()]
    for idx in movers:
        a, h, r = cell(P[idx])
        s = delta[a, h]
        k = np.maximum(0.0, r + s) / np.maximum(r, 1e-9)
        P[idx, 0] = cx + (P[idx, 0] - cx) * k
        P[idx, 2] = cz + (P[idx, 2] - cz) * k
        moved += int((np.abs(s) > 1e-4).sum())
    V['p'] = P.astype(np.float32)
    d[24:24 + len(V) * 52] = V.tobytes()
    open(os.path.join(ASSETS, 'av_top_20_%s.mdl' % sex), 'wb').write(bytes(d))
    fin = delta[np.isfinite(delta)]
    print('%s: the belt %.0f-%.0f mm under the neck: moved in by %.0f mm at most, out by %.0f at most (front %+.0f, back %+.0f); '
          '%d vertices moved' % (sex, -hi * 1000, -lo * 1000, -fin.min() * 1000, max(0.0, fin.max()) * 1000,
                                  delta[0].mean() * 1000, delta[BINS // 2].mean() * 1000, moved))


if __name__ == '__main__':
    for sex in 'mf':
        fit(sex)
