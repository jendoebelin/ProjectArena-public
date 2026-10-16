"""Project Arena base bodies from MPFB2 (MakeHuman for Blender; the generated meshes are CC0).

Run: blender -b -P tools/blender/make_body.py -- <out_dir>
(needs the MPFB extension installed in the user's Blender; not --factory-startup)

For each body (masc, fem) writes <out_dir>/body_<name>.npz:
  pos, nrm (float32 [V,3], app space: +X right, +Y up, -Z forward, metres, origin = the neck base),
  uv (float32 [V,2]), tris (int32 [T,3]), bone (uint8 [V,4]), weight (float32 [V,4]) over the game's 7 bones
  (0 torso, 1 neck, 2 head, 3 upper L, 4 upper R, 5 fore L, 6 fore R), region (uint8 [V]: 0 skin, 1 lips,
  2 scalp, 3 ears, 4 nails), joints (json: eye midpoint, neck base / top, shoulders, elbows, wrists, hips, eyes),
  and <out_dir>/body_<name>_ao.png: ambient occlusion baked into the MPFB UV layout (Cycles).
The hands (inside the gauntlet bracers), feet, legs below mid-thigh and all helper geometry are removed.
"""
import json
import os
import sys

import bmesh
import bpy
import numpy as np

from bl_ext.user_default.mpfb.services.humanservice import HumanService
from bl_ext.user_default.mpfb.services.targetservice import TargetService

OUT = sys.argv[sys.argv.index('--') + 1] if '--' in sys.argv else os.getcwd()
os.makedirs(OUT, exist_ok=True)

BODIES = {
    # athletic builds close to the owner's reference sheets (bald base; hair is a separate part)
    'masc': dict(gender=1.0, age=0.5, muscle=0.96, weight=0.48, proportions=0.80, height=0.58, cupsize=0.5, firmness=0.5),
    'fem': dict(gender=0.0, age=0.5, muscle=0.80, weight=0.46, proportions=0.80, height=0.50, cupsize=0.45, firmness=0.75),   # rounded and lifted (low firmness hangs the bust low on the torso)
}
GAME_BONE = {'neck_01': 1, 'head': 2, 'upperarm_l': 3, 'upperarm_r': 4, 'lowerarm_l': 5, 'lowerarm_r': 6}


def game_bone(name):
    if name in GAME_BONE:
        return GAME_BONE[name]
    for side, k in (('_l', 5), ('_r', 6)):
        if name.endswith(side) and (name.startswith(('hand', 'thumb', 'index', 'middle', 'ring', 'pinky')) or 'lowerarm' in name):
            return k
    for side, k in (('_l', 3), ('_r', 4)):
        if name.endswith(side) and 'upperarm' in name:
            return k
    return 0   # pelvis, spine, clavicles, legs: the torso


def to_app(P):
    """Blender (x right-of-viewer = character's left, -y forward, z up) -> app (+x character right, +y up, -z forward)."""
    P = np.asarray(P, float)
    return np.stack([-P[..., 0], P[..., 2], P[..., 1]], -1)


def clear():
    for o in list(bpy.data.objects):
        bpy.data.objects.remove(o, do_unlink=True)
    for m in list(bpy.data.meshes):
        bpy.data.meshes.remove(m)


def evaluated_coords(obj):
    dg = bpy.context.evaluated_depsgraph_get()
    ev = obj.evaluated_get(dg)
    me = ev.to_mesh()
    co = np.empty(len(me.vertices) * 3)
    me.vertices.foreach_get('co', co)
    ev.to_mesh_clear()
    return co.reshape(-1, 3)


def vertex_ao(P, N, T, rays=40, reach=0.22):
    """Ambient occlusion per vertex: cosine-weighted hemisphere rays against the body itself (creases under the
    chest, armpits, between the arm and torso, the ears and eye sockets), lightly smoothed over the mesh."""
    from mathutils import Vector
    from mathutils.bvhtree import BVHTree
    tree = BVHTree.FromPolygons([tuple(v) for v in P], [tuple(t) for t in T.tolist()])
    rng = np.random.default_rng(7)
    u1, u2 = rng.random(rays), rng.random(rays)
    local = np.stack([np.sqrt(u1) * np.cos(2 * np.pi * u2), np.sqrt(u1) * np.sin(2 * np.pi * u2), np.sqrt(1 - u1)], -1)
    ao = np.ones(len(P))
    for i in range(len(P)):
        n = N[i]
        a = np.array([1.0, 0, 0]) if abs(n[0]) < 0.9 else np.array([0, 1.0, 0])
        t = np.cross(n, a); t /= np.linalg.norm(t)
        b = np.cross(n, t)
        dirs = local[:, :1] * t + local[:, 1:2] * b + local[:, 2:] * n
        o = Vector(P[i] + n * 0.0015)
        hit = 0
        for d in dirs:
            loc, _, _, dist = tree.ray_cast(o, Vector(d), reach)
            if loc is not None:
                hit += 1.0 - 0.6 * dist / reach
        ao[i] = 1.0 - hit / rays
    # smooth twice over the triangle neighbourhood
    for _ in range(2):
        acc = np.zeros(len(P)); cnt = np.zeros(len(P))
        for c in range(3):
            for e in range(3):
                np.add.at(acc, T[:, c], ao[T[:, e]]); np.add.at(cnt, T[:, c], 1)
        ao = acc / np.maximum(cnt, 1)
    return np.clip(ao, 0.25, 1.0)


def bake_ao(obj, path, size=1024):
    """Ambient occlusion of the whole body into its UV layout (creases under the chest, armpits, ears, eyes)."""
    scn = bpy.context.scene
    scn.render.engine = 'CYCLES'
    scn.cycles.samples = 96
    scn.cycles.device = 'CPU'
    img = bpy.data.images.new('ao', size, size, alpha=False, float_buffer=False)
    mat = bpy.data.materials.new('bake')
    mat.use_nodes = True
    nt = mat.node_tree
    tex = nt.nodes.new('ShaderNodeTexImage')
    tex.image = img
    nt.nodes.active = tex
    obj.data.materials.clear()
    obj.data.materials.append(mat)
    for p in obj.data.polygons:
        p.material_index = 0
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.bake(type='AO', margin=8, use_clear=True)
    img.filepath_raw = path
    img.file_format = 'PNG'
    img.save()


def build(name, macro_vals):
    clear()
    macro = TargetService.get_default_macro_info_dict()
    for k, v in macro_vals.items():
        macro[k] = v
    h = HumanService.create_human(macro_detail_dict=macro)
    rig = HumanService.add_builtin_rig(h, 'game_engine')
    for m in list(h.modifiers):   # rest shape only: masks and the armature off
        h.modifiers.remove(m)
    co = evaluated_coords(h)                          # all vertices, shape keys applied (incl. helper geometry)
    me = h.data
    nv = len(me.vertices)
    groups = {g.index: g.name for g in h.vertex_groups}
    member = {}
    bw = np.zeros((nv, 7))
    for v in me.vertices:
        for g in v.groups:
            gname = groups[g.group]
            if gname.startswith(('joint-', 'helper-')) or gname in ('body', 'lips', 'scalp', 'ears', 'fingernails', 'toenails', 'nipple', 'nippleTip'):
                member.setdefault(gname, []).append(v.index)
            elif g.weight > 0 and gname in rig.data.bones:
                bw[v.index, game_bone(gname)] += g.weight

    def centroid(gname):
        return co[member[gname]].mean(0)
    J = {k: centroid(g) for k, g in {
        'eye_l': 'joint-l-eye', 'eye_r': 'joint-r-eye', 'neck': 'joint-neck', 'head': 'joint-head',
        'shoulder_l': 'joint-l-shoulder', 'shoulder_r': 'joint-r-shoulder', 'elbow_l': 'joint-l-elbow', 'elbow_r': 'joint-r-elbow',
        'wrist_l': 'joint-l-hand', 'wrist_r': 'joint-r-hand', 'hip_l': 'joint-l-upper-leg', 'hip_r': 'joint-r-upper-leg',
        'knee_l': 'joint-l-knee', 'knee_r': 'joint-r-knee', 'pelvis': 'joint-pelvis', 'thumb_l': 'joint-l-finger-1-2',
        'thumb_r': 'joint-r-finger-1-2'}.items()}
    # eyeball radius from the helper eye geometry
    eye_r = float(np.linalg.norm(co[member['helper-l-eye']] - J['eye_l'], axis=1).mean())

    # keep: body faces only, minus the hands (past the wrist), the feet and legs below mid-thigh
    body = np.zeros(nv, bool)
    body[member['body']] = True
    knee_z = (J['knee_l'][2] + J['knee_r'][2]) * 0.5
    hip_z = (J['hip_l'][2] + J['hip_r'][2]) * 0.5
    cut_z = knee_z + 0.30 * (hip_z - knee_z)   # thighs to a little above the knee
    keep = body & (co[:, 2] > cut_z - 0.15)     # generous: build_body_parts.py flattens the cut exactly
    for s in ('l', 'r'):
        e, w = J['elbow_' + s], J['wrist_' + s]
        d = (w - e) / np.linalg.norm(w - e)
        t = (co - e) @ d / np.linalg.norm(w - e)
        radial = np.linalg.norm((co - e) - np.outer((co - e) @ d, d), axis=1)
        near = (np.linalg.norm(co - w, axis=1) < 0.32) & (radial < 0.13)   # the hand itself, not the thigh beyond it
        hand = (t > 1.04) & near & ((co[:, 0] > 0) == (J['wrist_' + s][0] > 0))
        keep &= ~hand
    # triangulate the kept faces
    bm = bmesh.new()
    bm.from_mesh(me)
    uvl = bm.loops.layers.uv.active
    tris, tri_uv = [], []
    for f in bm.faces:
        idx = [v.index for v in f.verts]
        if not all(keep[i] for i in idx):
            continue
        uvs = [tuple(l[uvl].uv) for l in f.loops]
        for k in range(1, len(idx) - 1):
            tris.append((idx[0], idx[k], idx[k + 1]))
            tri_uv.append((uvs[0], uvs[k], uvs[k + 1]))
    bm.free()
    tris = np.array(tris)
    tri_uv = np.array(tri_uv)
    # split by uv: one output vertex per (vertex, uv) pair
    key = {}
    out_v, out_uv = [], []
    T = np.zeros_like(tris)
    for ti in range(len(tris)):
        for c in range(3):
            vi = int(tris[ti, c])
            u = (round(float(tri_uv[ti, c, 0]), 5), round(float(tri_uv[ti, c, 1]), 5))
            kk = (vi, u)
            if kk not in key:
                key[kk] = len(out_v)
                out_v.append(vi)
                out_uv.append(u)
            T[ti, c] = key[kk]
    out_v = np.array(out_v)
    P = co[out_v]
    # smooth normals over the original vertices (uv seams share a normal)
    N = np.zeros_like(co)
    fn = np.cross(co[tris[:, 1]] - co[tris[:, 0]], co[tris[:, 2]] - co[tris[:, 0]])
    for c in range(3):
        np.add.at(N, tris[:, c], fn)
    N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-12)
    # weights: top 4 of the 7 game bones, normalised
    W = bw[out_v]
    W[W.sum(1) < 1e-6, 0] = 1.0
    order = np.argsort(-W, axis=1)[:, :4]
    top = np.take_along_axis(W, order, 1)
    top /= top.sum(1, keepdims=True)
    region = np.zeros(len(out_v), np.uint8)
    for rid, gname in ((1, 'lips'), (2, 'scalp'), (3, 'ears'), (4, 'fingernails'), (5, 'nipple'), (5, 'nippleTip')):
        sel = np.zeros(nv, bool)
        sel[member.get(gname, [])] = True
        region[sel[out_v]] = rid
    ao = vertex_ao(P, N[out_v], T)
    origin = J['neck']
    Papp = to_app(P - origin)
    Napp = to_app(N[out_v])
    joints = {k: to_app(v - origin).tolist() for k, v in J.items()}
    joints['eye_mid'] = to_app((J['eye_l'] + J['eye_r']) * 0.5 - origin).tolist()
    joints['eye_radius'] = eye_r
    joints['thigh_cut'] = float(cut_z - origin[2])
    np.savez_compressed(os.path.join(OUT, 'body_%s.npz' % name), pos=Papp.astype(np.float32), nrm=Napp.astype(np.float32),
                        uv=np.array(out_uv, np.float32), tris=T.astype(np.int32), bone=order.astype(np.uint8),
                        weight=top.astype(np.float32), region=region, ao=ao.astype(np.float32), joints=json.dumps(joints))
    print('BODY', name, 'verts', len(out_v), 'tris', len(T), 'eye_r %.4f' % eye_r, 'neck->eyes', joints['eye_mid'])
    print('AO', name, 'mean %.3f' % ao.mean())


for name, vals in BODIES.items():
    build(name, vals)
