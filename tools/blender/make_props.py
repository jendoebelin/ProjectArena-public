"""Project Arena props, each authored in the frame the app attaches it to (Blender: X right, Y forward, Z up).

  projectile / projectile_shell  ball space, metres; the plasma sphere is 8 cm (the app scales it to the 10 cm ball)
  spawner                        floor space, pad top at 3 cm
  strikezone                     ring in the court's cross-section plane (app YZ = Blender YZ? -> axis X), r 0.75
  lancer / knuckleguard          grip frame (+X right, +Z thumb up, +Y forward); the knuckle guard matches the
                                 bumper box (centre (-0.022, 0.137, 0.010), half (0.064, 0.029, 0.048) here)
  shield                         shield frame: plate in X (+-0.20) by Y (+-0.32), thin along Z
  head                           head frame: eyes at the origin, looking +Y
  torso                          body frame: origin on the floor under the head, facing +Y

tools/build_open_assets.py converts each to the app's model space (tools/frames.py).
Run: blender -b --factory-startup -P tools/blender/make_props.py -- build/open_src/props.npz
"""
import math
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bmesh  # noqa: E402
import bpy  # noqa: E402
import numpy as np  # noqa: E402
from mathutils import Matrix, Vector  # noqa: E402

import common  # noqa: E402


def ball(b):
    b.sphere('ball_plasma', (0, 0, 0), 0.08, segs=40, rings=20, uv_scale=(2, 1))   # veins tile twice around


def shell(b):
    """Three hair-thin glowing filament orbits (one solid, two dashed) with tiny sparks: they spin with the ball."""
    for k, (tilt, yaw) in enumerate(((0, 0), (62, 0), (62, 120))):
        R = Matrix.Rotation(math.radians(yaw), 4, 'Z') @ Matrix.Rotation(math.radians(tilt), 4, 'X')
        tmp = bmesh.new()
        segs, tsegs, rr, r = 96, 4, 0.097, 0.0011
        rings = []
        for i in range(segs):
            a = 2 * math.pi * i / segs
            row = []
            for j in range(tsegs):
                bb = 2 * math.pi * j / tsegs
                q = rr + r * math.cos(bb)
                row.append(tmp.verts.new(R @ Vector((q * math.cos(a), q * math.sin(a), r * math.sin(bb)))))
            rings.append(row)
        for i in range(segs):
            if k > 0 and (i // 6) % 3 == 2:
                continue
            for j in range(tsegs):
                tmp.faces.new((rings[i][j], rings[(i + 1) % segs][j], rings[(i + 1) % segs][(j + 1) % tsegs],
                               rings[i][(j + 1) % tsegs]))
        b.add_bmesh(tmp, 'ball_filament', smooth=True)
        for n in range(2):
            a = 2 * math.pi * (n + 0.3 * k) / 2
            q = R @ Vector((rr * math.cos(a), rr * math.sin(a), 0))
            b.sphere('ball_spark', tuple(q), 0.0035, segs=8, rings=5)


def spawner(b):
    b.cylinder('pad_metal', (0, 0, 0.012), 0.24, 0.024, segs=64, smooth=False)
    b.cylinder('pad_rim', (0, 0, 0.026), 0.245, 0.006, segs=64, radius2=0.235, smooth=False)
    for r_in, r_out in ((0.205, 0.215), (0.15, 0.158), (0.095, 0.101)):
        b.ring('pad_glow', (0, 0, 0.0305), r_in, r_out, 0.003, segs=96)
    for k in range(12):   # radial tick marks
        a = 2 * math.pi * k / 12
        c = Vector((math.cos(a), math.sin(a), 0)) * 0.18
        b.box('pad_glow', (c.x, c.y, 0.0302), (0.024, 0.006, 0.003), rot=Matrix.Rotation(a, 3, 'Z'))
    b.sphere('pad_lens', (0, 0, 0.026), 0.05, segs=24, rings=8, scale=(1, 1, 0.25))


def strikezone(b):
    b.disc('zone_holo', (0, 0, 0), 0.75, segs=96, axis='X')
    b.disc('zone_holo', (0, 0, 0), 0.75, segs=96, axis='X', flip=True)
    b.torus('zone_edge', (0, 0, 0), 0.735, 0.006, segs=128, tsegs=6, axis='X')


def _frame_box(b, mat, center, size, frame, bevel=0.0, segments=2):
    b.box(mat, tuple(center), size, bevel=bevel, segments=segments, rot=frame)


def _capsule(b, mat, a, c, r, segs=12):
    a, c = Vector(a), Vector(c)
    d = c - a
    rot = d.to_track_quat('Z', 'Y').to_matrix().to_4x4()
    tmp = bmesh.new()
    bmesh.ops.create_cone(tmp, cap_ends=False, segments=segs, radius1=r, radius2=r, depth=d.length)
    bmesh.ops.transform(tmp, matrix=Matrix.Translation((a + c) / 2) @ rot, verts=tmp.verts)
    b.add_bmesh(tmp, mat, smooth=True)
    b.sphere(mat, tuple(a), r, segs=segs, rings=6)
    b.sphere(mat, tuple(c), r, segs=segs, rings=6)


def _shaped(b, mat, center, scale, frame, kind='sphere', segs=28, rings=14, depth=1.0, r1=1.0, r2=1.0, smooth=True):
    tmp = bmesh.new()
    if kind == 'sphere':
        bmesh.ops.create_uvsphere(tmp, u_segments=segs, v_segments=rings, radius=1.0)
    else:
        bmesh.ops.create_cone(tmp, cap_ends=True, segments=segs, radius1=r1, radius2=r2, depth=depth)
    bmesh.ops.scale(tmp, vec=scale, verts=tmp.verts)
    bmesh.ops.transform(tmp, matrix=Matrix.Translation(center) @ frame.to_4x4(), verts=tmp.verts)
    b.add_bmesh(tmp, mat, smooth=smooth)


def lancer(b):
    """The player's hand: a modern VR controller held in an armoured knuckle-guard gauntlet glove.
    Grip frame (Blender: X right, Y = aim forward, Z up), origin = handle centroid. The handle axis is tilted 60 deg
    from the aim ray (OpenXR aim vs grip), so it points up-forward: U = (0, 0.5, 0.866)."""
    U = Vector((0, 0.5, 0.866))
    F = Vector((0, 0.866, -0.5))
    R = Vector((1, 0, 0))
    fr = Matrix((R, F, U)).transposed()          # local (x=R, y=F, z=U) -> grip

    def P(r, f, u):
        return R * r + F * f + U * u

    # ---- controller: tapered handle, rounded head with face plate, thumbstick, two buttons, trigger
    _shaped(b, 'ctrl_body', P(0, 0, -0.008), (1, 1, 1), fr, kind='cone', segs=24, depth=0.10, r1=0.0175, r2=0.0205)
    b.sphere('ctrl_body', tuple(P(0, 0, -0.058)), 0.0175, segs=20, rings=8)
    head = P(0.0, 0.008, 0.058)
    _shaped(b, 'ctrl_body', head, (0.031, 0.037, 0.021), fr)
    face = head + U * 0.0185
    _shaped(b, 'ctrl_face', face, (1.0, 1.15, 1.0), fr, kind='cone', depth=0.003, r1=0.026, r2=0.024, smooth=False)
    _shaped(b, 'ctrl_button', face + F * -0.008 + R * -0.006 + U * 0.004, (1, 1, 1), fr, kind='cone', segs=18,
            depth=0.008, r1=0.009, r2=0.0085)
    for off in (P(0.010, 0.012, 0.0025), P(0.003, 0.022, 0.0025)):
        _shaped(b, 'ctrl_button', face + off, (1, 1, 1), fr, kind='cone', segs=14, depth=0.003, r1=0.0045, r2=0.0045)
    trig = Matrix.Rotation(math.radians(-25), 3, 'X')
    _frame_box(b, 'ctrl_button', P(0, 0.034, 0.040), (0.014, 0.012, 0.026), fr @ trig, bevel=0.004)

    # ---- glove: fist mass, four curled fingers across the front, thumb on the face
    _shaped(b, 'glove', P(0.006, 0.010, -0.006), (0.044, 0.036, 0.050), fr)
    for k, u in enumerate((0.026, 0.006, -0.014, -0.032)):
        _capsule(b, 'glove', P(0.034, 0.030, u), P(-0.030 + 0.004 * k, 0.040, u - 0.004), 0.0118 - 0.0006 * k)
        _capsule(b, 'glove', P(-0.030 + 0.004 * k, 0.040, u - 0.004), P(-0.036, 0.010, u - 0.006), 0.0105 - 0.0005 * k)
    _capsule(b, 'glove', P(-0.034, -0.004, 0.022), P(-0.012, 0.004, 0.064), 0.0115)
    _capsule(b, 'glove', P(-0.012, 0.004, 0.064), P(-0.004, 0.0, 0.080), 0.0105)

    # ---- gauntlet armour: back-of-hand plate with a neon strip, knuckle bar, wrist cuff, short bracer
    _frame_box(b, 'gaunt_shell', P(0.047, 0.004, -0.004), (0.012, 0.058, 0.078), fr, bevel=0.005)
    _frame_box(b, 'gaunt_glow', P(0.0535, 0.004, -0.004), (0.0015, 0.008, 0.06), fr)
    _frame_box(b, 'gaunt_shell', P(0.003, 0.054, -0.004), (0.078, 0.010, 0.072), fr, bevel=0.004)
    _frame_box(b, 'gaunt_glow', P(0.003, 0.0595, -0.004), (0.06, 0.0015, 0.004), fr)
    wrist = Vector((0.004, -0.050, -0.040))
    b.torus('gaunt_shell', tuple(wrist), 0.040, 0.0075, segs=32, tsegs=8, axis='Y')
    b.torus('gaunt_glow', tuple(wrist + Vector((0, -0.006, 0))), 0.0415, 0.0022, segs=40, tsegs=4, axis='Y')
    _shaped(b, 'gaunt_dark', wrist + Vector((0, -0.045, -0.004)), (1, 1, 1), Matrix.Rotation(math.pi / 2, 3, 'X'),
            kind='cone', segs=24, depth=0.08, r1=0.037, r2=0.033)


# ---------------------------------------------------------------- armoured fists (GAUNTLET builder option)
def _orient(a, c, wdir, outward):
    """Axes for a box from a to c: y along c - a, x across (toward wdir), z = thickness, turned to `outward`."""
    a, c = Vector(a), Vector(c)
    y = (c - a)
    ln = y.length
    y.normalize()
    w = Vector(wdir)
    x = w - y * w.dot(y)
    x.normalize()
    z = x.cross(y)
    if z.dot(Vector(outward)) < 0:
        x, z = -x, -z
    return Matrix((x, y, z)).transposed(), ln, x, y, z


def _seg(b, mat, a, c, width, thick, wdir, outward, lift=0.0, grow=0.0, bevel=0.003, segments=2):
    rot, ln, x, y, z = _orient(a, c, wdir, outward)
    center = (Vector(a) + Vector(c)) / 2 + z * lift
    b.box(mat, tuple(center), (width, max(0.004, ln + grow), thick), bevel=bevel, segments=segments, rot=rot)
    return center, x, y, z


def _tube(b, mat, pts, r, segs=6, closed=False, up=None, smooth=True):
    """A round tube along a polyline; r: one radius or one per point. up: a fixed reference for the ring frames
    (closed loops round an axis), else parallel transport."""
    pts = [Vector(q) for q in pts]
    n = len(pts)
    rs = list(r) if hasattr(r, '__len__') else [r] * n
    tmp = bmesh.new()
    rings, prev = [], None
    for i, q in enumerate(pts):
        t = (pts[(i + 1) % n] - pts[i - 1]) if closed else (pts[min(i + 1, n - 1)] - pts[max(i - 1, 0)])
        t.normalize()
        ref = Vector(up) if up is not None else (prev if prev is not None else
                                                (Vector((0, 0, 1)) if abs(t.z) < 0.9 else Vector((1, 0, 0))))
        nn = (ref - t * ref.dot(t)).normalized()
        prev = nn
        bb = t.cross(nn)
        rings.append([tmp.verts.new(q + (nn * math.cos(2 * math.pi * k / segs) + bb * math.sin(2 * math.pi * k / segs)) * rs[i])
                      for k in range(segs)])
    for i in range(n if closed else n - 1):
        r0, r1 = rings[i], rings[(i + 1) % n]
        for k in range(segs):
            tmp.faces.new((r0[k], r0[(k + 1) % segs], r1[(k + 1) % segs], r1[k]))
    b.add_bmesh(tmp, mat, smooth=smooth)


def _slab(b, mat, outer, inner, closed=False):
    """A plate between two point grids [i][j] (i along, j across): smooth faces outside and inside, flat rims on
    the open edges (separate vertices, so the faces keep crisp edges)."""
    ni, nj = len(outer), len(outer[0])
    jj = nj if closed else nj - 1
    o00, o01, o10 = Vector(outer[0][0]), Vector(outer[0][1]), Vector(outer[1][0])
    flip = (o01 - o00).cross(o10 - o00).dot(Vector(outer[0][0]) - Vector(inner[0][0])) < 0

    def grid(G, fl):
        tmp = bmesh.new()
        V = [[tmp.verts.new(Vector(G[i][j])) for j in range(nj)] for i in range(ni)]
        for i in range(ni - 1):
            for j in range(jj):
                q = (V[i][j], V[i][(j + 1) % nj], V[i + 1][(j + 1) % nj], V[i + 1][j])
                tmp.faces.new(q[::-1] if fl else q)
        return tmp
    b.add_bmesh(grid(outer, flip), mat, smooth=True)
    b.add_bmesh(grid(inner, not flip), mat, smooth=True)
    if closed:
        loops = [[(0, j) for j in range(nj)] + [(0, 0)], [(ni - 1, j) for j in range(nj)] + [(ni - 1, 0)]]
    else:
        loops = [[(0, j) for j in range(nj)] + [(i, nj - 1) for i in range(1, ni)] +
                 [(ni - 1, j) for j in range(nj - 2, -1, -1)] + [(i, 0) for i in range(ni - 2, -1, -1)]]
    cen = Vector()
    for row in outer:
        for q in row:
            cen += Vector(q)
    cen /= ni * nj
    tmp = bmesh.new()
    for lp in loops:
        for (i0, j0), (i1, j1) in zip(lp[:-1], lp[1:]):
            A, B = Vector(outer[i0][j0]), Vector(outer[i1][j1])
            C, D = Vector(inner[i1][j1]), Vector(inner[i0][j0])
            if (A - B).length < 1e-7:
                continue
            q = [A, B, C, D]
            if (B - A).cross(D - A).dot((A + B + C + D) / 4 - cen) < 0:
                q = q[::-1]
            tmp.faces.new([tmp.verts.new(v) for v in q])
    b.add_bmesh(tmp, mat, smooth=False)


def _lame(b, mat, a, c, rad, arc, thick, outward, n_arc=8, n_len=2, r_end=None, ease=1.0, point=0.0, closed=False,
          bulge=0.0):
    """A curved armour plate wrapped round the axis a -> c: radius rad (r_end at c, eased), covering `arc` radians
    centred on the `outward` side (closed: all round), `thick` thick inward. point: the far edge drawn out to a point
    on the outward side (gothic). bulge: radius added mid-length (domed). Returns the outer grid [i][j]."""
    a, c = Vector(a), Vector(c)
    y = c - a
    L = y.length
    y.normalize()
    z = Vector(outward) - y * Vector(outward).dot(y)
    z.normalize()
    x = y.cross(z)
    r1 = rad if r_end is None else r_end
    nj = n_arc if closed else n_arc + 1
    outer, inner = [], []
    for i in range(n_len + 1):
        t = i / n_len
        r = rad + (r1 - rad) * t ** ease + bulge * math.sin(math.pi * t)
        ro, ri = [], []
        for j in range(nj):
            th = (2 * math.pi * j / n_arc) if closed else (-0.5 + j / n_arc) * arc
            ext = point * max(0.0, math.cos(th)) ** 3 * t ** 2 if point else 0.0
            d = z * math.cos(th) + x * math.sin(th)
            base = a + y * (L * t + ext)
            ro.append(base + d * r)
            ri.append(base + d * (r - thick))
        outer.append(ro)
        inner.append(ri)
    _slab(b, mat, outer, inner, closed=closed)
    return outer


def _spike(b, mat, base, direction, r, h, segs=4):
    """A gadling: a small pyramid standing on `base`, pointing along `direction`."""
    d = Vector(direction).normalized()
    tmp = bmesh.new()
    bmesh.ops.create_cone(tmp, cap_ends=True, segments=segs, radius1=r, radius2=0.0002, depth=h)
    q = d.to_track_quat('Z', 'Y').to_matrix().to_4x4()
    bmesh.ops.transform(tmp, matrix=Matrix.Translation(Vector(base) + d * (h / 2)) @ q, verts=tmp.verts)
    b.add_bmesh(tmp, mat, smooth=False)


def gauntlet(b, style):
    """A medieval plate gauntlet closed round the (hidden) controller, in the lancer's grip frame: overlapping lames
    on every finger with domed joint plates, a knuckle plate, a plate over the back of the hand and a flared bell cuff
    with rolled edges, over a leather glove. style: 0 STRIKER (gothic: fluted back plate, pointed finger tips and cuff,
    small gadlings), 1 TITAN (heavy: thick plates, a raised ridge, spiked knuckles, wide cuff), 2 PHANTOM (sleek:
    thin plates, glowing inlays)."""
    U = Vector((0, 0.5, 0.866))
    F = Vector((0, 0.866, -0.5))
    R = Vector((1, 0, 0))

    def P(r, f, u):
        return R * r + F * f + U * u

    def radial(q):   # away from the handle axis
        return (q - U * U.dot(q)).normalized()
    plate, trim = 'gaunt_shell', 'gaunt_trim'
    core = 'gaunt_dark' if style == 2 else 'glove'
    heavy = style == 1
    t_pl = (0.0016, 0.0024, 0.0012)[style]
    edge = (0.0013, 0.0019, 0.0010)[style]
    fr = Matrix((R, F, U)).transposed()
    # the leather glove: a rounded palm from the wrist to the knuckles (the handle passes through it), the thumb's root
    _shaped(b, core, P(0.016, -0.014, -0.003), (0.020, 0.032, 0.027), fr, segs=20, rings=12)
    # a dark grip closed in the fist (where the controller's handle is), capped flush with the fist
    _shaped(b, 'gaunt_dark', P(0, 0, 0.000), (1, 1, 1), fr, kind='cone', segs=20, depth=0.068, r1=0.0160, r2=0.0160)
    for uu in (-0.034, 0.034):
        _shaped(b, 'gaunt_dark', P(0, 0, uu), (1, 1, 1), fr, kind='cone', segs=20, depth=0.003, r1=0.0168, r2=0.0168)

    # ---- fingers: a leather core, two overlapping lames on the first two phalanges, one on the tip, domed joints
    us = (0.031, 0.011, -0.009, -0.027)
    ws = (0.0185, 0.0192, 0.0180, 0.0158)
    ls = (1.0, 1.06, 1.0, 0.86)
    knuckles = []
    for k in range(4):
        u, w, sc = us[k], ws[k], ls[k]
        mcp = Vector((0.034, 0.030))
        pip = mcp + Vector((-0.036, 0.024)) * sc
        dip = pip + Vector((-0.028, -0.016)) * sc
        tip = dip + Vector((-0.004, -0.020)) * sc
        pts = [P(q.x, q.y, u - 0.0015 * i) for i, q in enumerate((mcp, pip, dip, tip))]
        knuckles.append(pts[0])
        rc = w * 0.44
        rcs = (rc, rc * 0.95, rc * 0.88, rc * 0.80)
        _tube(b, core, pts, rcs, segs=8)
        for i in range(3):
            a_, c_ = pts[i], pts[i + 1]
            out = radial((a_ + c_) / 2)
            rr = rcs[i] + 0.0009 + (0.0006 if heavy else 0.0)
            spans = (((0.06, 0.40), (0.34, 0.68), (0.62, 1.02)), ((0.02, 0.54), (0.47, 1.02)), ((0.02, 1.0),))[i]
            for t0, t1 in spans:   # narrow scales over the back of the finger, each a step proud of the next
                tip_pt = 0.0065 if (i == 2 and style == 0) else 0.0
                _lame(b, plate, a_ + (c_ - a_) * t0, a_ + (c_ - a_) * t1, rr + 0.0017, math.radians(150), t_pl, out,
                      n_arc=6, n_len=2, r_end=rr, point=tip_pt)
        if style != 0:
            b.sphere(plate, tuple(pts[3]), rcs[3] + 0.0011, segs=10, rings=6)
        else:
            b.sphere(core, tuple(pts[3]), rcs[3], segs=8, rings=5)

    # ---- knuckle plate across the four knuckles (round an axis through the hand, parallel to the handle)
    kc = P(0.008, 0.004, 0.0)
    kdir = (R * 0.70 + F * 0.71).normalized()
    krad = (0.0475, 0.0505, 0.0465)[style]
    ko = _lame(b, plate, kc + U * -0.035, kc + U * 0.041, krad, math.radians((34, 40, 30)[style]), t_pl * 1.4, kdir,
               n_arc=5, n_len=10, bulge=0.0025)
    for col in (0, -1):   # rolled edges
        _tube(b, trim, [row[col] for row in ko], edge, segs=6)
    for q in knuckles:
        on = kc + U * U.dot(q - kc)
        d = (q - on).normalized()
        base = on + d * (krad + 0.0025 * math.sin(math.pi * (U.dot(q - kc) + 0.035) / 0.076) - 0.0005)
        if style == 0:
            _spike(b, trim, base, d, 0.0042, 0.0075)
        elif style == 1:
            _spike(b, trim, base, d, 0.0060, 0.0130)
    if style == 2:
        _tube(b, 'gaunt_glow', [row[2] + (row[2] - kc - U * U.dot(row[2] - kc)).normalized() * 0.0006 for row in ko],
              0.0008, segs=4)

    # ---- plate over the back of the hand, curving up into the knuckle plate
    nf, nu_ = 7, 8
    f0, f1, ua, ub = -0.046, 0.022, -0.036, 0.040
    def r_back(fn, un):
        return 0.0372 + (0.0078 if heavy else 0.0064) * (1 - un * un) - 0.0012 * fn * fn + 0.0080 * max(0.0, fn) ** 2.2
    outer, inner = [], []
    for i in range(nf + 1):
        fn = 2 * i / nf - 1
        f = f0 + (f1 - f0) * i / nf
        ro, ri = [], []
        for j in range(nu_ + 1):
            un = 2 * j / nu_ - 1
            uu = ua + (ub - ua) * j / nu_
            r = r_back(fn, un)
            ro.append(P(r, f, uu))
            ri.append(P(r - t_pl * 1.3, f, uu))
        outer.append(ro)
        inner.append(ri)
    _slab(b, plate, outer, inner)
    _tube(b, trim, outer[0], edge * 1.2, segs=6)                      # rolled wrist edge
    um, uh = (ua + ub) / 2, (ub - ua) / 2
    if style == 0:   # gothic fluting fanning out from the wrist
        for un in (-0.55, 0.0, 0.55):
            line = []
            for i in range(9):
                fn = -0.9 + 1.6 * i / 8
                uu = um + uh * un * (0.55 + 0.45 * (fn + 1) / 2)
                line.append(P(r_back(fn, (uu - um) / uh) + 0.0004, f0 + (f1 - f0) * (fn + 1) / 2, uu))
            _tube(b, plate, line, 0.0012, segs=6)
    elif style == 1:   # a raised central ridge and rivets
        _tube(b, plate, [P(r_back(fn, 0) + 0.0006, f0 + (f1 - f0) * (fn + 1) / 2, um) for fn in np.linspace(-0.92, 0.7, 9)],
              0.0022, segs=6)
        for fn in (-0.75, 0.35):
            for un in (-0.78, 0.78):
                b.sphere(trim, tuple(P(r_back(fn, un), f0 + (f1 - f0) * (fn + 1) / 2, um + uh * un)), 0.0024, segs=8, rings=4)
    else:            # glowing inlays
        for un in (-0.5, 0.5):
            _tube(b, 'gaunt_glow', [P(r_back(fn, un) + 0.0003, f0 + (f1 - f0) * (fn + 1) / 2, um + uh * un)
                                    for fn in np.linspace(-0.85, 0.6, 8)], 0.0007, segs=4)

    # ---- thumb: core, lames, domed joints
    th_root = P(0.014, -0.034, 0.024)
    th_pts = [P(-0.008, -0.016, 0.040), P(-0.027, 0.006, 0.047), P(-0.041, 0.028, 0.034), P(-0.041, 0.043, 0.016)]
    trs = (0.0108, 0.0100, 0.0092, 0.0084)
    _tube(b, core, [th_root] + th_pts, (0.0125,) + trs, segs=8)
    # the thumb wing: a plate over the root of the thumb, from the back plate to the first joint
    wo = (U * 0.8 - R * 0.35 - F * 0.2).normalized()
    _lame(b, plate, th_root + (th_root - th_pts[0]) * 0.15, th_pts[0] + (th_pts[1] - th_pts[0]) * 0.15, 0.0150, math.radians(170),
          t_pl * 1.2, wo, n_arc=8, n_len=3, r_end=0.0128, bulge=0.0015)
    for i in range(3):
        a_, c_ = th_pts[i], th_pts[i + 1]
        mid = (a_ + c_) / 2
        out = (mid - U * U.dot(mid) + U * 0.02).normalized()
        rr = trs[i] + 0.0009 + (0.0006 if heavy else 0.0)
        spans = (((0.05, 0.40), (0.34, 0.68), (0.62, 1.02)), ((0.02, 0.55), (0.48, 1.02)), ((0.02, 1.0),))[i]
        for t0, t1 in spans:
            _lame(b, plate, a_ + (c_ - a_) * t0, a_ + (c_ - a_) * t1, rr + 0.0017, math.radians(165), t_pl, out,
                  n_arc=7, n_len=2, r_end=rr, point=0.006 if (i == 2 and style == 0) else 0.0)
    b.sphere(plate if style != 0 else core, tuple(th_pts[3]), trs[3] + (0.0011 if style != 0 else 0.0), segs=10, rings=6)

    # ---- wrist: leather sleeve, two lames over the wrist, a flared bell cuff with rolled rims
    wrist = Vector((0.004, -0.050, -0.040))
    fwd = Matrix.Rotation(math.pi / 2, 3, 'X')
    rb = (0.038, 0.042, 0.035)[style]
    _shaped(b, core, wrist + Vector((0, -0.045, -0.004)), (1, 1, 1), fwd, kind='cone', segs=24, depth=0.085,
            r1=rb - 0.003, r2=rb - 0.005)
    up_out = Vector((1.0, 0.0, 0.35)).normalized()
    for k, (y0, y1) in enumerate(((0.018, 0.005), (0.009, -0.004))):
        _lame(b, plate, wrist + Vector((0, y0, 0.004)), wrist + Vector((0, y1, 0.002)), rb + 0.0015 + 0.0016 * k,
              math.radians(190), t_pl, up_out, n_arc=10, n_len=1)
    a_c = wrist + Vector((0, -0.003, -0.001))
    c_c = wrist + Vector((0, -0.086, -0.008))
    flare = (0.017, 0.024, 0.011)[style]
    co = _lame(b, plate, a_c, c_c, rb + 0.002, 0.0, t_pl * 1.3, up_out, n_arc=28, n_len=5, r_end=rb + 0.002 + flare, ease=2.0,
               point=(0.022, 0.0, 0.012)[style], closed=True)
    ax = (c_c - a_c).normalized()
    _tube(b, trim, co[-1], edge * 1.8, segs=6, closed=True, up=ax)                        # rolled rim at the elbow end
    _tube(b, trim, co[0], edge * 1.3, segs=6, closed=True, up=ax)                         # and at the wrist
    mid = [q + (q - a_c - ax * ax.dot(q - a_c)).normalized() * 0.0006 for q in co[2]]
    _tube(b, 'gaunt_glow', mid, 0.0008, segs=4, closed=True, up=ax)                       # a thin glowing line


def gauntlet_0(b):
    gauntlet(b, 0)


def gauntlet_1(b):
    gauntlet(b, 1)


def gauntlet_2(b):
    gauntlet(b, 2)


def knuckleguard(b):
    """Rounded glass shell over the bumper box: centre (-0.022, 0.137, 0.010), half (0.064, 0.029, 0.048)."""
    tmp = bmesh.new()
    bmesh.ops.create_cube(tmp, size=1.0)
    bmesh.ops.scale(tmp, vec=(0.128, 0.058, 0.096), verts=tmp.verts)
    bmesh.ops.bevel(tmp, geom=tmp.edges[:] + tmp.verts[:], offset=0.022, segments=4, profile=0.5, affect='EDGES',
                    clamp_overlap=True)
    bmesh.ops.translate(tmp, vec=(-0.022, 0.137, 0.010), verts=tmp.verts)
    b.add_bmesh(tmp, 'knuckle_glass', smooth=True)


def shield(b):
    """Hexagonal energy shield, 0.40 x 0.64 m, hex texture + bright rim."""
    w, h = 0.20, 0.32
    hexpts = [Vector((w * math.cos(a), h * math.sin(a) * 1.0, 0)) for a in [math.radians(30 + 60 * k) for k in range(6)]]
    tmp = bmesh.new()
    lay = tmp.loops.layers.uv.new('UVMap')
    cv = tmp.verts.new((0, 0, 0.012))
    rim = [tmp.verts.new((p.x, p.y, 0.0)) for p in hexpts]
    for k in range(6):
        f = tmp.faces.new((cv, rim[k], rim[(k + 1) % 6]))
        for loop in f.loops:
            co = loop.vert.co
            loop[lay].uv = (0.5 + co.x / 0.40 * 1.6, 0.5 + co.y / 0.64 * 1.6 / 1.73)
    b.add_bmesh(tmp, 'shield_hex', smooth=False)
    for k in range(6):
        a, c = hexpts[k], hexpts[(k + 1) % 6]
        d = c - a
        rot = d.to_track_quat('Y', 'Z').to_matrix()
        b.box('shield_rim', tuple((a + c) / 2), (0.008, d.length + 0.008, 0.008), rot=rot)


def head(b):
    """Android head: smooth helmet, wraparound visor, ear pods, a crest fin."""
    b.sphere('avatar_shell', (0, -0.035, 0.03), 0.125, segs=32, rings=18, scale=(0.92, 1.12, 1.08))
    b.sphere('avatar_visor', (0, -0.005, 0.0), 0.121, segs=32, rings=12, scale=(0.95, 1.06, 0.42))
    for s in (-1, 1):
        b.cylinder('avatar_dark', (s * 0.112, -0.035, 0.0), 0.04, 0.03, segs=20, axis='X')
        b.torus('avatar_glow', (s * 0.128, -0.035, 0.0), 0.028, 0.003, segs=24, tsegs=5, axis='X')
    b.box('avatar_dark', (0, -0.06, 0.155), (0.012, 0.16, 0.03), bevel=0.005)
    b.cylinder('avatar_dark', (0, -0.05, -0.14), 0.045, 0.08, segs=16)


def torso(b):
    """Floating android torso (the head sits at 1.66 m): chest, shoulder pads, waist, chest light."""
    tmp = bmesh.new()
    bmesh.ops.create_cube(tmp, size=1.0)
    bmesh.ops.scale(tmp, vec=(0.34, 0.20, 0.36), verts=tmp.verts)
    bmesh.ops.bevel(tmp, geom=tmp.edges[:] + tmp.verts[:], offset=0.06, segments=4, profile=0.6, affect='EDGES',
                    clamp_overlap=True)
    bmesh.ops.translate(tmp, vec=(0, -0.04, 1.26), verts=tmp.verts)
    b.add_bmesh(tmp, 'avatar_shell', smooth=True)
    b.sphere('avatar_dark', (0, -0.04, 1.02), 0.13, segs=24, rings=12, scale=(1.05, 0.75, 0.9))
    for s in (-1, 1):
        b.sphere('avatar_shell', (s * 0.2, -0.04, 1.40), 0.075, segs=20, rings=10, scale=(1.0, 1.0, 0.8))
        b.torus('avatar_dark', (s * 0.2, -0.04, 1.40), 0.07, 0.008, segs=24, tsegs=5, axis='X')
    b.box('avatar_glow', (0, 0.063, 1.30), (0.12, 0.004, 0.012))
    b.box('avatar_glow', (0, 0.063, 1.255), (0.07, 0.004, 0.008))
    b.cylinder('avatar_dark', (0, -0.04, 1.49), 0.045, 0.09, segs=16)


PROPS = [('projectile', ball), ('projectile_shell', shell), ('spawner', spawner), ('strikezone', strikezone),
         ('lancer', lancer), ('gauntlet_0', gauntlet_0), ('gauntlet_1', gauntlet_1), ('gauntlet_2', gauntlet_2),
         ('knuckleguard', knuckleguard), ('shield', shield), ('head', head), ('torso', torso)]


def main():
    out = common.out_path() or os.path.join(os.getcwd(), 'props.npz')
    common.reset_scene()
    pos, nrm, uv0, uv1, mat, names, objects = [], [], [], [], [], [], []
    for name, fn in PROPS:
        bld = common.Builder(name)
        fn(bld)
        objs = bld.to_objects()
        for obj in objs.values():
            me = obj.data
            me.calc_loop_triangles()
            uvs = np.zeros(len(me.loops) * 2, np.float32)
            if 'UVMap' in me.uv_layers:
                me.uv_layers['UVMap'].data.foreach_get('uv', uvs)
            uvs = uvs.reshape(-1, 2)
            base = len(names)
            for m in me.materials:
                names.append(m.name)
                objects.append(name)
            for tri in me.loop_triangles:
                poly = me.polygons[tri.polygon_index]
                for k in range(3):
                    li = tri.loops[k]
                    vi = me.loops[li].vertex_index
                    p = me.vertices[vi].co
                    n = me.vertices[vi].normal if poly.use_smooth else poly.normal
                    pos.append((p.x, p.z, -p.y))       # app / frame space: x right, y up, -z forward
                    nrm.append((n.x, n.z, -n.y))
                    uv0.append((uvs[li][0], 1.0 - uvs[li][1]))
                    uv1.append((0.0, 0.0))
                    mat.append(base + poly.material_index)
    np.savez_compressed(out, pos=np.array(pos, np.float32), nrm=np.array(nrm, np.float32), uv0=np.array(uv0, np.float32),
                        uv1=np.array(uv1, np.float32), mat=np.array(mat, np.int32), names=np.array(names),
                        objects=np.array(objects))
    print('exported', out, len(pos) // 3, 'tris')


main()
