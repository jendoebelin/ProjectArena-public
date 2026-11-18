"""FACE 2-4 for the PR #25 heads: the MPFB faces' differences carried onto his heads.

The MPFB bodies had three more faces (tools/blender/make_body.py FACES: the same body, its face changed by MPFB's face
targets, up to 1-1.4 cm, the eyes where they were). Here each one's change to the MPFB base head (per vertex, the face
minus the base) is carried to the nearest points of his head, the heads lined up by their eyes: each of his head's
vertices takes the inverse-distance mean of its 8 nearest MPFB vertices' changes, smoothed over his mesh (a few
Laplacian passes), fading to nothing where the head's weight falls from 0.85 to 0.5 (the neck: the head part is cut at
0.5, so the seam with every top stays where it is). His brows ride the skin under them (each its nearest head
vertices' change); his eyes stay.

Reads build/open_src/body/body_<masc|fem>[_face<k>].npz (the MPFB bodies, tools/blender/make_body.py) and
experiments/avatar_anatomy/base; writes tools/bodies_q/faces_<masc|fem>.npz (the changes per vertex of his head and
brows, faces 1-3), which tools/repose_bodies.py applies (body_q/body_<masc|fem>_face<k>.npz, brows_<..>_face<k>.npz).

Usage: python -I tools/make_faces_q.py   (needs SciPy)
"""
import json
import os

import numpy as np
from scipy.spatial import cKDTree

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MPFB = os.path.join(ROOT, 'build', 'open_src', 'body')
Q = os.path.join(ROOT, 'experiments', 'avatar_anatomy', 'base')
OUT = os.path.join(ROOT, 'tools', 'bodies_q')
HEAD = 2


def head_weight(d):
    hw = np.zeros(len(d['pos']))
    for k in range(4):
        hw += np.where(d['bone'][:, k] == HEAD, d['weight'][:, k], 0.0)
    return hw / np.maximum(d['weight'].sum(1), 1e-9)


def smoothed(P, T, D, w, iters=6):
    """D (per vertex) relaxed over the mesh (welded by position), only where w > 0."""
    key = np.round(P * 1e5).astype(np.int64)
    _, cid = np.unique(key, axis=0, return_inverse=True)
    cid = cid.reshape(-1)
    n = cid.max() + 1
    Dc = np.zeros((n, 3))
    cnt = np.zeros(n)
    np.add.at(Dc, cid, D)
    np.add.at(cnt, cid, 1)
    Dc /= np.maximum(cnt, 1)[:, None]
    wc = np.zeros(n)
    np.maximum.at(wc, cid, w)
    Tc = cid[T]
    deg = np.zeros(n)
    for i in range(3):
        np.add.at(deg, Tc[:, i], 2)
    for _ in range(iters):
        acc = np.zeros_like(Dc)
        for i, j in ((0, 1), (1, 2), (2, 0), (1, 0), (2, 1), (0, 2)):
            np.add.at(acc, Tc[:, i], Dc[Tc[:, j]])
        Dc = Dc + (0.5 * (wc > 0))[:, None] * (acc / np.maximum(deg, 1)[:, None] - Dc)
    return Dc[cid]


def faces(name):
    base = np.load(os.path.join(MPFB, 'body_%s.npz' % name))
    eye_m = np.array(json.loads(str(base['joints']))['eye_mid'])
    hw_m = head_weight(base)
    sel = hw_m > 0.3
    tree = cKDTree(base['pos'][sel] - eye_m)
    q = np.load(os.path.join(Q, 'body_%s.npz' % name))
    eye_q = np.array(json.loads(str(q['joints']))['eye_mid'])
    hw = head_weight(q)
    fade = np.clip((hw - 0.5) / 0.35, 0, 1)
    fade = fade * fade * (3 - 2 * fade)
    brows = np.load(os.path.join(Q, 'brows_%s.npz' % name))
    out = {}
    for f in (1, 2, 3):
        face = np.load(os.path.join(MPFB, 'body_%s_face%d.npz' % (name, f)))
        change = (face['pos'] - base['pos'])[sel]

        def carried(P):
            dist, idx = tree.query(P - eye_q, k=8)
            w = 1.0 / np.maximum(dist, 1e-4)
            return (change[idx] * w[..., None]).sum(1) / w.sum(1)[:, None]
        D = carried(q['pos'].astype(np.float64)) * fade[:, None]
        D = smoothed(q['pos'].astype(np.float64), q['tris'], D, fade) * fade[:, None]
        # the brows ride the skin under them: each takes the change of its nearest head vertices (as moved, faded)
        hv = np.nonzero(hw > 0.5)[0]
        dist, idx = cKDTree(q['pos'][hv].astype(np.float64)).query(brows['pos'].astype(np.float64), k=4)
        w = 1.0 / np.maximum(dist, 1e-4)
        Db = (D[hv][idx] * w[..., None]).sum(1) / w.sum(1)[:, None]
        out['face%d' % f] = D.astype(np.float32)
        out['brows%d' % f] = Db.astype(np.float32)
        m = np.linalg.norm(D, axis=1)
        print('  %s face %d: %d vertices moved, most %.1f mm (the MPFB face: %.1f)' % (
            name, f + 1, int((m > 1e-4).sum()), m.max() * 1000, np.linalg.norm(face['pos'] - base['pos'], axis=1).max() * 1000))
    np.savez_compressed(os.path.join(OUT, 'faces_%s.npz' % name), **out)


def main():
    os.makedirs(OUT, exist_ok=True)
    for name in ('masc', 'fem'):
        faces(name)
    print('wrote', OUT)


if __name__ == '__main__':
    main()
