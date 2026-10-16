"""Quick look at the MPFB base bodies (build/open_src/body/body_*.npz) in the bind pose, with the per-vertex AO.
Usage: python -I tools/preview_body.py <out.png> [masc|fem] [yaw]"""
import importlib.util
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location('pr', os.path.join(ROOT, 'tools', 'preview_render.py'))
pr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pr)


def body_model(name, rgb=(0.80, 0.60, 0.48)):
    d = np.load(os.path.join(ROOT, 'build', 'open_src', 'body', 'body_%s.npz' % name))
    n = len(d['pos'])
    v = np.zeros(n, dtype=[('p', '<f4', 3), ('n', '<f4', 3), ('u0', '<f4', 2), ('u1', '<f4', 2), ('c', 'u1', 4)])
    v['p'], v['n'], v['u0'] = d['pos'], d['nrm'], d['uv']
    v['c'] = 255
    idx = d['tris'].reshape(-1).astype(np.uint32)
    mat = dict(shader=0, tint=np.array(list(rgb) + [1.0]), em=np.zeros(4), uv=0, flags=0, diffuse=None, mask=None)
    return (v, idx, [(0, len(idx), 0)], [mat]), d


def main():
    out = sys.argv[1]
    name = sys.argv[2] if len(sys.argv) > 2 else 'masc'
    yaw = float(sys.argv[3]) if len(sys.argv) > 3 else 20.0
    m, d = body_model(name)
    ao = d['ao']
    # bake the AO into a per-vertex tint by splitting into grey bands (the preview shader has no vertex colour)
    v, idx, secs, mats = m
    tri = idx.reshape(-1, 3)
    level = np.clip((ao[tri].mean(1) * 8).astype(int), 0, 8)
    order = np.argsort(level, kind='stable')
    idx2 = tri[order].reshape(-1)
    secs2, mats2 = [], []
    for L in range(9):
        cnt = int((level == L).sum()) * 3
        if not cnt:
            continue
        first = int(np.searchsorted(level[order], L)) * 3
        k = (L + 0.5) / 8.0
        mats2.append(dict(mats[0], tint=np.array([0.80 * k, 0.60 * k, 0.48 * k, 1.0])))
        secs2.append((first, cnt, len(mats2) - 1))
    a = np.radians(yaw)
    eye = (2.2 * np.sin(a), -0.15, -2.2 * np.cos(a))
    pr.W, pr.H = 600, 900
    pr.render([((v, idx2.astype(np.uint32), secs2, mats2), np.eye(4))], eye, (0, -0.25, 0), out, fov=40, bg=(0.82, 0.83, 0.86))


main()
