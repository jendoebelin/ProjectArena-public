"""The downloads are the game's own models: export every model to .glb (tools/export_glb.py, as the asset viewer's
downloads are made) and import it back (tools/import_glb.py): the same triangles, the same weights on each bone of the
9-bone rig (normalized: the game's shader draws a vertex whose weights add up to 2 the same, the perspective divide
cancels it), and each triangle's material the same (shader, tint, gloss, flags, textures).

    python -I tools/check_glb_roundtrip.py [model ...]
"""
import contextlib
import io
import os
import sys
import tempfile

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, ROOT + '/tools')
sys.path.insert(0, ROOT + '/tools/blender')
import export_glb as E  # noqa: E402
import import_glb as I  # noqa: E402

models = sys.argv[1:] or sorted(f[:-4] for f in os.listdir(E.ASSETS) if f.endswith('.mdl'))
tmp = tempfile.mkdtemp()
bad, n, grey = [], 0, []


def mat_key(m):
    return (m['shader'], tuple(round(x, 4) for x in m['tint']), tuple(round(x, 4) for x in m['em']), m['flags'],
            m['diffuse'], m['mask'])


def per_tri(vv, ii, secs, mats, skinned):
    tri_mat = np.zeros(len(ii) // 3, object)
    for first, count, mi in secs:
        tri_mat[first // 3:(first + count) // 3] = [mat_key(mats[mi])] * (count // 3)
    rows = []
    dense = np.zeros((len(vv), 9))
    if skinned:
        for k in range(4):
            np.add.at(dense, (np.arange(len(vv)), vv['bi'][:, k].astype(int)), vv['bw'][:, k] / 255.0)
        dense /= np.maximum(dense.sum(1, keepdims=True), 1e-9)
    for t in range(len(ii) // 3):
        c = ii[3 * t:3 * t + 3]
        rows.append((tuple(np.round(vv['p'][c] * 1e4).astype(int).ravel()), tuple(np.round(dense[c] * 50).astype(int).ravel()),
                     tri_mat[t]))
    return sorted(rows)


for m in models:
    src = os.path.join(E.ASSETS, m + '.mdl')
    ver, v, idx, secs, mats = E.read_mdl(src)
    g = os.path.join(tmp, m + '.glb')
    log = io.StringIO()
    with contextlib.redirect_stdout(log):
        E.export(m, g)
        I.convert(g, m + '_rt', tmp)
    if 'not in the palette' in log.getvalue():
        grey.append(m)
    ver2, v2, idx2, secs2, mats2 = E.read_mdl(os.path.join(tmp, m + '_rt.mdl'))
    a = per_tri(v, np.asarray(idx), secs, mats, ver >= 3)
    b = per_tri(v2, np.asarray(idx2), secs2, mats2, ver2 >= 3)
    same = len(a) == len(b) and all(x[2] == y[2] and max(abs(p - q) for p, q in zip(x[1], y[1])) <= 1 and max(abs(p - q) for p, q in zip(x[0], y[0])) <= 1
                                    for x, y in zip(a, b))
    n += 1
    if not same:
        bad.append(m)
print('  %s   %d models: export to .glb and back as they were (different: %s; materials not kept: %s)'
      % ('ok' if not bad and not grey else 'FAIL', n, bad or 'none', grey or 'none'))
sys.exit(1 if bad or grey else 0)
