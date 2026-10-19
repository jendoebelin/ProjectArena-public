"""Outfits draped with Blender's cloth simulation on the Project Arena bodies (their bind pose, arms ~47 deg down).

Run: blender -b --factory-startup -P tools/blender/make_cloth.py -- <body_dir> <out_dir> [hoodie]

The garment starts as the body's own surface (so it keeps the body's UV layout: the outfit design masks still fit)
pushed out to a loose fit; its edges are held at a tight fit (ribbed neckline, cuffs and hem), so under gravity the
fabric settles onto the shoulders, folds, bunches above the cuffs and blouses over the hem. The hood is a separate
piece sewn along the back of the neckline: it starts up over the head and falls down the back. Everything is
simulated in app space (+Y up, metres): the scene's gravity points down -Y.

Writes <out_dir>/cloth_<style>_<body>.npz: pos, nrm (per vertex), tris, uv (per corner [T,3,2]), part (per triangle:
0 the garment, 1 the hood, 2 the hood's rolled edge, where its lining shows, 3 the ribbing: the tight bands at the
garment's edges). tools/build_body_parts.py (PA_GARMENT_<n>) weights it to the 7 bones and adds the details.
"""
import json
import math
import os
import sys

import bmesh
import bpy
import numpy as np

args = sys.argv[sys.argv.index('--') + 1:]
BODY_DIR, OUT = args[0], args[1]
STYLE = args[2] if len(args) > 2 else 'hoodie'
os.makedirs(OUT, exist_ok=True)

STYLES = {   # loose fit off the torso / arms, tight fit at the ribbed edges, hem height above the hips (m), cloth stiffness
    'hoodie': dict(torso=0.030, arm=0.018, band=0.010, hem=0.02, bending=4.0, frames=90, hood=True, tris=8500),
}
S = STYLES[STYLE]
TORSO, NECK, HEAD = 0, 1, 2


def clear():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    for m in list(bpy.data.meshes):
        bpy.data.meshes.remove(m)
    for m in list(bpy.data.materials):
        bpy.data.materials.remove(m)


def mesh_object(name, verts, faces, uvs=None):
    me = bpy.data.meshes.new(name)
    me.from_pydata([tuple(v) for v in verts], [], [tuple(f) for f in faces])
    if uvs is not None:   # per corner, in face order
        lay = me.uv_layers.new(name='UVMap')
        lay.data.foreach_set('uv', np.asarray(uvs, np.float32).reshape(-1))
    me.update()
    ob = bpy.data.objects.new(name, me)
    bpy.context.collection.objects.link(ob)
    return ob


def weld(P, T, tol=1e-5):
    """One vertex per position (the body is split at its uv seams)."""
    key = np.round(P / tol).astype(np.int64)
    _, first, inv = np.unique(key, axis=0, return_index=True, return_inverse=True)
    return P[first], inv.reshape(-1)[T], first


def vertex_normals(P, T):
    N = np.zeros_like(P)
    fn = np.cross(P[T[:, 1]] - P[T[:, 0]], P[T[:, 2]] - P[T[:, 0]])
    for c in range(3):
        np.add.at(N, T[:, c], fn)
    return N / np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-12)


def boundary_vertices(T, n):
    E = np.sort(np.concatenate([T[:, [0, 1]], T[:, [1, 2]], T[:, [2, 0]]]), axis=1)
    u, c = np.unique(E, axis=0, return_counts=True)
    b = np.zeros(n, bool)
    b[u[c == 1].reshape(-1)] = True
    return b, u[c == 1]


def subdivide(ob):   # ~1 cm cloth
    sub = ob.modifiers.new('sub', 'SUBSURF')
    sub.subdivision_type = 'SIMPLE'
    sub.levels = sub.render_levels = 1
    bpy.ops.object.select_all(action='DESELECT')
    ob.select_set(True)
    bpy.context.view_layer.objects.active = ob
    bpy.ops.object.modifier_apply(modifier='sub')


def simulate(ob, frames, label, self_collision=True):
    """Drape ob (its 'pin' group held still) for `frames` frames; returns the result as a new plain object (the cloth
    object is removed). Logs how far the cloth spreads, so a blow-up shows at once."""
    cm = ob.modifiers.new('cloth', 'CLOTH')
    st = cm.settings
    st.quality = 10
    st.mass = 0.30
    st.air_damping = 2.0
    st.tension_stiffness = st.compression_stiffness = 25.0
    st.shear_stiffness = 8.0
    st.bending_stiffness = S['bending']
    st.tension_damping = st.compression_damping = 8.0
    st.bending_damping = 1.0
    st.vertex_group_mass = 'pin'
    st.pin_stiffness = 1.0
    cs = cm.collision_settings
    cs.collision_quality = 4
    cs.distance_min = 0.003
    cs.use_self_collision = self_collision
    cs.self_distance_min = 0.002
    cs.self_friction = 5.0
    scn = bpy.context.scene
    scn.use_gravity = True
    scn.gravity = (0.0, -9.81, 0.0)   # app space: +Y up
    cm.point_cache.frame_start = 1
    cm.point_cache.frame_end = frames
    scn.frame_set(1)
    co0 = np.array([v.co[:] for v in ob.data.vertices])
    for f in range(1, frames + 1):
        scn.frame_set(f)
        if f % 15 == 0 or f == frames:
            me = ob.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh()
            co = np.empty(len(me.vertices) * 3)
            me.vertices.foreach_get('co', co)
            ob.evaluated_get(bpy.context.evaluated_depsgraph_get()).to_mesh_clear()
            mv = np.linalg.norm(co.reshape(-1, 3) - co0, axis=1)
            print('  %s frame %d: moved median %.1f cm, max %.1f cm; %d of %d moved > 1 cm, %d > 30 cm' % (
                label, f, np.median(mv) * 100, mv.max() * 100, int((mv > 0.01).sum()), len(mv), int((mv > 0.3).sum())))
    dg = bpy.context.evaluated_depsgraph_get()
    me = ob.evaluated_get(dg).to_mesh()
    res = bpy.data.objects.new(ob.name + '_draped', me.copy())
    for m in ob.data.materials:
        res.data.materials.append(m) if m.name not in res.data.materials else None
    bpy.context.collection.objects.link(res)
    ob.evaluated_get(dg).to_mesh_clear()
    bpy.data.objects.remove(ob, do_unlink=True)
    return res


def make_garment(Pg, Tg, UVg, isarm, isb, J, hem_y):
    """The garment before draping: the body's surface, cut cleanly at the ribbing lines (a level cut above the hem,
    a cut across each forearm above the cuff, the neckline's own plane below the collar). The bands below the cuts
    are the ribbing: held at a tight fit (pinned) in the rim material; the rest stands off loose (more on the torso
    than the arms), easing out over a few centimetres above each band, so it blouses over them."""
    ob = mesh_object('garment', Pg, Tg, UVg)
    bm = bmesh.new()
    bm.from_mesh(ob.data)
    # the bands: (plane point, normal toward the edge, the region it cuts)
    bands = []
    tor = ~isarm
    bands.append((np.array([0.0, hem_y + 0.065, 0.0]), np.array([0.0, -1.0, 0.0]),
                  lambda P: (P[:, 1] < hem_y + 0.12) & torso_like(P)))
    for side in ('l', 'r'):
        W, E = np.array(J['wrist_' + side]), np.array(J['elbow_' + side])
        d = (W - E) / np.linalg.norm(W - E)
        ends = Pg[isb & isarm & (np.linalg.norm(Pg - W, axis=1) < 0.10)]
        Wc = ends.mean(0) if len(ends) else W
        bands.append((Wc - d * 0.05, d, (lambda Wc_: lambda P: np.linalg.norm(P - Wc_, axis=1) < 0.13)(Wc)))
    nk = Pg[isb & ~isarm & (Pg[:, 1] > -0.16) & (np.abs(Pg[:, 0]) < 0.16)]
    Nc = nk.mean(0)
    _, _, Vt = np.linalg.svd(nk - Nc)
    n = Vt[2] if Vt[2][1] > 0 else -Vt[2]          # the neckline's plane, its normal up
    def near_neckline(P):   # within 6 cm of the neckline itself (not the shoulders above the collar's plane)
        out = np.empty(len(P))
        for k in range(0, len(P), 500):
            out[k:k + 500] = np.sqrt(((P[k:k + 500, None] - nk[None]) ** 2).sum(-1)).min(1)
        return out < 0.06
    bands.append((Nc - n * 0.03, n, near_neckline))
    # torso_like: not an arm (nearest original vertex)
    tree_P, tree_arm = Pg, isarm

    def nearest_arm(P):
        out = np.empty(len(P), bool)
        for k in range(0, len(P), 500):
            out[k:k + 500] = tree_arm[np.argmin(((P[k:k + 500, None] - tree_P[None]) ** 2).sum(-1), 1)]
        return out

    def torso_like(P):
        return ~nearest_arm(P)
    for co, no, region in bands:
        F = [f for f in bm.faces]
        C = np.array([f.calc_center_median()[:] for f in F])
        sel = region(C)
        faces = [f for f, k in zip(F, sel) if k]
        geom = list({e for f in faces for e in f.edges}) + list({v for f in faces for v in f.verts}) + faces
        bmesh.ops.bisect_plane(bm, geom=geom, plane_co=tuple(co), plane_no=tuple(no), dist=1e-5)
    bm.verts.ensure_lookup_table()
    bm.normal_update()
    P = np.array([v.co[:] for v in bm.verts])
    N = np.array([v.normal[:] for v in bm.verts])
    va = nearest_arm(P)
    band = np.zeros(len(P), bool)
    ease = np.full(len(P), 1.0)                      # 0 at a band's cut .. 1 three and a half cm above it
    for co, no, region in bands:
        r = region(P)
        dist = (P - co) @ no                         # > 0: toward the edge (in the band)
        band |= r & (dist > -1e-4)
        e = np.clip(-dist / 0.035, 0, 1)
        ease = np.where(r & (dist <= 0), np.minimum(ease, e * e * (3 - 2 * e)), ease)
    loose = np.where(va, S['arm'], S['torso'])
    off = np.where(band, S['band'], S['band'] + (loose - S['band']) * ease)
    for v in bm.verts:
        v.co += v.normal * float(off[v.index])
    ob.data.materials.append(bpy.data.materials.new('garment'))
    ob.data.materials.append(bpy.data.materials.new('rib'))
    for f in bm.faces:
        f.material_index = 1 if all(band[v.index] for v in f.verts) else 0
    bm.to_mesh(ob.data)
    bm.free()
    vg = ob.vertex_groups.new(name='pin')          # the ribbing holds its shape: sewn tight
    vg.add([int(k) for k in np.nonzero(band)[0]], 1.0, 'REPLACE')
    kg = ob.vertex_groups.new(name='keep')         # and keeps its triangles (straight edges)
    kg.add([int(k) for k in np.nonzero(band | (ease < 1.0))[0]], 1.0, 'REPLACE')
    print('  garment: %d bands, %d of %d vertices in the ribbing' % (len(bands), int(band.sum()), len(P)))
    return ob


def build(bkey):
    clear()
    d = np.load(os.path.join(BODY_DIR, 'body_%s.npz' % bkey))
    P0, T0, UV0 = d['pos'].astype(np.float64), d['tris'].astype(np.int64), d['uv'].astype(np.float64)
    bone, wt = d['bone'].astype(np.int64), d['weight'].astype(np.float64)
    J = {k: (np.array(v) if isinstance(v, list) else v) for k, v in json.loads(str(d['joints'])).items()}
    hip_y = (J['hip_l'][1] + J['hip_r'][1]) * 0.5
    hem_y = hip_y + S['hem']
    dom = bone[:, 0]
    arm = dom >= 3
    # the body (collision), welded
    Pb, Tb, _ = weld(P0, T0)
    # below the waist the cloth meets the shorts (a shell ~1 cm off the skin), not the skin: collide with that
    waist_y = hip_y + 0.11
    Nb = vertex_normals(Pb, Tb)
    Pc = Pb + Nb * (0.014 * np.clip((waist_y + 0.03 - Pb[:, 1]) / 0.04, 0, 1))[:, None]
    body = mesh_object('body', Pc, Tb)
    mod = body.modifiers.new('collision', 'COLLISION')
    body.collision.thickness_outer = 0.003
    body.collision.thickness_inner = 0.004
    body.collision.cloth_friction = 6.0
    # the garment region: torso and arms, below the neck seam, above the hem
    c = P0[T0].mean(1)
    sel = (~np.isin(dom[T0], [NECK, HEAD])).all(1) & (c[:, 1] < -0.035) & (c[:, 1] > hem_y)
    Tg0 = T0[sel]
    Pg, Tg, first = weld(P0, Tg0)
    used = np.unique(Tg)
    remap = -np.ones(len(Pg), np.int64)
    remap[used] = np.arange(len(used))
    src = first[used]          # a body vertex at each garment vertex
    Pg, Tg = Pg[used], remap[Tg]
    UVg = UV0[Tg0.reshape(-1)].reshape(-1, 3, 2)
    Ng = vertex_normals(Pg, Tg)
    isb, bedges = boundary_vertices(Tg, len(Pg))
    # the region is cut out by whole triangles, so its edges are staircases: smooth each edge into a clean curve
    # (along the edge only; the hem also flat at its height) before anything is built on it
    nb = np.zeros(len(Pg))
    acc = np.zeros_like(Pg)
    for _ in range(12):
        acc[:] = 0
        nb[:] = 0
        np.add.at(acc, bedges[:, 0], Pg[bedges[:, 1]])
        np.add.at(acc, bedges[:, 1], Pg[bedges[:, 0]])
        np.add.at(nb, bedges.reshape(-1), 1)
        m = nb > 0
        Pg[m] = 0.5 * Pg[m] + 0.5 * acc[m] / nb[m, None]
    hem = isb & (Pg[:, 1] < hem_y + 0.04) & ~arm[src]
    Pg[hem, 1] = hem_y + 0.005
    garment = make_garment(Pg, Tg, UVg, arm[src], isb, J, hem_y)
    Pg = Pg + Ng * (S['band'] + 0.002)   # (the hood is sewn on the neckline's band)
    subdivide(garment)
    # 1: the garment on the body
    draped = simulate(garment, S['frames'], 'garment ' + bkey)
    if S.get('hood'):   # 2: the hood, falling onto the draped garment (a collider now) and the body
        col = draped.modifiers.new('collision', 'COLLISION')
        draped.collision.thickness_outer = 0.005
        draped.collision.cloth_friction = 6.0
        hood = make_hood(Pg, bedges, isb, J, draped)
        subdivide(hood)
        hv = np.array([v.co[:] for v in hood.data.vertices])
        dmin = np.sqrt(((hv[:, None] - Pb[None, ::3]) ** 2).sum(-1)).min(1)
        pw = np.zeros(len(hv))
        gi = hood.vertex_groups['pin'].index
        for v in hood.data.vertices:
            for g in v.groups:
                if g.group == gi:
                    pw[v.index] = g.weight
        print('  hood: %d verts, pinned (weight > 0.5) %d, closer than 1 cm to the body %d, y %.2f..%.2f' % (
            len(hv), int((pw > 0.5).sum()), int((dmin < 0.01).sum()), hv[:, 1].min(), hv[:, 1].max()))
        dhood = simulate(hood, 30, 'hood ' + bkey)   # (settling onto the back)
        draped.modifiers.remove(draped.modifiers['collision'])
        bpy.ops.object.select_all(action='DESELECT')
        draped.select_set(True)
        dhood.select_set(True)
        bpy.context.view_layer.objects.active = draped
        bpy.ops.object.join()
    res = draped
    # reduced to the triangle budget (the folds stay; flat areas lose triangles)
    n_tri = sum(len(p.vertices) - 2 for p in res.data.polygons)
    dec = res.modifiers.new('dec', 'DECIMATE')
    dec.ratio = min(1.0, S['tris'] / max(1, n_tri))
    # (evenly: protecting the bands pushed the reduction onto the rest of the cloth, which broke into shards)
    dg = bpy.context.evaluated_depsgraph_get()
    rev = res.evaluated_get(dg)
    me = rev.to_mesh()
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.triangulate(bm, faces=bm.faces[:])
    uvl = bm.loops.layers.uv.active
    P = np.array([v.co[:] for v in bm.verts])
    T = np.array([[v.index for v in f.verts] for f in bm.faces])
    UV = np.array([[l[uvl].uv[:] for l in f.loops] for f in bm.faces])
    PART = {'garment': 0, 'hood': 1, 'hood_lining': 2, 'rib': 3}
    names = [m.name.split('.')[0] for m in res.data.materials]
    part = np.array([PART.get(names[f.material_index], 0) if f.material_index < len(names) else 0 for f in bm.faces], np.uint8)
    bm.free()
    rev.to_mesh_clear()
    N = vertex_normals(P, T)
    out = os.path.join(OUT, 'cloth_%s_%s.npz' % (STYLE, bkey))
    np.savez_compressed(out, pos=P.astype(np.float32), nrm=N.astype(np.float32), tris=T.astype(np.int32),
                        uv=UV.astype(np.float32), part=part)
    print('CLOTH', STYLE, bkey, 'verts', len(P), 'tris', len(T), 'hood tris', int((part == 1).sum()), '->', out)


def make_hood(Pg, bedges, isb, J, draped):
    """The hood, down: a pouch sewn along the back of the neckline (its bottom row pinned there), the opening's
    folded edge rolled over at the top, hanging down the upper back over the draped garment - deepest at the spine,
    short where it reaches the shoulders, fuller in the middle. Material slot 1 marks it."""
    from mathutils import Vector
    from mathutils.bvhtree import BVHTree
    tree = BVHTree.FromObject(draped, bpy.context.evaluated_depsgraph_get())
    hc = np.array(J['head']) + np.array([0, 0.06, 0.02])
    # the neckline: boundary vertices near the neck (not the cuffs or the hem), the back half
    nl = np.nonzero(isb & (Pg[:, 1] > -0.10) & (np.abs(Pg[:, 0]) < 0.12))[0]
    ang = np.arctan2(Pg[nl, 0], Pg[nl, 2])            # 0 = straight back (+Z), +-pi = the front
    back = np.abs(ang) < math.radians(105)
    nl, ang = nl[back], ang[back]
    order = np.argsort(ang)
    nl, ang = nl[order], ang[order]
    cols = 26
    targets = np.linspace(ang[0], ang[-1], cols)
    print('  hood: %d neckline points across the back' % len(nl))
    base = np.stack([np.interp(targets, ang, Pg[nl, k]) for k in range(3)], -1)   # spaced along the neckline
    rng = np.random.default_rng(3)
    full = 1.0 + 0.25 * np.sin(np.arange(cols) * 1.7 + rng.random(cols))         # a little uneven: folds
    axis_z = -0.02                                      # the torso's middle (rays out to the garment from here)
    verts, faces = [], []
    rows_rim, rows = 3, 18
    for ci in range(cols):
        b0 = base[ci]
        phi = targets[ci]
        side = min(1.0, abs(phi) / math.radians(105))   # 0 at the spine .. 1 at the shoulders
        out = np.array([math.sin(phi), 0.0, math.cos(phi)])
        L = 0.05 + 0.20 * math.cos(side * math.pi / 2) ** 1.5
        col = [b0, b0 + out * 0.012 + np.array([0, 0.018, 0]), b0 + out * 0.030 + np.array([0, 0.006, 0])]   # the rolled edge
        for ri in range(rows - rows_rim):
            sv = (ri + 1) / (rows - rows_rim)
            y = b0[1] - 0.01 - L * sv
            o = np.array([0.0, y, axis_z])
            hit, _, _, _ = tree.ray_cast(Vector(o), Vector(out), 0.5)
            surf = np.array(hit) if hit is not None else o + out * 0.15
            bulge = (0.014 + 0.034 * math.sin(math.pi * min(1.0, sv * 1.15)) * full[ci]) * (1 - 0.7 * side * side) + 0.008
            col.append(surf + out * bulge)
        verts += col
    for ci in range(cols - 1):
        for ri in range(rows - 1):
            a = ci * rows + ri
            faces.append((a, a + rows, a + rows + 1, a + 1))
    uvs = []
    for f in faces:
        for v in f:
            uvs.append((0.02 + 0.06 * (v // rows) / cols, 0.02 + 0.06 * (v % rows) / rows))   # a corner of the layout
    hood = mesh_object('hood', np.array(verts), faces, uvs)
    vg = hood.vertex_groups.new(name='pin')
    vg.add([ci * rows + k for ci in range(cols) for k in (0, 1)], 1.0, 'REPLACE')   # sewn on, the rolled edge held
    kg = hood.vertex_groups.new(name='keep')
    kg.add([ci * rows + k for ci in range(cols) for k in range(rows_rim + 1)], 1.0, 'REPLACE')
    hood.data.materials.append(bpy.data.materials['garment'])
    hood.data.materials.append(bpy.data.materials.new('hood'))
    hood.data.materials.append(bpy.data.materials.new('hood_lining'))
    for p in hood.data.polygons:   # slot 2: the rolled edge, where the hood's inside (its lining) shows
        p.material_index = 2 if (p.vertices[0] % rows) < rows_rim - 1 else 1
    return hood


if os.environ.get('PA_CLOTH_FRAMES'):   # (quick stability checks)
    S['frames'] = int(os.environ['PA_CLOTH_FRAMES'])
for bkey in os.environ.get('PA_CLOTH_BODIES', 'masc fem').split():
    build(bkey)
