"""STRAPS, NOT BANDS (0.3.27; the owner, 2026-11-27: "the high pigtails don't work with several face shields, masks,
hats"). The tuck maps (tools/build_open_assets.py write_tuck: for each direction from the head's centre, the radius hair
has to stay inside) were grown one cell past the gear's edges everywhere, so hair tucks under a helmet's rim. Under a
face shield's strap that cell (5.6 degrees of latitude, about 1 cm at the head) took the strap's radius on to where the
back of the head bulges out past it: GOGGLES, HEADSET and HALO pulled every hairstyle into the head in a band under
their straps (bare patches of scalp at the back), and pinned DREADLOCKS along it.

This makes the face shields' maps again from their own models as they are (the same cells, the same margin), keeping
the growth (the map is sampled smoothly: without it, hair at a strap's edges came out over the strap) but easing it:
the cells past the edges hold hair EASE (15 mm) further out than the strap, and the cells it crosses OWN (4 mm)
further out than before (the cell is wider than the strap: hair just under it was pulled in past the scalp). Tried on
every hairstyle under GOGGLES and HEADSET: no bare scalp, the strap on top (OWN 12 mm: hair over the strap again). The
helmets' maps are as built (their rims). Run again: the same maps (made from the models each time).

Usage: python -I tools/fix_strap_tucks.py [--check]   (--check: write_tuck's maps as built, against the files: the
same algorithm, to show it reads the models as build_open_assets did; they matched, all 14)
"""
import math
import os
import struct
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, 'app', 'assets_open')
TUCK_W, TUCK_H, TUCK_R0, TUCK_R1 = 64, 32, 0.05, 0.55   # (build_open_assets.py's)
EASE = float(os.environ.get('PA_TUCK_EASE', '0.015'))   # (the cells past a strap's edge: this much further out)
OWN = float(os.environ.get('PA_TUCK_OWN', '0.004'))     # (the cells it crosses: this much further out)


def triangles(name):
    d = open(os.path.join(ASSETS, name + '.mdl'), 'rb').read()
    assert d[:4] == b'SMDL'
    ver, nv, ni, ns, nm = struct.unpack_from('<5I', d, 4)
    vsz = 52 if ver >= 3 else 44
    P = np.frombuffer(d, dtype=[('p', '<f4', 3), ('rest', 'u1', vsz - 12)], count=nv, offset=24)['p'].astype(np.float64)
    I = np.frombuffer(d, '<u4', count=ni, offset=24 + nv * vsz)
    return P[I].reshape(-1, 3, 3)


def tuck_map(tri, C, grow, ease=0.0, own=0.0):
    n = 6
    bary = np.array([(a / n, b / n, 1 - (a + b) / n) for a in range(n + 1) for b in range(n + 1 - a)])
    pts = np.einsum('sk,tkd->tsd', bary, tri).reshape(-1, 3) - np.array(C)
    r = np.linalg.norm(pts, axis=1)
    az = np.arctan2(pts[:, 0], pts[:, 2])
    el = np.arcsin(np.clip(pts[:, 1] / np.maximum(r, 1e-9), -1, 1))
    i = np.floor((az + math.pi) / (2 * math.pi) * TUCK_W).astype(int) % TUCK_W
    j = np.clip(np.floor((el + math.pi / 2) / math.pi * TUCK_H).astype(int), 0, TUCK_H - 1)
    lim = np.full(TUCK_H * TUCK_W, np.inf)
    np.minimum.at(lim, j * TUCK_W + i, r - 0.003)
    lim = lim.reshape(TUCK_H, TUCK_W)
    if own:   # (the cells it crosses: hair kept that much less far inside it)
        lim = np.where(np.isfinite(lim), lim + own, lim)
    if grow:   # (build_open_assets.py's growth: a cell past the edges)
        covered = np.isfinite(lim)
        near = np.full_like(lim, np.inf)
        for dj in (-1, 0, 1):
            for di in (-1, 0, 1):
                sh = np.roll(lim, di, axis=1)
                if dj:
                    sh = np.roll(sh, dj, axis=0)
                    if dj > 0:
                        sh[0] = np.inf
                    else:
                        sh[-1] = np.inf
                near = np.minimum(near, sh)
        lim = np.where(covered, lim, near + ease)
    return np.where(np.isfinite(lim), np.round(np.clip((lim - TUCK_R0) / (TUCK_R1 - TUCK_R0), 0, 1) * 255), 255).astype(np.uint8)


def main():
    check = '--check' in sys.argv
    for k in range(1, 8):
        for b in 'mf':
            name = 'av_visor_%d_%s' % (k, b)
            path = os.path.join(ASSETS, name + '.tuck')
            d = open(path, 'rb').read()
            if not check:
                orig = os.environ.get('PA_TUCK_FROM')   # (the maps as built, to start from)
                if orig:
                    d = open(os.path.join(orig, name + '.tuck'), 'rb').read()
            w, h = struct.unpack_from('<2I', d, 4)
            C = struct.unpack_from('<3f', d, 12)
            old = np.frombuffer(d, np.uint8, count=w * h, offset=24)
            tri = triangles(name)
            if check:
                same = tuck_map(tri, C, True).reshape(-1)
                print('%-16s as built: %d of %d cells differ (most by %d)' % (name, int((same != old).sum()), w * h,
                                                                        int(np.abs(same.astype(int) - old.astype(int)).max())))
                continue
            new = tuck_map(tri, C, True, EASE, OWN).reshape(-1)
            with open(path, 'wb') as f:
                f.write(d[:24])
                f.write(new.tobytes())
            print('%-16s kept in: %3d cells, was %3d' % (name, int((new < 255).sum()), int((old < 255).sum())))


if __name__ == '__main__':
    main()
