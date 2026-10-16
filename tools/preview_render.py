"""Offline software preview of the baked Quest assets (no headset needed).
Renders app/assets/*.mdl with the same shading rules as app/src/render.cpp (approximately) from a
camera pose, to validate transforms, orientation and materials.
Usage: python -I tools/preview_render.py <out.png> [eye_x eye_y eye_z look_x look_y look_z] [--extra name@x,y,z,yawdeg ...]
"""
import os, struct, sys
import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.environ.get('SPARC_ASSETS', os.path.join(ROOT, 'app', 'assets'))
W, H = 960, 540
TEAM = [np.array([0.1, 0.7, 1.0]), np.array([1.0, 0.4, 0.1])]
_tex = {}


def load_tex(rel):
    if not rel:
        return None
    if rel in _tex:
        return _tex[rel]
    p = os.path.join(ASSETS, 'tex', rel.replace('/', '_') + '.tex')
    if not os.path.exists(p):
        _tex[rel] = None
        return None
    b = open(p, 'rb').read()
    w, h, _ = struct.unpack_from('<3I', b, 4)
    _tex[rel] = np.frombuffer(b[16:], np.uint8).reshape(h, w, 4).astype(np.float32) / 255
    return _tex[rel]


def load_mdl(name):
    b = open(os.path.join(ASSETS, name + '.mdl'), 'rb').read()
    ver, nv, ni, ns, nm = struct.unpack_from('<5I', b, 4)
    off = 24
    dt = [('p', '<f4', 3), ('n', '<f4', 3), ('u0', '<f4', 2), ('u1', '<f4', 2), ('c', 'u1', 4)]
    if ver >= 3:   # skinned: 4 bone indices + 4 weights (bytes)
        dt += [('bi', 'u1', 4), ('bw', 'u1', 4)]
    v = np.frombuffer(b, dtype=dt, count=nv, offset=off)
    off += nv * (52 if ver >= 3 else 44)
    idx = np.frombuffer(b, '<u4', ni, off); off += ni * 4
    secs = [struct.unpack_from('<3I', b, off + i * 12) for i in range(ns)]; off += ns * 12
    mats = []
    for i in range(nm):
        sh, *rest = struct.unpack_from('<I4f4f2I', b, off); off += 4 + 32 + 8
        tint, em, uv, flags = rest[0:4], rest[4:8], rest[8], rest[9]
        d = b[off:off + 64].split(b'\0')[0].decode(); off += 64
        m = b[off:off + 64].split(b'\0')[0].decode(); off += 64
        mats.append(dict(shader=sh, tint=np.array(tint), em=np.array(em), uv=uv, flags=flags, diffuse=load_tex(d), mask=load_tex(m)))
    return v, idx, secs, mats


def look_at(eye, target):
    f = target - eye; f /= np.linalg.norm(f)
    r = np.cross(f, [0, 1, 0]); r /= np.linalg.norm(r)
    u = np.cross(r, f)
    V = np.eye(4)
    V[0, :3], V[1, :3], V[2, :3] = r, u, -f
    V[:3, 3] = -V[:3, :3] @ eye
    return V


def persp(fovy, aspect, n, f):
    t = 1 / np.tan(np.radians(fovy) / 2)
    P = np.zeros((4, 4))
    P[0, 0], P[1, 1] = t / aspect, t
    P[2, 2], P[2, 3], P[3, 2] = -(f + n) / (f - n), -2 * f * n / (f - n), -1
    return P


def sample(tex, uv):
    """Bilinear, wrapping."""
    h, w = tex.shape[:2]
    x = np.mod(uv[:, 0], 1) * w - 0.5
    y = np.mod(uv[:, 1], 1) * h - 0.5
    x0, y0 = np.floor(x).astype(int), np.floor(y).astype(int)
    fx, fy = (x - x0)[:, None], (y - y0)[:, None]
    x0 %= w; y0 %= h
    x1, y1 = (x0 + 1) % w, (y0 + 1) % h
    return (tex[y0, x0] * (1 - fx) * (1 - fy) + tex[y0, x1] * fx * (1 - fy) + tex[y1, x0] * (1 - fx) * fy + tex[y1, x1] * fx * fy)


def srgb_lin(c):
    return np.power(np.clip(c, 0, 1), 2.2)


def arena_env(r):
    band = np.exp(-((r[:, 1] - 0.10) / 0.09) ** 2)
    cols = (0.5 + 0.5 * np.sin(np.arctan2(r[:, 2], r[:, 0]) * 26.0)) ** 6
    t = np.clip((r[:, 1] + 0.2) / 1.2, 0, 1); t = t * t * (3 - 2 * t)
    c = np.array([0.004, 0.006, 0.012]) * (1 - t[:, None]) + np.array([0.04, 0.055, 0.08]) * t[:, None]
    sides = np.clip((np.abs(r[:, 2]) - 0.35) / 0.5, 0, 1); sides = sides * sides * (3 - 2 * sides)
    return c + np.array([0.42, 0.47, 0.55]) * (band * sides * (0.15 + 0.85 * cols))[:, None]


def render(models, eye, target, out, fov=90, bg=(0.01, 0.012, 0.02)):
    """models: (mdl, M[, alphaMul[, tintMul rgb[, emissiveAdd rgb[, opts]]]]) like the app's DrawParams;
    opts (avatars): channels (8x3 colours by material channel), rim (rgba), pattern (mask texture), mirrored."""
    eye = list(eye)
    VP = persp(fov, W / H, 0.05, 2000) @ look_at(np.array(eye, float), np.array(target, float))
    color = np.zeros((H, W, 3), np.float32) + np.array(bg, np.float32)
    depth = np.full((H, W), np.inf, np.float32)
    blended = []
    light = np.array([0.3, -1.0, 0.2]); light /= np.linalg.norm(light)
    for entry in models:
        (v, idx, secs, mats), M = entry[0], entry[1]
        alpha_mul = entry[2] if len(entry) > 2 else 1.0
        tint_mul = np.array(list(entry[3]) + [1.0]) if len(entry) > 3 else np.ones(4)
        em_add = np.array(entry[4], float) if len(entry) > 4 else np.zeros(3)
        opts = entry[5] if len(entry) > 5 else {}
        chans = opts.get('channels')
        rim = np.asarray(opts.get('rim', (0, 0, 0, 0)), float)
        pat = opts.get('pattern')
        mirrored = opts.get('mirrored', False)
        bones = opts.get('bones')
        if bones is not None and 'bi' in v.dtype.names:   # linear blend skinning, like the app's vertex shader
            S = np.einsum('vk,vkij->vij', v['bw'].astype(np.float64) / 255.0, np.asarray(bones)[v['bi']])
            ph = np.einsum('vij,vj->vi', S, np.c_[v['p'], np.ones(len(v))])
            P = ph @ M.T
            N = np.einsum('vij,vj->vi', S[:, :3, :3], v['n'].astype(np.float64)) @ M[:3, :3].T
        else:
            P = np.c_[v['p'], np.ones(len(v))] @ M.T
            N = v['n'] @ M[:3, :3].T
        clip = P @ VP.T
        for first, count, mi in secs:
            mat = mats[mi]
            tri = idx[first:first + count].reshape(-1, 3)
            blend = mat['shader'] in (2, 4) or alpha_mul < 0.999
            for t in tri:
                c = clip[t]
                if np.any(c[:, 3] < 0.05):
                    continue
                if not (mat['flags'] & 1) and not blend:   # back-face cull opaque single-sided geometry, like the app
                    fn = np.cross(P[t[1], :3] - P[t[0], :3], P[t[2], :3] - P[t[0], :3])
                    if (fn @ (np.array(eye, float) - P[t[0], :3]) < 0) != mirrored:
                        continue
                ndc = c[:, :3] / c[:, 3:4]
                sx = (ndc[:, 0] * 0.5 + 0.5) * W
                sy = (1 - (ndc[:, 1] * 0.5 + 0.5)) * H
                x0, x1 = int(max(0, np.floor(sx.min()))), int(min(W - 1, np.ceil(sx.max())))
                y0, y1 = int(max(0, np.floor(sy.min()))), int(min(H - 1, np.ceil(sy.max())))
                if x0 > x1 or y0 > y1:
                    continue
                area = (sx[1] - sx[0]) * (sy[2] - sy[0]) - (sx[2] - sx[0]) * (sy[1] - sy[0])
                if abs(area) < 1e-9:
                    continue
                xs, ys = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
                w0 = ((sx[1] - xs) * (sy[2] - ys) - (sx[2] - xs) * (sy[1] - ys)) / area
                w1 = ((sx[2] - xs) * (sy[0] - ys) - (sx[0] - xs) * (sy[2] - ys)) / area
                w2 = 1 - w0 - w1
                inside = (w0 >= 0) & (w1 >= 0) & (w2 >= 0)
                if not inside.any():
                    continue
                z = w0 * ndc[0, 2] + w1 * ndc[1, 2] + w2 * ndc[2, 2]
                yy, xx = np.nonzero(inside)
                yy += y0; xx += x0
                zz = z[inside]
                vis = zz < depth[yy, xx]
                if not vis.any():
                    continue
                yy, xx, zz = yy[vis], xx[vis], zz[vis]
                bw = np.stack([w0[inside][vis], w1[inside][vis], w2[inside][vis]], 1)
                uvk = 'u1' if mat['uv'] == 1 else 'u0'
                uv = bw @ v[uvk][t].astype(np.float64)
                nrm = bw @ N[t]
                nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-6)
                ch = (mat['flags'] >> 8) & 15
                mt = mat['tint'] * tint_mul
                if chans is not None and ch:
                    mt = mt * np.r_[chans[ch], 1.0]
                base = np.tile(mt, (len(zz), 1))
                if mat['diffuse'] is not None:
                    tx = sample(mat['diffuse'], uv)
                    base = base * np.c_[srgb_lin(tx[:, :3]), tx[:, 3:4]]
                base[:, 3] *= alpha_mul
                if mat['shader'] == 5:   # mirrors render.cpp SH_LIGHTMAP
                    wp = bw @ P[t][:, :3]
                    vdir = np.array(eye, float) - wp
                    vdir /= np.maximum(np.linalg.norm(vdir, axis=1, keepdims=True), 1e-6)
                    nn = np.where((np.sum(nrm * vdir, 1) < 0)[:, None], -nrm, nrm)
                    rr = -vdir + 2 * np.sum(nn * vdir, 1)[:, None] * nn
                    fres = (1 - np.clip(np.sum(nn * vdir, 1), 0, 1)) ** 5
                    f0 = mat['em'][:3]
                    F = f0[None, :] + (1 - f0[None, :]) * fres[:, None] * 0.6
                    col = base[:, :3] + arena_env(rr) * F * mat['em'][3]
                    if os.environ.get('DBGPX'):
                        qx, qy = [int(t) for t in os.environ['DBGPX'].split(',')]
                        hit = np.nonzero((xx == qx) & (yy == qy))[0]
                        for k in hit:
                            print('DBG uv', uv[k].round(4), 'base', base[k].round(4), 'col', col[k].round(4), 'n', nrm[k].round(2), 'z', zz[k])
                elif mat['shader'] == 1:   # mirrors render.cpp COURTMASK
                    vc = bw @ (v['c'][t].astype(np.float64) / 255.0)
                    wp = bw @ P[t][:, :3]
                    if mat['mask'] is not None:
                        m = sample(mat['mask'], uv)
                    else:
                        inside = ((np.abs(wp[:, 2]) <= 2.7) & (np.abs(wp[:, 0]) <= 9.7) & (wp[:, 1] <= 4.2)).astype(float)
                        inside *= np.where(np.abs(wp[:, 0]) > 9.4, 0.35, 1.0)
                        m = np.stack([np.clip((-wp[:, 0] - 4.6) / 4.9, 0, 1) * inside, vc[:, 3],
                                      np.clip((wp[:, 0] - 4.6) / 4.9, 0, 1) * inside, np.ones(len(wp))], 1)
                    alb = base[:, :3] * vc[:, :3] ** 2
                    metal = float(np.clip(mat['em'][3], 0, 1))
                    vdir = np.array(eye, float) - wp
                    vdir /= np.maximum(np.linalg.norm(vdir, axis=1, keepdims=True), 1e-6)
                    nn = np.where((np.sum(nrm * vdir, 1) < 0)[:, None], -nrm, nrm)
                    rr = -vdir + 2 * np.sum(nn * vdir, 1)[:, None] * nn
                    tt = np.clip((rr[:, 1] + 0.2) / 1.1, 0, 1); tt = tt * tt * (3 - 2 * tt)
                    env = np.array([0.010, 0.014, 0.026]) * (1 - tt[:, None]) + np.array([0.16, 0.24, 0.34]) * tt[:, None]
                    fres = 0.04 + 0.96 * (1 - np.clip(np.sum(nn * vdir, 1), 0, 1)) ** 5
                    spec = env * ((0.04 + fres * 0.3)[:, None] * (1 - metal) + (alb * 1.1 + 0.03) * metal)
                    diff = alb * (1 - 0.85 * metal) * (0.05 + 0.85 * m[:, 1:2] ** 2)
                    glow = (TEAM[0] * m[:, 0:1] ** 2 + TEAM[1] * m[:, 2:3] ** 2) * 0.30
                    col = diff + spec + glow
                elif mat['shader'] == 2:
                    col = base[:, :3] * base[:, 3:4]
                elif mat['shader'] == 3:
                    col = base[:, :3]
                else:   # mirrors render.cpp SH_LIT (+ avatar pattern, gloss, rim)
                    wp = bw @ P[t][:, :3]
                    vdir = np.array(eye, float) - wp
                    vdir /= np.maximum(np.linalg.norm(vdir, axis=1, keepdims=True), 1e-6)
                    nn = np.where((np.sum(nrm * vdir, 1) < 0)[:, None], -nrm, nrm)
                    alb = base[:, :3] * (bw @ (v['c'][t][:, :1].astype(np.float64) / 255.0))   # vertex colour R = AO
                    glow_add = 0.0
                    if (mat['flags'] & 32) and mat['mask'] is not None and chans is not None:   # garment design
                        dm = sample(mat['mask'], uv)
                        sec = chans[2][None]
                        alb = alb * (1 - dm[:, :1]) + sec * base[:, 3:4] * 0 + sec * dm[:, :1] * (bw @ (v['c'][t][:, :1].astype(np.float64) / 255.0))
                        alb = alb * (1 - dm[:, 2:3]) + 0.80 * dm[:, 2:3] * (bw @ (v['c'][t][:, :1].astype(np.float64) / 255.0))
                        glow_add = chans[3][None] * dm[:, 1:2]
                    if pat is not None and (mat['flags'] & 16) and mat['shader'] == 0 and chans is not None:
                        pt = sample(pat, uv)
                        sec = mat['tint'][:3] * tint_mul[:3] * chans[2]
                        alb = alb * (1 - pt[:, :1]) + sec[None] * pt[:, :1]
                        glow_add = chans[3][None] * pt[:, 1:2]
                    ndl = np.maximum(-(nn @ light), 0)[:, None]
                    hemi = 0.5 + 0.5 * nn[:, 1:2]
                    hv = vdir - light[None]
                    hv /= np.maximum(np.linalg.norm(hv, axis=1, keepdims=True), 1e-6)
                    ndh = np.clip(np.sum(nn * hv, 1), 0, 1)[:, None]
                    nv = np.clip(np.sum(nn * vdir, 1), 0, 1)[:, None]
                    matte = max(0.0, -float(mat['em'][3]))   # negative gloss: matte
                    col = alb * (0.10 + 0.18 * hemi + 0.80 * ndl) + ndh ** 32 * 0.25 * (1 - matte) + glow_add
                    gloss = float(mat['em'][3])
                    if gloss > 0:
                        rr = -vdir + 2 * np.sum(nn * vdir, 1)[:, None] * nn
                        fres = 0.04 + 0.96 * (1 - nv) ** 5
                        col = col + arena_env(rr) * (0.25 + fres) * (gloss * 1.5) + ndh ** 96 * gloss * 0.8
                    col = col + rim[None, :3] * (rim[3] * (1 - nv) ** 2.5 * (1 - 0.75 * matte))
                if mat['shader'] != 5:   # SH_LIGHTMAP uses emissive as specular colour, not glow
                    col = col + mat['em'][:3] + em_add
                if blend:
                    a = 1.0 if mat['shader'] == 2 else base[:, 3:4]
                    blended.append((yy, xx, zz, col, a, mat['shader'] == 2))
                else:
                    if os.environ.get('DBGPX'):
                        qx, qy = [int(t) for t in os.environ['DBGPX'].split(',')]
                        for k in np.nonzero((xx == qx) & (yy == qy))[0]:
                            print('WRITE sh', mat['shader'], 'z', zz[k], 'col', col[k].round(4), 'depth before', depth[qy, qx])
                    depth[yy, xx] = zz
                    color[yy, xx] = col
    for yy, xx, zz, col, a, add in blended:
        ok = zz < depth[yy, xx]
        if add:
            color[yy[ok], xx[ok]] += col[ok]
        else:
            aa = a[ok] if not np.isscalar(a) else a
            color[yy[ok], xx[ok]] = color[yy[ok], xx[ok]] * (1 - aa) + col[ok] * aa
    srgb = np.where(color <= 0.0031308, color * 12.92, 1.055 * np.power(np.clip(color, 0, None), 1 / 2.4) - 0.055)
    Image.fromarray((np.clip(srgb, 0, 1) * 255).astype(np.uint8)).save(out)
    print('wrote', out)


def pose_mat(pos, yaw_deg, scale=1.0):
    a = np.radians(yaw_deg)
    M = np.eye(4)
    M[:3, :3] = np.array([[np.cos(a), 0, np.sin(a)], [0, 1, 0], [-np.sin(a), 0, np.cos(a)]]) * scale
    M[:3, 3] = pos
    return M


if __name__ == '__main__':
    out = sys.argv[1]
    nums = [float(x) for x in sys.argv[2:8]] if len(sys.argv) >= 8 else [-7.0, 1.65, 0.0, 7.0, 1.4, 0.0]
    extras = [a for a in sys.argv[8:] if '@' in a]
    models = []
    if os.path.exists(os.path.join(ASSETS, 'floor.mdl')):
        models += [(load_mdl('floor_reflect'), np.eye(4)), (load_mdl('floor'), np.eye(4), 0.80)]
    models += [(load_mdl('environment'), np.eye(4)), (load_mdl('arena'), np.eye(4))]
    for extra in ('env_fx', 'court_fx'):
        if os.path.exists(os.path.join(ASSETS, extra + '.mdl')):
            models.append((load_mdl(extra), np.eye(4)))
    for p, sp in ((0, (-6.48, 0, 0)), (1, (6.48, 0.02, 0))):
        models.append((load_mdl('spawner'), pose_mat(sp, 0)))
    for e in extras:
        name, rest = e.split('@')
        x, y, z, yaw = [float(t) for t in rest.split(',')]
        models.append((load_mdl(name), pose_mat((x, y, z), yaw)))
    render(models, nums[:3], nums[3:6], out)
