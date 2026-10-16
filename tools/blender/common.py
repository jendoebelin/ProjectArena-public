"""Shared helpers for the Project Arena Blender generators (run inside Blender: blender -b -P <script> -- <out.npz>).

Coordinates: Blender X = court length, Y = across, Z = up, metres. Exports convert to the app's Y-up space
(app x = X, app y = Z, app z = -Y).

Geometry is routed by material kind (tools/blender/palette.py) into separate objects:
  lit   -> lightmap UVs (smart project) + Cycles DIFFUSE bake (direct + indirect + colour) into one image
  emit  -> emission materials: the light sources of the bake
  glass -> transparent in the bake
  fx / prop -> not rendered in the bake
"""
import math
import os
import sys

import bmesh
import bpy
import numpy as np
from mathutils import Matrix, Vector

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import palette  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))
TEXDIR = os.path.join(ROOT, 'build', 'open_src', 'tex')


def out_path():
    argv = sys.argv
    return argv[argv.index('--') + 1] if '--' in argv else None


def reset_scene():
    bpy.ops.wm.read_factory_settings(use_empty=True)


def kind_of(name):
    return palette.P[name]['kind'] if name in palette.P else 'prop'


ROT = {'Z': Matrix(), 'X': Matrix.Rotation(math.pi / 2, 4, 'Y'), 'Y': Matrix.Rotation(math.pi / 2, 4, 'X')}


class Builder:
    """Accumulates geometry into one bmesh per material kind, with per-face material slots and a UV map."""

    def __init__(self, name):
        self.name = name
        self.bms = {}
        self.mats = {}

    def _bm(self, kind):
        if kind not in self.bms:
            bm = bmesh.new()
            bm.loops.layers.uv.new('UVMap')
            self.bms[kind] = bm
            self.mats[kind] = []
        return self.bms[kind]

    # ------------------------------------------------------------ primitives (geometry built in a temp bmesh)
    def box(self, mat, center, size, bevel=0.0, segments=2, rot=None, subdiv=0):
        tmp = bmesh.new()
        bmesh.ops.create_cube(tmp, size=1.0)
        bmesh.ops.scale(tmp, vec=Vector(size), verts=tmp.verts)
        if subdiv:
            bmesh.ops.subdivide_edges(tmp, edges=tmp.edges[:], cuts=subdiv, use_grid_fill=True)
        if bevel > 0:
            bmesh.ops.bevel(tmp, geom=tmp.edges[:] + tmp.verts[:], offset=bevel, segments=segments, profile=0.5,
                            affect='EDGES', clamp_overlap=True)
        m = Matrix.Translation(Vector(center))
        if rot is not None:
            m = m @ rot.to_4x4()
        bmesh.ops.transform(tmp, matrix=m, verts=tmp.verts)
        self._merge(tmp, mat)

    def quad(self, mat, corners, uv=((0, 0), (1, 0), (1, 1), (0, 1)), nu=1, nv=1):
        """Bilinear quad patch from 4 corners (counter-clockwise seen from the front), with UVs."""
        tmp = bmesh.new()
        lay = tmp.loops.layers.uv.new('UVMap')
        c = [Vector(p) for p in corners]
        u0 = [Vector(t) for t in uv]

        def P(s, t):
            return c[0] * (1 - s) * (1 - t) + c[1] * s * (1 - t) + c[2] * s * t + c[3] * (1 - s) * t

        def U(s, t):
            return u0[0] * (1 - s) * (1 - t) + u0[1] * s * (1 - t) + u0[2] * s * t + u0[3] * (1 - s) * t

        vs = [[tmp.verts.new(P(i / nu, j / nv)) for j in range(nv + 1)] for i in range(nu + 1)]
        for i in range(nu):
            for j in range(nv):
                f = tmp.faces.new((vs[i][j], vs[i + 1][j], vs[i + 1][j + 1], vs[i][j + 1]))
                for loop, (a, b) in zip(f.loops, ((i, j), (i + 1, j), (i + 1, j + 1), (i, j + 1))):
                    loop[lay].uv = U(a / nu, b / nv)
        self._merge(tmp, mat)

    def card(self, mat, center, u_axis, v_axis, su, sv, uv=((0, 0), (1, 0), (1, 1), (0, 1)), double=False, nu=1, nv=1):
        """Rectangle centred on `center` spanning su along u_axis and sv along v_axis (front = u x v)."""
        c, U, V = Vector(center), Vector(u_axis).normalized() * su / 2, Vector(v_axis).normalized() * sv / 2
        self.quad(mat, (c - U - V, c + U - V, c + U + V, c - U + V), uv, nu, nv)
        if double:
            self.quad(mat, (c + U - V, c - U - V, c - U + V, c + U + V), (uv[1], uv[0], uv[3], uv[2]), nu, nv)

    def cylinder(self, mat, center, radius, depth, segs=24, axis='Z', radius2=None, cap=True, smooth=True):
        tmp = bmesh.new()
        bmesh.ops.create_cone(tmp, cap_ends=cap, segments=segs, radius1=radius,
                              radius2=radius if radius2 is None else radius2, depth=depth)
        for f in tmp.faces:
            f.smooth = smooth and len(f.verts) == 4
        bmesh.ops.transform(tmp, matrix=Matrix.Translation(Vector(center)) @ ROT[axis], verts=tmp.verts)
        self._merge(tmp, mat, keep_smooth=True)

    def ring(self, mat, center, r_in, r_out, depth, segs=64, axis='Z'):
        """Flat washer. Top face: uv.u = angle 0..1, uv.v = 0 inner .. 1 outer."""
        tmp = bmesh.new()
        lay = tmp.loops.layers.uv.new('UVMap')
        bi, bo, ti, to = [], [], [], []
        for k in range(segs + 1):
            a = 2 * math.pi * k / segs
            cs, sn = math.cos(a), math.sin(a)
            bi.append(tmp.verts.new((r_in * cs, r_in * sn, -depth / 2)))
            bo.append(tmp.verts.new((r_out * cs, r_out * sn, -depth / 2)))
            ti.append(tmp.verts.new((r_in * cs, r_in * sn, depth / 2)))
            to.append(tmp.verts.new((r_out * cs, r_out * sn, depth / 2)))
        for k in range(segs):
            n = k + 1
            f = tmp.faces.new((ti[k], to[k], to[n], ti[n]))
            for loop, uv in zip(f.loops, ((k / segs, 0), (k / segs, 1), (n / segs, 1), (n / segs, 0))):
                loop[lay].uv = uv
            if depth > 0:
                tmp.faces.new((bi[n], bo[n], bo[k], bi[k]))
                tmp.faces.new((bo[k], bo[n], to[n], to[k]))
                tmp.faces.new((bi[n], bi[k], ti[k], ti[n]))
        bmesh.ops.remove_doubles(tmp, verts=tmp.verts[:], dist=1e-7)
        bmesh.ops.transform(tmp, matrix=Matrix.Translation(Vector(center)) @ ROT[axis], verts=tmp.verts)
        self._merge(tmp, mat)

    def disc(self, mat, center, radius, segs=64, axis='Z', flip=False):
        """Flat disc with planar UVs 0..1 (for emblem / ring textures). Faces +axis unless flip."""
        tmp = bmesh.new()
        lay = tmp.loops.layers.uv.new('UVMap')
        cv = tmp.verts.new((0, 0, 0))
        rim = [tmp.verts.new((radius * math.cos(2 * math.pi * k / segs), radius * math.sin(2 * math.pi * k / segs), 0))
               for k in range(segs)]
        for k in range(segs):
            tri = (cv, rim[k], rim[(k + 1) % segs])
            f = tmp.faces.new(tri[::-1] if flip else tri)
            for loop in f.loops:
                co = loop.vert.co
                loop[lay].uv = (0.5 + co.x / (2 * radius), 0.5 + co.y / (2 * radius))
        bmesh.ops.transform(tmp, matrix=Matrix.Translation(Vector(center)) @ ROT[axis], verts=tmp.verts)
        self._merge(tmp, mat)

    def torus(self, mat, center, R, r, segs=48, tsegs=12, axis='Z', smooth=True):
        tmp = bmesh.new()
        rings = []
        for i in range(segs):
            a = 2 * math.pi * i / segs
            row = []
            for j in range(tsegs):
                b = 2 * math.pi * j / tsegs
                rr = R + r * math.cos(b)
                row.append(tmp.verts.new((rr * math.cos(a), rr * math.sin(a), r * math.sin(b))))
            rings.append(row)
        for i in range(segs):
            for j in range(tsegs):
                f = tmp.faces.new((rings[i][j], rings[(i + 1) % segs][j], rings[(i + 1) % segs][(j + 1) % tsegs],
                                   rings[i][(j + 1) % tsegs]))
                f.smooth = smooth
        bmesh.ops.transform(tmp, matrix=Matrix.Translation(Vector(center)) @ ROT[axis], verts=tmp.verts)
        self._merge(tmp, mat, keep_smooth=True)

    def sphere(self, mat, center, radius, segs=32, rings=16, scale=(1, 1, 1), smooth=True, uv_scale=(1, 1)):
        tmp = bmesh.new()
        lay = tmp.loops.layers.uv.new('UVMap')   # create_uvsphere only writes UVs into an existing layer
        bmesh.ops.create_uvsphere(tmp, u_segments=segs, v_segments=rings, radius=radius, calc_uvs=True)
        if tuple(uv_scale) != (1, 1):
            for f in tmp.faces:
                for lp in f.loops:
                    lp[lay].uv = (lp[lay].uv[0] * uv_scale[0], lp[lay].uv[1] * uv_scale[1])
        bmesh.ops.scale(tmp, vec=Vector(scale), verts=tmp.verts)
        bmesh.ops.translate(tmp, vec=Vector(center), verts=tmp.verts)
        for f in tmp.faces:
            f.smooth = smooth
        self._merge(tmp, mat, keep_smooth=True)

    def add_bmesh(self, tmp, mat, smooth=False):
        for f in tmp.faces:
            f.smooth = smooth
        self._merge(tmp, mat, keep_smooth=True)

    def _merge(self, tmp, mat, keep_smooth=False):
        kind = kind_of(mat)
        bm = self._bm(kind)
        mats = self.mats[kind]
        if mat not in mats:
            mats.append(mat)
        idx = mats.index(mat)
        me = bpy.data.meshes.new('tmp')
        tmp.to_mesh(me)
        tmp.free()
        for p in me.polygons:
            p.material_index = idx
            if not keep_smooth:
                p.use_smooth = False
        bm.from_mesh(me)
        bpy.data.meshes.remove(me)

    def to_objects(self):
        objs = {}
        for kind, bm in self.bms.items():
            me = bpy.data.meshes.new(self.name + '_' + kind)
            bm.normal_update()
            bm.to_mesh(me)
            bm.free()
            obj = bpy.data.objects.new(self.name + '_' + kind, me)
            bpy.context.scene.collection.objects.link(obj)
            for n in self.mats[kind]:
                me.materials.append(make_material(n))
            obj.hide_render = kind in ('fx', 'prop')
            objs[kind] = obj
        return objs


# ---------------------------------------------------------------- materials for the bake
_img_cache = {}


def _image(name):
    if name not in _img_cache:
        _img_cache[name] = bpy.data.images.load(os.path.join(TEXDIR, name + '.png'))
    return _img_cache[name]


def make_material(name):
    mat = bpy.data.materials.get(name)
    if mat:
        return mat
    mat = bpy.data.materials.new(name)
    mat.use_nodes = True
    nt = mat.node_tree
    nt.nodes.clear()
    out = nt.nodes.new('ShaderNodeOutputMaterial')
    d = palette.P.get(name, {'kind': 'prop'})
    k = d['kind']
    if k == 'lit':
        sh = nt.nodes.new('ShaderNodeBsdfDiffuse')
        sh.inputs['Color'].default_value = tuple(d['albedo']) + (1,)
        nt.links.new(sh.outputs[0], out.inputs['Surface'])
    elif k == 'emit':
        sh = nt.nodes.new('ShaderNodeEmission')
        sh.inputs['Strength'].default_value = d['strength']
        sh.inputs['Color'].default_value = tuple(d['color']) + (1,)
        if d.get('tex'):
            tx = nt.nodes.new('ShaderNodeTexImage')
            tx.image = _image(d['tex'])
            nt.links.new(tx.outputs['Color'], sh.inputs['Color'])
        nt.links.new(sh.outputs[0], out.inputs['Surface'])
    else:   # glass / fx / prop: let light through in the bake
        sh = nt.nodes.new('ShaderNodeBsdfTransparent')
        nt.links.new(sh.outputs[0], out.inputs['Surface'])
    return mat


# ---------------------------------------------------------------- lightmap bake
def lightmap_bake(obj, size=2048, samples=256, margin=8):
    """Smart-project lightmap UVs ('LM') and bake diffuse direct + indirect + colour into a float image.
    Returns an HxWx3 float array (row 0 = top, matching the exported uv1 with v flipped)."""
    scn = bpy.context.scene
    scn.render.engine = 'CYCLES'
    scn.cycles.device = 'CPU'
    scn.cycles.samples = samples
    scn.cycles.max_bounces = 6
    scn.cycles.diffuse_bounces = 4
    scn.cycles.transparent_max_bounces = 8
    if scn.world is None:
        scn.world = bpy.data.worlds.new('World')
    scn.world.use_nodes = True
    bg = scn.world.node_tree.nodes.get('Background')
    if bg:
        bg.inputs['Color'].default_value = (0.004, 0.006, 0.012, 1)
        bg.inputs['Strength'].default_value = 1.0
    me = obj.data
    lm = me.uv_layers.new(name='LM')
    me.uv_layers.active = lm
    bpy.ops.object.select_all(action='DESELECT')
    obj.select_set(True)
    bpy.context.view_layer.objects.active = obj
    bpy.ops.object.mode_set(mode='EDIT')
    bpy.ops.mesh.select_all(action='SELECT')
    bpy.ops.uv.smart_project(angle_limit=math.radians(60), island_margin=0.004, area_weight=0.0, scale_to_bounds=True)
    bpy.ops.object.mode_set(mode='OBJECT')
    img = bpy.data.images.new(obj.name + '_LM', size, size, alpha=False, float_buffer=True)
    for mat in me.materials:
        nt = mat.node_tree
        node = nt.nodes.new('ShaderNodeTexImage')
        node.image = img
        for n in nt.nodes:
            n.select = False
        node.select = True
        nt.nodes.active = node
    bpy.ops.object.bake(type='DIFFUSE', pass_filter={'DIRECT', 'INDIRECT', 'COLOR'}, margin=margin, use_clear=True)
    px = np.empty(size * size * 4, np.float32)
    img.pixels.foreach_get(px)
    return px.reshape(size, size, 4)[::-1, :, :3].copy()   # Blender rows are bottom-up


# ---------------------------------------------------------------- export
def export_mesh(path, objs, lightmap=None):
    """Unindexed triangle corners in app space: pos, nrm, uv0 (UVMap), uv1 (LM, v flipped), mat index, names."""
    pos, nrm, uv0, uv1, mat, names = [], [], [], [], [], []
    for obj in objs:
        me = obj.data
        me.calc_loop_triangles()
        n_loops = len(me.loops)
        uvs = {}
        for key in ('UVMap', 'LM'):
            arr = np.zeros(n_loops * 2, np.float32)
            if key in me.uv_layers:
                me.uv_layers[key].data.foreach_get('uv', arr)
            uvs[key] = arr.reshape(-1, 2)
        base = len(names)
        names += [m.name for m in me.materials]
        M = obj.matrix_world
        R = M.to_3x3()
        for tri in me.loop_triangles:
            poly = me.polygons[tri.polygon_index]
            for k in range(3):
                li = tri.loops[k]
                vi = me.loops[li].vertex_index
                p = M @ me.vertices[vi].co
                n = (R @ (me.vertices[vi].normal if poly.use_smooth else poly.normal)).normalized()
                pos.append((p.x, p.z, -p.y))
                nrm.append((n.x, n.z, -n.y))
                a, b = uvs['UVMap'][li], uvs['LM'][li]
                uv0.append((a[0], 1.0 - a[1]))   # texture rows: v = 0 at the top of the image
                uv1.append((b[0], 1.0 - b[1]))
                mat.append(base + poly.material_index)
    extra = {'lightmap': lightmap.astype(np.float16)} if lightmap is not None else {}
    np.savez_compressed(path, pos=np.array(pos, np.float32), nrm=np.array(nrm, np.float32),
                        uv0=np.array(uv0, np.float32), uv1=np.array(uv1, np.float32), mat=np.array(mat, np.int32),
                        names=np.array(names), **extra)
    print('exported', path, len(pos) // 3, 'tris')
