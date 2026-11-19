"""RONIN KABUTO: a headgear for Project Arena, made to go with the Ronin Wrap. Run in Blender:

blender -b -P make_ronin_kabuto.py -- BODY(m|f) OUT.glb [OUT.blend]   (Blender 4.1+)

Head space (the game's, as the template pack's head_<body>.glb): metres, +Y up, the face toward -Z, the origin
between the eyes. Everything is measured off the template head, so each body gets its own fit:
  - the bowl (hachi): a smooth lacquered dome cleared off the head, with raised ribs, a trim rim band and a crown ring
  - the neck guard (shikoro): four lames flaring out and down round the back, each one over the next, laced with
    cords in the secondary colour and hemmed in trim (the suit's shoulder lames, on the head)
  - the turned-back wings (fukigaeshi): solid plates on the guard's front ends in the bowl's own lacquer and gilt,
    closed onto it all round, each with a gilt crest
  - the brow peak (mabizashi) with a glowing edge
  - the crest (maedate): a tall trim crescent with a glowing inlay, on a mount with a glowing emblem
It is open-faced: face shields, masks (the BANDANA) and the face show as they are.
"""
import bpy, bmesh, sys, math
import numpy as np
from mathutils import Vector
from mathutils.bvhtree import BVHTree

argv = sys.argv[sys.argv.index('--') + 1:]
BODY, OUT = argv[0], argv[1]
BLEND = argv[2] if len(argv) > 2 else None
import os
# the template pack (the asset viewer's project-arena-templates.zip, unpacked): PA_TEMPLATES, else next to this script
TPL = os.environ.get('PA_TEMPLATES', os.path.join(os.path.dirname(os.path.abspath(__file__)), 'project-arena-templates')) + '/'

bpy.ops.wm.read_factory_settings(use_empty=True)
bpy.ops.import_scene.gltf(filepath=TPL + 'head_%s.glb' % ('masculine' if BODY == 'm' else 'feminine'))
for m in list(bpy.data.materials):
    m.name = 'template_' + m.name
heads = [o for o in bpy.data.objects if o.type == 'MESH']
dg = bpy.context.evaluated_depsgraph_get()
HV, HT = [], []
for o in heads:
    me = o.evaluated_get(dg).to_mesh()
    base = len(HV)
    HV += [tuple(o.matrix_world @ v.co) for v in me.vertices]
    me.calc_loop_triangles()
    HT += [tuple(base + i for i in t.vertices) for t in me.loop_triangles]
HB = np.array(HV)                                   # Blender space (Z up, the face toward -Y... see G())
bvh = BVHTree.FromPolygons(HV, HT)


def G(b):    # Blender -> glTF / game head space
    return np.stack([b[..., 0], b[..., 2], -b[..., 1]], -1)


def B(g):    # glTF -> Blender
    g = np.asarray(g, float)
    return np.stack([g[..., 0], -g[..., 2], g[..., 1]], -1)


HP = G(HB)   # the head's points in head space
lo, hi = HP.min(0), HP.max(0)
ZC = (lo[2] + hi[2]) / 2       # front-back middle of the head
print('head %s: x[%.3f,%.3f] y[%.3f,%.3f] z[%.3f,%.3f]' % (BODY, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2]))


def smooth(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def az_dir(th):   # azimuth th (rad): 0 = the front (-Z), +pi/2 = +X, pi = the back
    return np.stack([np.sin(th), np.zeros_like(th), -np.cos(th)], -1)


# ---------------------------------------------------------------- the bowl's shape: an ellipsoid cleared off the head
def y_rim(th):
    """The bowl's lower edge: over the brows at the front and, level, back past the ears (the face shields that wrap
    round to them stay under it), then lower at the back, above a headset's strap."""
    a = np.abs(((np.asarray(th) + math.pi) % (2 * math.pi)) - math.pi)   # 0 front .. pi back
    front, ears, back, turn = 0.050, 0.047, 0.024, math.radians(106)
    return np.where(a < turn, front + (ears - front) * smooth(0, turn, a), ears + (back - ears) * smooth(turn, math.pi, a))


YB = 0.0                                  # the ellipsoid's centre height
AX = np.array([0.080, 0.118, 0.106])      # its half-axes before fitting (x, y, z)
GAP = 0.012
rel = HP - np.array([0, YB, ZC])
th_p = np.arctan2(rel[:, 0], -rel[:, 2])
cover = HP[:, 1] > y_rim(th_p) - 0.012    # the head under the bowl
q = rel[cover]
q = q + q / np.linalg.norm(q, axis=1, keepdims=True) * GAP
S = float(np.max(np.sqrt(((q / AX) ** 2).sum(1))))
AXS = AX * S
print('  bowl half-axes %.3f %.3f %.3f (scale %.3f)' % (*AXS, S))


def bowl_pt(th, phi, bump=0.0):
    """The bowl's surface at azimuth th, elevation phi (from its centre), pushed out `bump` along its normal."""
    th, phi = np.broadcast_arrays(np.asarray(th, float), np.asarray(phi, float))
    p = np.stack([AXS[0] * np.cos(phi) * np.sin(th), YB + AXS[1] * np.sin(phi), ZC - AXS[2] * np.cos(phi) * np.cos(th)], -1)
    n = (p - np.array([0, YB, ZC])) / AXS ** 2
    n /= np.linalg.norm(n, axis=-1, keepdims=True)
    return p + n * np.asarray(bump)[..., None], n


def phi_rim(th):
    return np.arcsin(np.clip((y_rim(th) - YB) / AXS[1], -0.99, 0.99))


# ---------------------------------------------------------------- mesh accumulation (per part, per material)
PARTS = {}


HC = np.array([0.0, 0.01, ZC])   # the head's middle: single faces look away from it


def facing(want, cen):
    if want is None or (isinstance(want, str) and want == 'out'):
        return cen - HC
    if isinstance(want, str) and want == 'in':
        return HC - cen
    if callable(want):
        return want(cen)
    return np.asarray(want, float)


def add(part, mat, P, flip=False, two=False, want='out'):
    """A quad grid P[v, u] (glTF space) in material `mat` (a string, or an array [v-1, u-1] of names / None). Each
    face is wound to face `want`: 'out' (away from the head), 'in', a direction, or a function of its centre."""
    P = np.asarray(P, float)
    nv, nu = P.shape[:2]
    mats = np.full((nv - 1, nu - 1), mat, dtype=object) if isinstance(mat, str) else np.asarray(mat, dtype=object)
    d = PARTS.setdefault(part, {'V': [], 'F': [], 'M': []})
    base = len(d['V'])
    d['V'] += [tuple(x) for x in B(P.reshape(-1, 3))]
    flat = P.reshape(-1, 3)
    for j in range(nv - 1):
        for i in range(nu - 1):
            m = mats[j, i]
            if m is None:
                continue
            a, b, c, e = j * nu + i, j * nu + i + 1, (j + 1) * nu + i + 1, (j + 1) * nu + i
            n = np.cross(flat[c] - flat[a], flat[e] - flat[b])
            if np.linalg.norm(n) < 1e-14:
                continue
            q = (a, b, c, e) if np.dot(n, facing(want, flat[[a, b, c, e]].mean(0))) >= 0 else (a, e, c, b)
            d['F'].append(tuple(base + x for x in q))
            d['M'].append((m, two))


def add_poly(part, mat, ring_a, ring_b, flip=False, two=False, want='out'):
    """A strip between two point rows (same length)."""
    add(part, mat, np.stack([ring_a, ring_b]), two=two, want=want)


def add_fan(part, mat, ring, centre, flip=False, two=False, want='out'):
    d = PARTS.setdefault(part, {'V': [], 'F': [], 'M': []})
    base = len(d['V'])
    pts = np.vstack([ring, centre[None]])
    d['V'] += [tuple(x) for x in B(pts)]
    n_ = len(ring)
    for i in range(n_ - 1):
        f = (i, i + 1, n_)
        n = np.cross(pts[f[1]] - pts[f[0]], pts[f[2]] - pts[f[0]])
        if np.linalg.norm(n) < 1e-14:
            continue
        if np.dot(n, facing(want, pts[list(f)].mean(0))) < 0:
            f = f[::-1]
        d['F'].append(tuple(base + x for x in f))
        d['M'].append((mat, two))


# ---------------------------------------------------------------- the bowl: ribbed lacquer
NRIB = 16
per = np.array([-0.07, 0.0, 0.07, 0.5])          # rib edge, rib crest, rib edge, panel middle (in rib periods)
U = (np.arange(NRIB)[:, None] + per[None]).reshape(-1)
U = np.append(U, U[0] + NRIB)
TH = U / NRIB * 2 * math.pi
prof = np.tile(np.array([0.0, 0.0022, 0.0, -0.0006]), NRIB)
prof = np.append(prof, prof[0])
NV = 14
V_TOP = 0.90
rows = []
for j in range(NV + 1):
    v = j / NV * V_TOP
    ph0 = phi_rim(TH)
    phi = ph0 + (math.pi / 2 - ph0) * v
    fade = 1 - smooth(0.55, 0.92, v)                 # the ribs run out into the crown
    P, _ = bowl_pt(TH, phi, prof * fade)
    rows.append(P)
add('bowl', 'av_armor', np.array(rows), flip=False)
crown_ring = rows[-1]
# the crown: a trim ring round the top (tehen) and a low cap
cr_c = crown_ring.mean(0)
ring_out, _ = bowl_pt(TH, math.pi / 2 - (math.pi / 2 - phi_rim(TH)) * (1 - V_TOP), 0.0025)
lift = np.array([0, 0.004, 0])
add('bowl', 'av_trim', np.stack([crown_ring, ring_out, ring_out + lift, cr_c + (ring_out - cr_c) * 0.7 + lift]))
add_fan('bowl', 'av_trim', cr_c + (ring_out - cr_c) * 0.7 + lift, cr_c + np.array([0, 0.007, 0]))

# the rim band (koshimaki): proud of the bowl, all the way round
TB = np.linspace(0, 2 * math.pi, 97)
pr = phi_rim(TB)
dphi = 0.012 / AXS[1]
b_lo_in, _ = bowl_pt(TB, pr - 0.002 / AXS[1], -0.002)
b_lo, _ = bowl_pt(TB, pr - 0.002 / AXS[1], 0.003)
b_hi, _ = bowl_pt(TB, pr + dphi, 0.003)
b_top, _ = bowl_pt(TB, pr + dphi + 0.002 / AXS[1], 0.0)
add('band', 'av_trim', np.stack([b_lo_in, b_lo, b_hi, b_top]), two=True)


def rim_frame(th):
    """At azimuth th: the rim point, the outward horizontal direction there, the tangent round the rim."""
    p, n = bowl_pt(th, phi_rim(th) - 0.002 / AXS[1], 0.003)
    h = n.copy()
    h[..., 1] = 0
    h /= np.linalg.norm(h, axis=-1, keepdims=True)
    t = np.cross(np.array([0.0, 1.0, 0.0]), h)
    return p, h, t


# ---------------------------------------------------------------- the neck guard: four laced lames
TH0 = math.radians(72)              # where the top lame starts at the front (each lower one a little further back)
NL = 4
LACE = math.radians(11.0)
lame_ends = []
for k in range(NL):
    t0 = TH0 + math.radians(7) * k
    t1 = 2 * math.pi - t0
    n_per = max(3, int(round((t1 - t0) / LACE)))
    pp = np.array([0.0, 0.43, 0.57])
    uu = (np.arange(n_per)[:, None] + pp[None]).reshape(-1)
    uu = np.append(uu, n_per)
    TL = t0 + (t1 - t0) * uu / n_per
    lace_col = np.tile(np.array([False, True, False]), n_per)
    d0 = 0.003 + 0.021 * k                     # how far below the rim the lame starts, and its height
    hgt = 0.030
    vr = np.array([0.0, 0.45, 0.80, 1.0])
    out_k = 0.0025 + (NL - 1 - k) * 0.0032     # each lame over the next
    end = smooth(0.0, 0.10, (TL - t0) / (t1 - t0)) * smooth(0.0, 0.10, (t1 - TL) / (t1 - t0))   # ends sweep up
    p, h, t = rim_frame(TL)
    rows = []
    for v in vr:
        d = d0 + hgt * v - 0.010 * (1 - end) * v
        flare = 0.50 * d + 0.010 * v * v
        rows.append(p + h * (out_k + flare)[..., None] - np.array([0, 1.0, 0]) * d[..., None])
    rows = np.array(rows)
    mats = np.empty((len(vr) - 1, len(TL) - 1), dtype=object)
    mats[:] = 'av_armor'
    mats[:2, lace_col] = 'av_secondary'
    mats[-1, :] = 'av_trim'
    add('shikoro', mats, rows, flip=True, two=True)
    lame_ends.append((rows[:, 0], rows[:, -1]))

# ---------------------------------------------------------------- the turned-back wings (fukigaeshi)
# The top lames' front ends, folded back: a solid plate on each side of the guard, in the bowl's own lacquer and gilt.
# It follows the guard's curve and turns out from it toward its back edge; gilt edges close it onto the guard all
# round (no gap: it is part of the helmet), a gilt border on its face and a gilt crest (a ring round a diamond).
NS, NT = 12, 9
sw = np.linspace(0, 1, NS)
tw = np.linspace(0, 1, NT)
OUT0 = 0.0025 + (NL - 1) * 0.0032           # the top lame's own offset
for side in (1, -1):
    th = TH0 - math.radians(3) + math.radians(30) * sw
    th = th if side > 0 else 2 * math.pi - th
    p, h, _ = rim_frame(th)
    rc = smooth(0.62, 1.0, sw)                                       # the back corners, rounded
    d_top = -0.009 + 0.003 * sw + 0.014 * rc ** 2                    # from a little above the rim band
    d_bot = 0.052 - 0.005 * sw - 0.016 * rc ** 2
    lift = 0.0035 + 0.0150 * sw ** 1.2                               # turning out from the guard toward its back edge

    def surf(t, lf):
        d = d_top + (d_bot - d_top) * t
        return p + h * (OUT0 + 0.50 * np.maximum(d, 0) + lf)[:, None] - np.array([0, 1.0, 0]) * d[:, None]
    W = np.array([surf(t, lift + 0.0025 * np.sin(math.pi * t)) for t in tw])   # [t, s]: its face, a little domed
    base = np.array([surf(t, np.zeros_like(sw)) for t in tw])                    # where it meets the guard
    mats = np.full((NT - 1, NS - 1), 'av_armor', dtype=object)
    mats[:, :1] = 'av_trim'                                          # the gilt border: the fold, the edges
    mats[:, -1:] = 'av_trim'
    mats[0, :] = 'av_trim'
    mats[-1, :] = 'av_trim'
    add('wings', mats, W, two=True)
    for edge_w, edge_b in ((W[0], base[0]), (W[-1], base[-1]), (W[:, 0], base[:, 0]), (W[:, -1], base[:, -1])):
        add_poly('wings', 'av_trim', edge_w, edge_b, two=True)       # closed onto the guard all round
    # the crest (mon): a gilt ring round a gilt diamond, standing a little proud of the face
    ci, cj = NT // 2, 5
    c0 = W[ci, cj]
    along = W[ci, cj + 1] - W[ci, cj - 1]
    along /= np.linalg.norm(along)
    upv = W[ci - 1, cj] - W[ci + 1, cj]
    upv -= along * np.dot(upv, along)
    upv /= np.linalg.norm(upv)
    nrm = np.cross(along, upv)
    if np.dot(nrm, c0 - HC) < 0:
        nrm = -nrm
    ang = np.linspace(0, 2 * math.pi, 25)
    circ = np.cos(ang)[:, None] * along + np.sin(ang)[:, None] * upv
    lift_m = nrm * 0.0016
    add_poly('wings', 'av_trim', c0 + circ * 0.0115 + lift_m, c0 + circ * 0.0088 + lift_m, two=True)
    add_poly('wings', 'av_trim', c0 + circ * 0.0115 + lift_m, c0 + circ * 0.0115, two=True)
    dia = np.array([c0 + along * 0.0062, c0 + upv * 0.0062, c0 - along * 0.0062, c0 - upv * 0.0062, c0 + along * 0.0062]) + lift_m
    add_fan('wings', 'av_trim', dia, c0 + lift_m, two=True)

# ---------------------------------------------------------------- the brow peak (mabizashi)
TV = math.radians(70)
sv = np.linspace(-1, 1, 33)
thv = sv * TV
p, h, t = rim_frame(thv)
tilt = math.radians(28)
dirv = h * math.cos(tilt) - np.array([0, 1.0, 0]) * math.sin(tilt)
depth = 0.028 * (1 - sv ** 2) ** 0.7 + 0.003
ur = np.array([0.0, 0.55, 0.86, 1.0])
rows = [p + np.array([0, 0.002, 0]) + dirv * (depth * u)[:, None] - np.array([0, 1.0, 0]) * (0.003 * u * u) for u in ur]
mats = np.empty((len(ur) - 1, len(sv) - 1), dtype=object)
mats[:] = 'av_armor'
mats[-1, :] = 'av_glow'
add('peak', mats, np.array(rows), two=True)

# ---------------------------------------------------------------- the crest: a mount and a crescent (maedate)
# the mount: a shield-shaped plate on the bowl over the brow peak
ms = np.linspace(-1, 1, 9)
mv = np.linspace(0, 1, 6)
plate = []
for v in mv:
    half = math.radians(9.5) * (1 - 0.55 * v ** 1.6)
    ph = phi_rim(np.array(0.0)) + (0.012 + 0.036 * v) / AXS[1]
    P, _ = bowl_pt(ms * half, ph, 0.005)
    plate.append(P)
plate = np.array(plate)
add('crest', 'av_primary_np', plate)
for edge in (plate[0], plate[-1][::-1], plate[::-1, 0], plate[:, -1]):   # its rim, down to the bowl
    q_, _ = bowl_pt(np.arctan2(edge[:, 0], -(edge[:, 2] - ZC)), np.arcsin(np.clip((edge[:, 1] - YB) / AXS[1], -1, 1)), -0.001)
    add_poly('crest', 'av_trim', edge, q_, two=True)
Mc = plate[2, 4]                                        # the emblem, and the crescent's root
Mn = bowl_pt(np.array(0.0), np.arcsin((Mc[1] - YB) / AXS[1]))[1]
ang = np.linspace(0, 2 * math.pi, 21)
up = np.array([0, 1.0, 0])
xax = np.array([1.0, 0, 0])
yax = np.cross(Mn, xax)
yax /= np.linalg.norm(yax)
add_fan('crest', 'av_glow', Mc[None] + Mn * 0.0012 + (np.cos(ang)[:, None] * xax + np.sin(ang)[:, None] * yax) * 0.0085,
        Mc + Mn * 0.0012)

# the crescent: two circles' difference, horns up, in a plane leaning back with the forehead
RO = 0.088
CO = np.array([0.0, RO])               # (x, y) in the crest plane, the root at (0, 0)
CI = np.array([0.0, RO + 0.017])
RI = RO - 0.001
dx = CI - CO
dd = np.linalg.norm(dx)
a_ = (RO ** 2 - RI ** 2 + dd ** 2) / (2 * dd)
hh = math.sqrt(RO ** 2 - a_ ** 2)
tipY = CO[1] + a_ * dx[1] / dd
tip_ang = math.atan2(tipY - CO[1], hh)                 # the tips' angle on the outer circle
n_c = 40
ao = np.linspace(-math.pi - tip_ang, tip_ang, n_c)     # the outer arc, round the bottom, tip to tip
outer2 = CO + np.stack([np.cos(ao), np.sin(ao)], -1) * RO
ai = np.arctan2(outer2[:, 1] - CI[1], outer2[:, 0])    # the inner arc at the same sweep
inner2 = np.stack([np.cos(ai), np.sin(ai)], -1) * RI + CI
inner2[0], inner2[-1] = outer2[0], outer2[-1]
lean = math.radians(14)
cy = up * math.cos(lean) + np.array([0, 0, 1.0]) * math.sin(lean)   # up the crest, leaning back
cz = np.cross(xax, cy)                                               # its front (toward -Z)
root = Mc + Mn * 0.004 - cy * 0.006


def C3(p2, off):
    return root[None] + xax[None] * p2[:, :1] + cy[None] * p2[:, 1:2] + cz[None] * off


TCK = 0.0018
add_poly('crest', 'av_trim', C3(outer2, -TCK), C3(inner2, -TCK), two=True, want=-cz)   # its front
add_poly('crest', 'av_trim', C3(outer2, TCK), C3(inner2, TCK), two=True, want=cz)      # its back
add_poly('crest', 'av_trim', C3(outer2, TCK), C3(outer2, -TCK), two=True, want=lambda c: c - C3(CO[None], 0)[0])   # the outer edge
add_poly('crest', 'av_trim', C3(inner2, TCK), C3(inner2, -TCK), two=True, want=lambda c: C3(CI[None], 0)[0] - c)   # the inner edge
mid_a = outer2 + (inner2 - outer2) * 0.30
mid_b = outer2 + (inner2 - outer2) * 0.70
add_poly('crest', 'av_glow', C3(mid_a, -TCK - 0.0008), C3(mid_b, -TCK - 0.0008), two=True, want=-cz)   # the glowing inlay, front
add_poly('crest', 'av_glow', C3(mid_a, TCK + 0.0008), C3(mid_b, TCK + 0.0008), two=True, want=cz)      # and back

# ---------------------------------------------------------------- materials and objects
PREVIEW = {'av_armor': (0.045, 0.047, 0.052), 'av_trim': (0.85, 0.80, 0.55), 'av_secondary': (0.95, 0.45, 0.08),
           'av_primary_np': (0.12, 0.40, 1.0), 'av_glow': (0.2, 1.0, 1.0)}
CHANNEL = {'av_primary_np': 'primary', 'av_secondary': 'secondary', 'av_glow': 'glow', 'av_trim': 'trim'}
MATS = {}


def mat(name, two):
    key = (name, two)
    if key not in MATS:
        m = bpy.data.materials.new(name if not two else name + '.2s')
        c = PREVIEW[name]
        m.diffuse_color = c + (1,)
        m.use_backface_culling = not two
        m.use_nodes = True
        bs = m.node_tree.nodes['Principled BSDF']
        bs.inputs['Base Color'].default_value = c + (1,)
        bs.inputs['Roughness'].default_value = {'av_trim': 0.3, 'av_armor': 0.35, 'av_primary_np': 0.4}.get(name, 0.7)
        bs.inputs['Metallic'].default_value = 0.85 if name == 'av_trim' else 0.0
        if name == 'av_glow':
            bs.inputs['Emission Color'].default_value = c + (1,)
            bs.inputs['Emission Strength'].default_value = 2.0
        m['projectArena'] = {'palette': name, 'channel': CHANNEL.get(name, 'none'), 'twoSided': two, 'cloth': False}
        MATS[key] = m
    return MATS[key]


for o in heads:
    bpy.data.objects.remove(o)
objs = []
total = 0
for name, d in PARTS.items():
    me = bpy.data.meshes.new('ronin_kabuto_' + name)
    me.from_pydata(d['V'], [], d['F'])
    keys = sorted(set(d['M']))
    for k in keys:
        me.materials.append(mat(*k))
    for poly, mk in zip(me.polygons, d['M']):
        poly.material_index = keys.index(mk)
        poly.use_smooth = True
    bm = bmesh.new()
    bm.from_mesh(me)
    bmesh.ops.remove_doubles(bm, verts=bm.verts, dist=1e-6)
    bmesh.ops.triangulate(bm, faces=bm.faces[:])
    bm.to_mesh(me)
    bm.free()
    me.set_sharp_from_angle(angle=math.radians(40))
    ob = bpy.data.objects.new('ronin_kabuto_' + name, me)
    bpy.context.scene.collection.objects.link(ob)
    objs.append(ob)
    total += len(me.polygons)
    print('  %-8s %5d triangles' % (name, len(me.polygons)))
print('  ronin kabuto %s: %d triangles' % (BODY, total))

# ---------------------------------------------------------------- fit check: nothing in the head, how close it comes
dmin, inside, near = 1.0, 0, 0
for ob in objs:
    for v in ob.data.vertices:
        loc, nrm, _, dist = bvh.find_nearest(v.co)
        if loc is None:
            continue
        if (v.co - loc).dot(nrm) < 0 and dist < 0.03:   # (further out, the nearest normal can point anywhere)
            inside += 1
        dmin = min(dmin, dist)
        near += dist < 0.003
print('  fit: closest to the skin %.1f mm, %d vertices inside the head, %d within 3 mm' % (dmin * 1000, inside, near))

bpy.ops.object.select_all(action='DESELECT')
for ob in objs:
    ob.select_set(True)
bpy.ops.export_scene.gltf(filepath=OUT, export_format='GLB', use_selection=True, export_extras=True,
                          export_animations=False, export_yup=True)
if BLEND:
    bpy.ops.wm.save_as_mainfile(filepath=BLEND)
print('wrote', OUT)
