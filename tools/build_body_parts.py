"""Skinned avatar parts on the MPFB2 base bodies (tools/blender/make_body.py -> build/open_src/body/body_*.npz).

Garments are cut from the body surface: each garment is a continuous coverage field over the body (built from the
skin weights, which vary smoothly, and body-relative heights), triangles are split exactly along the field's zero
line, and the covered part is the surface pushed out by the garment's thickness, with a rim strip back down to the
skin along the clean edge. Uncovered faces stay skin, so every outfit fits
every body exactly. Trim, panels, piping and glowing lines live in design textures (R = trim colour, G = glow,
B = white accents) evaluated in 3D on the body and stored in the MPFB UV layout, shared by both bodies.

Writes (app/assets_open):
  av_bhead_<b>          head skin, eyes, brows, lips                     (<b> = m | f)
  av_top_<N>_<b>        suit N: torso, neck and arms (skin + garment + bracers), N = 0..9
  av_bottom_<K>_<b>     LOWER BODY K = 1..3: hips and thighs (skin + shorts) with the thigh fade
  av_wfade_<b>          the energy fade where a waist-up body ends (LOWER BODY = NONE)
  tex/open_design_*.tex garment designs; tex/open_pat_*.tex the PATTERN masks in the same UV layout
  app/src/avatar_bodies.inc  bind-pose joints for the rig (avatar.cpp)
Model format v3: the usual 44-byte vertex plus 4 bone indices and 4 weights (bytes) over the game's 7 bones
(0 torso, 1 neck, 2 head, 3 upper L, 4 upper R, 5 fore L, 6 fore R); vertex colour R = ambient occlusion.

Usage: python -I tools/build_body_parts.py
"""
import json
import math
import os
import struct
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SRC = os.path.join(ROOT, 'build', 'open_src', 'body')
OUT = os.path.join(ROOT, 'app', 'assets_open')
sys.path.insert(0, os.path.join(ROOT, 'tools', 'blender'))
import palette  # noqa: E402

CH, TWOSIDED, PATTERN = palette.CH, palette.TWOSIDED, palette.PATTERN
DESIGN = 32
CLOTH = 64   # fabric: the character shader adds a knit texture and a cloth sheen, and no glossy highlight
TORSO, NECK, HEAD, UPL, UPR, FOL, FOR = range(7)
SH = {'lit': 0, 'unlit': 3, 'add': 2, 'trans': 4}
TEX = 512


# ================================================================ body
class Body:
    def __init__(self, key):
        d = np.load(os.path.join(SRC, 'body_%s.npz' % {'m': 'masc', 'f': 'fem'}[key]))
        self.key = key
        self.P = d['pos'].astype(np.float64)
        self.N = d['nrm'].astype(np.float64)
        self.UV = d['uv'].astype(np.float64)
        self.T = d['tris'].astype(np.int64)
        self.B = d['bone'].astype(np.int64)
        self.W = d['weight'].astype(np.float64)
        self.region = d['region']
        self.ao = d['ao'].astype(np.float64)
        J = json.loads(str(d['joints']))
        self.J = {k: (np.array(v) if isinstance(v, list) else v) for k, v in J.items()}
        cut = self.J['thigh_cut']
        below = self.P[:, 1] < cut
        self.P[below, 1] = cut                       # flatten the ragged bottom of the thighs onto the cut plane
        keep = ~np.all(below[self.T], axis=1)
        self.T = self.T[keep]
        self.cut = cut
        if key == 'f':
            self.round_bust()   # a rounded, lifted bust instead of MPFB's conical one
        else:
            self.smooth_bust()
            self.smooth_bust(radius=0.035, iters=40, strength=1.0)   # then flatten the nipple point itself
        J = self.J
        self.sx = J['shoulder_r'][0]
        self.sy = J['shoulder_r'][1]
        self.hip_y = (J['hip_l'][1] + J['hip_r'][1]) * 0.5
        self.hip_x = J['hip_r'][0]
        self.waist_y = self.hip_y + 0.11
        self.armpit_y = self.sy - 0.11
        # per-face bone weights -> dominant bone; centroids; arm parameters
        F = len(self.T)
        FW = np.zeros((F, 7))
        for c in range(3):
            for k in range(4):
                np.add.at(FW, (np.arange(F), self.B[self.T[:, c], k]), self.W[self.T[:, c], k])
        self.fdom = FW.argmax(1)
        self.fc = self.P[self.T].mean(1)
        self.fat, self.fphi = self.arm_params(self.fc, self.fdom)
        self.vdom = self.B[:, 0]
        self.vat, self.vphi = self.arm_params(self.P, self.vdom)
        self.Wd = np.zeros((len(self.P), 7))
        for k in range(4):
            np.add.at(self.Wd, (np.arange(len(self.P)), self.B[:, k]), self.W[:, k])

    def smooth_bust(self, radius=0.095, iters=70, strength=0.85):
        """The base meshes' nipples make a pointed bust (worse once a garment is pushed out along the normals):
        relax the chest around them toward a natural rounded shape (Laplacian smoothing with a soft falloff)."""
        sel = self.region == 5
        if not sel.any():
            return
        key = np.round(self.P * 1e5).astype(np.int64)
        _, cid = np.unique(key, axis=0, return_inverse=True)
        cid = cid.reshape(-1)
        nc = cid.max() + 1
        Pc = np.zeros((nc, 3))
        Pc[cid] = self.P
        T = cid[self.T]
        centres = [self.P[sel & (self.P[:, 0] < 0)].mean(0), self.P[sel & (self.P[:, 0] > 0)].mean(0)]
        d = np.min([np.linalg.norm(Pc - c, axis=1) for c in centres], axis=0)
        w = np.clip(1 - d / radius, 0, 1) ** 1.2 * strength
        # Taubin smoothing (a shrink step, then an inflate step): plain Laplacian smoothing pulls a bump toward a
        # cone (the "triangle" bust); alternating keeps the volume and gives a rounded shape
        cnt = np.zeros(nc)
        for i in range(3):
            np.add.at(cnt, T[:, i], 2)
        for it in range(iters * 2):
            acc = np.zeros_like(Pc)
            for i, j in ((0, 1), (1, 2), (2, 0), (1, 0), (2, 1), (0, 2)):
                np.add.at(acc, T[:, i], Pc[T[:, j]])
            avg = acc / np.maximum(cnt, 1)[:, None]
            lam = 0.5 if it % 2 == 0 else -0.53
            Pc = Pc + (w * lam)[:, None] * (avg - Pc)
        self.P = Pc[cid]
        # normals again (area weighted over the welded mesh)
        Nc = np.zeros_like(Pc)
        fn = np.cross(Pc[T[:, 1]] - Pc[T[:, 0]], Pc[T[:, 2]] - Pc[T[:, 0]])
        for c in range(3):
            np.add.at(Nc, T[:, c], fn)
        Nc /= np.maximum(np.linalg.norm(Nc, axis=1, keepdims=True), 1e-12)
        touched = (w > 0)[cid]
        self.N[touched] = Nc[cid][touched]

    def _welded(self):
        key = np.round(self.P * 1e5).astype(np.int64)
        _, cid = np.unique(key, axis=0, return_inverse=True)
        cid = cid.reshape(-1)
        Pc = np.zeros((cid.max() + 1, 3))
        Pc[cid] = self.P
        return Pc, cid, cid[self.T]

    def _relax(self, Pc, T, w, iters, lam=0.9):
        """Laplacian relaxation of the welded mesh, w = per-vertex step weight (0 keeps a vertex fixed)."""
        cnt = np.zeros(len(Pc))
        for i in range(3):
            np.add.at(cnt, T[:, i], 2)
        for _ in range(iters):
            acc = np.zeros_like(Pc)
            for i, j in ((0, 1), (1, 2), (2, 0), (1, 0), (2, 1), (0, 2)):
                np.add.at(acc, T[:, i], Pc[T[:, j]])
            Pc = Pc + (w * lam)[:, None] * (acc / np.maximum(cnt, 1)[:, None] - Pc)
        return Pc

    def round_bust(self, lift=0.024, rx=0.068, ry_up=0.085, ry_lo=0.058, fill=1.15):
        """Rebuild each breast as a soft dome on the chest wall. The wall under the breast is a quadratic surface
        z(x, y) fitted to the chest around it; the dome h * (1 - r^2)^1.3 (r on an ellipse that is fuller below the
        apex, apex raised by `lift`) is laid on it along the forward axis, with the original height, so there is no
        tip and the fullest point sits higher. Nothing outside the bust moves."""
        sel = self.region == 5
        if not sel.any():
            return
        Pc, cid, T = self._welded()
        Nc0 = np.zeros_like(Pc)
        fn = np.cross(Pc[T[:, 1]] - Pc[T[:, 0]], Pc[T[:, 2]] - Pc[T[:, 0]])
        for c in range(3):
            np.add.at(Nc0, T[:, c], fn)
        Nc0 /= np.maximum(np.linalg.norm(Nc0, axis=1, keepdims=True), 1e-12)
        front = (Nc0[:, 2] < -0.25) & (Pc[:, 2] < -0.04)
        nip = [self.P[sel & (self.P[:, 0] * sg > 0)].mean(0) for sg in (-1, 1)]
        heights = []
        moved = np.zeros(len(Pc), bool)
        for c in nip:
            apex = c + np.array([0.0, lift, 0.0])

            def rr(Q, cen):
                dx = (Q[:, 0] - cen[0]) / rx
                dy = Q[:, 1] - cen[1]
                dy = np.where(dy > 0, dy / ry_up, dy / ry_lo)
                return np.sqrt(dx * dx + dy * dy)
            r_orig = rr(Pc, c)
            ring = front & (r_orig > 1.25) & (r_orig < 2.0) & (np.abs(Pc[:, 0]) > 0.012)
            X, Y = Pc[ring, 0], Pc[ring, 1]
            A = np.stack([np.ones_like(X), X, Y, X * X, X * Y, Y * Y], 1)
            coef = np.linalg.lstsq(A, Pc[ring, 2], rcond=None)[0]

            def wall(Q):
                x, y = Q[:, 0], Q[:, 1]
                return coef[0] + coef[1] * x + coef[2] * y + coef[3] * x * x + coef[4] * x * y + coef[5] * y * y
            h = float(wall(c[None])[0] - c[2]) * fill            # how far the breast stands out of the wall
            heights.append(h)
            r_new = rr(Pc, apex)
            # the old breast (and a margin) plus the new dome, front side only
            # the whole old breast (whatever way its faces point: the nipple's sides face down or back), plus the
            # front-facing margin and the new dome
            region = ((r_orig < 1.0) | (((r_orig < 1.3) | (r_new < 1.0)) & (Nc0[:, 2] < 0.2))) & (Pc[:, 2] < wall(Pc) + 0.05)
            target = wall(Pc) - h * np.clip(1 - r_new ** 2, 0, 1) ** 1.3
            # inside the new dome: the dome; inside the old breast: the bare wall (nothing of the old shape is
            # left below the raised dome); easing back to the original surface over the margin
            def ss(x):
                x = np.clip(x, 0, 1)
                return x * x * (3 - 2 * x)
            t = ss((r_orig - 1.0) / 0.3) * ss((r_new - 1.0) / 0.25)   # continuous across the dome's edge
            z = target * (1 - t) + Pc[:, 2] * t
            Pc[region, 2] = z[region]
            # the baked occlusion belongs to the old shape (dark under the old fold): use the chest's around it
            ao_c = np.zeros(len(Pc))
            ao_c[cid] = self.ao
            ring_ao = float(np.median(ao_c[ring])) if ring.any() else 1.0
            ao_c[region] = ring_ao * (1 - t[region]) + ao_c[region] * t[region]
            self.ao = ao_c[cid]
            moved |= region
        # even out the spacing the z-only move leaves (folds where the old nipple was): a short Taubin relax
        cnt = np.zeros(len(Pc))
        for i in range(3):
            np.add.at(cnt, T[:, i], 2)
        wm = moved.astype(float)
        for it in range(40):
            acc = np.zeros_like(Pc)
            for i, j in ((0, 1), (1, 2), (2, 0), (1, 0), (2, 1), (0, 2)):
                np.add.at(acc, T[:, i], Pc[T[:, j]])
            Pc = Pc + (wm * (0.5 if it % 2 == 0 else -0.53))[:, None] * (acc / np.maximum(cnt, 1)[:, None] - Pc)
        self.P = Pc[cid]
        Nc = np.zeros_like(Pc)
        fn = np.cross(Pc[T[:, 1]] - Pc[T[:, 0]], Pc[T[:, 2]] - Pc[T[:, 0]])
        for c in range(3):
            np.add.at(Nc, T[:, c], fn)
        Nc /= np.maximum(np.linalg.norm(Nc, axis=1, keepdims=True), 1e-12)
        self.N = Nc[cid]
        print('  bust: rounded, apex raised %.0f mm, height %.0f / %.0f mm' % (lift * 1000, heights[0] * 1000, heights[1] * 1000))

    def arm_params(self, X, dom):
        """at: 0..1 along the upper arm, 1..2 along the forearm (-1 elsewhere); phi: angle around the arm, 0 = outside."""
        at = np.full(len(X), -1.0)
        phi = np.zeros(len(X))
        for side, up, fo in (('l', UPL, FOL), ('r', UPR, FOR)):
            S, E, Wr = self.J['shoulder_' + side], self.J['elbow_' + side], self.J['wrist_' + side]
            out = np.array([-1.0 if side == 'l' else 1.0, 0, 0])
            for bone, a, b, off in ((up, S, E, 0.0), (fo, E, Wr, 1.0)):
                sel = dom == bone
                if not sel.any():
                    continue
                ax = b - a
                L = np.linalg.norm(ax)
                u = ax / L
                rel = X[sel] - a
                t = rel @ u / L
                at[sel] = off + np.clip(t, -0.3, 1.3)
                rad = rel - np.outer(rel @ u, u)
                ref = out - u * (out @ u)
                ref /= np.linalg.norm(ref)
                phi[sel] = np.arctan2(np.cross(ref, rad) @ u * (1 if side == 'r' else -1), rad @ ref)
        return at, phi

    def torso_u(self, X):
        """Arc-length-like coordinate around the torso: 0 front centre, +-0.25 the sides, +-0.5 the back."""
        return np.arctan2(X[:, 0], -X[:, 2]) / (2 * math.pi)


# ================================================================ helpers
def smooth_line(d, w):
    return np.exp(-(d / w) ** 2)


def seg_dist(px, py, pts):
    """Distance from points (px, py) to a 2D polyline."""
    best = np.full(px.shape, np.inf)
    for (x0, y0), (x1, y1) in zip(pts[:-1], pts[1:]):
        dx, dy = x1 - x0, y1 - y0
        L2 = dx * dx + dy * dy
        t = np.clip(((px - x0) * dx + (py - y0) * dy) / max(L2, 1e-12), 0, 1)
        best = np.minimum(best, np.hypot(px - x0 - t * dx, py - y0 - t * dy))
    return best


def hem_y(b, X, kind):
    """Height of a shorts hem at points X: 'dolphin' rises at the outside of each leg (a split hem)."""
    out = np.clip((np.abs(X[:, 0]) - b.hip_x) / 0.06, 0, 1)
    if kind == 'dolphin':
        return b.hip_y - 0.15 + 0.065 * out ** 2
    if kind == 'circuit':
        return b.hip_y - 0.22 + 0.0 * out
    return b.hip_y - 0.245                          # bike shorts: mid-thigh


# ---------------------------------------------------------------- coverage fields (> 0 = covered)
def ctx(b, S):
    """Quantities the fields use at soup corners: positions, dense weights, dominant bone, arm parameter."""
    X = S.P
    dom = S.Wd.argmax(1)
    at, _ = b.arm_params(X, dom)
    return X, S.Wd, dom, at


def f_top(b, style):
    def f(S):
        X, Wd, dom, at = ctx(b, S)
        x, y, z = X[:, 0], X[:, 1], X[:, 2]
        ax = np.abs(x)
        w_arm = Wd[:, 3:7].sum(1)
        g = np.minimum(y - b.waist_y, 0.5 - Wd[:, HEAD])                      # never the head, from the waist up
        if style.get('collar') is not None:
            g = np.minimum(g, np.maximum(0.5 - Wd[:, NECK], style['collar'] - y))
        else:
            g = np.minimum(g, (0.5 - Wd[:, NECK]) * 0.4)                       # crew neckline along the neck's weight
        if style.get('sleeve') is not None:
            g = np.minimum(g, np.maximum(0.5 - w_arm, (style['sleeve'] - at) * 0.3))
        else:
            g = np.minimum(g, (0.5 - w_arm) * 0.4)
        if style.get('armhole'):
            w0, w1 = style['armhole']
            xa = np.interp(y, [b.armpit_y - 0.01, b.armpit_y + 0.06, b.sy + 0.06], [0.6, w0 * b.sx, w1 * b.sx])
            g = np.minimum(g, xa - ax)
        if style.get('scoop'):
            front = z < 0
            # a round neckline (front lower than the back) between wide straps
            nf = np.maximum(b.sy - 0.010 - np.sqrt(np.maximum(0.0, (0.40 * b.sx) ** 2 - ax ** 2)) * 0.55 - y, ax - 0.40 * b.sx)
            nb = np.maximum(b.sy + 0.040 - np.sqrt(np.maximum(0.0, (0.34 * b.sx) ** 2 - ax ** 2)) * 0.25 - y, ax - 0.34 * b.sx)
            g = np.minimum(g, np.where(front, nf, nb))
        return g
    return f


def f_bottom(b, kind):
    def f(S):
        X, Wd, dom, at = ctx(b, S)
        y = X[:, 1]
        return np.minimum(np.minimum(b.waist_y - y, y - hem_y(b, X, kind)), 0.5 - Wd[:, 3:7].sum(1))
    return f


def f_bracer(b):
    def f(S):
        X, Wd, dom, at = ctx(b, S)
        return np.where(dom >= FOL, (at - 1.20) * 0.3, -1.0)
    return f


def f_plate(b, which):
    def f(S):
        X, Wd, dom, at = ctx(b, S)
        x, y, z = X[:, 0], X[:, 1], X[:, 2]
        ax = np.abs(x)
        if which in ('chest', 'back'):
            side = (-z) if which == 'chest' else z
            g = np.minimum.reduce([side - 0.02, -0.05 - y, y - (b.waist_y + 0.12), 0.72 * b.sx - ax, Wd[:, TORSO] - 0.5])
            return g
        if which == 'pauldron':
            return np.where(dom <= TORSO, np.minimum(ax - 0.80 * b.sx, y - (b.sy - 0.06)), np.where(dom <= UPR, (0.34 - at) * 0.3, -1.0))
        if which == 'vambrace':
            return np.where(dom >= FOL, np.minimum(at - 1.25, 1.95 - at) * 0.3, -1.0)
        raise KeyError(which)
    return f


# ---------------------------------------------------------------- designs (per texel: X positions, dom bones)
def design(name, b, X, dom, at, phi):
    """(R trim, G glow, B white) at body points X."""
    n = len(X)
    R, G, Bw = np.zeros(n), np.zeros(n), np.zeros(n)
    u = b.torso_u(X)
    y = X[:, 1]
    torso = dom == TORSO
    arm = dom >= UPL
    neck = dom == NECK
    sy, wy, ay = b.sy, b.waist_y, b.armpit_y
    au = np.abs(u)
    W = 0.0032

    def lines(pts, w=W, mask=None, mirror=False, back=False):
        uu = (np.abs(u) if mirror else u)
        if back:
            uu = 0.5 - np.abs(u)          # back coordinates: 0 = the spine
        g = smooth_line(seg_dist(uu * 1.0, y, pts), w)
        return g * (torso if mask is None else mask)

    def arm_line(phi0, t0, t1, w=0.07):
        return smooth_line(np.angle(np.exp(1j * (phi - phi0))), w) * (at >= t0) * (at <= t1) * arm

    if name == 'grid':          # GRID RUNNER: Tron piping
        G += lines([(-0.10, sy + 0.05), (0.0, sy - 0.10), (0.10, sy + 0.05)])
        G += lines([(0.0, sy - 0.10), (0.0, wy)])
        G += lines([(0.25, ay + 0.02), (0.25, wy)], mirror=True)
        G += lines([(0.0, sy + 0.06), (0.0, wy)], back=True)
        G += lines([(0.11, sy - 0.20), (0.11, sy - 0.27), (0.17, sy - 0.29), (0.17, wy)], mirror=True)
        G += smooth_line(np.abs(y - (wy + 0.02)), W) * torso
        G += arm_line(0.0, 0.0, 1.92)
        G += smooth_line(np.abs(at - 1.90), 0.012) * arm
        G += smooth_line(np.abs(y - 0.040), W) * neck
    elif name == 'vest':        # CIRCUIT VEST
        G += lines([(-0.12, sy + 0.07), (-0.04, sy - 0.03), (0.0, sy - 0.06), (0.04, sy - 0.03), (0.12, sy + 0.07)])
        G += lines([(0.0, sy - 0.06), (0.0, sy - 0.12)])
        G += lines([(-0.03, sy - 0.015), (0.0, sy - 0.035), (0.03, sy - 0.015)])
        G += lines([(0.19, ay), (0.165, ay - 0.10), (0.12, ay - 0.17), (0.12, wy)], mirror=True)
        G += lines([(0.27, ay - 0.01), (0.27, wy)], mirror=True)
        G += lines([(0.0, sy + 0.06), (0.0, sy - 0.10)], back=True)
        G += lines([(0.0, sy - 0.10), (0.065, sy - 0.17), (0.10, wy + 0.06)], back=True, mirror=False)
        G += lines([(0.0, sy - 0.10), (-0.065, sy - 0.17), (-0.10, wy + 0.06)], back=True)
        for c in (0.09, 0.03):
            G += smooth_line(np.abs(y - (wy + 0.015)), 0.004) * (np.abs(au - c) < 0.014) * torso
        R += (np.abs(au - 0.25) < 0.035) * (y < ay) * (y > wy + 0.03) * torso   # side panels in the trim colour
        G += smooth_line(np.abs(y - 0.052), W) * neck
        G += smooth_line(np.abs(np.abs(u) - 0.06), 0.004) * neck * (y < 0.05)
    elif name == 'echo':        # ECHO SUIT: V lines, a glowing identity disc on the back
        G += lines([(-0.18, sy + 0.05), (0.0, sy - 0.17), (0.18, sy + 0.05)])
        G += lines([(-0.11, sy - 0.17), (0.0, sy - 0.30), (0.11, sy - 0.17)])
        G += lines([(0.26, sy + 0.04), (0.26, sy - 0.23)], mirror=True)
        bu, by = 0.5 - np.abs(u), y - (sy - 0.07)
        r = np.hypot(bu * 1.9, by)
        G += smooth_line(np.abs(r - 0.060), 0.004) * torso
        R += (r < 0.046) * torso
        G += smooth_line(r, 0.010) * torso
        G += arm_line(0.6, 0.05, 0.95) + smooth_line(np.abs(at - 1.30), 0.012) * arm
        R += (at > 1.86) * arm
    elif name == 'pilot':       # PILOT: harness straps, belt, chest light
        for c in (0.05, 0.45):
            R += (np.abs(au - c) < 0.016) * (y < sy + 0.06) * torso
        R += (np.abs(y - (wy + 0.12)) < 0.018) * torso
        G += (np.abs(u) < 0.03) * (np.abs(y - (sy - 0.08)) < 0.012) * torso
        R += (at > 1.86) * arm
        R += ((at > 0.20) & (at < 0.40) & (np.abs(phi) < 0.6)) * arm
    elif name == 'jacket':      # STREET JACKET: zipper, cuffs, waistband
        R += (np.abs(u) < 0.012) * torso
        G += (np.abs(u) < 0.004) * (np.abs(y - (sy - 0.06)) < 0.012) * torso
        R += (y < wy + 0.045) * torso
        R += (at > 1.85) * arm
        R += neck * 1.0
    elif name == 'hoodie':      # HOODIE: kangaroo pocket, cuffs, waistband
        R += (np.abs(u) < 0.085) * (y > wy + 0.03) * (y < wy + 0.15) * torso
        R += (y < wy + 0.04) * torso
        R += (at > 1.85) * arm
    elif name == 'jersey':      # ARENA JERSEY: collar band, sleeve bands, side stripes
        R += (y > sy + 0.035) * torso
        R += ((at > 0.34) & (at <= 0.42)) * arm
        R += (np.abs(au - 0.25) < 0.012) * (y < ay) * torso
        R += (y < wy + 0.03) * torso
    elif name == 'tee':         # TRAINING TEE (reference 2): raglan and side panels in the trim colour
        rag = seg_dist(au, y, [(0.06, sy + 0.08), (0.20, ay + 0.02)])
        R += (rag < 0.028) * torso
        rag_b = seg_dist(0.5 - au, y, [(0.07, sy + 0.08), (0.20, ay + 0.02)])
        R += (rag_b < 0.028) * torso
        sw = np.interp(y, [wy, (wy + ay) * 0.5, ay], [0.020, 0.032, 0.040])
        R += (np.abs(au - 0.25) < sw) * (y < ay + 0.02) * torso
        R += (np.abs(phi) < 0.55) * (at < 0.45) * arm
        R += ((at > 0.38) & (at <= 0.45)) * arm * 0.0
    elif name == 'vanguard':    # VANGUARD undersuit: glowing core on the chest
        r = np.hypot(u * 1.9, y - (sy - 0.06))
        G += smooth_line(r, 0.014) * torso * (np.cos(2 * math.pi * u) > 0)
        G += smooth_line(np.abs(r - 0.030), 0.003) * torso * (np.cos(2 * math.pi * u) > 0)
    # bracers
    elif name == 'bracer_circuit':
        G += arm_line(0.0, 1.22, 1.95, 0.05) + arm_line(0.7, 1.32, 1.80, 0.05) + arm_line(-0.7, 1.32, 1.80, 0.05)
        G += smooth_line(np.abs(at - 1.28), 0.010) * arm
    elif name == 'bracer_silver':   # reference 1: a silver outer plate, black body
        Bw += (np.abs(phi) < 0.85) * (at > 1.30) * (at < 1.86) * arm
        G += 0.0
    elif name == 'bracer_gray':     # reference 2: grey plates on black
        R += (np.abs(phi) < 0.85) * (at > 1.30) * (at < 1.86) * arm
    # shorts
    elif name == 'dolphin':
        h = hem_y(b, X, 'dolphin')
        Bw += (np.abs(y - (h + 0.008)) < 0.008) * torso
        side = np.abs(np.abs(X[:, 0]) - (b.hip_x + 0.075)) < 0.012
        Bw += side * (y < b.hip_y - 0.06) * (y > h) * torso
    elif name == 'circuit_shorts':
        out = np.abs(X[:, 0]) - (b.hip_x + 0.05)
        G += smooth_line(np.abs(out - 0.012), 0.003) * (y < wy - 0.02) * torso
        h = hem_y(b, X, 'circuit')
        G += smooth_line(np.abs(y - (h + 0.012)), 0.003) * torso
        for c in (0.08, 0.42):
            G += smooth_line(np.abs(y - (wy - 0.02)), 0.004) * (np.abs(au - c) < 0.02) * torso
    elif name == 'bike':        # reference 2: side panels down the outside of each leg
        out = np.abs(X[:, 0]) - (b.hip_x + 0.02)
        R += (out > 0) * (y < wy - 0.03) * torso
        R += (np.abs(au - 0.25) < 0.03) * (y < wy - 0.03) * (y > b.hip_y - 0.02) * torso
    return np.clip(R, 0, 1), np.clip(G, 0, 1), np.clip(Bw, 0, 1)


def pattern(k, b, X, dom, at, phi):
    """PATTERN masks (R = trim regions, G = glowing lines) in the body UV layout: 1 CIRCUIT .. 7 BANDS."""
    u = b.torso_u(X)
    y = X[:, 1]
    torso = (dom == TORSO) | (dom == NECK)
    arm = dom >= UPL
    s = np.where(arm, at * 0.30, u * 1.0)   # one coordinate pair over torso and arms
    t = np.where(arm, phi / (2 * math.pi) * 0.25, y)
    R, G = np.zeros(len(X)), np.zeros(len(X))
    if k == 1:   # CIRCUIT: traces on a grid with pads
        gx, gy = (s * 22) % 1.0, (t * 22) % 1.0
        G += ((np.abs(gx - 0.5) < 0.045) & (np.floor(t * 22) % 3 == 0)) | ((np.abs(gy - 0.5) < 0.045) & (np.floor(s * 22) % 2 == 0))
        G += np.hypot(gx - 0.5, gy - 0.5) < 0.12
    elif k == 2:  # STRIPES: twin racing stripes front and back, pinstripes at the sides
        a = np.abs(u)
        R += ((np.abs(a - 0.035) < 0.012) | (np.abs(a - 0.465) < 0.012)) * torso
        R += (np.abs(a - 0.25) < 0.003) * torso
        R += (np.abs(phi) < 0.18) * arm
    elif k == 3:  # CHEVRON
        c = (y + 2.2 * np.abs(u)) * 14 % 1.0
        R += (c < 0.35) * torso * (np.abs(u) < 0.25)
    elif k == 4:  # HEX
        q = s * 30
        r = t * 30
        hx = np.abs(((q + r * 0.57735) % 1.0) - 0.5) + np.abs(((r * 1.1547) % 1.0) - 0.5) * 0.0
        G += (hx < 0.05) * 0.8
        R += (hx > 0.38) * 0.4
    elif k == 5:  # SPLIT: the left half in the trim colour
        R += (X[:, 0] < 0) * 1.0
    elif k == 6:  # PIXEL
        cell = np.floor(s * 26) * 7.0 + np.floor(t * 26) * 13.0
        R += (np.sin(cell * 12.9898) * 43758.5453 % 1.0 > 0.72) * 1.0
    elif k == 7:  # BANDS
        R += ((t * 9) % 1.0 < 0.30) * torso
        R += ((s * 9) % 1.0 < 0.30) * arm
    return np.clip(R, 0, 1), np.clip(G, 0, 1), np.zeros(len(X))


# ---------------------------------------------------------------- textures in the UV layout
class UVRaster:
    """Positions, dominant bones and arm parameters of the body surface at every texel of the UV layout."""

    def __init__(self, b, size=TEX):
        self.size = size
        X = np.zeros((size, size, 3))
        dom = np.full((size, size), -1, np.int64)
        P, UV, T = b.P, b.UV, b.T
        for f, (i, j, k) in enumerate(T):
            uv = UV[[i, j, k]] * size        # row 0 = v 0 (GL samples the first row at t = 0)
            x0, x1 = int(max(0, np.floor(uv[:, 0].min()) - 1)), int(min(size - 1, np.ceil(uv[:, 0].max()) + 1))
            y0, y1 = int(max(0, np.floor(uv[:, 1].min()) - 1)), int(min(size - 1, np.ceil(uv[:, 1].max()) + 1))
            if x1 < x0 or y1 < y0:
                continue
            gx, gy = np.meshgrid(np.arange(x0, x1 + 1) + 0.5, np.arange(y0, y1 + 1) + 0.5)
            (ax_, ay_), (bx_, by_), (cx_, cy_) = uv
            den = (by_ - cy_) * (ax_ - cx_) + (cx_ - bx_) * (ay_ - cy_)
            if abs(den) < 1e-12:
                continue
            l0 = ((by_ - cy_) * (gx - cx_) + (cx_ - bx_) * (gy - cy_)) / den
            l1 = ((cy_ - ay_) * (gx - cx_) + (ax_ - cx_) * (gy - cy_)) / den
            l2 = 1 - l0 - l1
            ins = (l0 >= -0.02) & (l1 >= -0.02) & (l2 >= -0.02)
            if not ins.any():
                continue
            pts = l0[ins, None] * P[i] + l1[ins, None] * P[j] + l2[ins, None] * P[k]
            yy, xx = gy[ins].astype(int), gx[ins].astype(int)
            X[yy, xx] = pts
            dom[yy, xx] = b.fdom[f]
        self.valid = dom >= 0
        self.X = X[self.valid]
        self.dom = dom[self.valid]
        self.at, self.phi = b.arm_params(self.X, self.dom)

    def image(self, rgb):
        img = np.zeros((self.size, self.size, 4))
        for c in range(3):
            plane = np.zeros((self.size, self.size))
            plane[self.valid] = rgb[c]
            img[..., c] = plane
        img[..., 3] = 1.0
        # dilate into the gutters so bilinear filtering never pulls in black
        filled = self.valid.copy()
        for _ in range(4):
            acc = np.zeros_like(img)
            cnt = np.zeros(self.valid.shape)
            for dy, dx in ((1, 0), (-1, 0), (0, 1), (0, -1)):
                sh = np.roll(np.roll(img, dy, 0), dx, 1)
                sf = np.roll(np.roll(filled, dy, 0), dx, 1)
                acc += sh * sf[..., None]
                cnt += sf
            grow = ~filled & (cnt > 0)
            img[grow] = acc[grow] / cnt[grow][:, None]
            filled |= grow
        return (np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8)


def write_tex(name, rgba):
    os.makedirs(os.path.join(OUT, 'tex'), exist_ok=True)
    h, w = rgba.shape[:2]
    with open(os.path.join(OUT, 'tex', name + '.tex'), 'wb') as f:
        f.write(b'STEX' + struct.pack('<3I', w, h, 0))
        f.write(np.ascontiguousarray(rgba).tobytes())


# ================================================================ mesh building
class Soup:
    """Triangle soup: per-corner position, normal, uv, ao and dense 7-bone weights ([3n, ...])."""

    def __init__(self, P, N, UV, AO, Wd):
        self.P, self.N, self.UV, self.AO, self.Wd = P, N, UV, AO, Wd

    @staticmethod
    def of(b, tris):
        i = tris.reshape(-1)
        return Soup(b.P[i].copy(), b.N[i].copy(), b.UV[i].copy(), b.ao[i].copy(), b.Wd[i].copy())

    @staticmethod
    def cat(soups):
        soups = [x for x in soups if len(x.P)]
        if not soups:
            return Soup(*(np.zeros((0, k)) for k in (3, 3, 2)), np.zeros(0), np.zeros((0, 7)))
        return Soup(*(np.concatenate([getattr(x, a) for x in soups]) for a in ('P', 'N', 'UV', 'AO', 'Wd')))

    def take(self, corners):
        return Soup(self.P[corners], self.N[corners], self.UV[corners], self.AO[corners], self.Wd[corners])

    def __len__(self):
        return len(self.P) // 3


def lerp_corner(S, i, j, t):
    t = t[:, None]
    N = S.N[i] * (1 - t) + S.N[j] * t
    N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-9)
    return (S.P[i] * (1 - t) + S.P[j] * t, N, S.UV[i] * (1 - t) + S.UV[j] * t,
            S.AO[i] * (1 - t[:, 0]) + S.AO[j] * t[:, 0], S.Wd[i] * (1 - t) + S.Wd[j] * t)


def clip(S, field):
    """Split a soup along field = 0: (covered, uncovered). Straddling triangles are cut exactly at the zero line."""
    f = field(S).reshape(-1, 3)
    pos = f >= 0
    npos = pos.sum(1)
    tri = np.arange(len(f))
    inside = [S.take((tri[npos == 3][:, None] * 3 + np.arange(3)).reshape(-1))]
    outside = [S.take((tri[npos == 0][:, None] * 3 + np.arange(3)).reshape(-1))]
    for k in np.nonzero((npos == 1) | (npos == 2))[0]:
        c = k * 3 + np.arange(3)
        fv = f[k]
        lone = int(np.nonzero(pos[k] if npos[k] == 1 else ~pos[k])[0][0])   # the vertex alone on its side
        a, b_, c_ = c[lone], c[(lone + 1) % 3], c[(lone + 2) % 3]
        fa, fb, fc = fv[lone], fv[(lone + 1) % 3], fv[(lone + 2) % 3]
        tab, tac = fa / (fa - fb), fa / (fa - fc)
        pab = lerp_corner(S, np.array([a]), np.array([b_]), np.array([tab]))
        pac = lerp_corner(S, np.array([a]), np.array([c_]), np.array([tac]))
        A = (S.P[[a]], S.N[[a]], S.UV[[a]], S.AO[[a]], S.Wd[[a]])
        B_ = (S.P[[b_]], S.N[[b_]], S.UV[[b_]], S.AO[[b_]], S.Wd[[b_]])
        C_ = (S.P[[c_]], S.N[[c_]], S.UV[[c_]], S.AO[[c_]], S.Wd[[c_]])
        small = Soup(*(np.concatenate(z) for z in zip(A, pab, pac)))                  # (a, ab, ac): keeps winding
        big = Soup(*(np.concatenate(z) for z in zip(pab, B_, C_, pab, C_, pac)))       # (ab, b, c) + (ab, c, ac)
        if npos[k] == 1:
            inside.append(small); outside.append(big)
        else:
            outside.append(small); inside.append(big)
    return Soup.cat(inside), Soup.cat(outside)


def weld_keys(P):
    return np.round(P * 2e4).astype(np.int64)


def boundary_edges(S):
    """Directed boundary edges (corner index pairs, in winding order) of a soup, matched on welded positions."""
    n = len(S)
    if n == 0:
        return np.zeros((0, 2), np.int64)
    key = weld_keys(S.P)
    _, vid = np.unique(key, axis=0, return_inverse=True)
    vid = vid.reshape(-1)
    c = np.arange(n * 3).reshape(-1, 3)
    e = np.concatenate([c[:, [0, 1]], c[:, [1, 2]], c[:, [2, 0]]])
    und = np.sort(vid[e], 1)
    _, inv, cnt = np.unique(und, axis=0, return_inverse=True, return_counts=True)
    return e[cnt[inv.reshape(-1)] == 1]


class Mesh:
    """Soups grouped by material key; written as a v3 (skinned) model."""

    def __init__(self):
        self.groups = {}

    def add(self, key, S):
        if len(S):
            self.groups.setdefault(key, []).append(S)

    def write(self, name, mats):
        verts, idxs, secs, mlist = [], [], [], []
        nv = ni = 0
        for mi, key in enumerate(sorted(self.groups)):
            S = Soup.cat(self.groups[key])
            rec = np.concatenate([np.round(S.P, 6), np.round(S.N, 4), np.round(S.UV, 6)], 1)
            _, first, inv = np.unique(rec, axis=0, return_index=True, return_inverse=True)
            inv = inv.reshape(-1)
            n = len(first)
            v = np.zeros(n, dtype=[('p', '<f4', 3), ('n', '<f4', 3), ('u0', '<f4', 2), ('u1', '<f4', 2), ('c', 'u1', 4),
                                   ('bi', 'u1', 4), ('bw', 'u1', 4)])
            v['p'], v['n'], v['u0'] = S.P[first], S.N[first], S.UV[first]
            a = np.clip(S.AO[first] * 255 + 0.5, 0, 255).astype(np.uint8)
            v['c'] = np.stack([a, a, a, np.full(n, 255, np.uint8)], 1)
            Wd = S.Wd[first]
            order = np.argsort(-Wd, axis=1)[:, :4]
            top = np.take_along_axis(Wd, order, 1)
            top /= np.maximum(top.sum(1, keepdims=True), 1e-9)
            w = np.clip(np.round(top * 255), 0, 255)
            w[:, 0] += 255 - w.sum(1)
            v['bi'], v['bw'] = order.astype(np.uint8), w.astype(np.uint8)
            verts.append(v)
            idxs.append(inv.astype(np.uint32) + nv)
            secs.append((ni, len(inv), mi))
            nv += n
            ni += len(inv)
            mlist.append(mats[key])
        V = np.concatenate(verts)
        I = np.concatenate(idxs)
        with open(os.path.join(OUT, name + '.mdl'), 'wb') as f:
            f.write(b'SMDL' + struct.pack('<5I', 3, len(V), len(I), len(secs), len(mlist)))
            f.write(V.tobytes())
            f.write(I.tobytes())
            for s_ in secs:
                f.write(struct.pack('<3I', *s_))
            for m in mlist:
                f.write(struct.pack('<I4f4f2I', m['shader'], *m['tint'], *m['em'], 0, m['flags']))
                f.write(m.get('diffuse', '').encode().ljust(64, b'\0'))
                f.write(m.get('mask', '').encode().ljust(64, b'\0'))
        print('  %-18s %6d verts %6d tris %2d mats' % (name, len(V), len(I) // 3, len(mlist)))
        return len(V)


def mat(pal, mask='', extra_flags=0, twosided=False):
    d = palette.P[pal]
    sh = SH[d.get('shader', 'lit')]
    return dict(shader=sh, tint=tuple(d.get('rgb', (1, 1, 1))) + (d.get('alpha', 1.0),),
                em=tuple(d.get('em', (0, 0, 0))) + (d.get('gloss', 0.0),),
                flags=d.get('flags', 0) | extra_flags | (DESIGN if mask else 0) | (TWOSIDED if twosided else 0),
                diffuse=('open_' + d['tex']) if d.get('tex') else '', mask=mask)


def drape(S, thick, max_lift, arms_too=False, reach=0.16, bend=0.6):
    """Cloth over the body: the shell `thick` off the skin, lifted (never lowered) so it clears the body around it the
    way fabric under tension does: over each point it must stay above every nearby body point (within `reach`,
    facing the same way) minus a parabola of radius `bend` (how tightly the cloth may curve inward). Large `bend`:
    loose cloth that spans hollows almost straight (between and under the breasts, the navel, the small of the back);
    small `bend`: a tight suit. At most `max_lift` above `thick`. Returns per-corner positions, normals, lift, ao."""
    key = np.round(S.P * 1e5).astype(np.int64)
    _, cid = np.unique(key, axis=0, return_inverse=True)
    cid = cid.reshape(-1)
    n = cid.max() + 1
    Pb = np.zeros((n, 3))
    Pb[cid] = S.P
    Nb = np.zeros((n, 3))
    np.add.at(Nb, cid, S.N)
    Nb /= np.maximum(np.linalg.norm(Nb, axis=1, keepdims=True), 1e-9)
    Wd = np.zeros((n, S.Wd.shape[1]))
    Wd[cid] = S.Wd
    T = cid.reshape(-1, 3)
    arm = Wd[:, 3:7].sum(1)
    # where cloth may lift: the torso and hips (the arms only for sleeves); never webbing across an armpit
    w = np.clip(1.0 - arm * (1.5 if arms_too else 3.0), 0.0, 1.0) * np.clip(1.0 - Wd[:, HEAD] * 3 - Wd[:, NECK], 0.0, 1.0)
    cnt = np.zeros(n)
    for i in range(3):
        np.add.at(cnt, T[:, i], 2)

    def blur(v, k):
        for _ in range(k):
            acc = np.zeros_like(v)
            for i, j in ((0, 1), (1, 2), (2, 0), (1, 0), (2, 1), (0, 2)):
                np.add.at(acc, T[:, i], v[T[:, j]])
            v = 0.5 * v + 0.5 * acc / (np.maximum(cnt, 1)[:, None] if v.ndim == 2 else np.maximum(cnt, 1))
        return v
    # the direction cloth lifts in: the body's broad facing (forward over the chest, not down under the bust)
    Nd = blur(Nb.copy(), 30)
    Nd /= np.maximum(np.linalg.norm(Nd, axis=1, keepdims=True), 1e-9)
    src = arm < (0.6 if arms_too else 0.25)          # the body points the cloth has to clear
    Ps, Ns = Pb[src] + Nb[src] * thick, Nb[src]
    base = Pb + Nb * thick
    # a morphological closing with a paraboloid of radius `bend`, done in each point's own frame (heights along its
    # broad facing): dilate the local heightfield, then erode it at the point. Fills hollows narrower than the bend
    # allows (cleavage, under the bust, navel, spine) and leaves convex shapes (the bust) exactly as they are.
    out = np.zeros(n)
    inv2b = 1.0 / (2.0 * bend)
    for v in np.nonzero(w > 0.01)[0]:
        d = Ps - base[v]
        dist2 = (d * d).sum(1)
        near = np.nonzero((dist2 < reach * reach) & ((Ns @ Nd[v]) > 0.4))[0]
        if len(near) > 160:
            near = near[np.argsort(dist2[near])[:160]]
        if len(near) < 4:
            continue
        dd = d[near]
        h = dd @ Nd[v]
        t = dd - h[:, None] * Nd[v]
        t2 = ((t[:, None, :] - t[None, :, :]) ** 2).sum(-1)
        dil = (h[None, :] - t2 * inv2b).max(1)          # dilation at every local point
        dil = np.maximum(dil, h)
        ero = (dil + (t * t).sum(1) * inv2b).min()        # erosion back at v (v sits at height 0, lateral 0)
        out[v] = min(max(ero, 0.0), max_lift) * w[v]
    lift = np.maximum(blur(out, 4), 0.0)
    # hems, necklines and armholes sit on the body: the lift fades out over the last few centimetres to an edge
    e0 = np.sort(np.concatenate([T[:, [0, 1]], T[:, [1, 2]], T[:, [2, 0]]]), axis=1)
    ue0, ec0 = np.unique(e0, axis=0, return_counts=True)
    near_edge = np.zeros(n)
    near_edge[ue0[ec0 == 1].reshape(-1)] = 1.0
    near_edge = blur(near_edge, 8)
    lift *= 1.0 - np.clip(near_edge * 5.0, 0.0, 0.85)
    Q = base + Nd * lift[:, None]
    # relax the lifted cloth (no folds where the underside moved forward), never into the body
    # (only along the body normal, so nothing slides; the garment's edges stay put)
    e = np.sort(np.concatenate([T[:, [0, 1]], T[:, [1, 2]], T[:, [2, 0]]]), axis=1)
    ue, ec = np.unique(e, axis=0, return_counts=True)
    edge = np.zeros(n, bool)
    edge[ue[ec == 1].reshape(-1)] = True
    moving = ((lift > 1e-4) | (blur(lift, 2) > 1e-4)) & ~edge
    for _ in range(25):
        acc = np.zeros_like(Q)
        for i, j in ((0, 1), (1, 2), (2, 0), (1, 0), (2, 1), (0, 2)):
            np.add.at(acc, T[:, i], Q[T[:, j]])
        step = ((acc / np.maximum(cnt, 1)[:, None] - Q) * Nb).sum(1)
        Qn = Q + 0.5 * step[:, None] * Nb
        under = ((Qn - base) * Nb).sum(1)
        Qn = Qn - np.minimum(under, 0.0)[:, None] * Nb      # back out to the skin + thickness
        Q = np.where(moving[:, None], Qn, Q)
    lift = ((Q - base) * Nd).sum(1)
    Nq = np.zeros_like(Q)
    fn = np.cross(Q[T[:, 1]] - Q[T[:, 0]], Q[T[:, 2]] - Q[T[:, 0]])
    for i in range(3):
        np.add.at(Nq, T[:, i], fn)
    Nq /= np.maximum(np.linalg.norm(Nq, axis=1, keepdims=True), 1e-9)
    Nq = np.where((Nq * Nb).sum(1, keepdims=True) < 0, -Nq, Nq)
    # the cloth's own occlusion: the skin's, blurred (no navel, ribs or muscle lines printed through the fabric)
    ao = np.zeros(n)
    ao[cid] = S.AO
    ao = blur(ao, 40)
    return Q[cid], Nq[cid], lift[cid], ao[cid]


def add_shell(mesh, S, thick, key, rimkey, rim_low=None, low_y=None, max_lift=0.0, arms_too=False, bend=0.6):
    """A garment: the covered soup pushed out by `thick` (and draped over hollows by up to max_lift), and a rim strip
    from its edge back to the skin. rim_low: another rim material for edges below low_y (white piping at a hem)."""
    if not len(S):
        return
    if max_lift > 0:
        Pc, Nc, lifted, ao = drape(S, thick, max_lift, arms_too=arms_too, bend=bend)
        # the cloth standing off the body catches less of the body's own occlusion
        out = Soup(Pc, Nc, S.UV, np.minimum(1.0, ao * 0.97 + np.clip(lifted / 0.03, 0, 1) * 0.06), S.Wd)
    else:
        out = Soup(S.P + S.N * thick, S.N, S.UV, S.AO * 0.97, S.Wd)
    mesh.add(key, out)
    be = boundary_edges(S)
    if not len(be):
        return
    a, c = be[:, 0], be[:, 1]
    pa_o, pc_o, pa_i, pc_i = out.P[a], out.P[c], S.P[a], S.P[c]
    nrm = np.cross(pc_o - pa_o, S.N[a])
    nrm /= np.maximum(np.linalg.norm(nrm, axis=1, keepdims=True), 1e-9)
    corner = np.stack([a, c, c, a, c, a], 1).reshape(-1)
    P = np.stack([pa_o, pc_o, pc_i, pa_o, pc_i, pa_i], 1).reshape(-1, 3)
    rim = Soup(P, np.repeat(nrm, 6, 0), S.UV[corner], S.AO[corner] * 0.85, S.Wd[corner])
    if rim_low is not None:
        low = np.repeat(np.minimum(S.P[a, 1], S.P[c, 1]) < low_y, 6)
        mesh.add(rim_low, rim.take(np.nonzero(low)[0]))
        mesh.add(rimkey, rim.take(np.nonzero(~low)[0]))
    else:
        mesh.add(rimkey, rim)


def add_fade(mesh, S, down=0.10, out=0.014, below=None):
    """Energy fade hanging below the bottom boundary of a soup (the waist or the thigh cut)."""
    be = boundary_edges(S)
    if not len(be):
        return
    a, c = be[:, 0], be[:, 1]
    lowest = np.minimum(S.P[a, 1], S.P[c, 1])
    sel = lowest < (below if below is not None else S.P[:, 1].min() + 0.05)
    a, c = a[sel], c[sel]
    rad = lambda v: np.stack([S.N[v][:, 0], np.zeros(len(v)), S.N[v][:, 2]], 1)
    ra, rc = rad(a), rad(c)
    rows = 5
    for r in range(rows):
        f0, f1 = r / rows, (r + 1) / rows

        def ring(v, rv, f):
            return S.P[v] + rv * (0.006 + out * f) + np.array([0, -down * f, 0])
        p00, p01, p10, p11 = ring(a, ra, f0), ring(c, rc, f0), ring(a, ra, f1), ring(c, rc, f1)
        P = np.stack([p00, p01, p11, p00, p11, p10], 1).reshape(-1, 3)
        corner = np.stack([a, c, c, a, c, a], 1).reshape(-1)
        fs = np.tile(np.array([f0, f0, f1, f0, f1, f1]), len(a))
        uv = np.stack([0.5 + 0.5 * fs, np.repeat(np.arange(len(a)) % 2, 6) * 0.5 + 0.25], 1)
        mesh.add('fade', Soup(P, np.repeat(ra, 6, 0), uv, np.ones(len(P)), S.Wd[corner]))


# ---------------------------------------------------------------- head extras: eyes, brows
def sphere(c, r, nu=18, nv=12):
    th = np.linspace(0, 2 * math.pi, nu + 1)
    ph = np.linspace(0, math.pi, nv + 1)
    TH, PH = np.meshgrid(th, ph)
    D = np.stack([np.sin(PH) * np.cos(TH), np.cos(PH), np.sin(PH) * np.sin(TH)], -1)
    Pg = c + r * D
    tris = []
    for i in range(nv):
        for j in range(nu):
            a, bb, cc, d = i * (nu + 1) + j, i * (nu + 1) + j + 1, (i + 1) * (nu + 1) + j, (i + 1) * (nu + 1) + j + 1
            tris += [(a, cc, bb), (bb, cc, d)]
    tris = np.array(tris)
    Pf, Df = Pg.reshape(-1, 3), D.reshape(-1, 3)
    return Pf[tris.reshape(-1)], Df[tris.reshape(-1)]


def disc(c, r, n, segs=20):
    n = n / np.linalg.norm(n)
    a = np.cross(n, [0, 1.0, 0]); a /= np.linalg.norm(a)
    bb = np.cross(n, a)
    ang = np.linspace(0, 2 * math.pi, segs + 1)
    ring = c + r * (np.outer(np.cos(ang), a) + np.outer(np.sin(ang), bb))
    P = np.stack([np.repeat(c[None], segs, 0), ring[:-1], ring[1:]], 1).reshape(-1, 3)
    return P, np.tile(n, (len(P), 1))


def cap(c, R, look, ang, rings=4, segs=24):
    """Spherical cap of radius R around c, centred on direction `look`, angular radius `ang` (radians)."""
    n = look / np.linalg.norm(look)
    a = np.cross(n, [0, 1.0, 0]); a /= np.linalg.norm(a)
    bb = np.cross(n, a)
    phi = np.linspace(0, 2 * math.pi, segs + 1)

    def ring(al):
        D = math.cos(al) * n[None] + math.sin(al) * (np.outer(np.cos(phi), a) + np.outer(np.sin(phi), bb))
        return D / np.linalg.norm(D, axis=1, keepdims=True)
    D = []
    for k in range(rings):
        r0, r1 = ring(ang * k / rings), ring(ang * (k + 1) / rings)
        for j in range(segs):
            if k == 0:
                D += [n, r1[j], r1[j + 1]]
            else:
                D += [r0[j], r1[j], r1[j + 1], r0[j], r1[j + 1], r0[j + 1]]
    D = np.array(D)
    return c + R * D, D


def ray_mesh(o, d, P, T):
    """First hit of rays o + t d (o, d: [n,3]) on triangles (Moller-Trumbore, brute force)."""
    v0, v1, v2 = P[T[:, 0]], P[T[:, 1]], P[T[:, 2]]
    e1, e2 = v1 - v0, v2 - v0
    out = np.full(len(o), np.nan)
    for i in range(len(o)):
        pv = np.cross(d[i], e2)
        det = (e1 * pv).sum(1)
        ok = np.abs(det) > 1e-12
        inv = np.where(ok, 1.0 / np.where(ok, det, 1), 0)
        tv = o[i] - v0
        uu = (tv * pv).sum(1) * inv
        qv = np.cross(tv, e1)
        vv = (d[i] * qv).sum(1) * inv
        tt = (e2 * qv).sum(1) * inv
        hit = ok & (uu >= 0) & (vv >= 0) & (uu + vv <= 1) & (tt > 0)
        if hit.any():
            out[i] = tt[hit].min()
    return out


def add_head_extras(mesh, b, head_tris):
    J = b.J
    hw = np.zeros(7)
    hw[HEAD] = 1.0

    def add(key, P, N):
        n = len(P)
        mesh.add(key, Soup(P, N, np.zeros((n, 2)), np.ones(n), np.tile(hw, (n, 1))))
    r = J['eye_radius'] * 0.95
    # the eyelid openings: the rim of the hole the head mesh has at each eye; the iris looks out through its centre
    rim = boundary_edges(Soup.of(b, head_tris))
    rimP = Soup.of(b, head_tris).P[rim.reshape(-1)]
    for side in ('l', 'r'):
        c = J['eye_' + side] + np.array([0, 0, 0.0005])
        near = rimP[np.linalg.norm(rimP - J['eye_' + side], axis=1) < 0.028]
        look = near.mean(0) - c if len(near) > 6 else np.array([0, 0, -1.0])
        look[2] = min(look[2], -1e-3)
        look /= np.linalg.norm(look)
        P, D = sphere(c, r, nu=36, nv=24)
        add('eye_white', P, D)
        for key, rr, lift in (('iris', r * 0.47, 0.00035), ('pupil', r * 0.21, 0.00060)):
            Pd, Nd = cap(c, r + lift, look, math.asin(rr / r), rings=4 if key == 'iris' else 2)
            add(key, Pd, Nd)
    # brows: ribbons on the brow ridge, found by casting rays from inside the head forward
    hv = np.unique(head_tris.reshape(-1))
    center = J['eye_mid'] + np.array([0, 0.0, 0.07])
    for sd in (-1, 1):
        ex = J['eye_' + ('l' if sd < 0 else 'r')]
        xs = np.linspace(0.006, 0.050, 10) * sd + 0.0 * ex[0]
        xs = np.abs(xs) * np.sign(sd)
        pts = []
        for i, x in enumerate(xs):
            target = np.array([x, J['eye_mid'][1] + 0.021 + 0.005 * math.sin(math.pi * (abs(x) - 0.006) / 0.044), J['eye_mid'][2] - 0.02])
            d = target - center
            d /= np.linalg.norm(d)
            o = center[None]
            t = ray_mesh(o, d[None], b.P, head_tris)
            if np.isnan(t[0]):
                continue
            pts.append(center + d * t[0])
        pts = np.array(pts)
        if len(pts) < 3:
            continue
        tang = np.gradient(pts, axis=0)
        tang /= np.linalg.norm(tang, axis=1, keepdims=True)
        nrm = (pts - center); nrm /= np.linalg.norm(nrm, axis=1, keepdims=True)
        up = np.cross(tang, nrm) * (-sd)
        width = np.linspace(0.0052, 0.0030, len(pts))
        lift = 0.0012
        top = pts + nrm * lift + up * width[:, None] * 0.5
        bot = pts + nrm * lift - up * width[:, None] * 0.5
        tri_p, tri_n = [], []
        for i in range(len(pts) - 1):
            q = [top[i], bot[i], bot[i + 1], top[i], bot[i + 1], top[i + 1]]
            tri_p += q
            tri_n += [nrm[i]] * 6
        add('brow', np.array(tri_p), np.array(tri_n))


# ================================================================ outfits
# how far cloth may stand off the body (m) and how tightly it may curve inward (bend radius, m): suits hug, tees hang
DRAPE_TOP = {0: (0.012, 0.15), 1: (0.050, 1.2), 2: (0.010, 0.15), 3: (0.055, 1.0), 4: (0.060, 1.2), 5: (0.012, 0.15),
             6: (0.050, 1.0), 7: (0.040, 0.6), 8: (0.040, 0.6), 9: (0.050, 1.2)}
DRAPE_BOTTOM = (0.025, 0.4)
TOPS = {   # N: (coverage style, thickness, material, rim, design, bracer, plates)
    0: (dict(sleeve=1.95, collar=0.045), 0.0028, 'av_primary', 'av_secondary', 'grid', None, ()),
    1: (dict(sleeve=0.42), 0.0085, 'av_primary', 'av_secondary', 'jersey', None, ()),
    2: (dict(sleeve=1.95, collar=0.040), 0.0028, 'av_dark', 'av_dark', 'vanguard', None, ('chest', 'back', 'pauldron', 'vambrace')),
    3: (dict(sleeve=1.95, collar=0.062), 0.0130, 'av_primary', 'av_secondary_dk', 'jacket', None, ()),
    4: (dict(sleeve=1.95, collar=0.030), 0.0150, 'av_primary', 'av_secondary_dk', 'hoodie', None, ()),
    5: (dict(sleeve=1.95, collar=0.055), 0.0030, 'av_primary', 'av_secondary', 'echo', None, ()),
    6: (dict(armhole=(0.80, 0.64), scoop=True), 0.0040, 'av_primary', 'av_primary_dk', None, 'silver', ()),
    7: (dict(sleeve=1.95, collar=0.040), 0.0070, 'av_primary', 'av_secondary_dk', 'pilot', None, ()),
    8: (dict(armhole=(0.88, 0.68), collar=0.060), 0.0040, 'av_primary', 'av_primary_np', 'vest', 'circuit', ()),
    9: (dict(sleeve=0.45), 0.0040, 'av_primary', 'av_primary_dk', 'tee', 'gray', ()),
}
BOTTOMS = {1: ('dolphin', 0.0100, 'av_secondary', 'av_piping', 'dolphin'),
           2: ('circuit', 0.0055, 'av_primary', 'av_primary_np', 'circuit_shorts'),
           3: ('bike', 0.0045, 'av_primary', 'av_primary_dk', 'bike')}
BRACER_DESIGN = {'circuit': 'bracer_circuit', 'silver': 'bracer_silver', 'gray': 'bracer_gray'}


def materials():
    m = {
        'skin': mat('av_skin'), 'lips': dict(mat('av_skin'), tint=(0.86, 0.60, 0.58, 1.0)),
        'eye_white': mat('av_eye_white'), 'iris': mat('av_iris', twosided=True), 'pupil': mat('av_eye', twosided=True),
        'brow': mat('av_hair', twosided=True),
        'fade': mat('av_fade'),
    }
    for pal in ('av_primary', 'av_secondary', 'av_dark', 'av_primary_np', 'av_primary_dk', 'av_secondary_dk', 'av_piping',
                'av_armor', 'av_secondary'):
        m['rim:' + pal] = mat(pal, twosided=True)
    return m


def build_body(b, raster, design_tex):
    mats = materials()
    total = 0
    whole = Soup.of(b, b.T)
    headS, rest = clip(whole, lambda S: S.Wd[:, HEAD] - 0.5)
    lowerS, topS = clip(rest, lambda S: np.where(S.Wd[:, 3:7].sum(1) > 0.5, -1.0, b.waist_y - S.P[:, 1]))
    # head
    mesh = Mesh()
    lip_keys = set(map(tuple, weld_keys(b.P[b.region == 1])))
    is_lip = np.array([tuple(k) in lip_keys for k in weld_keys(headS.P)]).reshape(-1, 3).all(1)
    tri = np.arange(len(headS))
    corners_of = lambda sel: (tri[sel][:, None] * 3 + np.arange(3)).reshape(-1)
    mesh.add('lips', headS.take(corners_of(is_lip)))
    mesh.add('skin', headS.take(corners_of(~is_lip)))
    add_head_extras(mesh, b, b.T[b.fdom == HEAD])
    total += mesh.write('av_bhead_' + b.key, mats)
    # tops
    for n, (style, thick, gmat, rim, dname, bracer, plate_list) in TOPS.items():
        mesh = Mesh()
        cov, skin = clip(topS, f_top(b, style))
        gkey = 'g:%s:%s' % (gmat, dname or '')
        mats[gkey] = mat(gmat, mask=('open_design_' + dname) if dname else '', extra_flags=CLOTH)
        if bracer:
            br, skin = clip(skin, f_bracer(b))
            bkey = 'g:av_armor:' + BRACER_DESIGN[bracer]
            mats[bkey] = mat('av_armor', mask='open_design_' + BRACER_DESIGN[bracer])
            add_shell(mesh, br, 0.011, bkey, 'rim:av_armor')
        mesh.add('skin', skin)
        lift, bend = DRAPE_TOP.get(n, (0.0, 0.6))
        add_shell(mesh, cov, thick, gkey, 'rim:' + rim, max_lift=lift, bend=bend, arms_too=style.get('sleeve', 0) > 1)
        for pl in plate_list:
            sel, _ = clip(cov, f_plate(b, pl))
            pkey = 'g:av_primary_np:'
            mats[pkey] = mat('av_primary_np')
            add_shell(mesh, Soup(sel.P + sel.N * thick, sel.N, sel.UV, sel.AO, sel.Wd),
                      0.016 if pl in ('chest', 'back') else 0.012, pkey, 'rim:av_secondary')
        total += mesh.write('av_top_%d_%s' % (n, b.key), mats)
    # waist fade (no lower body)
    mesh = Mesh()
    torso_top, _ = clip(topS, lambda S: S.Wd[:, TORSO] - 0.5)
    add_fade(mesh, torso_top, below=b.waist_y + 0.03)
    total += mesh.write('av_wfade_' + b.key, mats)
    # bottoms
    for k, (kind, thick, gmat, rim, dname) in BOTTOMS.items():
        mesh = Mesh()
        cov, skin = clip(lowerS, f_bottom(b, kind))
        mesh.add('skin', skin)
        gkey = 'g:%s:%s' % (gmat, dname)
        mats[gkey] = mat(gmat, mask='open_design_' + dname, extra_flags=CLOTH)
        add_shell(mesh, cov, thick, gkey, 'rim:' + gmat, rim_low='rim:' + rim, low_y=b.waist_y - 0.06, max_lift=DRAPE_BOTTOM[0], bend=DRAPE_BOTTOM[1])
        add_fade(mesh, lowerS, down=0.09, out=0.010, below=b.cut + 0.03)
        total += mesh.write('av_bottom_%d_%s' % (k, b.key), mats)
    return total


def write_bind(bodies):
    def v3(a):
        return '{%.5ff, %.5ff, %.5ff}' % tuple(a)
    lines = ['// Generated by tools/build_body_parts.py from the MPFB2 bodies -- do not edit.',
             '// Bind pose (app space, metres, origin = the neck base): the skinned avatar parts are authored here.',
             'static const BodyBind kBodyBind[2] = {']
    for b in bodies:
        J = b.J
        lines.append('    {%s, %s, {%s, %s}, {%s, %s}, {%s, %s}, {%s, %s}},   // %s' % (
            v3(J['eye_mid']), v3(J['head']), v3(J['shoulder_l']), v3(J['shoulder_r']), v3(J['elbow_l']), v3(J['elbow_r']),
            v3(J['wrist_l']), v3(J['wrist_r']), v3(J['thumb_l']), v3(J['thumb_r']), {'m': 'MASCULINE', 'f': 'FEMININE'}[b.key]))
    lines.append('};')
    open(os.path.join(ROOT, 'app', 'src', 'avatar_bodies.inc'), 'w', newline='\n').write('\n'.join(lines) + '\n')


def main():
    bodies = [Body('m'), Body('f')]
    raster = UVRaster(bodies[0])
    names = sorted({d for t in TOPS.values() if t[4] for d in [t[4]]} | set(BRACER_DESIGN.values()) | {v[4] for v in BOTTOMS.values()})
    for nm in names:
        write_tex('open_design_' + nm, raster.image(design(nm, bodies[0], raster.X, raster.dom, raster.at, raster.phi)))
    pat_names = {1: 'circuit', 2: 'stripes', 3: 'chevron', 4: 'hex', 5: 'split', 6: 'pixel', 7: 'bands'}
    for k, nm in pat_names.items():
        write_tex('open_pat_' + nm, raster.image(pattern(k, bodies[0], raster.X, raster.dom, raster.at, raster.phi)))
    print('textures: %d designs, %d patterns (%dx%d)' % (len(names), len(pat_names), TEX, TEX))
    total = 0
    for b in bodies:
        print('body', b.key, 'waist %.3f armpit %.3f hips %.3f cut %.3f' % (b.waist_y, b.armpit_y, b.hip_y, b.cut))
        total += build_body(b, raster, None)
    write_bind(bodies)
    print('done: %d vertices in all parts' % total)


if __name__ == '__main__':
    main()
