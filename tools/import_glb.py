"""Turn a contributed .glb into a Project Arena game model (.mdl).

Usage: python -I tools/import_glb.py IN.glb MODEL_NAME [--out DIR]     (default DIR: app/assets_open)

- Material names must be palette names (tools/blender/palette.py, 'prop' kind: av_primary, av_glow, gaunt_shell, ...);
  anything else is reported and drawn grey.
- Meshes with skin weights (outfits made on templates/body_*.glb) become skinned models (7-bone avatar rig);
  everything else becomes a rigid model in the space it was made in (head space for head parts, the hand frame for
  gauntlets).
"""
import argparse
import os
import struct
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'blender'))
import palette  # noqa: E402
from export_glb import prop_material, BONES  # noqa: E402
from glb import read_glb  # noqa: E402


TWOSIDED, DESIGN, CLOTH = 1, 32, 64


def material_record(name, double=False, cloth=False, design=''):
    d = palette.P.get(name)
    if d is None or d.get('kind') != 'prop':
        return None
    sh, tint, gloss, flags = prop_material(name)
    em = tuple(d.get('em', (0, 0, 0))) + (gloss,)
    diffuse = ('open_' + d['tex']) if d.get('tex') else ''
    flags |= (TWOSIDED if double else 0) | (CLOTH if cloth else 0) | (DESIGN if design else 0)
    return sh, tint, em, flags, diffuse, design or ''


def convert(path, name, out_dir):
    j, prims = read_glb(path)
    skinned = any(p['joints'] is not None for p in prims)
    if skinned:   # map the file's joint order onto the game's bones by name
        skin = j['skins'][0]
        names = [j['nodes'][n].get('name', '') for n in skin['joints']]
        remap = np.array([BONES.index(n) if n in BONES else 0 for n in names] + [0] * 8)
        missing = [n for n in names if n not in BONES]
        if missing:
            print('  WARN bones not in the avatar rig (weights go to the torso):', missing)
    groups = {}
    problems = []
    for p in prims:
        m = p['material'].split('.')[0]   # Blender adds .001 to duplicates
        if material_record(m) is None:
            problems.append(m)
        x = (p.get('extras') or {}).get('projectArena', {})
        key = (m, p.get('double', False) or bool(x.get('twoSided')), bool(x.get('cloth')), x.get('design') or '')
        groups.setdefault(key, []).append(p)
    verts, idxs, secs, mats = [], [], [], []
    nv = ni = 0
    dt = [('p', '<f4', 3), ('n', '<f4', 3), ('u0', '<f4', 2), ('u1', '<f4', 2), ('c', 'u1', 4)]
    if skinned:
        dt += [('bi', 'u1', 4), ('bw', 'u1', 4)]
    for mi, m in enumerate(sorted(groups)):
        for p in groups[m]:
            n = len(p['pos'])
            v = np.zeros(n, dtype=dt)
            v['p'], v['n'], v['u0'] = p['pos'], p['nrm'], p['uv0']
            if p['uv1'] is not None:
                v['u1'] = p['uv1']
            v['c'] = 255
            if skinned:
                if p['joints'] is not None:
                    jt = remap[p['joints']]
                    w = p['weights'] / np.maximum(p['weights'].sum(1, keepdims=True), 1e-9)
                    wq = np.clip(np.round(w * 255), 0, 255)
                    wq[:, 0] += 255 - wq.sum(1)
                    v['bi'], v['bw'] = jt.astype(np.uint8), wq.astype(np.uint8)
                else:   # an unweighted piece in a skinned file follows the torso
                    v['bi'] = 0
                    v['bw'] = np.array([255, 0, 0, 0], np.uint8)
            verts.append(v)
            idxs.append(p['idx'].astype(np.uint32) + nv)
            secs.append((ni, len(p['idx']), mi))
            nv += n
            ni += len(p['idx'])
        mats.append(material_record(*m) or (0, (0.5, 0.5, 0.5, 1.0), (0, 0, 0, 0), 0, '', ''))
    V = np.concatenate(verts)
    I = np.concatenate(idxs)
    os.makedirs(out_dir, exist_ok=True)
    out = os.path.join(out_dir, name + '.mdl')
    with open(out, 'wb') as f:
        f.write(b'SMDL' + struct.pack('<5I', 3 if skinned else 2, len(V), len(I), len(secs), len(mats)))
        f.write(V.tobytes())
        f.write(I.tobytes())
        for s in secs:
            f.write(struct.pack('<3I', *s))
        for sh, tint, em, flags, diffuse, mask in mats:
            f.write(struct.pack('<I4f4f2I', sh, *tint, *em, 0, flags))
            f.write(diffuse.encode().ljust(64, b'\0'))
            f.write(mask.encode().ljust(64, b'\0'))
    print('  %s: %d vertices, %d triangles, %d materials, %s -> %s' % (os.path.basename(path), len(V), len(I) // 3, len(mats),
                                                                     'skinned' if skinned else 'rigid', out))
    if problems:
        print('  WARN materials not in the palette (drawn grey):', sorted(set(problems)))
    return not problems


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('glb')
    ap.add_argument('name')
    ap.add_argument('--out', default=os.path.join(ROOT, 'app', 'assets_open'))
    a = ap.parse_args()
    sys.exit(0 if convert(a.glb, a.name, a.out) else 1)


if __name__ == '__main__':
    main()
