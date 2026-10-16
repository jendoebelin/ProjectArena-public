"""Project Arena avatar parts (run in Blender: blender -b --factory-startup -P tools/blender/make_avatar.py -- <out.npz>).

Human, waist-up or (LOWER BODY) down to mid-thigh; never feet. Built from smooth signed-distance shapes and lofts:
  av_head, av_neck              base head (skin, eyes, ears, brows) and the neck bone
  av_hair_1..8                  BUZZ CUT, SWEPT, PONYTAIL, MOHAWK, SPIKES, BOB, TOP KNOT, TOUSLED
  av_gear_1..6                  GRID HELMET, RACER HELMET, HOOD, CREST HELMET, HEADPHONES, CAP
  av_visor_1..7                 BAND VISOR, FULL SHIELD, SHADES, GOGGLES, HEADSET, HALO, SLIT VISOR
  av_mask_1..5                  MOUTH GUARD, CYBER JAW, BREATHER, BANDANA, FACEPLATE
  av_torso_N / av_torso_Nc      suit N torso, straight / curved build (N = 0..9, see app/src/avatar.cpp)
  av_upper_N, av_fore_N         suit N upper arm and forearm (bone meshes)
  av_waist / av_waist_c         the energy fade where a waist-up body ends (drawn when LOWER BODY = NONE)
  av_lower_K / av_lower_Kc      LOWER BODY K (1 ATHLETIC SHORTS, 2 CIRCUIT SHORTS): hips, shorts, thighs, a thigh fade
Spaces (Blender X right, Y forward, Z up; exported like make_props.py as app (x, z, -y)):
  head   origin between the eyes (the HMD pose)
  torso  origin at the neck base, upright
  bones  joint at the origin, the bone runs down -Z for its natural length (avatar.h kUpperArm / kForeArm / kNeck),
         front = +Y, outside = +X (right side; the app mirrors the left)
Materials (palette.py av_*) carry colour channels; patterns use uv0 (u around the body, v = depth / 0.5 m).
"""
import math
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import common  # noqa: E402

UPPER, FORE, NECK = 0.285, 0.265, 0.115   # must match app/src/avatar.h


# ================================================================ SDF toolkit (numpy, points (..., 3))
def sd_ellipsoid(P, c, r):
    r = np.asarray(r, float)
    q = (P - np.asarray(c, float)) / r
    k0 = np.linalg.norm(q, axis=-1)
    k1 = np.linalg.norm(q / r, axis=-1)
    return k0 * (k0 - 1.0) / np.maximum(k1, 1e-9)


def sd_sphere(P, c, r):
    return np.linalg.norm(P - np.asarray(c, float), axis=-1) - r


def sd_capsule(P, a, b, r):
    a, b = np.asarray(a, float), np.asarray(b, float)
    pa, ba = P - a, b - a
    h = np.clip((pa @ ba) / (ba @ ba), 0.0, 1.0)
    return np.linalg.norm(pa - h[..., None] * ba, axis=-1) - r


def smin(a, b, k):
    h = np.maximum(k - np.abs(a - b), 0.0) / k
    return np.minimum(a, b) - h * h * k * 0.25


def smax(a, b, k):
    return -smin(-a, -b, k)


# ---------------------------------------------------------------- the head (origin between the eyes)
def sd_skull(P):
    """Cranium, face mass, jaw and neck stub: the smooth base that hair, helmets and shields follow."""
    d = sd_ellipsoid(P, (0, -0.080, 0.022), (0.0765, 0.100, 0.099))                    # cranium
    d = smin(d, sd_ellipsoid(P, (0, -0.040, -0.040), (0.060, 0.050, 0.069)), 0.030)   # face mass
    d = smin(d, sd_ellipsoid(P, (0, -0.046, -0.073), (0.052, 0.050, 0.037)), 0.022)   # lower jaw
    for s in (-1, 1):   # mandible angles: a defined jawline under the ears
        d = smin(d, sd_ellipsoid(P, (s * 0.045, -0.066, -0.076), (0.012, 0.018, 0.013)), 0.020)
    d = smin(d, sd_capsule(P, (0, -0.083, -0.07), (0, -0.083, -0.12), 0.050), 0.020)   # neck stub (behind the jaw)
    return d


def sd_head(P):
    d = sd_skull(P)
    d = smin(d, sd_ellipsoid(P, (0, -0.002, -0.091), (0.022, 0.016, 0.016)), 0.012)     # chin
    for s in (-1, 1):
        d = smin(d, sd_ellipsoid(P, (s * 0.047, -0.018, -0.016), (0.017, 0.014, 0.011)), 0.014)   # cheekbones
    d = smin(d, sd_ellipsoid(P, (0, 0.003, 0.021), (0.058, 0.015, 0.012)), 0.011)      # brow ridge
    d = smin(d, sd_capsule(P, (0, 0.011, 0.004), (0, 0.021, -0.027), 0.0060), 0.008)   # nose bridge
    d = smin(d, sd_sphere(P, (0, 0.022, -0.030), 0.0072), 0.006)                       # nose tip
    for s in (-1, 1):
        d = smin(d, sd_ellipsoid(P, (s * 0.0090, 0.014, -0.034), (0.0060, 0.0060, 0.0050)), 0.005)   # nostril wings
    d = smin(d, sd_ellipsoid(P, (0, 0.013, -0.0505), (0.016, 0.0062, 0.0050)), 0.005)   # upper lip
    d = smin(d, sd_ellipsoid(P, (0, 0.011, -0.0615), (0.0145, 0.0065, 0.0055)), 0.005)   # lower lip
    d = smax(d, -sd_ellipsoid(P, (0, 0.017, -0.0558), (0.015, 0.004, 0.0010)), 0.0015)   # mouth line
    for s in (-1, 1):
        d = smax(d, -sd_sphere(P, (s * 0.032, 0.008, 0.002), 0.0135), 0.006)              # eye sockets
    return d


HEAD_C = np.array([0.0, -0.062, -0.008])   # ray origin for projecting onto the head (inside, star-shaped)

# ---------------------------------------------------------------- the MPFB heads (--head m|f)
# Head parts are fitted to the skinned bodies' real heads (tools/blender/make_body.py): the head + neck surface of
# body_<masc|fem>.npz, moved into head space (origin between the eyes), replaces the SDF skull for every projection.
HEAD_KEY = None
if '--head' in sys.argv:
    HEAD_KEY = sys.argv[sys.argv.index('--head') + 1]
    del sys.argv[sys.argv.index('--head'):sys.argv.index('--head') + 2]
HAIRLINE_TABLE = None
LM = dict(nose=np.array([0, 0.022, -0.030]), lips=np.array([0, 0.012, -0.056]), chin=np.array([0, -0.002, -0.094]),
          nose_el=-14.7, lips_el=-31.0, chin_el=-49.0, ear_el=-8.0, ear_x=0.0735, ear_y=-0.083, ear_z=-0.012, under_el=-62.0,
          ear_top=0.016, ear_bot=-0.040)
if HEAD_KEY:
    from mathutils import Vector
    from mathutils.bvhtree import BVHTree
    _ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
    _bd = np.load(os.path.join(_ROOT, 'build', 'open_src', 'body', 'body_%s.npz' % {'m': 'masc', 'f': 'fem'}[HEAD_KEY]))
    import json as _json
    _J = _json.loads(str(_bd['joints']))
    _eye = np.array(_J['eye_mid'])

    def _mk(Papp):   # app (x right, y up, -z forward) -> head space (x right, y forward, z up), origin = the eyes
        Q = np.asarray(Papp, float) - _eye
        return np.stack([Q[..., 0], -Q[..., 2], Q[..., 1]], -1)
    _T = _bd['tris']
    _dom = _bd['bone'][:, 0]
    _keep = np.isin(_dom[_T], [1, 2]).all(1)                       # head and neck faces
    _HP = _mk(_bd['pos'])
    _tree = BVHTree.FromPolygons([tuple(v) for v in _HP], [tuple(t) for t in _T[_keep].tolist()], epsilon=0.0)
    _headv = np.unique(_T[np.isin(_dom[_T], [2]).all(1)].reshape(-1))
    _cr = _HP[_headv][_HP[_headv][:, 2] > -0.01]                      # cranium: the head above the eyes
    HEAD_C = np.array([0.0, 0.5 * (_cr[:, 1].min() + _cr[:, 1].max()), -0.006])

    def sd_mpfb(P):
        """Radial distance outside the head (the current radial map, see RMAP): robust around the ears and nose."""
        P = np.asarray(P, float)
        V = P - HEAD_C
        _, r = rmap_lookup(V)
        return np.linalg.norm(V, axis=-1) - r
    # the hairline: the lowest elevation of MPFB's scalp area around the head, seen from HEAD_C
    _sc = _HP[np.nonzero(_bd['region'] == 2)[0]] - HEAD_C
    _az = np.degrees(np.abs(((np.arctan2(_sc[:, 1], _sc[:, 0]) - math.pi / 2 + math.pi) % (2 * math.pi)) - math.pi))
    _el = np.degrees(np.arctan2(_sc[:, 2], np.hypot(_sc[:, 0], _sc[:, 1])))
    _bins = np.arange(0, 181, 10)
    _vals = []
    for b0 in _bins:
        sel = np.abs(_az - b0) < 8
        _vals.append(np.percentile(_el[sel], 3) if sel.sum() > 3 else np.nan)
    _vals = np.array(_vals)
    _ok = ~np.isnan(_vals)
    _xs, _ys = _bins[_ok].astype(float), _vals[_ok]
    _fine = np.arange(0, 181, 2.5)
    _yf = np.interp(_fine, _xs, _ys)
    _ext = np.concatenate([_yf[1:][::-1], _yf, _yf[:-1][::-1]])            # mirrored at 0 and 180 degrees
    _g = np.exp(-0.5 * (np.arange(-12, 13) * 2.5 / 9.0) ** 2)
    _sm = np.convolve(_ext, _g / _g.sum(), mode='same')[len(_yf) - 1:2 * len(_yf) - 1]
    HAIRLINE_TABLE = (_fine, np.maximum(_yf, _sm))

    # radial distance of the head surface from HEAD_C on a 2-degree grid (azimuth CCW from +X, elevation)
    _AZ = np.radians(np.arange(0, 360, 2.0))
    _EL = np.radians(np.arange(-88, 89, 2.0))
    _R = np.full((len(_EL), len(_AZ)), np.nan)
    for i, el in enumerate(_EL):
        for j, az in enumerate(_AZ):
            d = Vector((math.cos(el) * math.cos(az), math.cos(el) * math.sin(az), math.sin(el)))
            # the outermost hit: the head mesh includes the mouth cavity and eye sockets behind the skin
            o, travelled, best = Vector(HEAD_C), 0.0, None
            for _ in range(8):
                loc, _, _, dist = _tree.ray_cast(o, d, 0.35 - travelled)
                if loc is None:
                    break
                travelled += dist + 1e-4
                best = travelled
                o = loc + d * 1e-4
            if best is not None:
                _R[i, j] = best
    for i in range(len(_EL)):   # misses (the open neck): carry the nearest row above
        row = _R[i]
        if np.isnan(row).all():
            continue
    for i in range(len(_EL) - 2, -1, -1):
        miss = np.isnan(_R[i])
        _R[i, miss] = _R[i + 1, miss]
    _R = np.where(np.isnan(_R), np.nanmax(_R), _R)

    def _shift(a, di, dj):
        b = np.roll(a, dj, axis=1)
        if di > 0:
            b = np.vstack([np.repeat(b[:1], di, 0), b[:-di]])
        elif di < 0:
            b = np.vstack([b[-di:], np.repeat(b[-1:], -di, 0)])
        return b

    def _envelope(R, half, blur):
        out = R.copy()
        for di in range(-half, half + 1):
            for dj in range(-half, half + 1):
                if di * di + dj * dj <= half * half:
                    out = np.maximum(out, _shift(R, di, dj))
        for _ in range(blur):   # 3x3 box blur, circular in azimuth
            acc = sum(_shift(out, di, dj) for di in (-1, 0, 1) for dj in (-1, 0, 1))
            out = np.maximum(acc / 9.0, R)   # never inside the real surface
        return out
    RMAP = {'tight': _envelope(_R, 2, 2), 'env': _envelope(_R, 5, 5)}
    PROJ = ['tight']   # which map project() uses (set per part family in main)

    # face landmarks of this head: masks and face shields are placed relative to them
    _lips = _HP[np.nonzero(_bd['region'] == 1)[0]].mean(0)
    _hv = _HP[_headv]
    _front = _hv[np.abs(_hv[:, 0]) < 0.012]
    _nose = _front[_front[:, 1].argmax()]
    _below = _front[_front[:, 2] < _lips[2] - 0.02]
    _chin = _below[_below[:, 1].argmax()]
    _ear = _HP[np.nonzero(_bd['region'] == 3)[0]]

    def _elev(q):
        v = np.asarray(q, float) - HEAD_C
        return math.degrees(math.atan2(v[2], math.hypot(v[0], v[1])))
    LM = dict(nose=_nose, lips=_lips, chin=_chin, nose_el=_elev(_nose), lips_el=_elev(_lips), chin_el=_elev(_chin),
              ear_el=_elev(np.array([np.abs(_ear[:, 0]).max(), _ear[:, 1].mean(), _ear[:, 2].mean()])),
              ear_x=float(np.abs(_ear[:, 0]).max()), ear_y=float(_ear[:, 1].mean()), ear_z=float(_ear[:, 2].mean()),
              under_el=_elev(_chin) - 13.0, ear_top=float(np.percentile(_ear[:, 2], 97)),
              ear_bot=float(np.percentile(_ear[:, 2], 3)))
    print('landmarks: nose %.1f lips %.1f chin %.1f ear %.1f' % (LM['nose_el'], LM['lips_el'], LM['chin_el'], LM['ear_el']))

    def rmap_lookup(D):
        D = np.asarray(D, float)
        Dn = D / np.maximum(np.linalg.norm(D, axis=-1, keepdims=True), 1e-12)
        az = np.degrees(np.arctan2(Dn[..., 1], Dn[..., 0])) % 360.0
        el = np.degrees(np.arcsin(np.clip(Dn[..., 2], -1, 1)))
        fj = az / 2.0
        fi = np.clip((el + 88.0) / 2.0, 0, len(_EL) - 1.001)
        j0 = np.floor(fj).astype(int) % len(_AZ)
        j1 = (j0 + 1) % len(_AZ)
        i0 = np.floor(fi).astype(int)
        i1 = np.minimum(i0 + 1, len(_EL) - 1)
        tj, ti = fj - np.floor(fj), fi - i0
        M = RMAP[PROJ[0]]
        r = (M[i0, j0] * (1 - tj) + M[i0, j1] * tj) * (1 - ti) + (M[i1, j0] * (1 - tj) + M[i1, j1] * tj) * ti
        return Dn, r
    print('MPFB head %s: HEAD_C %s hairline %s' % (HEAD_KEY, np.round(HEAD_C, 3), np.round(HAIRLINE_TABLE[1][::4], 1)))


if HEAD_KEY:
    sd_skull = sd_mpfb
    sd_head = sd_mpfb


def dirs(theta, elev):
    """Unit directions; theta = azimuth CCW from +X (front = +Y = pi/2), elev = elevation."""
    ce = np.cos(elev)
    return np.stack([ce * np.cos(theta), ce * np.sin(theta), np.sin(elev)], -1)


def project(sd, D, offset=0.0, origin=HEAD_C, r_max=0.35):
    """Points origin + t*D on the surface sd(P) = offset (bisection; sd < offset at the origin)."""
    D = np.asarray(D, float)
    if HEAD_KEY and sd is sd_mpfb and np.allclose(origin, HEAD_C):
        # the MPFB head: its radial map (tight, or the envelope over nose / lips / ears), pushed out by `offset`
        Dn, r = rmap_lookup(D)
        off = np.broadcast_to(np.asarray(offset, float), D.shape[:-1])
        return np.asarray(origin, float) + Dn * (r + off)[..., None]
    if HEAD_KEY and sd is sd_mpfb:   # another origin: first hit of the ray on the head mesh, pushed out by `offset`
        flatD = D.reshape(-1, 3)
        off = np.broadcast_to(np.asarray(offset, float), D.shape[:-1]).reshape(-1)
        o = np.asarray(origin, float)
        out = np.empty_like(flatD)
        for i, d in enumerate(flatD):
            dn = d / max(np.linalg.norm(d), 1e-12)
            loc, nrm, _, dist = _tree.ray_cast(Vector(o), Vector(dn), r_max)
            if loc is None:
                out[i] = o + dn * 0.09 + dn * off[i]
                continue
            c = max(0.35, abs(float(np.dot(np.array(nrm), dn))))
            out[i] = np.array(loc) + dn * (off[i] / c)
        return out.reshape(D.shape)
    lo = np.zeros(D.shape[:-1])
    hi = np.full(D.shape[:-1], r_max)
    off = np.broadcast_to(np.asarray(offset, float), lo.shape)
    for _ in range(40):
        mid = (lo + hi) * 0.5
        inside = sd(origin + D * mid[..., None]) < off
        lo = np.where(inside, mid, lo)
        hi = np.where(inside, hi, mid)
    return origin + D * ((lo + hi) * 0.5)[..., None]


def head_point(x, z, offset=0.0, sd=sd_head, front=True):
    """Surface point seen from the front (or back) at roughly (x, z)."""
    D = np.array([x - HEAD_C[0], (1.0 if front else -1.0), z - HEAD_C[2]])
    D[1] = 0.09 if front else -0.12
    D /= np.linalg.norm(D)
    return project(sd, D[None], offset)[0]


# ================================================================ mesh accumulation
def phi_of(theta):
    """Angle from the front (+Y), in (-pi, pi]; positive toward -X (the left side)."""
    return (theta - math.pi / 2 + math.pi) % (2 * math.pi) - math.pi


class Part:
    """Triangle soup for one model: per-corner position / normal / uv and a material name per triangle."""

    def __init__(self, name):
        self.name = name
        self.P, self.N, self.U, self.M = [], [], [], []

    def tris(self, P, N, U, mats):
        self.P.append(np.asarray(P, np.float64).reshape(-1, 3))
        self.N.append(np.asarray(N, np.float64).reshape(-1, 3))
        self.U.append(np.asarray(U, np.float64).reshape(-1, 2))
        self.M += list(mats)

    def grid(self, mat, P, UV=None, closed_u=False, keep=None, flip=False, normals=None, u_period=1.0):
        """Quad grid P[v, u] (u around / across, v upward for outward normals). mat: name, or array [v-1, u(-1)]
        of names per face (None = skip). Normals from the whole grid, smooth across materials."""
        P = np.asarray(P, np.float64)
        nv, nu = P.shape[:2]
        if UV is None:
            uu, vv = np.meshgrid(np.linspace(0, 1, nu + (1 if closed_u else 0))[:nu], np.linspace(0, 1, nv))
            UV = np.stack([uu, vv], -1)
        UV = np.asarray(UV, np.float64)
        if normals is None:
            if closed_u:
                du = np.roll(P, -1, 1) - np.roll(P, 1, 1)
            else:
                du = np.gradient(P, axis=1)
            dv = np.gradient(P, axis=0)
            N = np.cross(du, dv)
            ln = np.linalg.norm(N, axis=-1, keepdims=True)
            N = np.where(ln > 1e-12, N / np.maximum(ln, 1e-12), np.array([0, 0, 1.0]))
            if flip:
                N = -N
        else:
            N = normals
        nfu = nu if closed_u else nu - 1
        if isinstance(mat, str) or mat is None:
            mats = np.full((nv - 1, nfu), mat, dtype=object)
        else:
            mats = np.asarray(mat, dtype=object)
        P_out, N_out, U_out, M_out = [], [], [], []
        for j in range(nv - 1):
            for i in range(nfu):
                m = mats[j, i]
                if m is None or (keep is not None and not keep[j, i]):
                    continue
                i1 = (i + 1) % nu
                # the closing column of a closed grid reuses the first vertices but continues the u coordinate
                uv = [UV[j, i], UV[j, i1].copy(), UV[j + 1, i1].copy(), UV[j + 1, i]]
                if closed_u and i1 == 0:
                    for q in (1, 2):
                        uv[q][0] += u_period if uv[q][0] < uv[0][0] else 0.0
                a, b, c, d = P[j, i], P[j, i1], P[j + 1, i1], P[j + 1, i]
                na, nb, nc, nd = N[j, i], N[j, i1], N[j + 1, i1], N[j + 1, i]
                ua, ub, uc, ud = uv
                if flip:
                    P_out += [a, c, b, a, d, c]; N_out += [na, nc, nb, na, nd, nc]; U_out += [ua, uc, ub, ua, ud, uc]
                else:
                    P_out += [a, b, c, a, c, d]; N_out += [na, nb, nc, na, nc, nd]; U_out += [ua, ub, uc, ua, uc, ud]
                M_out += [m, m]
        if P_out:
            self.tris(P_out, N_out, U_out, M_out)

    def fan(self, mat, ring, apex, ring_n=None, apex_n=None, uv=(0.5, 0.5), flip=False):
        """Triangle fan closing a ring of points (ordered CCW seen from outside the cap)."""
        ring = np.asarray(ring, float)
        n = len(ring)
        apex = np.asarray(apex, float)
        if apex_n is None:
            c = ring.mean(0)
            apex_n = apex - c if np.linalg.norm(apex - c) > 1e-9 else np.array([0, 0, 1.0])
            apex_n = apex_n / np.linalg.norm(apex_n)
        if ring_n is None:
            ring_n = np.repeat(apex_n[None], n, 0)
        P, N, U = [], [], []
        for k in range(n):
            a, b = ring[k], ring[(k + 1) % n]
            if flip:
                P += [apex, b, a]; N += [apex_n, ring_n[(k + 1) % n], ring_n[k]]
            else:
                P += [apex, a, b]; N += [apex_n, ring_n[k], ring_n[(k + 1) % n]]
            U += [uv, uv, uv]
        self.tris(P, N, U, [mat] * n)

    def tube(self, mat, pts, r, segs=10, cap=True, uv_len=None):
        """Tube along a polyline (parallel-transport frames); r scalar or per point."""
        pts = np.asarray(pts, float)
        n = len(pts)
        r = np.broadcast_to(np.asarray(r, float), (n,))
        T = np.gradient(pts, axis=0)
        T /= np.maximum(np.linalg.norm(T, axis=-1, keepdims=True), 1e-12)
        ref = np.array([0, 0, 1.0]) if abs(T[0][2]) < 0.9 else np.array([1.0, 0, 0])
        Nf = np.cross(T[0], ref)
        Nf /= np.linalg.norm(Nf)
        frames = []
        for k in range(n):
            if k:
                Nf = Nf - T[k] * (Nf @ T[k])
                Nf /= max(np.linalg.norm(Nf), 1e-12)
            frames.append((Nf.copy(), np.cross(T[k], Nf)))
        ang = np.linspace(0, 2 * math.pi, segs, endpoint=False)
        P = np.zeros((n, segs, 3))
        Nn = np.zeros((n, segs, 3))
        for k in range(n):
            a, b = frames[k]
            for i, t in enumerate(ang):
                d = a * math.cos(t) + b * math.sin(t)
                P[k, i] = pts[k] + d * r[k]
                Nn[k, i] = d
        L = np.concatenate([[0], np.cumsum(np.linalg.norm(np.diff(pts, axis=0), axis=-1))])
        uu, vv = np.meshgrid(np.arange(segs) / segs, L / (uv_len or max(L[-1], 1e-6)))
        # grid rows go along the tube; with u = angle around the CCW-from-T direction the faces face outward
        self.grid(mat, P, np.stack([uu, vv], -1), closed_u=True, normals=Nn)
        if cap:
            self.fan(mat, P[0][::-1], pts[0] - T[0] * r[0] * 0.6, apex_n=-T[0])
            self.fan(mat, P[-1], pts[-1] + T[-1] * r[-1] * 0.6, apex_n=T[-1])

    def builder(self, fn):
        """Geometry from the common.Builder primitives (boxes, tori, spheres...)."""
        b = common.Builder(self.name)
        fn(b)
        for obj in b.to_objects().values():
            me = obj.data
            me.calc_loop_triangles()
            uvs = np.zeros(len(me.loops) * 2, np.float32)
            if 'UVMap' in me.uv_layers:
                me.uv_layers['UVMap'].data.foreach_get('uv', uvs)
            uvs = uvs.reshape(-1, 2)
            P, N, U, M = [], [], [], []
            for tri in me.loop_triangles:
                poly = me.polygons[tri.polygon_index]
                for k in range(3):
                    li = tri.loops[k]
                    v = me.vertices[me.loops[li].vertex_index]
                    P.append(tuple(v.co))
                    N.append(tuple(v.normal if poly.use_smooth else poly.normal))
                    U.append(tuple(uvs[li]))
                M.append(me.materials[poly.material_index].name.split('.')[0])
            if P:
                self.tris(P, N, U, M)
            bpy.data.objects.remove(obj)


def shell(part, mat, sd, theta0, theta1, e_lo, e_hi, offset, nu=72, nv=40, closed=False, cap_top=False,
          uv_scale=(1.0, 1.0), mat_fn=None, flip=False):
    """Surface sd = offset over azimuth theta0..theta1 and elevation e_lo(theta)..e_hi(theta) (callables or
    constants); offset may be a callable (theta, s) -> metres, s = 0 bottom .. 1 top."""
    th = np.linspace(theta0, theta1, nu, endpoint=not closed)
    s = np.linspace(0, 1, nv)
    TH, S = np.meshgrid(th, s)
    lo = e_lo(TH) if callable(e_lo) else np.full_like(TH, e_lo)
    hi = e_hi(TH) if callable(e_hi) else np.full_like(TH, e_hi)
    E = lo + (hi - lo) * S
    off = offset(TH, S) if callable(offset) else offset
    P = project(sd, dirs(TH, E), off)
    if not closed:
        UVu = (TH - theta0) / (2 * math.pi)
    else:
        UVu = (TH - theta0) / (theta1 - theta0)
    UV = np.stack([UVu * uv_scale[0], S * uv_scale[1]], -1)
    mats = mat
    if mat_fn is not None:
        thc = (TH[:-1, :] + np.roll(TH, -1, 1)[:-1, :]) * 0.5 if closed else (TH[:-1, :-1] + TH[:-1, 1:]) * 0.5
        sc = (S[:-1] + S[1:])[:, :thc.shape[1]] * 0.5
        mats = np.vectorize(mat_fn, otypes=[object])(thc, sc)
    part.grid(mats, P, UV, closed_u=closed, flip=flip)
    if cap_top:
        ring = P[-1]
        top = project(sd, dirs(np.array([0.0]), np.array([math.pi / 2]))[None], offset(np.zeros(1), np.ones(1))
                      if callable(offset) else offset)[0, 0]
        part.fan(mat if isinstance(mat, str) else mat_fn(0.0, 1.0), ring, top, apex_n=np.array([0, 0, 1.0]), flip=flip)
    return P


def interp(x, xs, ys):
    return np.interp(x, xs, ys)


# ================================================================ head
def hairline(theta, lift=0.0):
    """Elevation (rad, from HEAD_C) of the hairline around the head."""
    a = np.degrees(np.abs(phi_of_np(theta)))
    if HAIRLINE_TABLE is not None:
        # MPFB's scalp area, a little lower at the front for a fuller hairline
        return np.radians(interp(a, HAIRLINE_TABLE[0], HAIRLINE_TABLE[1]) + 1.5 - 7.0 * np.exp(-(a / 50.0) ** 2) + lift)
    return np.radians(interp(a, [0, 25, 40, 60, 80, 95, 110, 130, 155, 180], [30, 29, 31, 24, 12, 2, 8, 0, -16, -27]) + lift)


def phi_of_np(theta):
    return (theta - math.pi / 2 + math.pi) % (2 * math.pi) - math.pi


def make_head():
    p = Part('av_head')
    # skin: one closed surface (pole cap on top), bottom = neck stub end
    th = np.linspace(-math.pi / 2, 1.5 * math.pi, 64, endpoint=False)
    el = np.radians(np.linspace(-84, 86, 46))
    TH, EL = np.meshgrid(th, el)
    P = project(sd_head, dirs(TH, EL))
    UV = np.stack([(TH + math.pi / 2) / (2 * math.pi), (EL - EL.min()) / (EL.max() - EL.min())], -1)
    p.grid('av_skin', P, UV, closed_u=True)
    top = project(sd_head, np.array([[0, 0, 1.0]]))[0]
    p.fan('av_skin', P[-1], top, apex_n=np.array([0, 0, 1.0]))
    bot = project(sd_head, np.array([[0, 0, -1.0]]))[0]
    p.fan('av_skin', P[0][::-1], bot, apex_n=np.array([0, 0, -1.0]))

    def details(b):
        for s in (-1, 1):
            ec = Vector((s * 0.032, -0.008, 0.0015))
            b.sphere('av_eye_white', tuple(ec), 0.0115, segs=20, rings=12)
            b.disc('av_iris', tuple(ec + Vector((0, 0.01128, 0))), 0.0063, segs=20, axis='Y', flip=True)
            b.disc('av_eye', tuple(ec + Vector((0, 0.01138, 0))), 0.0028, segs=14, axis='Y', flip=True)
            for top, cut in ((True, 0.0024), (False, -0.0062)):   # eyelids: skin caps over the eyeball (a relaxed look)
                tmp = bmesh.new()
                bmesh.ops.create_uvsphere(tmp, u_segments=20, v_segments=12, radius=0.0127)
                kill = [v for v in tmp.verts if (v.co.z < cut if top else v.co.z > cut)]
                bmesh.ops.delete(tmp, geom=kill, context='VERTS')
                bmesh.ops.translate(tmp, vec=ec, verts=tmp.verts)
                b.add_bmesh(tmp, 'av_skin', smooth=True)
            # ears: a flattened shell with a rim, tilted back
            rot = Matrix.Rotation(math.radians(-14 * s), 3, 'Z') @ Matrix.Rotation(math.radians(-12), 3, 'X')
            tmp = bmesh.new()
            bmesh.ops.create_uvsphere(tmp, u_segments=18, v_segments=12, radius=1.0)
            bmesh.ops.scale(tmp, vec=(0.0095, 0.019, 0.028), verts=tmp.verts)
            bmesh.ops.transform(tmp, matrix=Matrix.Translation((s * 0.0735, -0.083, -0.012)) @ rot.to_4x4(), verts=tmp.verts)
            b.add_bmesh(tmp, 'av_skin', smooth=True)
    p.builder(details)
    # brows: short hair-coloured strokes riding the brow ridge
    for s in (-1, 1):
        pts = np.array([head_point(s * x, 0.021 + 0.004 * math.sin(math.pi * (x - 0.012) / 0.036), 0.0012)
                        for x in np.linspace(0.012, 0.048, 9)])
        p.tube('av_hair', pts, np.linspace(0.0034, 0.0022, 9), segs=8)
    return p


def make_neck():
    p = Part('av_neck')
    z = np.linspace(-NECK - 0.02, 0.02, 14)
    th = np.linspace(-math.pi / 2, 1.5 * math.pi, 28, endpoint=False)
    TH, Z = np.meshgrid(th, z)
    t = (Z - z.min()) / (z.max() - z.min())
    rx = 0.058 + 0.018 * (1 - t) ** 2       # wider into the shoulders (trapezius)
    ry = 0.055 + 0.006 * (1 - t) ** 2
    P = np.stack([rx * np.cos(TH), ry * np.sin(TH) + 0.004 * np.maximum(np.sin(TH), 0), Z], -1)
    p.grid('av_skin', P, closed_u=True)
    return p


# ---------------------------------------------------------------- hair
def strands(theta, s, amp=0.0018, n=38):
    return amp * np.sin(theta * n + 3.0 * s) * np.clip(s * 4, 0, 1)


def hair_shell(p, thickness, lift=0.0, low=None, fringe=0.0, ridges=0.0018, mat='av_hair'):
    lo = (lambda th: hairline(th, lift)) if low is None else low

    def off(th, s):
        t = thickness(th, s) if callable(thickness) else thickness * np.ones_like(th)
        t = t * np.clip(s * 6.0, 0.0, 1.0) ** 0.6           # taper into the scalp at the hairline
        if fringe:
            front = np.exp(-(np.degrees(np.abs(phi_of_np(th))) / 40.0) ** 2)
            t = t + fringe * front * np.exp(-((s - 0.25) / 0.2) ** 2)
        return np.maximum(t + strands(th, s, ridges), 0.0009)
    return shell(p, mat, sd_skull, -math.pi / 2, 1.5 * math.pi, lo, math.radians(87), off, nu=72, nv=24,
                 closed=True, cap_top=True)


def lock(p, path_dirs, offsets, widths, mat='av_hair_lock', arch=0.0035, nx=5):
    """One lock of hair: a ribbon along directions from HEAD_C (projected onto the skull at `offsets`), `widths`
    across, arched in the middle so it catches the light."""
    C = project(sd_skull, path_dirs, offsets)
    n = len(C)
    T = np.gradient(C, axis=0)
    T /= np.maximum(np.linalg.norm(T, axis=-1, keepdims=True), 1e-9)
    N = C - HEAD_C
    N /= np.linalg.norm(N, axis=-1, keepdims=True)
    B = np.cross(N, T)
    B /= np.maximum(np.linalg.norm(B, axis=-1, keepdims=True), 1e-9)
    xs = np.linspace(-1, 1, nx)
    P = np.zeros((n, nx, 3))
    for i in range(n):
        for j, x in enumerate(xs):
            P[i, j] = C[i] + B[i] * widths[i] * x + N[i] * arch * (1 - x * x) * min(1.0, widths[i] / 0.01)
    p.grid(mat, P, flip=True)


def clumps(p, n, thickness, start_el=74.0, end_lift=-2.0, curl=0.0, flow=None, reach=0.006, r0=0.0062, seed=3, sel=None):
    """Tapered strand clumps lying on the hair shell, from near the crown to past the hairline (a soft, broken
    edge instead of a helmet rim). flow(theta) -> start azimuth offset; sel(theta) -> keep this clump."""
    rng = np.random.default_rng(seed)
    for k in range(n):
        th_end = -math.pi / 2 + 2 * math.pi * (k + rng.random() * 0.6) / n
        if sel is not None and not sel(th_end):
            continue
        th0 = th_end + (flow(th_end) if flow else 0.0) + rng.normal(0, 0.06)
        e0 = math.radians(start_el + rng.normal(0, 4))
        e1 = hairline(np.array(th_end)) + math.radians(end_lift + rng.normal(0, 1.5))
        m = 9
        u = np.linspace(0, 1, m)
        TH = th0 + (th_end - th0) * u + curl * np.sin(math.pi * u)
        EL = e0 + (e1 - e0) * u
        off = (thickness + 0.0025) * (1 - 0.35 * u) + reach * u ** 3
        pts = project(sd_skull, dirs(TH, EL), off)
        p.tube('av_hair', pts, r0 * (1 - 0.82 * u) + 0.0009, segs=5, cap=False)


def make_hair(k):
    p = Part('av_hair_%d' % k)
    if k == 1:      # BUZZ CUT
        hair_shell(p, 0.0032, ridges=0.0005)
    elif k == 2:    # SWEPT: a short shell under layered locks combed up and back, a lifted front
        hair_shell(p, 0.004, ridges=0.0006)
        rng = np.random.default_rng(2)
        u = np.linspace(0, 1, 10)
        for layer, (n, e_lo_add, off0) in enumerate(((22, 0.0, 0.006), (16, 10.0, 0.010), (10, 22.0, 0.013))):
            for kk in range(n):
                th0 = -math.pi / 2 + 2 * math.pi * (kk + 0.5 * layer + rng.random() * 0.4) / n
                front = math.exp(-(math.degrees(abs(phi_of(th0))) / 55.0) ** 2)
                e0 = float(hairline(np.array(th0))) + math.radians(e_lo_add - 3.0 + 4.0 * front)
                e1 = math.radians(84)
                TH = th0 + 0.10 * np.sin(math.pi * u) * (1 if kk % 2 else -1)
                EL = e0 + (e1 - e0) * u
                lift = off0 + 0.006 * front * np.exp(-((u - 0.12) / 0.15) ** 2)
                w = (0.016 + 0.006 * rng.random()) * np.sqrt(np.clip(np.sin(math.pi * np.clip(u * 0.9 + 0.08, 0, 1)), 0, 1))
                lock(p, dirs(TH, EL), lift + 0.0 * u, w + 0.002)
    elif k == 3:    # PONYTAIL: pulled back into a high tie, a full wavy tail, loose strands framing the face
        hair_shell(p, 0.006, ridges=0.0012)
        tie = head_point(0.0, 0.040, 0.014, sd=sd_skull, front=False)
        rng = np.random.default_rng(31)
        u = np.linspace(0, 1, 12)
        for kk in range(30):   # locks combed from the hairline back to the tie
            th0 = -math.pi / 2 + 2 * math.pi * (kk + rng.random() * 0.5) / 30
            e0 = float(hairline(np.array(th0))) + math.radians(3.0)
            P0 = project(sd_skull, dirs(np.array(th0), np.array(e0))[None], 0.006)[0]
            path = np.array([P0 + (tie - P0) * f for f in u])
            D = path - HEAD_C
            D /= np.linalg.norm(D, axis=-1, keepdims=True)
            grow = np.clip(u / 0.22, 0, 1) ** 0.7                       # rise out of the shell over the first fifth
            lock(p, D, 0.0035 + 0.0035 * grow + 0.0025 * np.sin(math.pi * u),
                 ((0.017 + 0.004 * rng.random()) * (1 - 0.6 * u) + 0.003) * (0.35 + 0.65 * grow), arch=0.0016)
        nt = 20
        for kk in range(11):   # the tail: a bundle of wavy strands that splay toward the ends
            ang = 2 * math.pi * kk / 11
            off = np.array([0.008 * math.cos(ang), 0.006 * math.sin(ang), 0])
            ph = rng.random() * 6.28
            pts = np.array([tie + off * (1 + 1.6 * t) + np.array([0.010 * math.sin(5.5 * t + ph), -0.018 * t - 0.05 * t * t, -0.21 * t])
                            for t in np.linspace(0, 1, nt)])
            p.tube('av_hair', pts, 0.0135 * (1 - np.linspace(0, 1, nt)) ** 0.75 + 0.0020, segs=8)
        p.tube('av_secondary', [tie + np.array([0, 0.006, 0.004]), tie + np.array([0, -0.006, -0.004])], 0.0135, segs=16)
    elif k == 4:    # MOHAWK: buzz sides, a crest fin front to back
        hair_shell(p, 0.0022, ridges=0.0004)
        a0 = float(np.degrees(hairline(np.array(math.pi / 2)))) + 4.0     # start at the hairline, not on the forehead
        a = np.radians(np.linspace(a0, 160, 30))
        base = [project(sd_skull, np.array([[0, math.cos(t), math.sin(t)]]), 0.002)[0] for t in a]
        height = 0.055 * np.sin(np.linspace(0.1, math.pi - 0.25, 30)) ** 0.6 + 0.012
        # crest: stacked thin tubes from the scalp outward give a solid fin with soft edges
        for f in np.linspace(0.0, 1.0, 6):
            pts = [base[i] + (base[i] - HEAD_C) / np.linalg.norm(base[i] - HEAD_C) * height[i] * f for i in range(30)]
            p.tube('av_hair', pts, 0.0065 * (1 - 0.6 * f), segs=8)
    elif k == 5:    # SPIKES
        hair_shell(p, 0.007, ridges=0.0012)
        rng = np.random.default_rng(5)
        spikes = []
        for az in np.linspace(0, 2 * math.pi, 9, endpoint=False):
            for el in (55, 75):
                spikes.append((az + (0.35 if el == 75 else 0), math.radians(el)))
        spikes.append((0.0, math.radians(88)))

        def sp(b):
            for az, el in spikes:
                d = dirs(np.array(az), np.array(el))
                base = project(sd_skull, d[None], 0.004)[0]
                tip_dir = d + np.array([0, -0.45, 0.25])
                tip_dir /= np.linalg.norm(tip_dir)
                ln = 0.045 + 0.02 * rng.random()
                q = Vector(tip_dir).to_track_quat('Z', 'Y')
                tmp = bmesh.new()
                bmesh.ops.create_cone(tmp, cap_ends=True, segments=8, radius1=0.012, radius2=0.0008, depth=ln)
                bmesh.ops.transform(tmp, matrix=Matrix.Translation(Vector(base) + Vector(tip_dir) * ln * 0.5)
                                    @ q.to_matrix().to_4x4(), verts=tmp.verts)
                b.add_bmesh(tmp, 'av_hair', smooth=True)
        p.builder(sp)
    elif k == 6:    # BOB: chin-length, fringe (clumps fall to the ends)
        def low(th):
            a = np.degrees(np.abs(phi_of_np(th)))
            return np.radians(interp(a, [0, 30, 55, 80, 180], [30, 14, -30, -42, -40]))

        def th_(th, s):
            a = np.degrees(np.abs(phi_of_np(th)))
            flare = np.exp(-((s - 0.0) / 0.35) ** 2) * np.clip((a - 40) / 30, 0, 1)
            return 0.010 + 0.010 * flare + 0.006 * np.exp(-((s - 0.75) / 0.3) ** 2)
        hair_shell(p, th_, low=low, fringe=0.006, ridges=0.0012)
        rng = np.random.default_rng(6)
        u = np.linspace(0, 1, 12)
        for layer, n in enumerate((26, 18)):
            for kk in range(n):
                th_end = -math.pi / 2 + 2 * math.pi * (kk + 0.5 * layer + rng.random() * 0.4) / n
                e1 = float(low(np.array(th_end))) - math.radians(3 + 2 * rng.random())
                TH = th_end + 0.05 * np.sin(math.pi * u)
                EL = math.radians(84) + (e1 - math.radians(84)) * u
                off = th_(np.full(12, th_end), 1 - u) + 0.004 + 0.003 * layer + 0.006 * u ** 2
                w = (0.019 + 0.006 * rng.random()) * (1 - 0.7 * u ** 2)
                lock(p, dirs(TH, EL), off, w)
    elif k == 7:    # TOP KNOT
        hair_shell(p, 0.0045, ridges=0.0008)
        top = project(sd_skull, dirs(np.array(-math.pi / 2), np.array(math.radians(72)))[None], 0.004)[0]

        def bun(b):
            b.sphere('av_hair', tuple(top + np.array([0, 0, 0.022])), 0.026, segs=20, rings=12, scale=(1, 1, 0.85))
            b.torus('av_secondary', tuple(top + np.array([0, 0, 0.004])), 0.015, 0.004, segs=20, tsegs=6)
        p.builder(bun)
    elif k == 8:    # TOUSLED: short-medium, layered and messy, volume on top, a fringe falling to one side
        hair_shell(p, 0.006, ridges=0.0010)
        rng = np.random.default_rng(8)
        u = np.linspace(0, 1, 11)
        for layer, (n, lift0) in enumerate(((32, 0.0055), (26, 0.0080), (18, 0.0105))):
            for kk in range(n):
                th_end = -math.pi / 2 + 2 * math.pi * (kk + 0.45 * layer + rng.random() * 0.5) / n
                front = math.exp(-(math.degrees(abs(phi_of(th_end))) / 50.0) ** 2)
                side_sweep = 0.30 * front                                     # the fringe falls toward the left
                th0 = th_end - side_sweep + rng.normal(0, 0.12)
                fall = front > 0.55 and rng.random() < 0.18                  # a few strands fall forward
                e1 = float(hairline(np.array(th_end))) + math.radians((-7.0 if fall else 3.0 * front) + rng.normal(0, 2.0) - 1.0)
                e0 = math.radians(78 + rng.normal(0, 5))
                TH_ = th0 + (th_end - th0) * u + rng.normal(0, 0.06) * np.sin(math.pi * u)
                EL = e0 + (e1 - e0) * u
                vol = lift0 + 0.005 * np.sin(math.pi * np.clip(u * 1.1, 0, 1)) + 0.008 * front * np.sin(math.pi * np.clip(u * 1.25, 0, 1))
                flick = 0.0025 * u ** 3 * (1 - 0.5 * front) * (0.0 if fall else 1.0)   # ends lift a little
                w = (0.016 + 0.006 * rng.random()) * np.clip(np.sin(math.pi * np.clip(u * 0.92 + 0.06, 0, 1)), 0, 1) ** 0.6 + 0.0008
                settle = 1.0 - 0.55 * np.clip((u - 0.75) / 0.25, 0, 1) ** 1.5
                lock(p, dirs(TH_, EL), (vol + flick) * settle, w, arch=0.0028)
    return p


# ---------------------------------------------------------------- headgear
def face_opening(lift_deg=0.0, width=58.0, chin=-58.0):
    """Lower edge of a helmet: high across the face (the opening), low at the sides and back."""
    def lo(th):
        a = np.degrees(np.abs(phi_of_np(th)))
        return np.radians(interp(a, [0, width - 18, width, width + 16, 110, 180], [24 + lift_deg, 22 + lift_deg, 6, chin, chin + 4, -40]))
    return lo


def seam_line(sd, lo, offset, theta0=-math.pi / 2, theta1=1.5 * math.pi, n=160, s=0.0):
    th = np.linspace(theta0, theta1, n)
    e = lo(th)
    return project(sd, dirs(th, e + (math.radians(1.2) if s == 0 else s)), offset)


def make_gear(k):
    p = Part('av_gear_%d' % k)
    if k in (1, 4):     # GRID HELMET / CREST HELMET
        lo = face_opening()
        off = 0.019
        shell(p, 'av_primary_np', sd_skull, -math.pi / 2, 1.5 * math.pi, lo, math.radians(87), off, nu=80, nv=26,
              closed=True, cap_top=True)
        rim = seam_line(sd_skull, lo, off + 0.001, n=96)
        p.tube('av_glow' if k == 1 else 'av_trim', np.vstack([rim, rim[:1]]), 0.0032, segs=8, cap=False)
        # midline from brow to nape, and a ring around the crown
        a = np.radians(np.linspace(27, 168, 60))
        mid = np.array([project(sd_skull, np.array([[0, math.cos(t), math.sin(t)]]), off + 0.0008)[0] for t in a])
        if k == 1:
            p.tube('av_glow', mid, 0.0026, segs=8)
            for side in (-1, 1):
                ln = np.array([project(sd_skull, dirs(np.array(math.pi / 2 + side * math.radians(x)), np.array(math.radians(40)))[None],
                                       off + 0.0008)[0] for x in np.linspace(35, 150, 40)])
                p.tube('av_glow', ln, 0.0022, segs=8)
        else:   # crest fin + cheek guards
            tops = mid + (mid - HEAD_C) / np.linalg.norm(mid - HEAD_C, axis=1, keepdims=True) * \
                (0.012 + 0.03 * np.sin(np.linspace(0.2, math.pi - 0.2, 60))[:, None])
            for f in np.linspace(0, 1, 5):
                p.tube('av_secondary', mid + (tops - mid) * f, 0.006 * (1 - 0.5 * f), segs=8)
            p.tube('av_glow', tops, 0.0024, segs=6)

            for s in (-1, 1):   # cheek guards: plates hanging from the helmet's sides over the cheeks
                a0, a1 = sorted((math.pi / 2 + s * math.radians(54), math.pi / 2 + s * math.radians(82)))
                Q = shell(p, 'av_primary_np', sd_skull, a0, a1, math.radians(LM['lips_el'] + 3), math.radians(1.0), off - 0.006,
                          nu=16, nv=10)
                p.tube('av_trim', Q[0] if s > 0 else Q[-1], 0.0022, segs=6)   # the front edge
    elif k == 2:        # RACER HELMET: egg shell, top stripes, small rear spoiler
        def sd_r(P):
            return smin(sd_skull(P), sd_ellipsoid(P, (0, -0.07, 0.02), (0.09, 0.115, 0.115)), 0.03)
        lo = face_opening(lift_deg=-4, width=56, chin=-48)
        off = 0.024
        shell(p, 'av_primary_np', sd_r, -math.pi / 2, 1.5 * math.pi, lo, math.radians(87), off, nu=80, nv=26,
              closed=True, cap_top=True)
        rim = seam_line(sd_r, lo, off + 0.001, n=96)
        p.tube('av_dark', np.vstack([rim, rim[:1]]), 0.006, segs=10, cap=False)
        for side in (-1, 1):
            a = np.radians(np.linspace(28, 165, 60))
            ln = np.array([project(sd_r, np.array([[side * 0.16, math.cos(t), math.sin(t)]]) /
                                   np.linalg.norm([side * 0.16, math.cos(t), math.sin(t)]), off + 0.0008)[0] for t in a])
            p.tube('av_secondary', ln, 0.0055, segs=8)
        back = project(sd_r, dirs(np.array(-math.pi / 2), np.array(math.radians(25)))[None], off)[0]

        def spoiler(b):
            b.box('av_secondary', tuple(back + np.array([0, -0.012, 0.004])), (0.07, 0.012, 0.012), bevel=0.004)
        p.builder(spoiler)
    elif k == 3:        # HOOD: soft fabric shell with a peak at the back and a wide face opening
        def sd_h(P):
            d = smin(sd_skull(P), sd_ellipsoid(P, (0, -0.13, 0.04), (0.07, 0.09, 0.09)), 0.04)
            return d
        lo = face_opening(lift_deg=6, width=62, chin=-62)
        off = lambda th, s: 0.022 + 0.010 * np.exp(-((s - 0.35) / 0.3) ** 2)
        shell(p, 'av_primary', sd_h, -math.pi / 2, 1.5 * math.pi, lo, math.radians(87), off, nu=80, nv=26,
              closed=True, cap_top=True)
        rim = seam_line(sd_h, lo, off(np.zeros(1), np.zeros(1))[0] + 0.004)
        p.tube('av_secondary', np.vstack([rim, rim[:1]]), 0.007, segs=10, cap=False)
    elif k == 5:        # HEADPHONES: band over the top, cups with glow rings
        a = np.radians(np.linspace(-2, 182, 50))
        band = np.array([project(sd_skull, np.array([[math.cos(t), -0.15, math.sin(t)]]) /
                                 np.linalg.norm([math.cos(t), -0.15, math.sin(t)]), 0.024)[0] for t in a])
        p.tube('av_secondary', band, 0.0075, segs=10)

        def cups(b):
            for s in (-1, 1):
                c = (s * 0.088, -0.083, -0.010)
                b.cylinder('av_dark', c, 0.036, 0.026, segs=28, axis='X')
                b.cylinder('av_secondary', (c[0] + s * 0.015, c[1], c[2]), 0.031, 0.008, segs=28, axis='X')
                b.torus('av_glow', (c[0] + s * 0.0195, c[1], c[2]), 0.022, 0.0022, segs=28, tsegs=5, axis='X')
        p.builder(cups)
    elif k == 6:        # CAP: crown + curved brim + button
        def lo(th):
            a = np.degrees(np.abs(phi_of_np(th)))
            return np.radians(interp(a, [0, 60, 100, 180], [30, 22, 6, -6]))   # down over the back of the head
        shell(p, 'av_primary_np', sd_skull, -math.pi / 2, 1.5 * math.pi, lo, math.radians(87), 0.011, nu=96, nv=26,
              closed=True, cap_top=True)
        front = project(sd_skull, dirs(np.array(math.pi / 2), np.array(math.radians(31)))[None], 0.012)[0]
        u = np.linspace(-1, 1, 24)
        v = np.linspace(0, 1, 8)
        U, V = np.meshgrid(u, v)
        X = U * (0.072 - 0.022 * V)
        Y = front[1] - 0.016 + V * 0.072 - (U ** 2) * (0.026 - 0.012 * V)
        Z = front[2] - 0.006 - V * 0.014 - 0.012 * (U ** 2)
        P = np.stack([X, Y, Z], -1)
        p.grid('av_secondary', P)
        p.grid('av_secondary', P - np.array([0, 0, 0.003]), flip=True)
        top = project(sd_skull, np.array([[0, 0, 1.0]]), 0.011)[0]

        def btn(b):
            b.sphere('av_secondary', tuple(top + np.array([0, 0, 0.003])), 0.007, segs=12, rings=6)
        p.builder(btn)
    return p


# ---------------------------------------------------------------- face shields
def make_visor(k):
    p = Part('av_visor_%d' % k)
    if k == 1:      # BAND VISOR: wraparound glass band over the eyes, glowing edges
        t0, t1 = math.pi / 2 - math.radians(108), math.pi / 2 + math.radians(108)
        lo = lambda th: np.radians(-9 + 4 * (np.abs(phi_of_np(th)) / math.pi) * 3)
        hi = lambda th: np.radians(13 - 2 * (np.abs(phi_of_np(th)) / math.pi) * 3)
        P = shell(p, 'av_visor', sd_skull, t0, t1, lo, hi, 0.024, nu=80, nv=10)
        p.tube('av_glow', P[0], 0.0018, segs=6)
        p.tube('av_glow', P[-1], 0.0018, segs=6)
    elif k == 2:    # FULL SHIELD: smooth dome over the whole face
        def sd_f(Pp):   # clears the nose tip (LM) by ~1 cm
            front = float(LM['nose'][1]) + 0.010
            return sd_ellipsoid(Pp, (0, -0.058, -0.04), (0.094, front + 0.058, 0.124))
        t0, t1 = math.pi / 2 - math.radians(92), math.pi / 2 + math.radians(92)
        lo = lambda th: np.radians(-60 + 18 * (np.abs(phi_of_np(th)) / math.radians(92)) ** 2)
        hi = lambda th: np.radians(30 - 6 * (np.abs(phi_of_np(th)) / math.radians(92)) ** 2)
        P = shell(p, 'av_visor', sd_f, t0, t1, lo, hi, 0.004, nu=80, nv=30)
        edge = np.vstack([P[0], P[1:-1, -1], P[-1][::-1], P[-2:0:-1, 0]])
        p.tube('av_glow', np.vstack([edge, edge[:1]]), 0.0024, segs=6, cap=False)
    elif k in (3, 4):   # SHADES / GOGGLES
        def face_y(x, z):   # the face surface seen straight from the front at (x, z)
            if HEAD_KEY:
                loc, _, _, _ = _tree.ray_cast(Vector((x, 0.25, z)), Vector((0, -1.0, 0)), 0.5)
                if loc is not None:
                    return loc[1]
                return HEAD_C[1]
            return head_point(x, z, 0.0)[1]

        def clear(P, gap):   # push a point radially out of the head (current radial map) to `gap` above it
            V = np.asarray(P, float) - HEAD_C
            on = project(sd_skull, (V / np.linalg.norm(V))[None], gap)[0]
            return on if np.linalg.norm(on - HEAD_C) > np.linalg.norm(V) else np.asarray(P, float)

        # how far forward the face reaches around each eye (brow, cheekbone, nose bridge) within the lens radius
        def face_front(x0, z0, rad):
            return max(face_y(x0 + rr * math.cos(ang), z0 + rr * math.sin(ang))
                       for ang in np.linspace(0, 2 * math.pi, 16, endpoint=False) for rr in (0.4 * rad, rad))
        rad_l = 0.021 if k == 4 else 0.020
        y_face = max(face_front(s_ * 0.031, 0.002, rad_l) for s_ in (-1, 1))
        cy = y_face + (0.006 if k == 3 else 0.008)

        def lens(b):
            for s in (-1, 1):
                c = (s * 0.031, cy, 0.002)
                if k == 3:
                    tmp = bmesh.new()
                    bmesh.ops.create_cube(tmp, size=1.0)
                    bmesh.ops.scale(tmp, vec=(0.040, 0.004, 0.025), verts=tmp.verts)
                    bmesh.ops.bevel(tmp, geom=tmp.edges[:] + tmp.verts[:], offset=0.0018, segments=3, affect='EDGES',
                                    clamp_overlap=True)
                    rot = Matrix.Rotation(math.radians(-10 * s), 4, 'Z')
                    bmesh.ops.transform(tmp, matrix=Matrix.Translation(c) @ rot, verts=tmp.verts)
                    b.add_bmesh(tmp, 'av_visor', smooth=True)
                    b.box('av_dark', (c[0], c[1] - 0.001, c[2] + 0.0135), (0.044, 0.005, 0.004), bevel=0.0015,
                          rot=Matrix.Rotation(math.radians(-10 * s), 3, 'Z'))
                    # hinge block at the outer top corner of the frame
                    b.box('av_dark', (s * 0.0525, cy - 0.006, 0.0125), (0.004, 0.007, 0.005), bevel=0.0012)
                else:
                    b.cylinder('av_visor', (c[0], c[1] + 0.0035, c[2]), 0.0175, 0.003, segs=24, axis='Y')
                    b.torus('av_trim', (c[0], c[1] + 0.0035, c[2]), 0.019, 0.0022, segs=24, tsegs=6, axis='Y')
                    b.cylinder('av_dark', (c[0], c[1] + 0.0005, c[2]), 0.0205, 0.004, segs=24, axis='Y')
            if k == 3:
                b.box('av_dark', (0, cy + 0.001, 0.008), (0.016, 0.004, 0.004), bevel=0.0015)
            else:
                b.box('av_dark', (0, cy - 0.002, 0.004), (0.022, 0.007, 0.007), bevel=0.0025)
        p.builder(lens)
        if k == 3:      # temple arms: hinge -> straight back over the top of the ear -> a short turn down behind it
            for s_ in (-1, 1):
                H = np.array([s_ * 0.0535, cy - 0.009, 0.0125])
                E = np.array([s_ * (LM['ear_x'] - 0.004), LM['ear_y'] + 0.004, LM['ear_top'] + 0.002])
                pts = [clear(H + (E - H) * t, 0.0022) for t in np.linspace(0, 1, 14)]
                for d in ((0, -0.008, -0.005), (0, -0.013, -0.013), (0, -0.015, -0.021)):
                    pts.append(clear(E + np.array(d), 0.0018))
                p.tube('av_dark', np.array(pts), 0.0019, segs=6)
        else:           # cups: from the lens back to the face, sealed with a soft rubber rim; strap around the head
            ang = np.linspace(0, 2 * math.pi, 28, endpoint=False)
            for s_ in (-1, 1):
                cx = s_ * 0.031
                front = np.stack([cx + 0.0205 * np.cos(ang), np.full_like(ang, cy - 0.0015), 0.002 + 0.0205 * np.sin(ang)], -1)
                back = np.array([[cx + 0.0225 * math.cos(a_), 0.0, 0.002 + 0.0225 * math.sin(a_)] for a_ in ang])
                back[:, 1] = [min(face_y(q[0], q[2]) - 0.0025, cy - 0.006) for q in back]
                tmp = bmesh.new()
                vf = [tmp.verts.new(tuple(q)) for q in front]
                vb = [tmp.verts.new(tuple(q)) for q in back]
                n_ = len(ang)
                for i in range(n_):
                    j = (i + 1) % n_
                    f = tmp.faces.new((vf[i], vb[i], vb[j], vf[j]) if s_ < 0 else (vf[j], vb[j], vb[i], vf[i]))
                    f.smooth = True
                bmesh.ops.recalc_face_normals(tmp, faces=tmp.faces)
                bmesh.ops.solidify(tmp, geom=tmp.faces[:], thickness=0.0012)
                p.builder(lambda b, tmp=tmp: b.add_bmesh(tmp, 'av_dark', smooth=True))
                p.tube('av_dark', np.vstack([back, back[:1]]) + np.array([0, 0.0008, 0]), 0.0022, segs=6, cap=False)
            th = np.linspace(-math.pi / 2, 1.5 * math.pi, 80, endpoint=False)
            band = project(sd_skull, dirs(th, np.full_like(th, math.radians(8))), 0.004)
            sel = np.abs(phi_of_np(th)) > math.radians(48)
            pts = band[sel]
            order = np.argsort(np.mod(th[sel] - math.pi / 2, 2 * math.pi))
            pts = pts[order]
            side = lambda s_: np.array([s_ * 0.0535, cy - 0.010, 0.003])
            pts = np.vstack([side(-1)[None], pts, side(1)[None]])
            p.tube('av_dark', pts, 0.0055, segs=6)
    elif k == 5:    # HEADSET: VR goggles box + straps
        def box(b):
            tmp = bmesh.new()
            bmesh.ops.create_cube(tmp, size=1.0)
            bmesh.ops.scale(tmp, vec=(0.165, 0.075, 0.088), verts=tmp.verts)
            bmesh.ops.bevel(tmp, geom=tmp.edges[:] + tmp.verts[:], offset=0.022, segments=4, affect='EDGES', clamp_overlap=True)
            HY = 0.032   # the face gasket rests on the brow and cheekbones; the nose sits inside the box
            bmesh.ops.translate(tmp, vec=(0, HY, 0.004), verts=tmp.verts)
            b.add_bmesh(tmp, 'av_primary_np', smooth=True)
            b.box('av_secondary', (0, HY + 0.0385, 0.004), (0.13, 0.004, 0.06), bevel=0.012, segments=3)
            b.box('av_glow', (0, HY + 0.041, -0.018), (0.07, 0.002, 0.004))
            for s in (-1, 1):
                b.sphere('av_dark', (s * 0.040, HY + 0.0405, 0.020), 0.006, segs=10, rings=6)
        p.builder(box)
        th = np.linspace(-math.pi / 2, 1.5 * math.pi, 80, endpoint=False)
        band = project(sd_skull, dirs(th, np.full_like(th, math.radians(10))), 0.006)
        sel = np.abs(phi_of_np(th)) > math.radians(50)
        order = np.argsort(np.mod(th[sel] - math.pi / 2, 2 * math.pi))
        p.tube('av_dark', band[sel][order], 0.009, segs=6)
        a = np.radians(np.linspace(40, 150, 30))
        top = np.array([project(sd_skull, np.array([[0, math.cos(t), math.sin(t)]]), 0.006)[0] for t in a])
        p.tube('av_dark', top, 0.008, segs=6)
    elif k == 6:    # HALO: a glowing ring orbiting the head (Tron)
        def halo(b):
            b.torus('av_glow', (0, -0.072, 0.050), 0.135, 0.0035, segs=96, tsegs=6)
            for a in (math.radians(90), math.radians(210), math.radians(330)):
                b.sphere('av_glow', (0.135 * math.cos(a), -0.072 + 0.135 * math.sin(a), 0.050), 0.0075, segs=12, rings=8)
        p.builder(halo)
        # also a slim visor line at the eyes
        t0, t1 = math.pi / 2 - math.radians(70), math.pi / 2 + math.radians(70)
        P = shell(p, 'av_visor', sd_skull, t0, t1, math.radians(-2), math.radians(5), 0.020, nu=50, nv=4)
    elif k == 7:    # SLIT VISOR: opaque plate across the eyes with a glowing slit
        t0, t1 = math.pi / 2 - math.radians(100), math.pi / 2 + math.radians(100)

        def mat_fn(th, s):
            return 'av_glow' if 0.58 < s < 0.68 and abs(phi_of(th)) < math.radians(62) else 'av_primary_np'
        P = shell(p, None, sd_skull, t0, t1, math.radians(-14), math.radians(20), 0.022, nu=90, nv=26, mat_fn=mat_fn)
        p.tube('av_trim', P[0], 0.0025, segs=6)
        p.tube('av_trim', P[-1], 0.0025, segs=6)
    return p


# ---------------------------------------------------------------- masks
def make_mask(k):
    p = Part('av_mask_%d' % k)
    L = LM
    sub = L['nose_el'] + 0.42 * (L['lips_el'] - L['nose_el'])   # just under the nose
    if k == 1:      # MOUTH GUARD: a plate from under the nose to under the chin, vents over the mouth
        t0, t1 = math.pi / 2 - math.radians(66), math.pi / 2 + math.radians(66)
        lo = lambda th: np.radians(L['under_el'] + 10 * (np.abs(phi_of_np(th)) / math.radians(66)) ** 2)
        hi = lambda th: np.radians(sub - 9 * (np.abs(phi_of_np(th)) / math.radians(66)) ** 2)
        P = shell(p, 'av_secondary', sd_skull, t0, t1, lo, hi, 0.009, nu=60, nv=16)
        p.tube('av_glow', P[-1], 0.0018, segs=6)
        p.tube('av_trim', P[0], 0.0020, segs=6)

        def vents(b):
            for dz in (0.004, -0.008, -0.020):
                c = head_point(0, L['lips'][2] + dz, 0.0105, sd=sd_skull)
                b.box('av_dark', tuple(c), (0.030, 0.004, 0.004), bevel=0.0014)
        p.builder(vents)
    elif k == 2:    # CYBER JAW: armoured jaw from ear to ear, glowing vents
        A = 104.0
        t0, t1 = math.pi / 2 - math.radians(A), math.pi / 2 + math.radians(A)
        lo = lambda th: np.radians(L['under_el'] + 4 + 16 * (np.abs(phi_of_np(th)) / math.radians(A)) ** 2)
        hi = lambda th: np.radians(L['lips_el'] + 3 + (L['ear_el'] - 12 - L['lips_el'] - 3) * (np.abs(phi_of_np(th)) / math.radians(A)) ** 2)
        P = shell(p, 'av_trim', sd_skull, t0, t1, lo, hi, 0.008, nu=80, nv=16)
        p.tube('av_dark', P[-1], 0.0022, segs=6)
        p.tube('av_dark', P[0], 0.0022, segs=6)

        def vents(b):
            for s_ in (-1, 1):
                for i in range(3):
                    th = math.pi / 2 + s_ * math.radians(38 + 9 * i)
                    el = math.radians(L['lips_el'] - 6 - 3 * i)
                    c = project(sd_skull, dirs(np.array(th), np.array(el))[None], 0.0105)[0]
                    b.box('av_glow', tuple(c), (0.004, 0.016, 0.003), rot=Matrix.Rotation(th - math.pi / 2, 3, 'Z'))
        p.builder(vents)
    elif k == 3:    # BREATHER: a mask over nose and mouth, a snout, side filters and straps
        A = 60.0
        t0, t1 = math.pi / 2 - math.radians(A), math.pi / 2 + math.radians(A)
        lo = lambda th: np.radians(L['chin_el'] - 4 + 8 * (np.abs(phi_of_np(th)) / math.radians(A)) ** 2)
        hi = lambda th: np.radians(L['nose_el'] + 8 - 10 * (np.abs(phi_of_np(th)) / math.radians(A)) ** 2)
        P = shell(p, 'av_secondary', sd_head, t0, t1, lo, hi, 0.008, nu=48, nv=14)
        p.tube('av_dark', P[-1], 0.002, segs=6)
        p.tube('av_dark', P[0], 0.002, segs=6)
        snout = head_point(0, L['lips'][2] + 0.004, 0.009)

        def parts(b):
            b.cylinder('av_dark', tuple(snout + np.array([0, 0.010, 0])), 0.019, 0.022, segs=20, axis='Y')
            b.torus('av_trim', tuple(snout + np.array([0, 0.022, 0])), 0.017, 0.0028, segs=20, tsegs=6, axis='Y')
            b.disc('av_glow', tuple(snout + np.array([0, 0.0215, 0])), 0.008, segs=16, axis='Y', flip=True)
            for s_ in (-1, 1):
                th = math.pi / 2 + s_ * math.radians(40)
                c = project(sd_head, dirs(np.array(th), np.array(math.radians(L['lips_el'] - 4)))[None], 0.012)[0]
                rot = Matrix.Rotation(math.radians(32 * s_), 3, 'Z') @ Matrix.Rotation(math.radians(-12), 3, 'X')
                tmp = bmesh.new()
                bmesh.ops.create_cone(tmp, cap_ends=True, segments=20, radius1=0.015, radius2=0.015, depth=0.026)
                bmesh.ops.transform(tmp, matrix=Matrix.Translation(Vector(c)) @ (rot @ Matrix.Rotation(math.pi / 2, 3, 'X')).to_4x4(),
                                    verts=tmp.verts)
                b.add_bmesh(tmp, 'av_dark', smooth=False)
        p.builder(parts)
        for s_ in (-1, 1):
            th = np.linspace(math.pi / 2 + s_ * math.radians(56), math.pi / 2 + s_ * math.radians(178), 30)
            strap = project(sd_skull, dirs(th, np.full_like(th, math.radians(L['lips_el'] + 12))), 0.003)
            p.tube('av_dark', strap, 0.0038, segs=6)
    elif k == 4:    # BANDANA: a printed cloth over nose and mouth, tied at the nape, its point hanging below the chin
        A = 84.0                                   # the cloth reaches back to just in front of the ears
        t0, t1 = math.pi / 2 - math.radians(A), math.pi / 2 + math.radians(A)
        top = L['nose_el'] + 11.0
        side_top = L['lips_el'] + 6.0              # the top edge comes down the cheeks toward the ear lobes

        def lo(th):   # the hem: a V down to the point under the chin, up to the jaw at the sides
            a_ = np.abs(phi_of_np(th)) / math.radians(A)
            v = L['under_el'] - 17.0
            return np.radians(v + (L['lips_el'] - 16.0 - v) * a_ ** 0.8)

        def hi(th):
            a_ = np.abs(phi_of_np(th)) / math.radians(A)
            return np.radians(top + (side_top - top) * a_ ** 1.6)

        def off(th, s_):   # loose cloth: stands off the face low down, shallow folds running down from the ties
            ph = np.abs(phi_of_np(th))
            fold = 0.0022 * np.sin(np.degrees(ph) / 9.0) * (1 - s_)
            hang = 0.014 * (1 - s_) ** 2.2 * np.exp(-(np.degrees(ph) / 55) ** 2)
            return 0.0055 + hang + fold
        nu_, nv_ = 80, 26
        P = shell(Part('tmp'), 'av_cloth', sd_skull, t0, t1, lo, hi, off, nu=nu_, nv=nv_)
        drop = math.radians(L['chin_el'] - 3.0)
        down = np.array([0.0, 0.10, -1.0])
        down /= np.linalg.norm(down)
        P0 = P.copy()
        for iu in range(nu_):   # rows run from the hem (0) up to the top edge
            V = P0[:, iu] - HEAD_C
            el = np.arctan2(V[:, 2], np.hypot(V[:, 0], V[:, 1]))
            below = np.nonzero(el < drop)[0]
            if len(below) == 0 or below.max() >= nv_ - 2:
                continue
            j0 = below.max() + 1
            acc = 0.0
            for j in range(j0 - 1, -1, -1):
                acc += np.linalg.norm(P0[j] [iu] - P0[j + 1][iu])
                P[j, iu] = P[j0, iu] + down * acc
        mats = np.where((np.arange(nv_ - 1)[:, None] + 0.5) / (nv_ - 1) < 0.09, 'av_cloth_dk', 'av_cloth')
        p.grid(np.broadcast_to(mats, (nv_ - 1, nu_ - 1)).astype(object), P)
        p.tube('av_cloth_dk', P[-1], 0.0022, segs=6)                        # rolled top edge
        p.tube('av_cloth_dk', P[0], 0.0015, segs=5)                         # stitched hem

        # the print: white dots in staggered rows over the cloth, a finer row along the border
        du = np.gradient(P, axis=1)
        dv = np.gradient(P, axis=0)
        Nn = np.cross(du, dv)
        Nn /= np.maximum(np.linalg.norm(Nn, axis=-1, keepdims=True), 1e-12)
        out = np.sign(np.sum(Nn * (P - HEAD_C), -1, keepdims=True))
        Nn *= np.where(out == 0, 1, out)
        spots = []
        for iv in range(5, 24, 3):
            for iu in range(3 + (iv // 3) % 2 * 2, 77, 4):
                spots.append((iv, iu, 0.0017))
        for iu in range(2, 78, 2):
            spots.append((1, iu, 0.0010))

        def prn(b):
            for iv, iu, r in spots:
                c = P[iv, iu] + Nn[iv, iu] * 0.0006
                q = Vector(tuple(Nn[iv, iu])).to_track_quat('Z', 'Y')
                tmp = bmesh.new()
                bmesh.ops.create_circle(tmp, cap_ends=True, radius=r, segments=8)
                bmesh.ops.transform(tmp, matrix=Matrix.Translation(Vector(tuple(c))) @ q.to_matrix().to_4x4(), verts=tmp.verts)
                b.add_bmesh(tmp, 'av_piping', smooth=False)
        p.builder(prn)

        # ties and tails follow the real head (the tight map), under the ears
        keep = PROJ[0]
        PROJ[0] = 'tight'

        def at_z(phi_deg, z, offset):   # the head point at azimuth phi (from the front) and height z
            th = math.pi / 2 + math.radians(phi_deg)
            el = 0.0
            for _ in range(4):
                q = project(sd_skull, dirs(np.array(th), np.array(el))[None], offset)[0]
                r = np.linalg.norm(q - HEAD_C)
                el = math.asin(max(-0.95, min(0.95, (z - HEAD_C[2]) / r)))
            return q
        z_tie = LM['ear_bot'] - 0.007
        knot = at_z(180.0, z_tie, 0.008)
        for s_ in (-1, 1):   # each tie: from the cloth's top corner, under the ear, round to the knot
            c0 = P[-1, 0 if s_ < 0 else -1]
            ph0 = math.degrees(abs(float(phi_of_np(np.arctan2(c0[1] - HEAD_C[1], c0[0] - HEAD_C[0])))))
            pts = [c0]
            for t in np.linspace(0, 1, 26)[1:]:
                ph = ph0 + (176.0 - ph0) * t
                f = min(1.0, (ph - ph0) / (115.0 - ph0))
                f = f * f * (3 - 2 * f)
                pts.append(at_z(s_ * ph, c0[2] + (z_tie - c0[2]) * f, 0.0045))
            p.tube('av_cloth', np.array(pts), 0.0040, segs=6)

        def kn(b):
            b.sphere('av_cloth', tuple(knot), 0.0100, segs=14, rings=8, scale=(1.35, 0.8, 1.0))
        p.builder(kn)
        kd = (knot - HEAD_C) / np.linalg.norm(knot - HEAD_C)
        k_th = math.atan2(kd[1], kd[0])
        k_el = math.asin(kd[2])
        u = np.linspace(0, 1, 12)
        for s_ in (-1, 1):   # two short tails hanging from the knot
            TH = np.full(12, k_th) + s_ * np.radians(3 + 6 * u)
            EL = k_el - np.radians(3 + 17 * u)
            w = (0.0070 + 0.0025 * u) * np.where(u > 0.8, 1 - (u - 0.8) / 0.2 * 0.6, 1.0)
            lock(p, dirs(TH, EL), 0.0080 + 0.004 * u, w, mat='av_cloth', arch=0.0012, nx=4)
        PROJ[0] = keep
    elif k == 5:    # FACEPLATE: smooth full-face plate with glowing eye slits
        A = 78.0
        t0, t1 = math.pi / 2 - math.radians(A), math.pi / 2 + math.radians(A)
        lo_el = lambda th: L['chin_el'] - 6 + 12 * (np.abs(phi_of_np(th)) / math.radians(A)) ** 2
        hi_el = 34.0

        def mat_fn(th, s_):
            ph = abs(phi_of(th))
            el = float(lo_el(np.array(th))) + s_ * (hi_el - float(lo_el(np.array(th))))
            if 1.0 < el < 7.5 and math.radians(9) < ph < math.radians(46):
                return 'av_glow'
            return 'av_secondary'
        P = shell(p, None, sd_skull, t0, t1, lambda th: np.radians(lo_el(th)), math.radians(hi_el), 0.012, nu=80, nv=44,
                  mat_fn=mat_fn)
        edge = np.vstack([P[0], P[1:-1, -1], P[-1][::-1], P[-2:0:-1, 0]])
        p.tube('av_trim', np.vstack([edge, edge[:1]]), 0.0022, segs=6, cap=False)
    return p


# ================================================================ torso + arms
# (z, half width a, front depth bf, back depth bb, superellipse exponent)
PROFILE = np.array([
    [0.010, 0.056, 0.048, 0.054, 2.0],
    [0.000, 0.074, 0.058, 0.066, 2.0],
    [-0.014, 0.112, 0.066, 0.076, 2.05],
    [-0.030, 0.142, 0.078, 0.084, 2.1],
    [-0.050, 0.165, 0.090, 0.090, 2.15],
    [-0.080, 0.173, 0.101, 0.095, 2.2],
    [-0.120, 0.172, 0.107, 0.097, 2.25],
    [-0.170, 0.164, 0.109, 0.096, 2.25],
    [-0.240, 0.149, 0.102, 0.091, 2.2],
    [-0.320, 0.131, 0.093, 0.086, 2.15],
    [-0.400, 0.123, 0.089, 0.084, 2.15],
    [-0.470, 0.127, 0.090, 0.086, 2.15],
    [-0.520, 0.132, 0.091, 0.088, 2.15]])
Z_BOTTOM = -0.52


def spe(t, n):
    return np.sign(t) * np.abs(t) ** (2.0 / n)


def torso_point(theta, z, inflate=0.0, curved=False):
    """Point(s) on the torso surface: theta = azimuth CCW from +X (front = +Y), z <= 0.008 (torso space)."""
    theta, z = np.broadcast_arrays(np.asarray(theta, float), np.asarray(z, float))
    zs = PROFILE[::-1, 0]
    a = np.interp(z, zs, PROFILE[::-1, 1])
    bf = np.interp(z, zs, PROFILE[::-1, 2])
    bb = np.interp(z, zs, PROFILE[::-1, 3])
    n = np.interp(z, zs, PROFILE[::-1, 4])
    phi = phi_of_np(theta)
    if curved:
        a = a * (1 - 0.05 * np.exp(-((z + 0.05) / 0.06) ** 2) - 0.10 * np.exp(-((z + 0.39) / 0.09) ** 2)
                 + 0.04 * np.exp(-((z + 0.50) / 0.04) ** 2))
    c, s = np.cos(theta), np.sin(theta)
    b = np.where(s > 0, bf, bb)
    x = (a + inflate) * spe(c, n)
    y = (b + inflate) * spe(s, n)
    # chest: pectorals (or a bust on the curved build), a spine groove at the back
    pa, pw, pz, pzw = (0.026, 26.0, -0.165, 0.055) if curved else (0.010, 30.0, -0.140, 0.055)
    chest = sum(np.exp(-((np.degrees(phi) - k * 34.0) / pw) ** 2) for k in (-1, 1)) * np.exp(-((z - pz) / pzw) ** 2)
    y = y + pa * chest * (s > 0)
    y = y - 0.004 * np.exp(-((np.degrees(np.abs(phi)) - 180.0) / 9.0) ** 2) * np.exp(-((z + 0.25) / 0.2) ** 2)
    blades = sum(np.exp(-((np.degrees(np.abs(phi)) - 145.0) / 18.0) ** 2) for _ in (0,)) * np.exp(-((z + 0.13) / 0.06) ** 2)
    y = y - 0.008 * blades * (s < 0)   # shoulder blades
    return np.stack([x, y, z], -1)


def torso_uv(theta, z):
    return np.stack([((theta + math.pi / 2) / (2 * math.pi)) % 1.0, 1.0 + z / 0.5], -1)


def loft(p, mat_fn, inflate=0.0, curved=False, z0=Z_BOTTOM, z1=0.008, nz=34, nth=56, cap_bottom=True):
    th = np.linspace(-math.pi / 2, 1.5 * math.pi, nth, endpoint=False)
    zz = np.linspace(z0, z1, nz)
    TH, Z = np.meshgrid(th, zz)
    infl = inflate(TH, Z) if callable(inflate) else inflate
    P = torso_point(TH, Z, infl, curved)
    UV = torso_uv(TH, Z)
    thc = (TH[:-1] + np.roll(TH, -1, 1)[:-1]) * 0.5
    thc[:, -1] = TH[0, -1] + (2 * math.pi / nth) * 0.5
    zc = (Z[:-1] + Z[1:]) * 0.5
    mats = np.vectorize(mat_fn, otypes=[object])(thc, zc) if callable(mat_fn) else mat_fn
    p.grid(mats, P, UV, closed_u=True)
    if cap_bottom:
        p.fan('av_dark', P[0][::-1], np.array([0, -0.005, z0 - 0.01]), apex_n=np.array([0, 0, -1.0]))
    return P


def band(p, mat, z0, z1, inflate, curved=False, nz=4, nth=56):
    return loft(p, mat, inflate, curved, z0, z1, nz, nth, cap_bottom=False)


def surface_line(pts_theta_z, inflate, curved, n=16):
    """Dense points along a polyline given in (theta, z) on the torso surface."""
    out = []
    for (t0, z0), (t1, z1) in zip(pts_theta_z[:-1], pts_theta_z[1:]):
        for f in np.linspace(0, 1, n, endpoint=False):
            out.append((t0 + (t1 - t0) * f, z0 + (z1 - z0) * f))
    out.append(pts_theta_z[-1])
    T = np.array(out)
    infl = inflate(T[:, 0], T[:, 1]) if callable(inflate) else inflate
    return torso_point(T[:, 0], T[:, 1], infl, curved)


def waist_fade(p, curved, inflate=0.006):
    """Energy fade where the body ends: an additive band from the waist down (u = 0.5 bright .. 1 dark)."""
    th = np.linspace(-math.pi / 2, 1.5 * math.pi, 48, endpoint=False)
    zz = np.linspace(Z_BOTTOM - 0.11, Z_BOTTOM + 0.03, 6)
    TH, Z = np.meshgrid(th, zz)
    zc = np.minimum(Z, Z_BOTTOM)
    P = torso_point(TH, np.maximum(Z, Z_BOTTOM - 0.11), inflate + 0.012 * np.clip((Z_BOTTOM - Z) / 0.11, 0, 1), curved)
    P[..., 2] = Z
    t = (Z.max() - Z) / (Z.max() - Z.min())
    UV = np.stack([0.5 + 0.5 * t, (TH + math.pi / 2) / (2 * math.pi)], -1)
    p.grid('av_fade', P, UV, closed_u=True, u_period=0.0)


DEG = math.pi / 180.0


def TH(phi_deg):
    """Azimuth from an angle measured from the front (positive toward the left, -X)."""
    return math.pi / 2 + phi_deg * DEG


def arm_profile(kind):
    """(t along the bone 0..1, x radius, y radius)."""
    if kind == 'upper':
        return np.array([[0.0, 0.059, 0.055], [0.15, 0.057, 0.052], [0.40, 0.048, 0.048], [0.60, 0.045, 0.046],
                         [0.85, 0.040, 0.040], [1.0, 0.037, 0.036]])
    return np.array([[0.0, 0.038, 0.037], [0.15, 0.044, 0.040], [0.35, 0.041, 0.036], [0.65, 0.033, 0.029],
                     [0.90, 0.029, 0.023], [1.0, 0.028, 0.022]])


def arm_point(theta, t, kind, inflate=0.0, L=None):
    L = L or (UPPER if kind == 'upper' else FORE)
    pr = arm_profile(kind)
    rx = np.interp(t, pr[:, 0], pr[:, 1]) + inflate
    ry = np.interp(t, pr[:, 0], pr[:, 2]) + inflate
    if kind == 'upper':   # biceps (front) and triceps (back) bulges, the deltoid cap on the outside
        ry = ry + np.where(np.sin(theta) > 0, 0.008, 0.005) * np.exp(-((t - 0.45) / 0.18) ** 2) * np.abs(np.sin(theta))
        rx = rx + 0.006 * np.exp(-((t - 0.06) / 0.13) ** 2) * np.clip(np.cos(theta), 0, 1)
    # dome over the joint (t < 0): shoulder / elbow ball
    dome = 0.056 if kind == 'upper' else 0.035
    zt = -t * L
    k = np.where(t < 0, np.sqrt(np.clip(1 - (zt / dome) ** 2, 0, 1)), 1.0)
    return np.stack([rx * k * np.cos(theta), ry * k * np.sin(theta), zt], -1)


def arm(p, mat_fn, kind, inflate=0.0, t0=None, t1=1.0, nt=18, nth=22):
    """Bone mesh along -Z. mat_fn(theta, t) -> material; t < 0 is the joint dome."""
    L = UPPER if kind == 'upper' else FORE
    dome_t = (0.056 if kind == 'upper' else 0.035) / L
    t0 = -dome_t if t0 is None else t0
    tt = np.linspace(t1, t0, nt)      # bottom (far end) to top: v upward
    th = np.linspace(-math.pi / 2, 1.5 * math.pi, nth, endpoint=False)
    THm, T = np.meshgrid(th, tt)
    infl = inflate(THm, T) if callable(inflate) else inflate
    P = arm_point(THm, T, kind, infl, L)
    circ = 2 * math.pi * 0.042
    UV = np.stack([((THm + math.pi / 2) / (2 * math.pi)) * circ, 1.0 - T * L / 0.5], -1)
    thc = (THm[:-1] + np.roll(THm, -1, 1)[:-1]) * 0.5
    thc[:, -1] = THm[0, -1] + (2 * math.pi / nth) * 0.5
    tc = (T[:-1] + T[1:]) * 0.5
    mats = np.vectorize(mat_fn, otypes=[object])(thc, tc) if callable(mat_fn) else mat_fn
    p.grid(mats, P, UV, closed_u=True, u_period=circ)
    return P


def arm_line(kind, phi_deg, t0, t1, inflate, n=24):
    th = TH(phi_deg) - math.pi   # +X (outside) is phi = -90: keep the caller's angle convention
    t = np.linspace(t0, t1, n)
    return arm_point(np.full(n, TH(phi_deg)), t, kind, inflate)


def ring_on_arm(p, mat, kind, t, inflate, r=0.004):
    th = np.linspace(-math.pi / 2, 1.5 * math.pi, 28)
    pts = arm_point(th, np.full(28, t), kind, inflate)
    p.tube(mat, pts, r, segs=6, cap=False)


# ---------------------------------------------------------------- suits
def suit(n, curved):
    t = Part('av_torso_%d%s' % (n, 'c' if curved else ''))
    up = Part('av_upper_%d' % n)
    fo = Part('av_fore_%d' % n)
    C = curved
    G = 'av_glow'
    if n == 0:      # GRID RUNNER: tight suit, glowing circuit piping (Tron), high collar
        loft(t, 'av_primary', 0.0, C)
        band(t, 'av_secondary', -0.006, 0.040, lambda th, z: 0.021 - 0.008 * np.clip(z / 0.04, 0, 1), C)   # collar wider than the neck
        I = 0.0012
        lines = [[(TH(58), -0.045), (TH(14), -0.165), (TH(0), -0.205), (TH(-14), -0.165), (TH(-58), -0.045)],
                 [(TH(0), -0.205), (TH(0), -0.47)],
                 [(TH(90), -0.10), (TH(90), -0.47)], [(TH(-90), -0.10), (TH(-90), -0.47)],
                 [(TH(180), -0.03), (TH(180), -0.47)],
                 [(TH(128), -0.12), (TH(180), -0.12), (TH(232), -0.12)],
                 [(TH(22), -0.30), (TH(22), -0.38), (TH(52), -0.40), (TH(52), -0.47)],
                 [(TH(-22), -0.30), (TH(-22), -0.38), (TH(-52), -0.40), (TH(-52), -0.47)]]
        for ln in lines:
            t.tube(G, surface_line(ln, I, C), 0.0026, segs=6)
        t.tube(G, surface_line([(TH(-180 + k * 9), -0.47) for k in range(41)], I, C, n=2), 0.0028, segs=8, cap=False)
        for d in ((14, -0.165), (-14, -0.165), (52, -0.40), (-52, -0.40)):
            c = torso_point(TH(d[0]), d[1], I, C)

            def node(b, c=c):
                b.sphere(G, tuple(c), 0.0055, segs=10, rings=6)
            t.builder(node)
        arm(up, 'av_primary', 'upper')
        up.tube(G, arm_line('upper', -90, -0.2, 0.98, 0.0012), 0.0024, segs=8)
        ring_on_arm(up, 'av_secondary', 'upper', 0.55, 0.002, 0.005)
        arm(fo, 'av_primary', 'fore')
        fo.tube(G, arm_line('fore', -90, 0.05, 0.92, 0.0012), 0.0024, segs=8)
        ring_on_arm(fo, G, 'fore', 0.9, 0.0016, 0.003)
    elif n == 1:    # ARENA JERSEY: loose crew-neck tee, short sleeves, bare forearms
        loft(t, lambda th, z: 'av_secondary' if z > -0.012 or z < -0.49 else 'av_primary', 0.010, C)
        t.tube('av_secondary', surface_line([(TH(-180 + k * 9), -0.012) for k in range(41)], 0.007, C, n=2), 0.0035, cap=False)
        arm(up, lambda th, tt: 'av_skin' if tt > 0.47 else ('av_secondary' if tt > 0.40 else 'av_primary'), 'upper',
            lambda th, tt: np.where(tt < 0.47, 0.011, 0.0))
        arm(fo, 'av_skin', 'fore')
        ring_on_arm(fo, 'av_secondary', 'fore', 0.86, 0.004, 0.007)
    elif n == 2:    # VANGUARD: undersuit, chest plate, segmented abs, glowing core, gorget, pauldrons, vambraces
        loft(t, 'av_dark', 0.0, C)
        loft(t, lambda th, z: 'av_primary_np' if (abs(phi_of(th)) < math.radians(78) and -0.27 < z < -0.03) or
             (abs(phi_of(th)) > math.radians(115) and -0.30 < z < -0.04) else None, 0.014, C, z0=-0.30, z1=-0.02, nz=20,
             cap_bottom=False)
        for i, z in enumerate((-0.30, -0.355, -0.41)):
            loft(t, lambda th, zz: 'av_secondary' if abs(phi_of(th)) < math.radians(60) else None, 0.012, C,
                 z0=z - 0.045, z1=z, nz=5, cap_bottom=False)
        band(t, 'av_trim', -0.012, 0.03, lambda th, z: 0.021 + 0.002 * np.clip(z / 0.03, 0, 1), C)
        core = torso_point(TH(0), -0.15, 0.016, C)

        def coreb(b):
            b.torus('av_trim', tuple(core), 0.022, 0.005, segs=28, tsegs=8, axis='Y')
            b.disc(G, tuple(core + np.array([0, 0.002, 0])), 0.018, segs=24, axis='Y', flip=True)
        t.builder(coreb)
        arm(up, lambda th, tt: 'av_primary_np' if tt < 0.42 else 'av_dark', 'upper',
            lambda th, tt: np.where(tt < 0.42, 0.016 + 0.02 * np.clip(0.3 - tt, 0, 0.3), 0.0))
        ring_on_arm(up, 'av_secondary', 'upper', 0.42, 0.017, 0.006)
        arm(fo, lambda th, tt: 'av_primary_np' if tt > 0.18 else 'av_dark', 'fore', lambda th, tt: np.where(tt > 0.18, 0.010, 0.0))
        fo.tube(G, arm_line('fore', -90, 0.25, 0.85, 0.0115), 0.0028, segs=8)
        ring_on_arm(fo, 'av_trim', 'fore', 0.2, 0.011, 0.005)
    elif n == 3:    # STREET JACKET: puffy, open collar, zipper with a glowing pull, ribbed cuffs and waistband
        loft(t, lambda th, z: 'av_secondary_dk' if z < -0.47 else 'av_primary', lambda th, z: 0.012 + 0.003 * np.sin(z * 90) ** 2, C)
        band(t, 'av_secondary', -0.03, 0.03, lambda th, z: 0.016 + 0.03 * np.clip(z / 0.03, 0, 1), C)
        t.tube('av_trim', surface_line([(TH(0), -0.03), (TH(0), -0.47)], 0.014, C), 0.0025, segs=6)
        pull = torso_point(TH(0), -0.12, 0.016, C)

        def zipb(b):
            b.box(G, tuple(pull), (0.008, 0.004, 0.016), bevel=0.002)
        t.builder(zipb)
        arm(up, 'av_primary', 'upper', 0.014)
        arm(fo, lambda th, tt: 'av_secondary_dk' if tt > 0.86 else 'av_primary', 'fore', 0.012)
    elif n == 4:    # HOODIE: soft, hood down behind the neck, kangaroo pocket, drawstrings
        loft(t, lambda th, z: 'av_secondary_dk' if z < -0.48 else 'av_primary', 0.016, C)

        def hood(b):
            b.torus('av_primary', (0, -0.035, -0.005), 0.070, 0.024, segs=36, tsegs=12)
            b.sphere('av_primary', (0, -0.115, -0.04), 0.06, segs=24, rings=12, scale=(1.4, 0.55, 1.0))
        t.builder(hood)
        loft(t, lambda th, z: 'av_secondary' if abs(phi_of(th)) < math.radians(34) else None, 0.022, C, z0=-0.45, z1=-0.33,
             nz=8, cap_bottom=False)
        for s in (-1, 1):
            a = torso_point(TH(s * 9), -0.005, 0.02, C)
            pts = [a + np.array([0, 0.004, -0.11 * f]) for f in np.linspace(0, 1, 10)]
            t.tube('av_secondary', pts, 0.0028, segs=6)

            def tip(b, c=pts[-1]):
                b.sphere(G, tuple(c), 0.005, segs=10, rings=6)
            t.builder(tip)
        arm(up, 'av_primary', 'upper', 0.017)
        arm(fo, lambda th, tt: 'av_secondary_dk' if tt > 0.85 else 'av_primary', 'fore', 0.015)
    elif n == 5:    # ECHO SUIT: sleek, high collar, glowing identity disc on the back, V lines, shoulder seams
        loft(t, 'av_primary', 0.002, C)
        band(t, 'av_secondary', -0.006, 0.05, lambda th, z: 0.021 - 0.007 * np.clip(z / 0.05, 0, 1), C)
        disc = torso_point(TH(180), -0.17, 0.004, C)

        def discb(b):
            b.torus(G, tuple(disc + np.array([0, -0.008, 0])), 0.062, 0.006, segs=48, tsegs=8, axis='Y')
            b.torus('av_trim', tuple(disc + np.array([0, -0.008, 0])), 0.045, 0.004, segs=40, tsegs=6, axis='Y')
            b.cylinder('av_secondary', tuple(disc + np.array([0, -0.006, 0])), 0.040, 0.008, segs=40, axis='Y')
            b.disc(G, tuple(disc + np.array([0, -0.0105, 0])), 0.012, segs=20, axis='Y')
        t.builder(discb)
        for ln in ([(TH(70), -0.04), (TH(0), -0.26), (TH(-70), -0.04)], [(TH(40), -0.25), (TH(0), -0.40), (TH(-40), -0.25)],
                   [(TH(95), -0.04), (TH(95), -0.30)], [(TH(-95), -0.04), (TH(-95), -0.30)]):
            t.tube(G, surface_line(ln, 0.0032, C), 0.0024, segs=8)
        arm(up, 'av_primary', 'upper', 0.002)
        up.tube(G, arm_line('upper', -60, -0.15, 0.95, 0.0032), 0.0022, segs=8)
        arm(fo, 'av_primary', 'fore', 0.002)
        ring_on_arm(fo, G, 'fore', 0.30, 0.004, 0.0032)
        ring_on_arm(fo, 'av_secondary', 'fore', 0.88, 0.004, 0.006)
    elif n == 6:    # TANK TOP: sleeveless, scoop neck, glowing belt line, bare arms with wristbands
        def mt(th, z):
            ph = abs(phi_of(th))
            x = abs(math.cos(th))
            if z > -0.02:
                return 'av_skin'
            if z > -0.14 and x > 0.56:
                return 'av_skin'                       # bare shoulders / armholes
            if z > -0.09 and ph < math.radians(40):
                return 'av_skin'                       # scoop neckline
            return 'av_primary'
        loft(t, mt, 0.0, C)
        t.tube(G, surface_line([(TH(-180 + k * 9), -0.48) for k in range(41)], 0.004, C, n=2), 0.0035, segs=8, cap=False)
        arm(up, 'av_skin', 'upper')
        ring_on_arm(up, G, 'upper', 0.40, 0.001, 0.003)
        arm(fo, 'av_skin', 'fore')
        ring_on_arm(fo, 'av_secondary', 'fore', 0.84, 0.006, 0.009)
    elif n == 7:    # PILOT: flight suit, chest harness, chest light, collar, wrist computer
        loft(t, 'av_primary', 0.008, C)
        band(t, 'av_secondary', -0.008, 0.028, lambda th, z: 0.020 - 0.004 * np.clip(z / 0.028, 0, 1), C)
        for s in (-1, 1):
            t.tube('av_secondary', surface_line([(TH(s * 32), 0.0), (TH(s * 22), -0.20), (TH(s * 12), -0.42)], 0.011, C), 0.009, segs=6)
            t.tube('av_secondary', surface_line([(TH(s * 160), 0.0), (TH(s * 170), -0.25), (TH(s * 175), -0.42)], 0.011, C), 0.009, segs=6)
        t.tube('av_secondary', surface_line([(TH(-180 + k * 9), -0.34) for k in range(41)], 0.011, C, n=2), 0.010, segs=6, cap=False)
        lamp = torso_point(TH(0), -0.18, 0.012, C)

        def lampb(b):
            b.box('av_trim', tuple(lamp), (0.05, 0.010, 0.032), bevel=0.004)
            b.box(G, tuple(lamp + np.array([0, 0.0055, 0])), (0.034, 0.002, 0.016))
        t.builder(lampb)
        arm(up, 'av_primary', 'upper', 0.010)
        patch = arm_point(np.array(TH(-90)), np.array(0.3), 'upper', 0.012)

        def pat(b):
            b.box('av_trim', tuple(patch), (0.004, 0.032, 0.032), bevel=0.003)
        up.builder(pat)
        arm(fo, lambda th, tt: 'av_secondary_dk' if tt > 0.86 else 'av_primary', 'fore', 0.009)
        comp = arm_point(np.array(TH(0)), np.array(0.62), 'fore', 0.013)

        def wc(b):
            b.box('av_trim', tuple(comp), (0.030, 0.012, 0.044), bevel=0.004)
            b.box(G, tuple(comp + np.array([0, 0.0065, 0])), (0.020, 0.002, 0.030))
        fo.builder(wc)
    elif n == 8:    # CIRCUIT VEST: sleeveless compression top, mock collar, glowing circuit lines, armoured bracers
        def mv(th, z):
            x = abs(math.cos(th))
            ph = abs(math.degrees(phi_of(th)))
            if x > 0.80 + 1.2 * max(0.0, -0.05 - z) and z > -0.15:
                return 'av_skin'                       # armholes: bare shoulders
            if 62 < ph < 112 and -0.44 < z < -0.16:
                return 'av_primary_dk'                 # perforated side panels
            return 'av_primary'
        loft(t, mv, 0.0, C)
        band(t, 'av_primary_np', -0.004, 0.058, lambda th, z: 0.023 - 0.010 * np.clip(z / 0.058, 0, 1), C)   # mock collar over the neck
        band(t, 'av_primary_np', -0.505, -0.465, 0.006, C)            # belt
        I = 0.0012
        lines = [[(TH(46), -0.03), (TH(16), -0.115), (TH(0), -0.15), (TH(-16), -0.115), (TH(-46), -0.03)],   # chest Y
                 [(TH(0), -0.15), (TH(0), -0.205)],
                 [(TH(11), -0.105), (TH(0), -0.125), (TH(-11), -0.105)],
                 [(TH(70), -0.15), (TH(62), -0.26), (TH(44), -0.33), (TH(44), -0.46)],        # front side lines
                 [(TH(-70), -0.15), (TH(-62), -0.26), (TH(-44), -0.33), (TH(-44), -0.46)],
                 [(TH(100), -0.17), (TH(100), -0.46)], [(TH(-100), -0.17), (TH(-100), -0.46)],
                 [(TH(180), -0.04), (TH(180), -0.20)],                                          # back Y
                 [(TH(180), -0.20), (TH(156), -0.27), (TH(144), -0.40)],
                 [(TH(180), -0.20), (TH(204), -0.27), (TH(216), -0.40)],
                 [(TH(136), -0.04), (TH(150), -0.14)], [(TH(224), -0.04), (TH(210), -0.14)],
                 [(TH(22), 0.046), (TH(22), 0.004)], [(TH(-22), 0.046), (TH(-22), 0.004)]]   # collar
        for ln in lines:
            t.tube(G, surface_line(ln, I if ln[0][1] < 0.0 else (lambda th, z: 0.0245 - 0.010 * np.clip(z / 0.058, 0, 1)), C), 0.0024, segs=6)
        for a0 in (-34, -12, 12, 34, 158, 202):                                                 # belt dashes
            t.tube(G, surface_line([(TH(a0 - 6), -0.485), (TH(a0 + 6), -0.485)], 0.0075, C, n=6), 0.0026, segs=6)
        arm(up, 'av_skin', 'upper')
        arm(fo, 'av_skin', 'fore')
        bracer(fo, 'av_primary_np', G, plate=None)
    elif n == 9:    # TRAINING TEE: fitted short-sleeve tee, crew neck, white armour bracers with a light strip
        loft(t, lambda th, z: 'av_primary_dk' if z > -0.012 or z < -0.485 else 'av_primary', 0.005, C)
        for sd in (-1, 1):   # side seams
            t.tube('av_primary_dk', surface_line([(TH(sd * 96), -0.17), (TH(sd * 94), -0.48)], 0.0058, C), 0.0012, segs=4)
        arm(up, lambda th, tt: 'av_skin' if tt > 0.36 else ('av_primary_dk' if tt > 0.32 else 'av_primary'), 'upper',
            lambda th, tt: np.where(tt < 0.36, 0.006, 0.0))
        arm(fo, 'av_skin', 'fore')
        bracer(fo, 'av_dark', G, plate='av_armor_white')
    return t, up, fo


def bracer(fo, base, glow, plate=None):
    """Armoured forearm bracer from below the elbow to the wrist (the gauntlet prop continues it over the hand).
    plate: a raised outer plate (white armour) with a light strip; None: dark armour with glowing circuit lines."""
    T0, T1 = 0.16, 1.0
    arm(fo, base, 'fore', lambda th, tt: 0.008 + 0.003 * np.exp(-((tt - 0.45) / 0.3) ** 2), t0=T0, t1=T1, nt=14)
    ring_on_arm(fo, 'av_trim' if plate else base, 'fore', T0 + 0.004, 0.0085, 0.0042)
    ring_on_arm(fo, 'av_trim' if plate else glow, 'fore', 0.93, 0.010, 0.0034 if plate else 0.0024)
    if plate:
        def pm(th, tt):
            ph = math.degrees(phi_of(th))
            return plate if abs(ph + 90.0) < 52.0 else None   # the outside (back of the forearm)
        arm(fo, pm, 'fore', lambda th, tt: 0.0135 + 0.003 * np.exp(-((tt - 0.5) / 0.25) ** 2), t0=0.24, t1=0.86, nt=12, nth=36)
        fo.tube(glow, arm_line('fore', -90, 0.34, 0.74, 0.0175), 0.0032, segs=8)
        for phd in (-90 - 54, -90 + 54):
            fo.tube('av_dark', arm_line('fore', phd, 0.24, 0.86, 0.0135), 0.0026, segs=6)
    else:
        fo.tube(glow, arm_line('fore', -90, 0.22, 0.92, 0.0122), 0.0024, segs=8)
        fo.tube(glow, arm_line('fore', -54, 0.30, 0.78, 0.0118), 0.0022, segs=8)
        fo.tube(glow, arm_line('fore', -126, 0.30, 0.78, 0.0118), 0.0022, segs=8)
        ring_on_arm(fo, glow, 'fore', 0.26, 0.0115, 0.0022)


# ---------------------------------------------------------------- lower body (LOWER BODY = shorts, thighs)
LOWER_TOP, CROTCH, THIGH_END = -0.465, -0.715, -0.985
HIP_PROFILE = np.array([   # z, half width, front depth, back depth (torso space, below the waist)
    [-0.465, 0.131, 0.093, 0.090],
    [-0.520, 0.147, 0.098, 0.099],
    [-0.570, 0.157, 0.101, 0.108],
    [-0.620, 0.158, 0.093, 0.112],
    [-0.660, 0.146, 0.078, 0.104],
    [-0.700, 0.112, 0.060, 0.082],
    [-0.715, 0.080, 0.050, 0.060]])


def hip_point(theta, z, inflate=0.0, curved=False):
    theta, z = np.broadcast_arrays(np.asarray(theta, float), np.asarray(z, float))
    zs = HIP_PROFILE[::-1, 0]
    a = np.interp(z, zs, HIP_PROFILE[::-1, 1])
    bf = np.interp(z, zs, HIP_PROFILE[::-1, 2])
    bb = np.interp(z, zs, HIP_PROFILE[::-1, 3])
    if curved:   # wider hips, fuller seat
        a = a * (1 + 0.07 * np.exp(-((z + 0.62) / 0.08) ** 2))
        bb = bb * (1 + 0.05 * np.exp(-((z + 0.63) / 0.06) ** 2))
    c, s_ = np.cos(theta), np.sin(theta)
    b = np.where(s_ > 0, bf, bb)
    return np.stack([(a + inflate) * spe(c, 2.2), (b + inflate) * spe(s_, 2.2), z], -1)


def thigh_frame(side, z, curved):
    """Centre and radii (x, y) of a thigh at height z (side +1 = right)."""
    f = np.clip((CROTCH + 0.04 - z) / (CROTCH + 0.04 - THIGH_END), 0, 1)
    top = np.clip((z - (CROTCH + 0.04)) / 0.065, 0, 1)            # above the crotch the thighs merge into the hips
    cx = side * ((0.083 if curved else 0.079) + 0.006 * f)
    rx = (0.080 if curved else 0.075) - 0.015 * f - 0.010 * top
    ry = 0.077 - 0.013 * f - 0.012 * top
    return cx, rx, ry


def make_lower(k, curved):
    p = Part('av_lower_%d%s' % (k, 'c' if curved else ''))
    C = curved
    body = 'av_secondary' if k == 1 else 'av_primary'
    nth = 56
    th = np.linspace(-math.pi / 2, 1.5 * math.pi, nth, endpoint=False)
    # hips: waistband to the crotch
    zz = np.linspace(CROTCH, LOWER_TOP, 18)
    THm, Z = np.meshgrid(th, zz)
    P = hip_point(THm, Z, 0.0, C)
    band_top = LOWER_TOP - 0.035
    mats = np.where((Z[:-1] + Z[1:]) * 0.5 > band_top, 'av_secondary_dk' if k == 1 else 'av_primary_np', body).astype(object)
    p.grid(mats, P, torso_uv(THm, Z), closed_u=True)
    p.fan(body, P[0][::-1], np.array([0, 0.0, CROTCH - 0.012]), apex_n=np.array([0, 0, -1.0]))
    # hem height around each leg: athletic shorts are cut high at the outside (split hem); circuit shorts are long
    def hem(side, a):   # a = azimuth around the thigh; outside = +X for the right leg
        out = max(0.0, math.cos(a) * side)
        return (-0.800 + 0.075 * out ** 3) if k == 1 else -0.865
    for side in (-1, 1):
        zl = np.linspace(THIGH_END, CROTCH + 0.105, 24)
        A = np.linspace(-math.pi / 2, 1.5 * math.pi, 32, endpoint=False)
        AA, ZL = np.meshgrid(A, zl)
        cx, rx, ry = thigh_frame(side, ZL, C)
        P = np.stack([cx + rx * np.cos(AA), ry * np.sin(AA) + 0.004, ZL], -1)
        ac = (AA[:-1] + np.roll(AA, -1, 1)[:-1]) * 0.5
        ac[:, -1] = AA[0, -1] + (2 * math.pi / 32) * 0.5
        zc = (ZL[:-1] + ZL[1:]) * 0.5
        shorts_infl = np.where(np.vectorize(lambda a, z: z > hem(side, a))(AA, ZL), 0.010, 0.0)
        P[..., 0] += shorts_infl * np.cos(AA)
        P[..., 1] += shorts_infl * np.sin(AA)
        m = np.vectorize(lambda a, z: body if z > hem(side, a) else 'av_skin', otypes=[object])(ac, zc)
        p.grid(m, P, np.stack([(AA + math.pi / 2) / (2 * math.pi), 1.0 + ZL / 0.5], -1), closed_u=True)
        # hem trim: white piping (athletic) or a glowing hem (circuit)
        ring = []
        for a in np.linspace(-math.pi / 2, 1.5 * math.pi, 49):
            z = hem(side, a)
            cx, rx, ry = thigh_frame(side, z, C)
            ring.append([cx + (rx + 0.0105) * math.cos(a), (ry + 0.0105) * math.sin(a) + 0.004, z + 0.002])
        p.tube('av_piping' if k == 1 else 'av_glow', np.array(ring), 0.0034 if k == 1 else 0.0024, segs=6, cap=False)
        if k == 1:   # piping up the side split
            pts = []
            for z in np.linspace(hem(side, 0.0 if side > 0 else math.pi), LOWER_TOP - 0.04, 14):
                q = hip_point(np.array(0.0 if side > 0 else math.pi), np.array(max(z, CROTCH)), 0.0045, C)
                if z < CROTCH:
                    cx, rx, ry = thigh_frame(side, z, C)
                    q = np.array([cx + side * (rx + 0.0105), 0.004, z])
                pts.append(q)
            p.tube('av_piping', np.array(pts), 0.0030, segs=6)
        else:        # circuit lines down the outside of each leg, an angled kick at the front
            pts = [hip_point(np.array(0.0 if side > 0 else math.pi), np.array(z), 0.0055, C) for z in np.linspace(-0.50, CROTCH, 8)]
            for z in np.linspace(CROTCH, -0.86, 8):
                cx, rx, ry = thigh_frame(side, z, C)
                pts.append(np.array([cx + side * (rx + 0.0112), 0.004, z]))
            p.tube('av_glow', np.array(pts), 0.0024, segs=6)
            fr = []
            for z, a in ((-0.74, 0.55), (-0.80, 0.95), (-0.86, 0.95)):
                cx, rx, ry = thigh_frame(side, z, C)
                ang = math.pi / 2 - side * (math.pi / 2 - a)
                fr.append([cx + (rx + 0.0112) * math.cos(ang), (ry + 0.0112) * math.sin(ang) + 0.004, z])
            p.tube('av_glow', np.array(fr), 0.0022, segs=6)
        # thigh fade: where the legs end, an energy band (like the waist fade)
        zf = np.linspace(THIGH_END - 0.10, THIGH_END + 0.03, 6)
        AA, ZF = np.meshgrid(np.linspace(-math.pi / 2, 1.5 * math.pi, 32, endpoint=False), zf)
        cx, rx, ry = thigh_frame(side, np.maximum(ZF, THIGH_END), C)
        g = 0.004 + 0.010 * np.clip((THIGH_END - ZF) / 0.10, 0, 1)
        P = np.stack([cx + (rx + g) * np.cos(AA), (ry + g) * np.sin(AA) + 0.004, ZF], -1)
        tt = (ZF.max() - ZF) / (ZF.max() - ZF.min())
        p.grid('av_fade', P, np.stack([0.5 + 0.5 * tt, (AA + math.pi / 2) / (2 * math.pi)], -1), closed_u=True, u_period=0.0)
    if k == 1:   # drawstring at the front of the waistband
        for sd in (-1, 1):
            a = hip_point(np.array(TH(sd * 6)), np.array(LOWER_TOP - 0.02), 0.004, C)
            p.tube('av_piping', [a, a + np.array([sd * 0.004, 0.003, -0.05])], 0.0022, segs=5)
    else:        # glowing dashes on the waistband
        for a0 in (-30, 30, 180):
            p.tube('av_glow', [hip_point(np.array(TH(a0 + d)), np.array(LOWER_TOP - 0.018), 0.003, C) for d in (-7, 0, 7)], 0.0024, segs=6)
    return p


def make_waist(curved):
    p = Part('av_waist%s' % ('_c' if curved else ''))
    waist_fade(p, curved)
    return p


# ================================================================ export
def export(parts, out):
    pos, nrm, uv0, uv1, mat, names, objects = [], [], [], [], [], [], []
    for p in parts:
        if not p.P:
            continue
        P = np.concatenate(p.P)
        N = np.concatenate(p.N)
        U = np.concatenate(p.U)
        M = np.array(p.M, dtype=object)
        local = sorted(set(M))
        for m in local:
            sel = np.nonzero(M == m)[0]
            idx = (sel[:, None] * 3 + np.arange(3)[None]).reshape(-1)
            mi = len(names)
            names.append(m)
            objects.append(p.name)
            pos.append(np.stack([P[idx, 0], P[idx, 2], -P[idx, 1]], -1))
            nrm.append(np.stack([N[idx, 0], N[idx, 2], -N[idx, 1]], -1))
            uv0.append(np.stack([U[idx, 0], 1.0 - U[idx, 1]], -1))
            uv1.append(np.zeros((len(idx), 2)))
            mat.append(np.full(len(idx), mi))
        print('  %-14s %6d tris' % (p.name, len(M)))
    np.savez_compressed(out, pos=np.concatenate(pos).astype(np.float32), nrm=np.concatenate(nrm).astype(np.float32),
                        uv0=np.concatenate(uv0).astype(np.float32), uv1=np.concatenate(uv1).astype(np.float32),
                        mat=np.concatenate(mat).astype(np.int32), names=np.array(names), objects=np.array(objects))
    print('exported', out, sum(len(x) for x in pos) // 3, 'tris')


def main():
    out = common.out_path() or os.path.join(os.getcwd(), 'avatar.npz')
    only = sys.argv[sys.argv.index('--') + 2:] if '--' in sys.argv and len(sys.argv) > sys.argv.index('--') + 2 else []
    common.reset_scene()
    parts = []

    def want(name):
        return not only or any(name.startswith(o) for o in only)
    if HEAD_KEY:   # head parts for one MPFB head: av_hair_K_<b>, av_gear_K_<b>, av_visor_K_<b>, av_mask_K_<b>
        PROJ[0] = 'tight'
        for k in range(1, 9):
            if want('av_hair_%d' % k):
                parts.append(make_hair(k))
        PROJ[0] = 'env'
        for k in range(1, 7):
            if want('av_gear_%d' % k):
                parts.append(make_gear(k))
        for k in range(1, 8):
            if want('av_visor_%d' % k):
                PROJ[0] = 'env' if k in (1, 2, 7) else 'tight'
                parts.append(make_visor(k))
        PROJ[0] = 'env'
        for k in range(1, 6):
            if want('av_mask_%d' % k):
                parts.append(make_mask(k))
        for p_ in parts:
            p_.name += '_' + HEAD_KEY
        export(parts, out)
        return
    if want('av_head'):
        parts.append(make_head())
    if want('av_neck'):
        parts.append(make_neck())
    for k in range(1, 9):
        if want('av_hair_%d' % k):
            parts.append(make_hair(k))
    for k in range(1, 7):
        if want('av_gear_%d' % k):
            parts.append(make_gear(k))
    for k in range(1, 8):
        if want('av_visor_%d' % k):
            parts.append(make_visor(k))
    for k in range(1, 6):
        if want('av_mask_%d' % k):
            parts.append(make_mask(k))
    for n in range(10):
        if want('av_torso_%d' % n) or want('av_upper_%d' % n) or want('av_fore_%d' % n):
            t, u, f = suit(n, False)
            parts += [t, u, f]
            tc, _, _ = suit(n, True)
            parts.append(tc)
    if want('av_waist'):
        parts += [make_waist(False), make_waist(True)]
    for k in (1, 2):
        if want('av_lower_%d' % k):
            parts += [make_lower(k, False), make_lower(k, True)]
    export(parts, out)


main()
