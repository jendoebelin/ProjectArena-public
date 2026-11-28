"""FACIAL HAIR (0.3.27, the owner, 2026-11-27: "longer beard, mustache, and goatee options"): fitted to every face of
both bodies, grown from that face's own skin. For each head (av_bhead_<m|f>[1-5]) the face's landmarks come from its
own profile (the nose's tip, the gap between the lips, the chin's bottom); each style is a weight over the skin
(1 inside, fading at its edge), cut along the line where the weight is kCut (a smooth outline, not the head's
triangles) and lifted off the skin by that weight times the style's thickness. LONG BEARD is the full beard, thicker,
and below the chin a part of its own (the head ends there; the neck is the outfit's): a lens-shaped mass, front and
back sheets meeting at its rounded point, hanging in front of the neck; its top follows the head, its point partly the
neck and the chest. Skinned as the head is (each vertex the bones of the skin under it), in the hair colour (MAT
channel 6), two-sided, a fibre texture running down the face.

Writes app/assets_open/av_fhair_<style>_<m|f><face>.mdl and tex/open_beard_fibres.tex (.lod files: tools/make_lods).
Styles: 1 MUSTACHE, 2 GOATEE, 3 SHORT BEARD, 4 LONG BEARD.
Usage: python -I tools/build_facial_hair.py [--landmarks]"""
import os
import struct
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, 'app', 'assets_open')
DT = [('p', '<f4', 3), ('n', '<f4', 3), ('u0', '<f4', 2), ('u1', '<f4', 2), ('c', 'u1', 4), ('bi', 'u1', 4), ('bw', 'u1', 4)]
NB = 9            # (the rig's bones: avatar.h BONE_COUNT)
B_TORSO, B_NECK, B_HEAD = 0, 1, 2
kCut = 0.05       # the outline: where a style's weight is this


def load(name):
    d = open(os.path.join(ASSETS, name + '.mdl'), 'rb').read()
    ver, nv, ni, ns, nm = struct.unpack_from('<5I', d, 4)
    V = np.frombuffer(d, dtype=DT, count=nv, offset=24).copy()
    I = np.frombuffer(d, '<u4', count=ni, offset=24 + nv * 52).copy()
    S = [struct.unpack_from('<3I', d, 24 + nv * 52 + ni * 4 + 12 * k) for k in range(ns)]
    return V, I, S


def smooth(a, b, x):
    t = np.clip((x - a) / (b - a), 0.0, 1.0)
    return t * t * (3 - 2 * t)


def dense(bi, bw):
    """(n,4) bone indices / weights (0-255) -> (n, NB) weights summing to 1"""
    out = np.zeros((len(bi), NB))
    for k in range(4):
        np.add.at(out, (np.arange(len(bi)), bi[:, k].astype(int)), bw[:, k] / 255.0)
    return out / np.maximum(out.sum(1, keepdims=True), 1e-9)


def packed(w):
    """(n, NB) weights -> the four largest as bytes summing to 255"""
    idx = np.argsort(-w, axis=1)[:, :4]
    ww = np.take_along_axis(w, idx, 1)
    ww = ww / np.maximum(ww.sum(1, keepdims=True), 1e-9)
    b = np.floor(ww * 255 + 0.5).astype(int)
    b[:, 0] += 255 - b.sum(1)
    return idx.astype(np.uint8), np.clip(b, 0, 255).astype(np.uint8)


def landmarks(V, I, S):
    """From the centre line (|x| < 4 mm, the skin, the face's front): the nose's tip (the most forward point under the
    eyes), the mouth (the gap between the lips: the deepest point between the nose and the chin, the inside of the
    mouth, well behind the lips), the chin's bottom (the lowest point still at the face's front), the chin's front."""
    f, c, _ = S[2]
    skin = np.unique(I[f:f + c])
    P = V['p'][skin].astype(np.float64)
    Nn = V['n'][skin].astype(np.float64)
    f1, c1, _ = S[1]
    eye_y = float(V['p'][np.unique(I[f1:f1 + c1])][:, 1].mean())
    on = (np.abs(P[:, 0]) < 0.004) & (P[:, 2] < -0.05) & (P[:, 1] < eye_y - 0.02)
    mid, midn = P[on], Nn[on]
    nose = mid[np.argmin(mid[:, 2])]
    nose_y = float(nose[1])
    band = mid[(mid[:, 1] < nose_y - 0.015) & (mid[:, 1] > nose_y - 0.05)]
    mouth_y = float(band[np.argmax(band[:, 2])][1])
    lip_z = float(band[:, 2].min())
    chin = mid[(mid[:, 2] < lip_z + 0.015) & (mid[:, 1] < mouth_y - 0.01)]
    chin_y = min(float(chin[:, 1].min()) if len(chin) else 1.0, mouth_y - 0.03)   # (a face whose chin sits back: 3 cm under the mouth)
    cf = mid[(mid[:, 1] > chin_y) & (mid[:, 1] < chin_y + 0.02)]
    chin_z = float(cf[:, 2].min()) if len(cf) else lip_z
    # the nose's bottom: the lowest point under its tip still facing down (below it the upper lip faces forward)
    under = mid[(midn[:, 1] < -0.6) & (mid[:, 1] < nose_y) & (mid[:, 1] > max(mouth_y + 0.008, nose_y - 0.022))]
    sub_y = float(under[:, 1].min()) if len(under) else nose_y - 0.0145
    # the lower lip: its fullest point, how far under the mouth (FACE 6's masculine 10.5 mm, the others 9.2 or less)
    low = mid[(mid[:, 1] < mouth_y - 0.002) & (mid[:, 1] > chin_y + 0.004)]
    lip_under = float(mouth_y - low[np.argmin(low[:, 2])][1]) if len(low) else 0.009
    # the ear: where the head's side stands out past 7.3 cm, between the mouth's height and the eyes', behind the cheek:
    # its front edge and its lowest point (the lobe's bottom)
    ear = (np.abs(P[:, 0]) > 0.073) & (P[:, 1] > mouth_y + 0.005) & (P[:, 1] < eye_y + 0.01) & (P[:, 2] > -0.055)
    ear_z = float(P[ear, 2].min()) if ear.any() else -0.03
    # (its lowest point: where the ear still stands 7 mm and more out from the head 4.5 cm behind it, within its own
    # depth, from 1.6 cm over the mouth up (lower, the jaw's angle stands out too): a lobe hangs lower and closer in
    # than the ear's top, the feminine FACE 5's to 2 cm over the mouth)
    side = P[:, 0] > 0
    Q = P[side]
    ear_y = float(P[ear, 1].min()) if ear.any() else mouth_y + 0.02
    for k in np.argsort(Q[:, 1]):
        q = Q[k]
        if q[1] < mouth_y + 0.016 or q[1] > ear_y or q[2] < ear_z - 0.005 or q[2] > ear_z + 0.04:
            continue
        behind = Q[(np.abs(Q[:, 1] - q[1]) < 0.004) & (Q[:, 2] > q[2] + 0.037) & (Q[:, 2] < q[2] + 0.053)]
        if len(behind) and q[0] - behind[:, 0].min() > 0.007:
            ear_y = float(q[1])
            break
    return dict(eye_y=eye_y, nose_y=nose_y, mouth_y=mouth_y, chin_y=chin_y, chin_z=chin_z, front_z=lip_z, sub_y=sub_y,
                ear_x=float(np.abs(P[:, 0]).max()), lip_under=lip_under, ear_z=ear_z, ear_y=ear_y)


def weights(P, N, L, style):
    x, y, z = P[:, 0], P[:, 1], P[:, 2]
    ax = np.abs(x)
    nz = N[:, 2] / np.maximum(np.linalg.norm(N, axis=1), 1e-9)
    m, nose, chin = L['mouth_y'], L['nose_y'], L['chin_y']
    sub = L['sub_y'] + 0.0005                              # (the nose's bottom)
    front = smooth(-0.02, -0.06, z)                         # (the face's front half)
    # MUSTACHE: under the nose to over the top of the upper lip, a little past the mouth's corners, drooping there
    edge = m + 0.0055 - 0.007 * smooth(0.014, 0.030, ax)    # (its lower edge: over the lip, lower at the ends)
    top = sub + 0.001 - 0.014 * smooth(0.012, 0.034, ax)    # (its upper edge: down to the ends, which taper)
    must = smooth(0.035, 0.029, ax) * smooth(edge - 0.001, edge + 0.002, y) * smooth(top, top - 0.004, y) * front
    # GOATEE: the chin (under the lower lip to just under the chin), joined to the mustache at the mouth's corners
    # (under the chin only to 3 cm behind its front: along a long, flat jaw it ran back under it and stood off it as a
    # flap, the feminine FACE 3's, the owner's screenshot, 2026-11-27)
    chinw = (smooth(0.031, 0.022, ax) * smooth(chin - 0.012, chin - 0.002, y) * smooth(m - 0.007, m - 0.012, y) * front *
             smooth(L['chin_z'] + 0.045, L['chin_z'] + 0.030, z))
    join = smooth(0.034, 0.029, ax) * smooth(0.021, 0.026, ax) * smooth(chin + 0.008, chin + 0.018, y) * smooth(m + 0.004, m - 0.001, y) * front
    goatee = np.maximum(np.maximum(must, chinw), join)
    # BEARDS: under a cheek line from the nose's side rising to the sideburns, the jaw and the chin, down to the head's
    # lower edge; the mustache with them
    t = np.clip((ax - 0.020) / (L['ear_x'] - 0.020), 0, 1)
    cheek = sub + 0.002 + 0.036 * t ** 1.3
    # (its back edge: below the ear, the jaw's angle; from just under the lobe up, 8 mm in front of the ear's front edge,
    # each face's own: it ran straight down behind the ear at every height and grew up the ear, its line rising to the
    # head's widest point, every masculine face and the feminine FACE 5, the owner's screenshots, 2026-11-27: "still
    # covering ear lobe")
    zb = -0.004 + (L['ear_z'] - 0.008 + 0.004) * smooth(L['ear_y'] - 0.024, L['ear_y'] - 0.010, y)
    side = smooth(zb + 0.004, zb - 0.004, z)
    beard = smooth(cheek + 0.003, cheek - 0.004, y) * side
    # (not on the ears: from the mouth's height up, the head's side stands out past 7 cm where the ear is, its front
    # 3.5 cm behind the face's; the sideburns grew onto it, the owner, 2026-11-27: "sometimes the beard is on the ears")
    beard = beard * (1 - smooth(0.063, 0.070, ax) * smooth(-0.046, -0.036, z) * smooth(m + 0.008, m + 0.018, y))
    beard = np.maximum(beard, must)
    w = {1: must, 2: goatee, 3: beard, 4: beard}[style]
    # never the nose, nor inside the mouth (the lips' inner sides and the mouth's corners: not facing forward)
    nosem = smooth(0.023, 0.019, ax) * smooth(sub - 0.003, sub + 0.0005, y)
    inner = smooth(0.034, 0.030, ax) * smooth(0.010, 0.007, np.abs(y - m)) * smooth(-0.55, -0.40, nz)
    w = w * (1 - nosem) * (1 - inner)
    if style == 1:
        return w
    # the lips stay bare (the mustache's own lower edge already sits above the lower one)
    # (a lower lip fuller than 9.5 mm under the mouth: bare to 3.5 mm past its fullest point; FACE 6's masculine beard
    # covered most of it, the owner's screenshot, 2026-11-27)
    lowlip = 0.0095 if L['lip_under'] <= 0.0095 else L['lip_under'] + 0.0035
    r = np.sqrt((ax / 0.026) ** 2 + ((y - m) / np.where(y > m, 0.0085, lowlip)) ** 2)
    lips = smooth(0.92, 1.12, r)
    return np.where(y > m, np.maximum(w * lips, must * (1 - nosem) * (1 - inner)), w * lips)


STYLE = {1: ('MUSTACHE', 0.0045), 2: ('GOATEE', 0.005), 3: ('SHORT BEARD', 0.005), 4: ('LONG BEARD', 0.0075)}
# its share of the beard's swing (UV1.x; the game: avatar.cpp stepBeard, render.cpp uBeardMotion, 25 mm at most): the
# lower a beard's hair, the more; LONG BEARD's point all of it; a mustache's ends a touch
SWAY = {1: 0.06, 2: 0.10, 3: 0.12, 4: 0.22}


def share(P, L, style):
    """its share of the swing at the skin's points P (head's bind space)"""
    if style == 1:
        return SWAY[1] * smooth(0.014, 0.034, np.abs(P[:, 0]))
    return SWAY[style] * smooth(L['mouth_y'] - 0.012, L['chin_y'] - 0.004, P[:, 1])


def refine(P, N, bones, tri, wf, off):
    """Where a style's edge fades, each triangle into four: first every triangle the weight rises across (neither all
    out nor all in, judged at its corners, middle and edges' middles: a triangle can be crossed with its corners all
    out), then those the outline (kCut) crosses once more: the outline and the shell's rise follow the weight, not the
    head's 8 mm triangles. Midpoints lie on the skin's own triangles; offset normals are interpolated, not normalised.
    A midpoint on an edge shared with a triangle left whole (inside at both ends) takes its ends' mean weight and
    offset (the lift off the skin, off(w) along the normal): it stays on that triangle's edge, no crack."""
    P, N, B = list(P), list(N), list(bones)
    w = list(wf(np.array(P), np.array(N)))
    D = list(np.array(N) * off(np.array(w))[:, None])
    for level, keep in ((0, 0.98), (1, kCut)):
        Pa, Na = np.array(P), np.array(N)
        t = np.array(tri, int)
        probe = [wf((Pa[t[:, 0]] + Pa[t[:, 1]] + Pa[t[:, 2]]) / 3, (Na[t[:, 0]] + Na[t[:, 1]] + Na[t[:, 2]]) / 3)]
        for i, j in ((0, 1), (1, 2), (2, 0)):
            probe.append(wf((Pa[t[:, i]] + Pa[t[:, j]]) / 2, (Na[t[:, i]] + Na[t[:, j]]) / 2))
        wa = np.array(w)
        allw = np.concatenate([wa[t], np.stack(probe, 1)], 1)
        lo, hi = allw.min(1), allw.max(1)
        split = (hi > 0) & (lo < 0.98) if level == 0 else (hi >= kCut) & (lo < kCut)
        mids = {}

        def mid(i, j):
            k = (min(i, j), max(i, j))
            if k not in mids:
                P.append((P[i] + P[j]) * 0.5)
                N.append((N[i] + N[j]) * 0.5)
                B.append((B[i] + B[j]) * 0.5)
                inside = w[i] >= keep and w[j] >= keep
                w.append((w[i] + w[j]) * 0.5 if inside else None)
                D.append((D[i] + D[j]) * 0.5 if inside else None)
                mids[k] = len(P) - 1
            return mids[k]

        for (a, b, c), sp in zip(tri, split):
            if sp:
                mid(a, b), mid(b, c), mid(c, a)
        # (then each triangle by the midpoints on its edges: four, or a neighbour left whole cut to fit the midpoints on
        # its edges, in two or three, so no corner sits on another triangle's edge: a T there leaves pinholes on screen)
        out = []
        for a, b, c in tri:
            v = (a, b, c)
            m = [mids.get((min(v[k], v[(k + 1) % 3]), max(v[k], v[(k + 1) % 3]))) for k in range(3)]
            n = sum(x is not None for x in m)
            if n == 0:
                out.append((a, b, c))
            elif n == 3:
                ab, bc, ca = m
                out += [(a, ab, ca), (ab, b, bc), (ca, bc, c), (ab, bc, ca)]
            elif n == 1:
                k = next(k for k in range(3) if m[k] is not None)
                x, y, z = v[k], v[(k + 1) % 3], v[(k + 2) % 3]
                out += [(x, m[k], z), (m[k], y, z)]
            else:
                k = next(k for k in range(3) if m[k] is None)   # (z -> x has none; x -> y and y -> z have)
                z, x, y = v[k], v[(k + 1) % 3], v[(k + 2) % 3]
                m1, m2 = m[(k + 1) % 3], m[(k + 2) % 3]
                out += [(m1, y, m2), (x, m1, m2), (x, m2, z)]
        todo = [k for k in range(len(w)) if w[k] is None]
        if todo:
            ev = wf(np.array([P[k] for k in todo]), np.array([N[k] for k in todo]))
            for k, v in zip(todo, ev):
                w[k] = float(v)
                D[k] = N[k] * off(float(v))
        tri = out
    return np.array(P), np.array(N), np.array(B), np.array(w), np.array(D), np.array(tri, int)


def clip(P, N, bones, tri, w, D, off):
    """The triangles where w >= kCut, cut along w = kCut: (positions, normals, bones, offsets, triangles)"""
    pos, nor, bon, dd = list(P), list(N), list(bones), list(D)
    cut = {}

    def cross(i, j):
        k = (min(i, j), max(i, j))
        if k not in cut:
            t = (kCut - w[i]) / (w[j] - w[i])
            pos.append(P[i] + (P[j] - P[i]) * t)
            nor.append(N[i] + (N[j] - N[i]) * t)
            bon.append(bones[i] + (bones[j] - bones[i]) * t)
            dd.append(nor[-1] * off(kCut))
            cut[k] = len(pos) - 1
        return cut[k]

    out = []
    for a, b, c in tri:
        ins = [w[v] >= kCut for v in (a, b, c)]
        if all(ins):
            out.append((a, b, c))
            continue
        if not any(ins):
            continue
        poly = []
        vs = (a, b, c)
        for k in range(3):
            i, j = vs[k], vs[(k + 1) % 3]
            if w[i] >= kCut:
                poly.append(i)
            if (w[i] >= kCut) != (w[j] >= kCut):
                poly.append(cross(i, j))
        for k in range(1, len(poly) - 1):
            out.append((poly[0], poly[k], poly[k + 1]))
    return np.array(pos), np.array(nor), np.array(bon), np.array(dd), np.array(out, int)


def bib(L, P, N, bones):
    """LONG BEARD below the chin: (positions, bone weights, triangles), a front and a back sheet sharing their outline"""
    NU, NV = 15, 10
    chin, cz = L['chin_y'], L['chin_z']
    sk = P[P[:, 2] < -0.03]
    skb = bones[P[:, 2] < -0.03]
    u = np.linspace(-1, 1, NU)
    v = np.linspace(0, 1, NV)
    U, Vv = np.meshgrid(u, v)                      # (NV, NU)
    # the top edge: along the jaw from the chin, inside the beard over it (1.5 mm off the skin)
    xt = 0.058 * U
    yt = chin + 0.006 + 0.026 * U ** 2
    near = np.argmin((sk[None, :, 0] - xt.ravel()[:, None]) ** 2 + (sk[None, :, 1] - yt.ravel()[:, None]) ** 2 * 0.5
                     + 0.002 * (sk[None, :, 2] + 0.2), axis=1)
    zt = sk[near, 2].reshape(xt.shape) - 0.0015
    topw = skb[near].reshape(NV, NU, NB)
    length = 0.070 * (1 - 0.55 * U ** 4)          # (longest at the middle: rounded)
    x = xt * (1 - 0.30 * Vv)
    y = yt - length * Vv
    zf = zt * U ** 2 + (cz - 0.0085) * (1 - U ** 2)  # (flush with the beard over the chin; the sides under the jaw)
    z = zt + (zf - zt) * smooth(0.0, 0.30, Vv) + 0.010 * Vv ** 2
    front = np.stack([x, y, z], -1)
    thick = 0.050 * (1 - U ** 2) * (1 - Vv) ** 0.85          # (the back sheet: toward the neck, meeting at the edges)
    back = front + np.stack([np.zeros_like(x), 0.004 * (1 - Vv) * (1 - U ** 2), thick], -1)
    tip = np.zeros(NB)
    tip[B_HEAD], tip[B_NECK], tip[B_TORSO] = 0.45, 0.45, 0.10
    s = smooth(0.0, 1.0, Vv)[..., None]
    bw = topw * (1 - s) + tip * s
    pts = np.concatenate([front.reshape(-1, 3), back.reshape(-1, 3)])
    bws = np.concatenate([bw.reshape(-1, NB), bw.reshape(-1, NB)])
    tris = []
    for sheet in range(2):
        o = sheet * NU * NV
        for j in range(NV - 1):
            for i in range(NU - 1):
                a, b, c, d = o + j * NU + i, o + j * NU + i + 1, o + (j + 1) * NU + i, o + (j + 1) * NU + i + 1
                tris += [(a, b, c), (b, d, c)] if sheet == 0 else [(a, c, b), (b, c, d)]   # (the front sheet faces forward)
    return pts, bws, np.array(tris, int)


def build(bk, face, style, L, V, I, S):
    f, c, _ = S[2]
    tri = I[f:f + c].reshape(-1, 3)
    P, N = V['p'].astype(np.float64), V['n'].astype(np.float64)
    bones = dense(V['bi'], V['bw'])
    # (the head's texture seams repeat vertices: one per place, so both sides of a seam split and cut alike)
    _, first, inv = np.unique(np.round(P / 1e-6).astype(np.int64), axis=0, return_index=True, return_inverse=True)
    tri = first[inv.ravel()][tri]
    w = weights(P, N, L, style)
    c3 = (P[tri[:, 0]] + P[tri[:, 1]] + P[tri[:, 2]]) / 3
    n3 = (N[tri[:, 0]] + N[tri[:, 1]] + N[tri[:, 2]]) / 3
    tri = tri[(w[tri] > 0).any(1) | (weights(c3, n3, L, style) > 0)]
    if not len(tri):
        return None
    thick = STYLE[style][1]
    off = lambda w: 0.0006 + thick * w * w * (3 - 2 * w)   # (the lift off the skin: an eased rise, the edge hugs the skin)
    P1, N1, B1, w1, D1, t1 = refine(P, N, bones, tri, lambda p, n: weights(p, n, L, style), off)
    P2, N2, B2, D2, t2 = clip(P1, N1, B1, t1, w1, D1, off)
    used = np.unique(t2)
    remap = -np.ones(len(P2), int)
    remap[used] = np.arange(len(used))
    p = P2[used] + D2[used]
    sh = share(P2[used], L, style)
    n = N2[used] / np.maximum(np.linalg.norm(N2[used], axis=1, keepdims=True), 1e-9)   # (lit as the skin under it: a thin layer)
    bw = B2[used]
    t = remap[t2]
    if style == 4:
        bp, bb, bt = bib(L, P, N, bones)
        # (the hanging part lit as the beard over the chin: facing forward and down, as a chin does, turning out at its sides; its
        # back sheet toward the neck. From its own folds it shone in a band under the chin)
        half = len(bp) // 2
        side = np.clip(bp[:, 0] / 0.058, -1, 1)
        bn = np.stack([side * 0.8, np.full(len(bp), -0.45), -np.ones(len(bp))], 1)
        bn[half:, 1:] *= -1
        bn /= np.linalg.norm(bn, axis=1, keepdims=True)
        # (its share: the beard's over the chin at its top, all of it at its point)
        nu = 15
        vrow = (np.arange(len(bp)) % half) // nu / (half // nu - 1)
        top = share(np.stack([bp[:, 0], np.full(len(bp), L['chin_y'] + 0.006), bp[:, 2]], 1), L, style)
        sh = np.concatenate([sh, top + (1 - top) * vrow ** 1.3])
        t = np.concatenate([t, bt + len(p)])
        p = np.concatenate([p, bp])
        n = np.concatenate([n, bn])
        bw = np.concatenate([bw, bb])
    out = np.zeros(len(p), dtype=DT)
    out['p'] = p
    out['n'] = n
    ang = np.arctan2(p[:, 0], -p[:, 2])
    out['u0'] = np.stack([ang * 2.0, -p[:, 1] * 9.0], 1)     # (fibres running down the face)
    # (x: its share of the swing; y: 1 where it lies on the skin, so its swing never pushes it in: render.cpp. LONG
    # BEARD's hanging part, after the skin's vertices, 0)
    hug = np.zeros(len(sh))
    hug[:len(P2[used])] = 1.0
    out['u1'] = np.stack([sh, hug], 1)
    out['c'] = 255
    out['bi'], out['bw'] = packed(bw)
    idx = t.astype('<u4').ravel()
    name = 'av_fhair_%d_%s%s' % (style, bk, face if face else '')
    mat = struct.pack('<I4f4f2I', 0, 1, 1, 1, 1, 0, 0, 0, -0.52, 0, 1537) + b'open_beard_fibres'.ljust(64, b'\0') + b''.ljust(64, b'\0')
    with open(os.path.join(ASSETS, name + '.mdl'), 'wb') as fp:
        fp.write(b'SMDL' + struct.pack('<5I', 3, len(out), len(idx), 1, 1))
        fp.write(out.tobytes())
        fp.write(idx.tobytes())
        fp.write(struct.pack('<3I', 0, len(idx), 0))
        fp.write(mat)
    return name, len(out), len(idx) // 3


def fibres():
    """Strands running down (v), each fading in and out (no hard starts), drawn at sub-pixel places, tiling both ways"""
    rng = np.random.default_rng(7)
    W, H = 256, 256
    img = np.zeros((H, W))
    for _ in range(1400):
        x0 = rng.uniform(0, W)
        y0 = rng.uniform(0, H)
        L = rng.uniform(24, 110)
        a = rng.uniform(-0.12, 0.12)
        val = rng.uniform(-0.20, 0.13)
        for yy in range(int(L)):
            y = int(y0 + yy) % H
            x = x0 + a * yy
            k = int(np.floor(x))
            fr = x - k
            v = val * np.sin(np.pi * yy / L)
            img[y, k % W] += v * (1 - fr)
            img[y, (k + 1) % W] += v * fr
    img = (img + np.roll(img, 1, 0) + np.roll(img, -1, 0)) / 3        # (a little soft along the strands)
    img = np.clip(0.84 + img, 0.50, 1.0)
    rgba = np.stack([img, img, img, np.ones_like(img)], -1)
    with open(os.path.join(ASSETS, 'tex', 'open_beard_fibres.tex'), 'wb') as fp:
        fp.write(b'STEX' + struct.pack('<3I', W, H, 0))
        fp.write(np.round(rgba * 255).astype(np.uint8).tobytes())


def shaven():
    """FACE 6's masculine head (av_bhead_m5) has a beard and stubble painted into its skin (rowan_complexion); under the
    facial hair worn they showed around it, wherever its outline wasn't the painted one's. av_bhead_m5s: the same head,
    its skin plain as the other faces' (no texture), drawn while facial hair is worn (avatar.cpp drawList)"""
    d = bytearray(open(os.path.join(ASSETS, 'av_bhead_m5.mdl'), 'rb').read())
    ver, nv, ni, ns, nm = struct.unpack_from('<5I', d, 4)
    start = len(d) - nm * 172
    done = 0
    for i in range(nm):
        o = start + 172 * i
        if bytes(d[o + 44:o + 108]).split(b'\0')[0] == b'rowan_complexion':
            d[o + 44:o + 108] = b'\0' * 64
            struct.pack_into('<f', d, o + 32, 0.05)   # (the other faces' skin's gloss)
            done += 1
    assert done == 1
    open(os.path.join(ASSETS, 'av_bhead_m5s.mdl'), 'wb').write(bytes(d))
    lod = os.path.join(ASSETS, 'av_bhead_m5.lod')
    if os.path.exists(lod):
        open(os.path.join(ASSETS, 'av_bhead_m5s.lod'), 'wb').write(open(lod, 'rb').read())


def main():
    show = '--landmarks' in sys.argv
    if not show:
        fibres()
        shaven()
    for bk in 'mf':
        for face in range(6):
            name = 'av_bhead_%s%s' % (bk, face if face else '')
            if not os.path.exists(os.path.join(ASSETS, name + '.mdl')):
                continue
            V, I, S = load(name)
            L = landmarks(V, I, S)
            print('%-12s eyes %.3f nose %.3f (bottom %.3f) mouth %.3f chin %.3f (front %.3f)' % (
                name, L['eye_y'], L['nose_y'], L['sub_y'], L['mouth_y'], L['chin_y'], L['chin_z']))
            if show:
                continue
            for style in STYLE:
                r = build(bk, face, style, L, V, I, S)
                if r:
                    print('   %-18s %5d verts %5d tris' % r)


if __name__ == '__main__':
    main()
