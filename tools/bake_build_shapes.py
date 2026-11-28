"""BUILD's body shapes (0.3.27, the owner, 2026-11-26: the characters' bodies "should be marked as a different body
build"): each new build is how one character's body differs from the standard one, measured vertex for vertex on the
top and shorts made for it (contrib/character/*/native: the same meshes as the game's TANK TOP or TRAINING TEE and
ATHLETIC SHORTS, reshaped). The difference is spread into a 3D grid over the body's rest pose (bind space): each cell
the inverse-distance-weighted movement of the nearest measured points, full within 4 cm of them, none from 14 cm out,
none above the neck (the face keeps its shape). The game moves every outfit's vertices by it before skinning
(render.cpp, uShape), so the build fits every top, shorts, skirt and dress without a model of its own.

.grd: 'SGRD', u32 nx, ny, nz, f32 min[3], f32 max[3], then nx*ny*nz movements as half floats (x, y, z), x fastest.
Usage: python -I tools/bake_build_shapes.py   (writes app/assets_open/av_build_<m|f><4|5>.grd)"""
import os
import struct

import numpy as np
from scipy.spatial import cKDTree

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, 'app', 'assets_open')
CONTRIB = os.path.join(ROOT, 'contrib', 'character')
CELL = 0.02          # 2 cm
FULL, NONE = 0.04, 0.14   # the falloff from the nearest measured point
NECK0, NECK1 = 0.10, 0.16  # (bind-space height: the neckline, and where the face starts)
K = 8

# build index -> per body type: the character and the standard parts its own were made from
BUILDS = {
    ('m', 4): ('rook', [('av_top_6_m4', 'av_top_6_m'), ('av_bottom_1_m4', 'av_bottom_1_m')]),
    ('m', 5): ('rowan', [('av_top_6_m5', 'av_top_6_m'), ('av_bottom_1_m5', 'av_bottom_1_m')]),
    ('f', 4): ('sable', [('av_top_6_f4', 'av_top_6_f'), ('av_bottom_1_f4', 'av_bottom_1_f')]),
    ('f', 5): ('tessa', [('av_top_9_f5', 'av_top_9_f'), ('av_bottom_1_f5', 'av_bottom_1_f')]),
}


def positions(path):
    d = open(path, 'rb').read()
    ver, nv = struct.unpack_from('<2I', d, 4)
    assert d[:4] == b'SMDL' and ver >= 3, path
    return np.frombuffer(d, dtype=[('p', '<f4', 3), ('rest', 'u1', 40)], count=nv, offset=24)['p'].astype(np.float64)


def body_bounds(bk):
    lo, hi = np.full(3, 1e9), np.full(3, -1e9)
    for f in os.listdir(ASSETS):
        if f.endswith('_%s.mdl' % bk) and f.split('_')[1] in ('top', 'bottom', 'sbottom', 'wfade', 'bhead', 'tpulse'):
            p = positions(os.path.join(ASSETS, f))
            lo, hi = np.minimum(lo, p.min(0)), np.maximum(hi, p.max(0))
    return lo - 0.03, hi + 0.03


def bake(bk, idx, name, pairs):
    P, D = [], []
    for own, std in pairs:
        a, b = positions(os.path.join(CONTRIB, name, 'native', own + '.mdl')), positions(os.path.join(ASSETS, std + '.mdl'))
        assert a.shape == b.shape, (own, std)
        P.append(b)
        D.append(a - b)
    P, D = np.concatenate(P), np.concatenate(D)
    lo, hi = body_bounds(bk)
    n = np.maximum(2, np.ceil((hi - lo) / CELL).astype(int) + 1)
    hi = lo + (n - 1) * CELL
    xs, ys, zs = (np.linspace(lo[i], hi[i], n[i]) for i in range(3))
    Z, Y, X = np.meshgrid(zs, ys, xs, indexing='ij')   # (x fastest in memory)
    C = np.stack([X.ravel(), Y.ravel(), Z.ravel()], 1)
    dist, near = cKDTree(P).query(C, k=K)
    w = 1.0 / (dist * dist + 1e-6)
    disp = (D[near] * w[..., None]).sum(1) / w.sum(1)[:, None]
    fall = np.clip((NONE - dist[:, 0]) / (NONE - FULL), 0, 1)
    neck = np.clip((NECK1 - C[:, 1]) / (NECK1 - NECK0), 0, 1)
    disp *= (fall * neck)[:, None]
    out = os.path.join(ASSETS, 'av_build_%s%d.grd' % (bk, idx))
    with open(out, 'wb') as f:
        f.write(b'SGRD' + struct.pack('<3I6f', int(n[0]), int(n[1]), int(n[2]), *lo, *hi))
        f.write(disp.astype('<f2').tobytes())
    m = np.linalg.norm(disp, axis=1)
    print('  %-16s %dx%dx%d cells, %d KB, largest move %.1f mm (%s)' % (os.path.basename(out), n[0], n[1], n[2],
                                                                        os.path.getsize(out) // 1024, m.max() * 1000, name))


if __name__ == '__main__':
    for (bk, idx), (name, pairs) in sorted(BUILDS.items()):
        bake(bk, idx, name, pairs)
