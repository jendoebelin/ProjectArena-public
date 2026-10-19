"""Build the Project Arena "open" asset set (original art, no CCP content) into app/assets_open.

Pipeline
  1. tools/gen_open_textures.py      procedural textures (PNG for Blender, .tex for the app)
  2. tools/blender/make_arena.py     court + stadium in one Cycles scene, lightmaps baked   -> build/open_src/*.npz
     tools/blender/make_props.py     ball, spawner, gauntlet, knuckle guard, shield, strike ring, avatar
  3. tools/gen_open_audio.py         the synthesized sound set
  4. this script                     npz -> .mdl (welded, per-material shaders from tools/blender/palette.py),
                                     lightmaps -> .tex, floor + mirrored-light reflection layer, collision

Usage: python -I tools/build_open_assets.py [--preview] [--skip-blender | --skip-arena]
"""
import math
import os
import re
import shutil
import struct
import subprocess
import sys

import numpy as np
from PIL import Image, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools', 'blender'))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import palette  # noqa: E402
import frames  # noqa: E402

SRC = os.path.join(ROOT, 'build', 'open_src')
OUT = os.path.join(ROOT, 'app', 'assets_open')
CLEAN = os.path.join(ROOT, 'app', 'assets_clean')
BLENDER = r'C:\Program Files\Blender Foundation\Blender 5.2\blender.exe'

SH_LIT, SH_COURT, SH_ADD, SH_UNLIT, SH_TRANS, SH_LM = 0, 1, 2, 3, 4, 5
TWOSIDED = 1


# ---------------------------------------------------------------- npz -> primitive groups
def load_npz(path):
    d = np.load(path)
    names = [str(n).split('.')[0] for n in d['names']]
    groups = {}
    for mi, name in enumerate(names):
        sel = np.nonzero(d['mat'] == mi)[0]
        if len(sel) == 0:
            continue
        g = groups.setdefault(name, {k: [] for k in ('pos', 'nrm', 'uv0', 'uv1')})
        for k in g:
            g[k].append(d[k][sel])
    for g in groups.values():
        for k in g:
            g[k] = np.concatenate(g[k])
    lm = d['lightmap'].astype(np.float32) if 'lightmap' in d.files else None
    return groups, lm


def weld(g):
    """Unindexed corners -> unique vertices + indices (rounded to sub-mm / 1e-4 uv)."""
    key = np.c_[np.round(g['pos'] * 2e4), np.round(g['nrm'] * 1e3), np.round(g['uv0'] * 1e4), np.round(g['uv1'] * 2e4)]
    _, first, inv = np.unique(key.astype(np.int64), axis=0, return_index=True, return_inverse=True)
    return {k: v[first] for k, v in g.items()}, inv.reshape(-1).astype(np.uint32)


def material(name, lm_tex=None, lm_scale=1.0):
    """(shader, tint, emissive, uv channel, flags, diffuse texture) for a palette material."""
    d = palette.P.get(name)
    if d is None:
        print('  WARN unknown material', name)
        return SH_LIT, (0.5, 0.5, 0.5, 1), (0, 0, 0, 0), 0, 0, ''
    k = d['kind']
    if k == 'lit':
        return SH_LM, (lm_scale, lm_scale, lm_scale, 1), tuple(d['f0']) + (d['gloss'],), 1, 0, lm_tex or ''
    if k == 'emit':
        return SH_UNLIT, tuple(d['show']) + (1,), (0, 0, 0, 0), 0, 0, ('open_' + d['tex']) if d.get('tex') else ''
    if k == 'glass':
        return SH_TRANS, tuple(d['rgb']) + (d['alpha'],), (0, 0, 0, 0), 0, TWOSIDED, ''
    if k == 'fx':
        return SH_ADD, tuple(d['show']) + (1,), (0, 0, 0, 0), 0, TWOSIDED, ('open_' + d['tex']) if d.get('tex') else ''
    sh = {'unlit': SH_UNLIT, 'add': SH_ADD, 'lit': SH_LIT, 'trans': SH_TRANS}[d.get('shader', 'lit')]
    tint = tuple(d.get('rgb', (1, 1, 1))) + (d.get('alpha', 1.0),)
    return sh, tint, tuple(d.get('em', (0, 0, 0))) + (d.get('gloss', d.get('metal', 0.0)),), 0, d.get('flags', 0), \
        ('open_' + d['tex']) if d.get('tex') else ''


def write_mdl(path, groups, lm_tex=None, lm_scale=1.0, mats_override=None):
    verts, idxs, secs, mats = [], [], [], []
    nv = ni = 0
    for mi, name in enumerate(sorted(groups)):
        g, idx = weld(groups[name])
        n = len(g['pos'])
        v = np.zeros(n, dtype=[('p', '<f4', 3), ('n', '<f4', 3), ('u0', '<f4', 2), ('u1', '<f4', 2), ('c', 'u1', 4)])
        v['p'], v['n'], v['u0'], v['u1'] = g['pos'], g['nrm'], g['uv0'], g['uv1']
        v['c'] = 255
        verts.append(v)
        idxs.append(idx + nv)
        secs.append((ni, len(idx), mi))
        nv += n
        ni += len(idx)
        m = (mats_override or {}).get(name) or material(name, lm_tex, lm_scale)
        mats.append(m)
    V = np.concatenate(verts)
    I = np.concatenate(idxs).astype(np.uint32)
    with open(path, 'wb') as f:
        f.write(b'SMDL' + struct.pack('<5I', 2, len(V), len(I), len(secs), len(mats)))
        f.write(V.tobytes())
        f.write(I.tobytes())
        for s in secs:
            f.write(struct.pack('<3I', *s))
        for sh, tint, em, uv, flags, tex in mats:
            f.write(struct.pack('<I4f4f2I', sh, *tint, *em, uv, flags))
            f.write(tex.encode().ljust(64, b'\0'))
            f.write(b''.ljust(64, b'\0'))
    print('  %-18s %7d verts %7d tris %3d mats' % (os.path.basename(path), len(V), len(I) // 3, len(mats)))


# ---------------------------------------------------------------- lightmaps
def write_tex(name, rgba_u8):
    os.makedirs(os.path.join(OUT, 'tex'), exist_ok=True)
    h, w = rgba_u8.shape[:2]
    with open(os.path.join(OUT, 'tex', name + '.tex'), 'wb') as f:
        f.write(b'STEX' + struct.pack('<3I', w, h, 0))
        f.write(np.ascontiguousarray(rgba_u8).tobytes())


def process_lightmap(name, lm, exposure, scale=2.0, out_size=None):
    """Denoise lightly, expose, store sRGB(lm * exposure / scale); the material tint multiplies `scale` back."""
    img = np.clip(lm * exposure / scale, 0, 1)
    pil = Image.fromarray((np.power(img, 1 / 2.2) * 255 + 0.5).astype(np.uint8), 'RGB')
    pil = pil.filter(ImageFilter.GaussianBlur(0.9))
    if out_size and out_size != pil.size[0]:
        pil = pil.resize((out_size, out_size), Image.LANCZOS)
    rgb = np.asarray(pil)
    rgba = np.dstack([rgb, np.full(rgb.shape[:2], 255, np.uint8)])
    write_tex(name, rgba)
    pil.save(os.path.join(SRC, name + '.png'))
    clipped = (lm * exposure / scale > 1).mean()
    print('  %-18s %dx%d  exposure %.3f  clipped %.2f%%' % (name, rgb.shape[1], rgb.shape[0], exposure, clipped * 100))


# ---------------------------------------------------------------- reflection layer
def mirrored(groups, dim):
    """Mirror emissive / glow groups below the floor (y -> -y) as additive geometry for the glossy floor."""
    out, mats = {}, {}
    for name, g in groups.items():
        d = palette.P.get(name, {})
        if d.get('kind') not in ('emit', 'fx'):
            continue
        if name.startswith(('twinkle', 'pulse', 'grid')) or name in ('beam', 'emblem_fx'):
            continue
        if g['pos'][:, 1].max() < 0.02:          # lies on the floor itself
            continue
        m = {k: v.copy() for k, v in g.items()}
        m['pos'][:, 1] *= -1
        m['nrm'][:, 1] *= -1
        key = 'refl_' + name
        out[key] = m
        sh, tint, em, uv, flags, tex = material(name)
        col = tuple(c * dim for c in tint[:3]) + (1,)
        mats[key] = (SH_ADD, col, (0, 0, 0, 0), 0, TWOSIDED, tex)
    return out, mats


# ---------------------------------------------------------------- collision (same layout as the original court)
TAG_ARENA, TAG_RETURN, TAG_FOUL, TAG_TEAM0, TAG_TEAM1 = 1, 2, 4, 8, 16


def write_collision(path):
    HX, HY, HZ = 9.5, 4.0, 2.5
    tris = []

    def quad(a, b, c, d, tag, n=4):
        a, b, c, d = (np.array(p, np.float32) for p in (a, b, c, d))
        for i in range(n):
            for j in range(n):
                u0, u1, v0, v1 = i / n, (i + 1) / n, j / n, (j + 1) / n

                def P(u, v):
                    return a + (b - a) * u + (d - a) * v + (a - b + c - d) * u * v
                p00, p10, p11, p01 = P(u0, v0), P(u1, v0), P(u1, v1), P(u0, v1)
                tris.append((p00, p10, p11, tag))
                tris.append((p00, p11, p01, tag))

    quad((-HX, 0, -HZ), (-HX, 0, HZ), (HX, 0, HZ), (HX, 0, -HZ), TAG_ARENA)
    quad((-HX, HY, -HZ), (HX, HY, -HZ), (HX, HY, HZ), (-HX, HY, HZ), TAG_ARENA)
    quad((-HX, 0, -HZ), (HX, 0, -HZ), (HX, HY, -HZ), (-HX, HY, -HZ), TAG_ARENA)
    quad((-HX, 0, HZ), (-HX, HY, HZ), (HX, HY, HZ), (HX, 0, HZ), TAG_ARENA)
    quad((-HX, 0, -HZ), (-HX, HY, -HZ), (-HX, HY, HZ), (-HX, 0, HZ), TAG_ARENA | TAG_RETURN | TAG_TEAM0, 3)
    quad((HX, 0, -HZ), (HX, 0, HZ), (HX, HY, HZ), (HX, HY, -HZ), TAG_ARENA | TAG_RETURN | TAG_TEAM1, 3)
    for sgn, tm in ((-1, TAG_TEAM0), (1, TAG_TEAM1)):
        x0, x1 = sorted((sgn * 4.75, sgn * HX))
        tag = TAG_ARENA | TAG_FOUL | tm
        y0, y1, z = 0.05, 3.97, HZ - 0.05
        quad((x0, y0, -z), (x0, y0, z), (x1, y0, z), (x1, y0, -z), tag)
        quad((x0, y1, -z), (x1, y1, -z), (x1, y1, z), (x0, y1, z), tag)
        quad((x0, y0, -z), (x1, y0, -z), (x1, y1, -z), (x0, y1, -z), tag)
        quad((x0, y0, z), (x0, y1, z), (x1, y1, z), (x1, y0, z), tag)
    with open(path, 'wb') as f:
        f.write(b'SCOL' + struct.pack('<I', len(tris)))
        for a, b, c, tag in tris:
            f.write(np.concatenate([a, b, c]).astype('<f4').tobytes())
            f.write(struct.pack('<I', tag))
    print('  %-18s %7d tris' % (os.path.basename(path), len(tris)))


# ---------------------------------------------------------------- props (authored in attachment frames)
PROP_FRAMES = {
    'lancer': lambda p, n: frames.to_model(frames.LANCER, p, n),
    'gauntlet_0': lambda p, n: frames.to_model(frames.LANCER, p, n),
    'gauntlet_1': lambda p, n: frames.to_model(frames.LANCER, p, n),
    'gauntlet_2': lambda p, n: frames.to_model(frames.LANCER, p, n),
    **{'gauntlet_%d_%s' % (k, part): (lambda p, n: frames.to_model(frames.LANCER, p, n)) for k in range(3) for part in ('hand', 'cuff')},
    'knuckleguard': lambda p, n: frames.to_model(frames.KNUCKLE, p, n),
    'shield': lambda p, n: frames.to_model(frames.PLATE, p, n),
    'head': frames.head_to_model,
    'torso': frames.body_to_model,
}


def build_props(name='props.npz'):
    path = os.path.join(SRC, name)
    if not os.path.exists(path):
        return set()
    d = np.load(path)
    names = [str(n) for n in d['names']]
    obj_of = [str(n) for n in d['objects']]
    built = set()
    head = head_frame(d, names, obj_of)   # a head-parts file: the scalp and the centre the hair rigs and gear maps use
    for model in sorted(set(obj_of)):
        groups = {}
        for mi, full in enumerate(names):
            if obj_of[mi] != model:
                continue
            sel = np.nonzero(d['mat'] == mi)[0]
            if len(sel) == 0:
                continue
            mname = full.split('.')[0]
            g = groups.setdefault(mname, {k: [] for k in ('pos', 'nrm', 'uv0', 'uv1')})
            pos, nrm = d['pos'][sel], d['nrm'][sel]
            if model in PROP_FRAMES:
                pos, nrm = PROP_FRAMES[model](pos.astype(np.float64), nrm.astype(np.float64))
            g['pos'].append(pos)
            g['nrm'].append(nrm)
            g['uv0'].append(d['uv0'][sel])
            g['uv1'].append(d['uv1'][sel])
        for g in groups.values():
            for k in g:
                g[k] = np.concatenate(g[k]).astype(np.float32)
        hk = re.match(r'av_hair_(\d+)_[mf]$', model)
        if re.match(r'av_hair_\d+t_[mf]$', model):   # swinging ends (the ponytail's tail, the top knot's ends)
            write_tail(os.path.join(OUT, model), groups, head)
        elif hk and head and int(hk.group(1)) in HAIR_RIGS:   # a style whose free hair moves
            write_hair_rig(os.path.join(OUT, model), groups, head, HAIR_RIGS[int(hk.group(1))])
        else:
            write_mdl(os.path.join(OUT, model + '.mdl'), groups)
        if head and re.match(r'av_(gear|visor)_\d+_[mf]$', model):   # what hair under it has to stay inside
            write_tuck(os.path.join(OUT, model + '.tuck'), groups, head)
        built.add(model)
    return built


# ---------------------------------------------------------------- hair that moves, hair under headgear
# Hair parts are drawn in head space. A style whose hair moves is a v3 skinned model: bone 0 = the head (rigid),
# then the bones of its spring chains, which the game swings (app/src/main.cpp swingBones). <model>.swing lists them:
#   centre x y z                   the rig's centre (no chain point swings into a sphere round it: the head)
#   chain k c follow limit         spring (per unit mass), damping, how much of the head's tilt the rest shape
#                                  follows (1 = gelled, lower = it hangs), the most it swings from rest (degrees)
#   x y z ...                      the chain's rest points: where it leaves the scalp, its joints, its tip
#   exit                           under a helmet that covers its root, it comes out under the helmet's rim (a ponytail)
SWING_BONES = 3   # the ponytail's tail (several ends: 2 each)


def head_frame(d, names, obj_of):
    """A head-parts file (it has the BUZZ CUT): its body key, the scalp (the buzz shell's points) and the centre the
    hair rigs and the gear maps use (head space, metres). None for other files."""
    buzz = [o for o in set(obj_of) if re.match(r'av_hair_1_[mf]$', o)]
    if not buzz:
        return None
    mats = [i for i, o in enumerate(obj_of) if o == buzz[0]]
    scalp = np.unique(np.round(d['pos'][np.isin(d['mat'], mats)].astype(np.float64), 4), axis=0)
    lo, hi = scalp.min(0), scalp.max(0)
    return dict(b=buzz[0][-1], scalp=scalp[::2], C=np.array([0.0, hi[1] - 0.09, (lo[2] + hi[2]) / 2]))


def scalp_distance(p, scalp):
    out = np.empty(len(p))
    for i in range(0, len(p), 1500):
        q = p[i:i + 1500]
        out[i:i + 1500] = np.sqrt(((q[:, None, :] - scalp[None, :, :]) ** 2).sum(-1)).min(1)
    return out


def ramp(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def write_v3(base, groups, weights, note):
    """<base>.mdl as a v3 skinned model; weights(pos, uv1) -> (bone indices n x 4, weights n x 4 summing to 1)."""
    verts, idxs, secs, mats = [], [], [], []
    nv = ni = 0
    for mi, name in enumerate(sorted(groups)):
        g, idx = weld(groups[name])
        n = len(g['pos'])
        v = np.zeros(n, dtype=[('p', '<f4', 3), ('n', '<f4', 3), ('u0', '<f4', 2), ('u1', '<f4', 2), ('c', 'u1', 4),
                               ('bi', 'u1', 4), ('bw', 'u1', 4)])
        v['p'], v['n'], v['u0'], v['u1'] = g['pos'], g['nrm'], g['uv0'], g['uv1']
        v['c'] = 255
        bi, w = weights(g['pos'].astype(np.float64), g['uv1'])
        w8 = np.clip(np.round(w * 255), 0, 255)
        w8[:, 0] += 255 - w8.sum(1)
        v['bi'], v['bw'] = bi.astype(np.uint8), w8.astype(np.uint8)
        verts.append(v)
        idxs.append(idx + nv)
        secs.append((ni, len(idx), mi))
        nv += n
        ni += len(idx)
        mats.append(material(name, None, 1.0))
    V = np.concatenate(verts)
    I = np.concatenate(idxs).astype(np.uint32)
    with open(base + '.mdl', 'wb') as f:
        f.write(b'SMDL' + struct.pack('<5I', 3, len(V), len(I), len(secs), len(mats)))
        f.write(V.tobytes())
        f.write(I.tobytes())
        for s_ in secs:
            f.write(struct.pack('<3I', *s_))
        for sh, tint, em, uv, flags, tex in mats:
            f.write(struct.pack('<I4f4f2I', sh, *tint, *em, uv, flags))
            f.write(tex.encode().ljust(64, b'\0'))
            f.write(b''.ljust(64, b'\0'))
    print('  %-18s %7d verts %7d tris %3d mats (%s)' % (os.path.basename(base) + '.mdl', len(V), len(I) // 3, len(mats), note))


def write_rig_text(base, C, chains, exits=False):
    with open(base + '.swing', 'w', newline='\n') as f:
        f.write('# %s: hair swing rig (tools/build_open_assets.py; the game: app/src/main.cpp swingBones)\n' % os.path.basename(base))
        f.write('centre %.5f %.5f %.5f\n' % tuple(C))
        if exits:
            f.write('exit\n')
        for spring, pts in chains:
            f.write('chain %g %g %g %g\n' % tuple(spring))
            for q in pts:
                f.write('%.5f %.5f %.5f\n' % tuple(q))


TAIL_SPRING = {'3': (110, 7, 0.35, 75), '7': (150, 6, 0.35, 70)}   # the ponytail, the top knot's ends


def write_tail(base, groups, head):
    """Swinging ends (uv1.x = 0 at the root .. 1 at the tip, uv1.y = 1 + which end): a chain per end (one end:
    SWING_BONES segments, several: 2 each), blended across each joint; rest points measured along the middle of
    the geometry."""
    allp = np.concatenate([g['pos'] for g in groups.values()])
    allt = np.concatenate([g['uv1'][:, 0] for g in groups.values()])
    alle = np.concatenate([np.round(g['uv1'][:, 1]) for g in groups.values()]).astype(int) - 1
    ends = int(alle.max()) + 1
    nseg = SWING_BONES if ends == 1 else 2
    style = re.match(r'av_hair_(\d+)t_', os.path.basename(base)).group(1)
    spring = TAIL_SPRING.get(style, TAIL_SPRING['3'])
    chains = []
    for e in range(ends):
        pts = []
        for j in range(nseg + 1):
            tj = j / nseg
            sel = (np.abs(allt - tj) < 0.04) & (alle == e)
            if not sel.any():
                sel = np.argsort(np.abs(allt - tj) + 10 * (alle != e))[:20]
            pts.append(allp[sel].mean(0))
        chains.append((spring, pts))
    C = head['C'] if head else np.zeros(3)
    write_rig_text(base, C, chains, exits=style == '3')

    def weights(pos, uv1):
        e = np.clip(np.round(uv1[:, 1]).astype(int) - 1, 0, ends - 1)
        s = np.clip(uv1[:, 0], 0, 1) * nseg
        j = np.clip(np.floor(s), 0, nseg - 1).astype(int)
        f = s - j
        w_next = np.where(j < nseg - 1, np.clip((f - 0.75) * 2, 0, 0.5), 0.0)
        w_prev = np.where(j > 0, np.clip((0.25 - f) * 2, 0, 0.5), 0.0)
        b0 = 1 + e * nseg   # the end's first bone
        bi = np.zeros((len(pos), 4), int)
        bi[:, 0], bi[:, 1], bi[:, 2] = b0 + j, b0 + np.minimum(j + 1, nseg - 1), b0 + np.maximum(j - 1, 0)
        w = np.zeros((len(pos), 4))
        w[:, 1], w[:, 2] = w_next, w_prev
        w[:, 0] = 1 - w_next - w_prev
        return bi, w
    write_v3(base, groups, weights, 'swing, the head + %d end(s) of %d bones' % (ends, nseg))


# Directions from the rig's centre (head space: x right, y up, -z forward) that name a style's regions.
_FRONT, _BACK, _LEFT, _RIGHT, _TOP = (0, 0.35, -1), (0, 0.1, 1), (-1, 0.1, 0), (1, 0.1, 0), (0, 1, 0.1)
_CREST = tuple((0, math.sin(math.radians(t)), -math.cos(math.radians(t))) for t in (55, 100, 145))   # the mohawk, front to back
# Per style (av_hair_<k>): which hair is free (and how free, 0..1), its regions (one spring chain each, to where the
# region's free hair is) and the springs (k, c, follow, limit).
#   ('scalp', a, b): free from a to b metres off the scalp    ('hang', y0, y1): free from height y0 down to y1
#   ('front', a, b): over the forehead, free from a to b off the scalp (a fringe)
# Where each chain swings from (pivot, one per region or one for all):
#   'base'   on the scalp under the region's free hair: crests, spikes, a bun tilt from their roots
#   'centre' the rig's centre: layers of hair slide over the head (they never sink into it)
#   'hang'   above the ear (no lower than hang_y): hanging hair swings from higher up
HAIR_RIGS = {
    2: dict(free=[('scalp', 0.004, 0.016)], regions=(_FRONT, _TOP), width=0.8, pivot='centre',
            spring=(260, 10, 1.0, 6)),                                                                            # SWEPT
    4: dict(free=[('scalp', 0.006, 0.050)], regions=_CREST, width=0.45, pivot='base', spring=(170, 4.5, 1.0, 25)),  # MOHAWK
    5: dict(free=[('scalp', 0.010, 0.060)], regions=(_FRONT, _LEFT, _RIGHT, _BACK, _TOP), width=0.6, pivot='base',
            spring=(420, 9, 1.0, 10)),                                                                            # SPIKES
    6: dict(free=[('hang', 0.02, -0.09), ('front', 0.020, 0.045)], regions=(_LEFT, _RIGHT, _BACK, _FRONT), width=0.7,
            pivot=('hang', 'hang', 'hang', 'centre'), hang_y=0.02, spring=(95, 7.5, 0.55, 25)),                   # BOB
    7: dict(free=[('scalp', 0.008, 0.035)], regions=(_TOP,), width=1.0, pivot='base', spring=(200, 5, 1.0, 18)),  # TOP KNOT
    8: dict(free=[('scalp', 0.006, 0.020)], regions=(_FRONT, _LEFT, _RIGHT, _BACK), width=0.7, pivot='centre',
            spring=(170, 8, 0.9, 8)),                                                                             # TOUSLED
}


def write_hair_rig(base, groups, head, spec):
    C, scalp = head['C'], head['scalp']

    def freedom(pos):
        ds = scalp_distance(pos, scalp)
        u = pos - C
        u /= np.maximum(np.linalg.norm(u, axis=1, keepdims=True), 1e-9)
        f = np.zeros(len(pos))
        for kind, a, b in spec['free']:
            if kind == 'scalp':
                f = np.maximum(f, ramp(a, b, ds))
            elif kind == 'hang':
                f = np.maximum(f, ramp(a, b, pos[:, 1]))
            elif kind == 'front':
                f = np.maximum(f, ramp(a, b, ds) * np.clip((-u[:, 2] - 0.55) / 0.25, 0, 1))
        return f, u

    def membership(u, dirs):
        D = np.array(dirs, float)
        D /= np.linalg.norm(D, axis=1, keepdims=True)
        ang = np.arccos(np.clip(u @ D.T, -1, 1))
        m = np.exp(-(ang / spec['width']) ** 2) + 1e-6
        return m / m.sum(1, keepdims=True)

    # the chains from all of the style's geometry
    allp = np.concatenate([g['pos'] for g in groups.values()]).astype(np.float64)
    allp = np.unique(np.round(allp, 5), axis=0)
    f, u = freedom(allp)
    dirs = list(spec['regions'])
    while True:   # drop regions with (almost) no free hair
        m = membership(u, dirs)
        mass = (m * f[:, None]).sum(0)
        keep = mass > 0.04 * mass.sum()
        if keep.all():
            break
        dirs = [d_ for d_, k_ in zip(dirs, keep) if k_]
    modes = spec['pivot'] if isinstance(spec['pivot'], tuple) else (spec['pivot'],) * len(spec['regions'])
    modes = [mo for d_, mo in zip(spec['regions'], modes) if d_ in dirs]
    chains = []
    for i in range(len(dirs)):
        wp = m[:, i] * f * (1 - f)
        wt = m[:, i] * f * f
        pivot = (allp * wp[:, None]).sum(0) / wp.sum()   # where the region's hair comes free
        tip = (allp * wt[:, None]).sum(0) / wt.sum()
        if modes[i] == 'centre':
            pivot = C.copy()
        elif modes[i] == 'base':   # down onto the scalp along the ray from the centre
            ray = (pivot - C) / np.linalg.norm(pivot - C)
            on = scalp[np.argsort(np.linalg.norm(np.cross(scalp - C, ray), axis=1) + 10 * (np.dot(scalp - C, ray) < 0))[:8]]
            pivot = C + ray * np.linalg.norm(on - C, axis=1).mean()
        elif modes[i] == 'hang':
            pivot[1] = max(pivot[1], spec['hang_y'])
        chains.append((spec['spring'], [pivot, tip]))
        print('    chain %d: pivot %s tip %s (%.1f cm), free hair %.0f verts' % (
            i + 1, np.round(pivot, 3), np.round(tip, 3), np.linalg.norm(tip - pivot) * 100, (m[:, i] * f).sum()))
    write_rig_text(base, C, chains)

    def weights(pos, uv1):
        fv, uv = freedom(pos)
        mv = membership(uv, dirs)
        top = np.argsort(-mv, axis=1)[:, :2]
        m2 = np.take_along_axis(mv, top, 1)
        m2 /= m2.sum(1, keepdims=True)
        bi = np.zeros((len(pos), 4), int)
        w = np.zeros((len(pos), 4))
        bi[:, 1:3] = top + 1
        w[:, 0] = 1 - fv
        w[:, 1:3] = fv[:, None] * m2
        return bi, w
    write_v3(base, groups, weights, 'swing, the head + %d chains' % len(chains))


# Hair under headgear: for each direction from the head's centre (TUCK_W x TUCK_H cells, longitude x latitude),
# the radius hair has to stay inside (the gear's nearest surface, less a margin; one cell past the gear's edges too, so
# hair tucks under a rim). The game combines a helmet's and a face shield's maps and the hair's vertex shader pulls
# hair inside (app/src/render.cpp uTuck). <model>.tuck: 'TUCK', u32 W, u32 H, f32 centre xyz, W*H bytes (rows from
# straight down to straight up; 255 = nothing there), radius = TUCK_R0 + byte / 255 * (TUCK_R1 - TUCK_R0).
TUCK_W, TUCK_H, TUCK_R0, TUCK_R1 = 64, 32, 0.05, 0.55


def write_tuck(path, groups, head):
    C = head['C']
    tri = np.concatenate([g['pos'] for g in groups.values()]).astype(np.float64).reshape(-1, 3, 3)
    n = 6
    bary = np.array([(a / n, b / n, 1 - (a + b) / n) for a in range(n + 1) for b in range(n + 1 - a)])
    pts = np.einsum('sk,tkd->tsd', bary, tri).reshape(-1, 3) - C
    r = np.linalg.norm(pts, axis=1)
    az = np.arctan2(pts[:, 0], pts[:, 2])
    el = np.arcsin(np.clip(pts[:, 1] / np.maximum(r, 1e-9), -1, 1))
    i = np.floor((az + math.pi) / (2 * math.pi) * TUCK_W).astype(int) % TUCK_W
    j = np.clip(np.floor((el + math.pi / 2) / math.pi * TUCK_H).astype(int), 0, TUCK_H - 1)
    lim = np.full(TUCK_H * TUCK_W, np.inf)
    np.minimum.at(lim, j * TUCK_W + i, r - 0.003)
    lim = lim.reshape(TUCK_H, TUCK_W)
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
    lim = np.where(covered, lim, near)
    v = np.where(np.isfinite(lim), np.round(np.clip((lim - TUCK_R0) / (TUCK_R1 - TUCK_R0), 0, 1) * 255), 255).astype(np.uint8)
    with open(path, 'wb') as f:
        f.write(b'TUCK' + struct.pack('<2I3f', TUCK_W, TUCK_H, *C))
        f.write(v.tobytes())
    print('  %-18s covers %3.0f%% of directions, radius %.3f .. %.3f m' % (
        os.path.basename(path), covered.mean() * 100, lim[covered].min(), lim[covered].max()))


FACE_KEYS = ('m1', 'm2', 'm3', 'f1', 'f2', 'f3')   # tools/blender/make_body.py FACES (FACE 1 is the base head)


# ---------------------------------------------------------------- main
def run_blender(script, args, factory=True):
    """factory=False keeps the user's Blender extensions (make_body.py needs the MPFB2 add-on)."""
    r = subprocess.run([BLENDER, '-b'] + (['--factory-startup'] if factory else []) +
                       ['-P', os.path.join(ROOT, 'tools', 'blender', script), '--'] + args, capture_output=True, text=True)
    if r.returncode != 0 or 'Traceback' in r.stdout + r.stderr:
        print(r.stdout[-3000:], r.stderr[-3000:])
        raise SystemExit('blender failed: ' + script)
    for line in r.stdout.splitlines():
        if line.startswith('exported'):
            print('  ' + line)


def main():
    preview = '--preview' in sys.argv
    os.makedirs(SRC, exist_ok=True)
    os.makedirs(OUT, exist_ok=True)
    print('textures')
    subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'gen_open_textures.py')], check=True,
                   stdout=subprocess.DEVNULL)
    if '--skip-blender' not in sys.argv:
        if '--skip-arena' not in sys.argv:   # --skip-arena: keep the baked court/stadium, rebuild only the props
            print('blender: arena' + (' (preview quality)' if preview else ''))
            run_blender('make_arena.py', [SRC] + (['preview'] if preview else []))
        if os.path.exists(os.path.join(ROOT, 'tools', 'blender', 'make_props.py')):
            print('blender: props')
            run_blender('make_props.py', [os.path.join(SRC, 'props.npz')])
        print('blender: MPFB2 bodies (needs the MPFB extension in Blender)')
        run_blender('make_body.py', [os.path.join(SRC, 'body')], factory=False)
        for key in ('m', 'f'):
            print('blender: head parts on the %s head' % key)
            run_blender('make_avatar.py', [os.path.join(SRC, 'avatar_head_%s.npz' % key), '--head', key])
        for key in FACE_KEYS:   # FACE 2..4: face shields and masks fitted to each face (hair and helmets fit them all)
            print('blender: face shields and masks on face', key)
            run_blender('make_avatar.py', [os.path.join(SRC, 'avatar_head_%s.npz' % key), '--head', key, 'av_visor', 'av_mask'])
    print('models')
    court, lm_c = load_npz(os.path.join(SRC, 'court.npz'))
    env, lm_e = load_npz(os.path.join(SRC, 'environment.npz'))
    # one exposure for both lightmaps so the court and stadium match
    lit = np.concatenate([lm_c[lm_c.sum(2) > 1e-4].reshape(-1, 3), lm_e[lm_e.sum(2) > 1e-4].reshape(-1, 3)])
    exposure = 0.42   # absolute: lit surfaces vs. light sources (emitters keep their own display colour)
    print('  lightmap radiance p50 %.3f p90 %.3f p99 %.3f' % tuple(np.percentile(lit.max(1), [50, 90, 99])))
    process_lightmap('open_court_lm', lm_c, exposure)
    process_lightmap('open_env_lm', lm_e, exposure, out_size=1024)
    floor = {k: v for k, v in court.items() if k.startswith('floor_')}
    pulses = {k: v for k, v in court.items() if k.startswith('pulse_')}
    fx_court = {k: v for k, v in court.items() if palette.P.get(k, {}).get('kind') == 'fx' and k not in pulses}
    arena = {k: v for k, v in court.items() if k not in floor and k not in fx_court and k not in pulses}
    fx_env = {k: v for k, v in env.items() if palette.P.get(k, {}).get('kind') == 'fx'}
    twinkle = {k: v for k, v in fx_env.items() if k.startswith('twinkle')}
    static_env_fx = {k: v for k, v in fx_env.items() if k not in twinkle}
    environment = {k: v for k, v in env.items() if k not in fx_env}
    write_mdl(os.path.join(OUT, 'arena.mdl'), arena, 'open_court_lm', 2.0)
    # PASSTHROUGH > OUTLINE: only the court's neon edges and its floor lines, over your room
    outline = {k: v for k, v in court.items() if k in ('neon_white', 'neon_t0', 'neon_t1', 'foul_t0', 'foul_t1', 'center_mark')}
    write_mdl(os.path.join(OUT, 'court_outline.mdl'), outline, 'open_court_lm', 2.0)
    write_mdl(os.path.join(OUT, 'floor.mdl'), floor, 'open_court_lm', 2.0)
    write_mdl(os.path.join(OUT, 'environment.mdl'), environment, 'open_env_lm', 2.0)
    # the court's own glow layers and the stadium's (light beams, halos) apart: PASSTHROUGH hides the stadium
    write_mdl(os.path.join(OUT, 'court_fx.mdl'), fx_court, mats_override={k: material(k) for k in fx_court})
    write_mdl(os.path.join(OUT, 'stadium_fx.mdl'), static_env_fx, mats_override={k: material(k) for k in static_env_fx})
    write_mdl(os.path.join(OUT, 'env_fx.mdl'), twinkle)
    write_mdl(os.path.join(OUT, 'court_pulse.mdl'), pulses)
    refl, rmats = mirrored(court, 0.30)
    refl_e, rmats_e = mirrored(env, 0.22)
    write_mdl(os.path.join(OUT, 'floor_reflect.mdl'), refl, mats_override=rmats)          # the court in the floor
    write_mdl(os.path.join(OUT, 'floor_reflect_env.mdl'), refl_e, mats_override=rmats_e)  # the stadium in the floor
    write_collision(os.path.join(OUT, 'arena.col'))
    built = {'arena', 'environment'} | build_props() | build_props('avatar_head_m.npz') | build_props('avatar_head_f.npz')
    for key in FACE_KEYS:
        built |= build_props('avatar_head_%s.npz' % key)
    print('skinned bodies')
    subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'build_body_parts.py')], check=True)
    # parts of the old formula-built avatars (before the MPFB bodies)
    old = ('av_head.', 'av_neck.', 'av_torso_', 'av_upper_', 'av_fore_', 'av_waist', 'av_lower_')
    for f in os.listdir(OUT):
        unsuffixed = f.startswith(('av_hair_', 'av_gear_', 'av_visor_', 'av_mask_')) and f[:-4].count('_') == 2
        if f.endswith('.mdl') and (f.startswith(old) or unsuffixed):
            os.remove(os.path.join(OUT, f))
            print('  removed old part', f)
    for f in sorted(os.listdir(CLEAN)):
        src = os.path.join(CLEAN, f)
        if os.path.isfile(src) and f.endswith('.mdl') and f[:-4] not in built:
            shutil.copy2(src, os.path.join(OUT, f))
            print('  %-18s (from assets_clean)' % f)
    print('audio')
    subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'gen_open_audio.py')], check=True,
                   stdout=subprocess.DEVNULL)
    print('done ->', OUT)


if __name__ == '__main__':
    main()
