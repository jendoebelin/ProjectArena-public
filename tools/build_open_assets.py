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
import os
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
        write_mdl(os.path.join(OUT, model + '.mdl'), groups)
        built.add(model)
    return built


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
