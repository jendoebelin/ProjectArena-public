"""PORTAL GLOW (0.3.27; the owner, 2026-11-27, watching the floor reflections: "it's a little strange seeing leg holes in
the reflection, it should just show portal glow not hollow legs").

The avatars end above the knee (or at the waist, LOWER BODY NONE), and the legs are open tubes: from above the
openings face away, but in the floor's mirror image they face the viewer, and the reflection fades with depth so the
leg ends are its brightest part. This fills each opening with the energy fade's glow (av_fade: additive, the player's
glow colour, the soft strip texture): a disc across the top of the fade ring where there is one (the shorts made here,
the suits' own shorts, the waist fade), or across the leg's end where there isn't (the community's LONG SHORTS, TENNIS
SKIRT and KILT, whose legs simply end; the fade's material is added to them). Its rim is as bright as the ring's top,
its middle a little less, like a portal the leg comes out of.

Usage: python -I tools/add_leg_portals.py    (writes the models in place; then their .lod: build/tools/make_lods.exe)
Run it again and nothing changes (a disc's vertices are known by their first UV's v, 0.5, which the fade ring's never
have, and their normal straight down). Their second UV is the rim's own: the TENNIS SKIRT's and KILT's swing reads it
as how far a vertex swings and how far it keeps from the body (render.cpp uSwish); a marker there of (7, 7), the first
version's, swung the discs seven times as far and seven metres out, glowing blades across the court (the owner's and
astrobleem's videos, 2026-11-27).
"""
import glob
import os
import struct

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, 'app', 'assets_open')
DT = [('p', '<f4', 3), ('n', '<f4', 3), ('u0', '<f4', 2), ('u1', '<f4', 2), ('c', 'u1', 4), ('bi', 'u1', 4), ('bw', 'u1', 4)]
RIM_U, MID_U = 0.5, 0.6   # (the fade texture: 0.5 its bright top, 1.0 gone)
FADE_FROM = 'av_bottom_1_m'   # (the fade's material, copied into models without one)


def read(name):
    d = open(os.path.join(ASSETS, name + '.mdl'), 'rb').read()
    assert d[:4] == b'SMDL'
    ver, nv, ni, ns, nm = struct.unpack_from('<5I', d, 4)
    assert ver >= 3, name   # (skinned)
    o = 24
    V = np.frombuffer(d, dtype=DT, count=nv, offset=o).copy()
    o += nv * 52
    I = np.frombuffer(d, '<u4', count=ni, offset=o).copy()
    o += ni * 4
    S = [list(struct.unpack_from('<3I', d, o + 12 * k)) for k in range(ns)]
    o += ns * 12
    mats = [d[o + 172 * i:o + 172 * (i + 1)] for i in range(nm)]
    o += 172 * nm
    assert o == len(d), (name, o, len(d))
    return ver, V, I, S, mats


def write(name, ver, V, I, S, mats):
    out = bytearray(b'SMDL')
    out += struct.pack('<5I', ver, len(V), len(I), len(S), len(mats))
    out += V.tobytes() + I.astype('<u4').tobytes()
    for s in S:
        out += struct.pack('<3I', *s)
    for m in mats:
        out += m
    open(os.path.join(ASSETS, name + '.mdl'), 'wb').write(bytes(out))


def diffuse(m):
    return m[44:108].split(b'\0')[0].decode()


def loops(edges, V):
    """Edges (vertex pairs) grouped into connected loops (their ends matched on welded positions): lists of edges."""
    key = {}
    for a, b in edges:
        for v in (a, b):
            key[v] = tuple(np.round(V['p'][v].astype(np.float64) * 1e4).astype(np.int64))
    parent = {}

    def find(a):
        while parent.setdefault(a, a) != a:
            parent[a] = parent[parent[a]]
            a = parent[a]
        return a
    for a, b in edges:
        parent[find(key[a])] = find(key[b])
    groups = {}
    for a, b in edges:
        groups.setdefault(find(key[a]), []).append((a, b))
    return list(groups.values())


def rim_closed(V):
    """Legs already closed at their ends (a flat cap): each side's lowest ring of points, in order round its middle."""
    low = V['p'][:, 1].min()
    pts = np.where(V['p'][:, 1] < low + 0.002)[0]
    edges = []
    for side in (pts[V['p'][pts, 0] < 0], pts[V['p'][pts, 0] >= 0]):
        if len(side) < 8:
            continue
        P = V['p'][side].astype(np.float64)
        c = P.mean(0)
        r = np.hypot(P[:, 0] - c[0], P[:, 2] - c[2])
        ring = side[r > 0.6 * r.max()]   # (the outer edge, not the cap's inner points)
        ang = np.arctan2(V['p'][ring, 2] - c[2], V['p'][ring, 0] - c[0])
        ring = ring[np.argsort(ang)]
        edges += [(int(ring[i]), int(ring[(i + 1) % len(ring)])) for i in range(len(ring))]
    return edges


def rim_from_fade(V, I, S, fm):
    """The fade ring's top edges (both ends at its bright top, u = 0.5)."""
    edges = []
    for f, c, m in S:
        if m != fm:
            continue
        tris = I[f:f + c].reshape(-1, 3)
        top = np.abs(V['u0'][:, 0] - RIM_U) < 1e-3
        for t in tris:
            for a, b in ((t[0], t[1]), (t[1], t[2]), (t[2], t[0])):
                if top[a] and top[b]:
                    edges.append((int(a), int(b)))
    return edges


def rim_from_legs(V, I):
    """The legs' open ends: the mesh's boundary edges (matched on welded positions) within 1 cm of its lowest point."""
    key = np.round(V['p'].astype(np.float64) * 1e4).astype(np.int64)
    _, vid = np.unique(key, axis=0, return_inverse=True)
    vid = vid.reshape(-1)
    tris = I.reshape(-1, 3)
    e = np.concatenate([tris[:, [0, 1]], tris[:, [1, 2]], tris[:, [2, 0]]])
    und = np.sort(vid[e], 1)
    _, inv, cnt = np.unique(und, axis=0, return_inverse=True, return_counts=True)
    bound = e[cnt[inv.reshape(-1)] == 1]
    low = V['p'][:, 1].min()
    sel = (V['p'][bound[:, 0], 1] < low + 0.01) & (V['p'][bound[:, 1], 1] < low + 0.01)
    return [(int(a), int(b)) for a, b in bound[sel]]


def portal(name, fade_mat):
    ver, V, I, S, mats = read(name)
    if ((np.abs(V['u0'][:, 1] - 0.5) < 1e-6) & (V['n'][:, 1] < -0.999)).any():
        return 'already'
    fm = next((i for i, m in enumerate(mats) if 'soft_strip' in diffuse(m)), -1)
    edges = rim_from_fade(V, I, S, fm) if fm >= 0 else rim_from_legs(V, I)
    closed = False
    if not edges and fm < 0:
        edges, closed = rim_closed(V), True
    if fm < 0:
        mats.append(fade_mat)
        fm = len(mats) - 1
    groups = [g for g in loops(edges, V) if len(g) >= 8]
    if not groups:
        return 'no rim'
    newV, newI = [], []
    base = len(V)
    for g in groups:
        vs = sorted({v for e in g for v in e})
        P = V['p'][vs].astype(np.float64)
        centre = P.mean(0)
        # (the disc's vertices: copies of the rim's, facing down, bright; and its middle, a little less)
        idx = {}
        for v in vs:
            r = V[v].copy()
            if closed:   # (just under the skin's own cap)
                r['p'] = (r['p'][0], r['p'][1] - 0.002, r['p'][2])
            r['n'] = (0.0, -1.0, 0.0)
            r['u0'] = (RIM_U, 0.5)   # (u1: the rim's own, as copied)
            idx[v] = base + len(newV)
            newV.append(r)
        mid = V[vs[0]].copy()
        mid['p'] = (centre - np.array([0, 0.002 if closed else 0.0, 0])).astype(np.float32)
        mid['n'] = (0.0, -1.0, 0.0)
        mid['u0'] = (MID_U, 0.5)
        mid['u1'] = V['u1'][vs].mean(0)
        mi = base + len(newV)
        newV.append(mid)
        for a, b in g:
            newI += [mi, idx[b], idx[a]]   # (a fan; the material is two-sided)
    V2 = np.concatenate([V, np.array(newV, dtype=DT)])
    I2 = np.concatenate([I, np.array(newI, dtype='<u4')])
    S.append([len(I), len(newI), fm])
    write(name, ver, V2, I2, S, mats)
    return '%d discs, %d edges' % (len(groups), sum(len(g) for g in groups))


def main():
    _, _, _, _, src = read(FADE_FROM)
    fade_mat = next(m for m in src if 'soft_strip' in diffuse(m))
    names = sorted({os.path.basename(p)[:-4] for p in glob.glob(os.path.join(ASSETS, 'av_bottom_*.mdl')) +
                    glob.glob(os.path.join(ASSETS, 'av_sbottom_*.mdl')) + glob.glob(os.path.join(ASSETS, 'av_wfade_*.mdl'))})
    for n in names:
        print('%-22s %s' % (n, portal(n, fade_mat)))


if __name__ == '__main__':
    main()
