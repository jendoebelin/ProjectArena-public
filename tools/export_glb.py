"""Export Project Arena game models (app/assets_open/*.mdl) to .glb for Blender and other 3D tools.

Usage: python -I tools/export_glb.py OUT_DIR [model ...]      (no model names: every model)

- Units are metres, +Y up, the model faces -Z (as in the game).
- Materials are named after the palette (tools/blender/palette.py: av_primary, av_glow, gaunt_shell, ...); keep the
  names when you edit a model, they tell the game how to colour and light each part. Colour channels (primary,
  secondary, glow, skin, hair, trim, visor) are shown in a sample colour; the player's choice replaces it in game.
- Skinned avatar parts (body, outfits) carry the 7-bone avatar rig in its bind pose and their vertex weights.
- Head parts (hair, headgear, face shields, masks) are in head space: origin between the eyes, facing -Z.
- Arena models show their baked lighting (lightmap on the second UV set).
"""
import os
import re
import struct
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'blender'))
import palette  # noqa: E402
from glb import Glb  # noqa: E402

ASSETS = os.path.join(ROOT, 'app', 'assets_open')
SH_LIT, SH_COURT, SH_ADD, SH_UNLIT, SH_TRANS, SH_LM = 0, 1, 2, 3, 4, 5
TWOSIDED, PATTERN, DESIGN, CLOTH = 1, 16, 32, 64
CHANNELS = {1: ('primary', (0.12, 0.32, 0.80)), 2: ('secondary', (0.22, 0.23, 0.27)), 3: ('glow', (0.15, 0.90, 1.00)),
            4: ('skin', (0.62, 0.43, 0.33)), 5: ('visor', (0.30, 0.42, 0.55)), 6: ('hair', (0.24, 0.14, 0.08)),
            7: ('trim', (0.72, 0.74, 0.78))}
BONES = ['torso', 'neck', 'head', 'upper_arm.L', 'upper_arm.R', 'forearm.L', 'forearm.R']
PARENTS = [-1, 0, 1, 0, 0, 3, 4]


def prop_material(name):
    """(shader, tint rgba, gloss, flags) as tools/build_open_assets.py writes it for a 'prop' palette entry."""
    d = palette.P[name]
    sh = {'unlit': SH_UNLIT, 'add': SH_ADD, 'lit': SH_LIT, 'trans': SH_TRANS}[d.get('shader', 'lit')]
    tint = tuple(d.get('rgb', (1, 1, 1))) + (d.get('alpha', 1.0),)
    return sh, tint, d.get('gloss', d.get('metal', 0.0)), d.get('flags', 0)


LOOKUP, LOOSE = {}, {}
for _n, _d in palette.P.items():
    if _d.get('kind') == 'prop':
        _sh, _t, _g, _f = prop_material(_n)
        _k = (_sh, tuple(round(x, 3) for x in _t), round(_g, 3))
        LOOKUP.setdefault(_k + (_f & ~(DESIGN | CLOTH),), _n)
        LOOSE.setdefault(_k + (_f & ~(TWOSIDED | DESIGN | CLOTH),), _n)


def name_of(m, model, i):
    k = (m['shader'], tuple(round(x, 3) for x in m['tint']), round(m['em'][3], 3))
    return (LOOKUP.get(k + (m['flags'] & ~(DESIGN | CLOTH),)) or LOOSE.get(k + (m['flags'] & ~(TWOSIDED | DESIGN | CLOTH),))
            or '%s_mat%d' % (model, i))


def read_tex(rel):
    if not rel:
        return None
    p = os.path.join(ASSETS, 'tex', rel.replace('/', '_') + '.tex')
    if not os.path.exists(p):
        return None
    b = open(p, 'rb').read()
    w, h, _ = struct.unpack_from('<3I', b, 4)
    return np.frombuffer(b[16:], np.uint8).reshape(h, w, 4)


def read_mdl(path):
    b = open(path, 'rb').read()
    ver, nv, ni, ns, nm = struct.unpack_from('<5I', b, 4)
    off = 24
    dt = [('p', '<f4', 3), ('n', '<f4', 3), ('u0', '<f4', 2), ('u1', '<f4', 2), ('c', 'u1', 4)]
    if ver >= 3:
        dt += [('bi', 'u1', 4), ('bw', 'u1', 4)]
    v = np.frombuffer(b, dtype=dt, count=nv, offset=off)
    off += nv * (52 if ver >= 3 else 44)
    idx = np.frombuffer(b, '<u4', ni, off)
    off += ni * 4
    secs = [struct.unpack_from('<3I', b, off + i * 12) for i in range(ns)]
    off += ns * 12
    mats = []
    for _ in range(nm):
        sh, *rest = struct.unpack_from('<I4f4f2I', b, off)
        off += 44
        d = b[off:off + 64].split(b'\0')[0].decode()
        off += 64
        mk = b[off:off + 64].split(b'\0')[0].decode()
        off += 64
        mats.append(dict(shader=sh, tint=rest[0:4], em=rest[4:8], uv=rest[8], flags=rest[9], diffuse=d, mask=mk))
    return ver, v, idx, secs, mats


def bind_joints(body):
    """Joint positions of the avatar rig in bind pose for body 0 (masculine) / 1 (feminine), from avatar_bodies.inc."""
    src = open(os.path.join(ROOT, 'app', 'src', 'avatar_bodies.inc')).read()
    rows = [l for l in src.splitlines() if l.strip().startswith('{{')]
    nums = [float(x) for x in re.findall(r'-?\d+\.\d+', rows[body])]
    P = np.array(nums).reshape(-1, 3)   # eye, head, shoulder L/R, elbow L/R, wrist L/R, thumb L/R
    return [np.array([0.0, (P[2][1] + P[6][1]) * 0.5, 0.0]), np.zeros(3), P[1], P[2], P[3], P[4], P[5]]


def body_of(model):
    return 1 if model.endswith('_f') else 0


def export(model, out_path, glb=None, offset=None, save=True, rigid=False):
    ver, v, idx, secs, mats = read_mdl(os.path.join(ASSETS, model + '.mdl'))
    g = glb or Glb()
    skin = None
    if ver >= 3 and not rigid:
        skin = g.add_skin(BONES, bind_joints(body_of(model)), PARENTS)
    tex_cache = {}
    prims = []
    for first, count, mi in secs:
        m = mats[mi]
        name = name_of(m, model, mi)
        ch = (m['flags'] >> 8) & 0xFF
        col = np.array(m['tint'][:3], float)
        if ch in CHANNELS:
            col = col * np.array(CHANNELS[ch][1])
        sh = m['shader']
        emissive = col if sh in (SH_UNLIT, SH_ADD) else np.zeros(3)
        tex = None
        if m['diffuse']:
            if m['diffuse'] not in tex_cache:
                img = read_tex(m['diffuse'])
                tex_cache[m['diffuse']] = g.image(img) if img is not None else None
            tex = tex_cache[m['diffuse']]
        extras = {'projectArena': {'palette': name if name in palette.P else None, 'channel': CHANNELS.get(ch, ('none',))[0],
                                   'shader': ['lit', 'court', 'additive', 'unlit', 'translucent', 'lightmapped'][sh],
                                   'twoSided': bool(m['flags'] & TWOSIDED), 'cloth': bool(m['flags'] & CLOTH),
                                   'design': m['mask'] or None}}
        gloss = m['em'][3]
        mat = g.material(name, color=tuple(col) + (m['tint'][3] if sh == SH_TRANS else 1.0,), emissive=tuple(emissive),
                         roughness=0.9 if gloss <= 0 else max(0.15, 0.8 - 0.6 * min(1.0, gloss)),
                         blend=sh in (SH_TRANS, SH_ADD), double=bool(m['flags'] & TWOSIDED), texture=tex,
                         tex_coord=1 if m['uv'] == 1 else 0, extras=extras)
        ii = np.asarray(idx[first:first + count], np.int64)
        used, local = np.unique(ii, return_inverse=True)
        vv = v[used]
        p = dict(pos=vv['p'].astype(np.float32), nrm=vv['n'].astype(np.float32), uv0=vv['u0'].astype(np.float32),
                 idx=local.astype(np.uint32), material=mat)
        if sh == SH_LM or m['uv'] == 1:
            p['uv1'] = vv['u1'].astype(np.float32)
        if offset is not None:
            p['pos'] = p['pos'] + np.asarray(offset, np.float32)
        if ver >= 3 and not rigid:
            p['joints'] = vv['bi'].astype(np.uint8)
            p['weights'] = vv['bw'].astype(np.uint8)
        prims.append(p)
    g.add_mesh(model, prims, skin=skin)
    if save:
        g.save(out_path)
    return g


def main():
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    names = sys.argv[2:] or sorted(f[:-4] for f in os.listdir(ASSETS) if f.endswith('.mdl'))
    for n in names:
        p = os.path.join(out, n + '.glb')
        export(n, p)
        print('  %-24s %7.1f kB' % (n, os.path.getsize(p) / 1024))


if __name__ == '__main__':
    main()
