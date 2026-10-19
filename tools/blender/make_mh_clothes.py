"""MakeHuman clothes fitted to the Project Arena bodies (MPFB2), as starting points for outfits.

Run: blender -b -P tools/blender/make_mh_clothes.py -- <out_dir> <item.mhclo> [<item.mhclo> ...]
(needs the MPFB extension, like make_body.py; the clothes come from the MakeHuman System Assets pack, CC0)

For each body (masc, fem) and item writes <out_dir>/mhclo_<item>_<body>.npz: pos, nrm (app space, origin = the neck
base, exactly like body_<name>.npz), uv, tris, bone / weight (top 4 of the game's 7 bones, MPFB's weights copied from
the body). tools/build_body_parts.py turns them into outfits.
"""
import os
import sys

import bmesh
import bpy
import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_body as mb  # noqa: E402  (the bodies, the app-space transform, the bone mapping)
from bl_ext.user_default.mpfb.services.humanservice import HumanService  # noqa: E402
from bl_ext.user_default.mpfb.services.targetservice import TargetService  # noqa: E402

args = sys.argv[sys.argv.index('--') + 1:]
OUT, ITEMS = args[0], args[1:]
os.makedirs(OUT, exist_ok=True)


def rest_coords(obj):
    for m in list(obj.modifiers):   # rest shape only
        obj.modifiers.remove(m)
    return mb.evaluated_coords(obj)


for name, vals in mb.BODIES.items():
    for item in ITEMS:
        mb.clear()
        macro = TargetService.get_default_macro_info_dict()
        macro.update(vals)
        h = HumanService.create_human(macro_detail_dict=macro)
        rig = HumanService.add_builtin_rig(h, 'game_engine')
        cl = HumanService.add_mhclo_asset(item, h, subdiv_levels=0, set_up_rigging=True, interpolate_weights=True)
        co_h = rest_coords(h)
        neck = [g.index for g in h.vertex_groups if g.name == 'joint-neck'][0]
        origin = co_h[[v.index for v in h.data.vertices if any(g.group == neck for g in v.groups)]].mean(0)
        co = rest_coords(cl)
        me = cl.data
        groups = {g.index: g.name for g in cl.vertex_groups}
        bw = np.zeros((len(me.vertices), 7))
        for v in me.vertices:
            for g in v.groups:
                gname = groups[g.group]
                if g.weight > 0 and gname in rig.data.bones:
                    bw[v.index, mb.game_bone(gname)] += g.weight
        bm = bmesh.new()
        bm.from_mesh(me)
        uvl = bm.loops.layers.uv.active
        tris, tri_uv = [], []
        for f in bm.faces:
            idx = [v.index for v in f.verts]
            uvs = [tuple(l[uvl].uv) if uvl else (0.0, 0.0) for l in f.loops]
            for k in range(1, len(idx) - 1):
                tris.append((idx[0], idx[k], idx[k + 1]))
                tri_uv.append((uvs[0], uvs[k], uvs[k + 1]))
        bm.free()
        tris = np.array(tris)
        N = np.zeros_like(co)
        fn = np.cross(co[tris[:, 1]] - co[tris[:, 0]], co[tris[:, 2]] - co[tris[:, 0]])
        for c in range(3):
            np.add.at(N, tris[:, c], fn)
        N /= np.maximum(np.linalg.norm(N, axis=1, keepdims=True), 1e-12)
        bw[bw.sum(1) < 1e-6, 0] = 1.0
        order = np.argsort(-bw, axis=1)[:, :4]
        top = np.take_along_axis(bw, order, 1)
        top /= top.sum(1, keepdims=True)
        key = os.path.splitext(os.path.basename(item))[0]
        out = os.path.join(OUT, 'mhclo_%s_%s.npz' % (key, name))
        np.savez_compressed(out, pos=mb.to_app(co - origin).astype(np.float32), nrm=mb.to_app(N).astype(np.float32),
                            tris=tris.astype(np.int32), uv=np.array(tri_uv, np.float32), bone=order.astype(np.uint8),
                            weight=top.astype(np.float32))
        print('MHCLO', key, name, 'verts', len(co), 'tris', len(tris), '->', out)
