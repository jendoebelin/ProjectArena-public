"""The new clothing from ProjectArena-public PR #25 (plan item 23) into app/assets_open.

Usage: python -I tools/import_pr25.py <the PR's clothing/assets.zip, extracted> [--fit candidate|liked]

His models come with fixed colours (the material tints, no colour channels); every other avatar part takes the
player's colours, so each material is given a channel by its tint and drawn white under it (render.cpp: the tint is
multiplied by the channel's colour): the dark cloth MAIN COLOR, the light panels and the bronze TRIM COLOR, the darker
bronze the metal trim, both glows (cyan, violet) GLOW. The near-black eye slits and the skin keep theirs. The kilt's
tartan: its slate cloth MAIN COLOR, the lighter bands TRIM COLOR darkened, the inside and the shorts under it the main
colour darkened.

The fits for the bodies the game has (MPFB): the opaque long shorts', the tennis skirt's and the kilt's _original, the
helmets' candidate (the same file as liked for the masculine head; for the feminine head the two differ). The skirts
also get their sway (sway() below). And his refits of our own helmets to our heads (headwear-public: the RACER HELMET
and the CAP sit closer; HEADPHONES kept as they were), as they are (they keep our colour channels).
"""
import argparse, os, shutil, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'app', 'assets_open')
CH_PRIMARY, CH_SECONDARY, CH_GLOW, CH_SKIN, CH_TRIM = 1, 2, 3, 4, 7
SH_ADDITIVE = 3


def channel_of(shader, tint, flags, tartan=False):
    """The channel a material of his gets and its tint under it, or None to keep it as it is."""
    if (flags >> 8) & 15:
        return None                                   # (already one: the skin)
    r, g, b = tint[:3]
    if tartan == 'nightforge' and shader != SH_ADDITIVE and 0.06 < max(r, g, b) < 0.2:
        return CH_SECONDARY, 0.5                      # NIGHTFORGE's graphite panels: TRIM COLOR darkened
    if tartan is True and shader != SH_ADDITIVE and max(r, g, b) < 0.2:
        # the kilt: its slate cloth (the outside) MAIN COLOR, the lighter bands TRIM COLOR darkened, the darker inside
        # and the shorts under it the main colour darkened
        m = max(r, g, b)
        return (CH_PRIMARY, 0.5) if m < 0.06 else (CH_PRIMARY, 1.0) if m < 0.15 else (CH_SECONDARY, 0.55)
    if shader == SH_ADDITIVE:
        return CH_GLOW                                # cyan and violet glows
    if max(r, g, b) < 0.025:
        return None                                   # the near-black eye slits: as they are
    if abs(r - 0.70) < 0.03 and abs(g - 0.76) < 0.03 and abs(b - 0.79) < 0.03:
        return CH_SECONDARY                           # the light panels
    if r > 0.55 and g > 0.5:
        return CH_SECONDARY                           # the light bronze
    if r > 0.3 and r > b * 1.8:
        return CH_TRIM                                # the darker bronze: the metal trim
    if max(r, g, b) < 0.2:
        return CH_PRIMARY                             # the dark cloth / graphite shell
    raise SystemExit('a material of an unknown colour: shader %d tint %s' % (shader, tint))


def recolour(src, dst, tartan=False, body=None):
    if body:   # (made on the PR #25 bodies as they come, arms out: posed as the game binds them first)
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import repose_bodies
        repose_bodies.repose_mdl(src, dst, body)
        src = dst
    b = bytearray(open(src, 'rb').read())
    ver, nv, ni, ns, nm = struct.unpack_from('<5I', b, 4)
    off = 24 + nv * (52 if ver >= 3 else 44) + ni * 4 + ns * 12
    done = []
    for i in range(nm):
        sh, *rest = struct.unpack_from('<I4f4f2I', b, off)
        tint, em, uv, flags = rest[0:4], rest[4:8], rest[8], rest[9]
        ch = channel_of(sh, tint, flags, tartan)
        if ch is not None:
            ch, k = ch if isinstance(ch, tuple) else (ch, 1.0)
            struct.pack_into('<4f', b, off + 4, k, k, k, tint[3])
            struct.pack_into('<I', b, off + 4 + 32 + 4, (flags & ~(15 << 8)) | (ch << 8))
            done.append(ch)
        off += 4 + 32 + 8 + 64 + 64
    assert off == len(b), (src, off, len(b))
    open(dst, 'wb').write(bytes(b))
    return done


# ---------------------------------------------------------------- the skirts' sway (TENNIS SKIRT, KILT)
# His swish (inspector/swish.mjs) moves a skirt's vertices round the hips by the avatar's own swing (x, z, twist), each
# by its weight, then pushes any that went inside the body back out to the body's radius (+ its layer's thickness and
# a little room). The game does that per vertex in the shader (render.cpp uSwish), so each vertex carries its weight
# and the room it keeps from the body in its second UV (u1: weight, extra), and the body's radii go in <model>.swish:
#   'SWSH', u32 kind (0 tennis, 1 kilt), f32 waistY, hemY, centre x, z, maxDisplacement, first row's y, row step,
#   u32 rows, cols, then rows x cols f32 radii (angle = atan2(x - cx, -(z - cz)), 0 .. 2 pi over the columns)
TUCK, TUCK_FULL, TUCK_END = 0.008, 0.008, 0.035   # the skirts' waistband in under the shirt (m: by, down to, out by)


def radial_radius(rows, ys, y, theta):
    import numpy as np
    n = rows.shape[1]
    lo = np.clip(np.searchsorted(ys, y) - 1, 0, len(ys) - 1)
    hi = np.clip(lo + 1, 0, len(ys) - 1)
    lo = np.where(y <= ys[0], 0, lo); hi = np.where(y <= ys[0], 0, hi)
    lo = np.where(y >= ys[-1], len(ys) - 1, lo); hi = np.where(y >= ys[-1], len(ys) - 1, hi)
    u = ((theta / (2 * np.pi) + 1) % 1) * n
    i = np.floor(u).astype(int) % n; j = (i + 1) % n; f = u - np.floor(u)
    ra = rows[lo, i] * (1 - f) + rows[lo, j] * f
    rb = rows[hi, i] * (1 - f) + rows[hi, j] * f
    t = np.where(hi == lo, 0, np.clip((y - ys[lo]) / np.maximum(1e-9, ys[hi] - ys[lo]), 0, 1))
    return ra + (rb - ra) * t


def sway(src_mdl, src_motion, dst_mdl, dst_swish):
    import json, numpy as np
    meta = json.load(open(src_motion))
    done = recolour(src_mdl, dst_mdl, tartan=meta['type'] == 'kilt')
    b = bytearray(open(dst_mdl, 'rb').read())
    ver, nv, ni, ns, nm = struct.unpack_from('<5I', b, 4)
    assert ver >= 3 and nv == len(meta['clothWeight']), (src_mdl, nv)
    stride = 52
    P = np.array([struct.unpack_from('<3f', b, 24 + k * stride) for k in range(nv)])
    w = np.clip(np.array(meta['clothWeight'], float), 0, 1)
    assert meta.get('weightsAreFinal'), src_motion
    cx, cz = (meta.get('radialCenterXZ') or meta['centerXZ'])
    # the waistband tucked in under the shirt (the owner, 2026-11-17, her video: the team kit's jersey and the skirt cut
    # through each other in a ragged band at the waist: the skirt's top sat as far out as the jersey's hem, and above
    # it). Every top reaches 8 mm below the skirt's top (the jersey 17 mm), so its top is pulled 8 mm in toward the
    # hips' centre and comes back out by 3.5 cm down: hidden under any top, out from under it where the top ends
    d = P[:, 1].max() - P[:, 1]
    s = np.clip((d - TUCK_FULL) / (TUCK_END - TUCK_FULL), 0.0, 1.0)
    rr = np.maximum(np.hypot(P[:, 0] - cx, P[:, 2] - cz), 1e-6)
    k_in = TUCK * (1.0 - s * s * (3.0 - 2.0 * s)) / rr
    P[:, 0] -= (P[:, 0] - cx) * k_in
    P[:, 2] -= (P[:, 2] - cz) * k_in
    for k in range(nv):
        struct.pack_into('<3f', b, 24 + k * stride, *P[k])
    # each layer's thickness over its innermost vertex (vertices of a layer share the authored angle / height samples)
    layer = np.zeros(nv)
    t, ang, kind = meta['clothT'], meta['clothAngle'], meta['clothKind']
    groups = {}
    r = np.hypot(P[:, 0] - cx, P[:, 2] - cz)
    for k in range(nv):
        if kind[k]:
            groups.setdefault((round(t[k] * 1e6), round(ang[k] * 1e6), kind[k]), []).append(k)
    for ids in groups.values():
        layer[ids] = r[ids] - r[ids].min()
    prof = sorted(meta['radialCollisionProfile'], key=lambda row: row['y'])
    ys = np.array([row['y'] for row in prof]); rows = np.array([row['radii'] for row in prof], float)
    step = np.diff(ys)
    assert np.allclose(step, step[0], atol=1e-6), 'rows not evenly spaced'
    reserve = np.maximum(0, r - radial_radius(rows, ys, P[:, 1], np.arctan2(P[:, 0] - cx, -(P[:, 2] - cz))) - layer - 1e-7)
    extra = np.minimum(meta.get('radialSafetyMargin', 0.003), reserve) + layer
    for k in range(nv):
        struct.pack_into('<2f', b, 24 + k * stride + 32, float(w[k]), float(extra[k]) if w[k] > 0 else 0.0)
    open(dst_mdl, 'wb').write(bytes(b))
    kinds = {'tennis': 0, 'kilt': 1}
    with open(dst_swish, 'wb') as f:
        f.write(b'SWSH')
        f.write(struct.pack('<I7f2I', kinds[meta['type']], meta['waistY'], meta['hemY'], cx, cz, meta['maxDisplacement'],
                            float(ys[0]), float(step[0]), rows.shape[0], rows.shape[1]))
        f.write(rows.astype('<f4').tobytes())
    return done, int((w > 0).sum())


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('src', help="the PR's clothing/assets.zip, extracted")
    ap.add_argument('--bodies', default='q', choices=['q', 'mpfb'],
                    help="whose bodies the game has: q (PR #25's, since 2026-11-17: his liked fits) or mpfb (_original)")
    ap.add_argument('--out', default=OUT, help='where to write them (default app/assets_open)')
    a = ap.parse_args()
    q = a.bodies == 'q'
    fit, lower = ('liked', '') if q else ('candidate', '_original')
    jobs = []   # (source, destination, the body it was made on as it comes, or None)
    for s in 'mf':
        jobs.append(('lowers/av_opaque_bottom_%s%s.mdl' % (s, lower), 'av_bottom_4_%s.mdl' % s, s if q else None))   # LONG SHORTS
        for n, folder, name in ((7, 'horned', 'av_horned'), (8, 'vector-helmet', 'av_vector')):        # HORNED, VECTOR
            jobs.append(('%s/%s_%s_%s.mdl' % (folder, name, fit, s), 'av_gear_%d_%s.mdl' % (n, s), None))
            jobs.append(('%s/%s_%s_%s.tuck' % (folder, name, fit, s), 'av_gear_%d_%s.tuck' % (n, s), None))
        # his fits of our helmets (q: to his heads, all six; mpfb: to ours, HEADPHONES kept as it was): as they are
        for n in ((1, 2, 3, 4, 5, 6) if q else (1, 2, 3, 4, 6)):
            for ext in ('mdl', 'tuck'):
                jobs.append(('headwear-%s/av_gear_%d_%s.%s' % ('liked' if q else 'public', n, s, ext), 'av_gear_%d_%s.%s.copy' % (n, s, ext), None))
        if q:   # VECTOR and NIGHTFORGE (suits 14, 15): their tops, and their own versions of LOWER BODY 1-4
            for n, name in ((14, 'vector'), (15, 'nightforge')):
                jobs.append(('base/av_%s_top_%s.mdl' % (name, s), 'av_top_%d_%s.mdl' % (n, s), s))
                for k in (1, 2, 3, 4):
                    jobs.append(('%s/av_%s_bottom_%d_%s.mdl' % (name, name, k, s), 'av_sbottom_%d_%d_%s.mdl' % (n, k, s), s))
    for src, dst, body in jobs:
        p = os.path.join(a.src, src)
        if not os.path.exists(p):
            raise SystemExit('missing ' + p)
        if dst.endswith('.copy'):
            shutil.copyfile(p, os.path.join(a.out, dst[:-5]))
            print(dst[:-5], '<-', src)
        elif dst.endswith('.mdl'):
            style = 'nightforge' if 'nightforge' in src else False
            print(dst, '<-', src, 'channels', recolour(p, os.path.join(a.out, dst), style, body), '(posed)' if body else '')
        else:
            shutil.copyfile(p, os.path.join(a.out, dst))
            print(dst, '<-', src)
    for s in 'mf':   # TENNIS SKIRT, KILT (lower body 5, 6): with their sway
        for n, name in ((5, 'tennis'), (6, 'kilt')):
            src = os.path.join(a.src, 'lowers', 'av_%s_lower_%s%s' % (name, s, lower))
            dst = os.path.join(a.out, 'av_bottom_%d_%s' % (n, s))
            done, moving = sway(src + '.mdl', src + '.motion.json', dst + '.mdl', dst + '.swish')
            print('av_bottom_%d_%s.mdl/.swish <-' % (n, s), os.path.basename(src), 'channels', done, 'swaying vertices', moving)


if __name__ == '__main__':
    main()
