"""Project Arena arena: the court and the stadium around it, built and lit together in one Cycles scene.

Court (gameplay dimensions from the recovered collision): interior 19 x 5 x 4 m, foul zones |x| 4.75..9.5 m,
team 0 (blue) at -X, team 1 (orange) at +X. The design is original: polished dark floor with a pale runway, steel
ribs with light inlays, glass bays to a luminous light-slat gallery, hex-energy end zones, holographic targets.

Outputs (npz, app space): <out>/court.npz (lit + emit + glass + fx, court lightmap) and <out>/environment.npz.
Run: blender -b --factory-startup -P tools/blender/make_arena.py -- <out_dir> [preview]
"""
import math
import os
import random
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import bpy  # noqa: E402
from mathutils import Matrix, Vector  # noqa: E402

import common  # noqa: E402

HX, HY, H = 9.5, 2.5, 4.0
FOUL = 4.75
RIBS = [-9.5, -7.125, -4.75, -2.375, 0.0, 2.375, 4.75, 7.125, 9.5]
SQ3 = math.sqrt(3.0)
rnd = random.Random(7)


def tm(x):
    return 't0' if x < 0 else 't1'


def halo_line(b, mat, a, c, normal, width, glow):
    """Additive soft halo card along segment a->c (in the plane facing `normal`), `glow` metres wide."""
    a, c, n = Vector(a), Vector(c), Vector(normal).normalized()
    d = (c - a)
    side = d.cross(n).normalized() * glow / 2
    off = n * 0.004
    b.quad(mat, (a - side + off, a + side + off, c + side + off, c - side + off),
           uv=((0, 0), (1, 0), (1, 1), (0, 1)))


def hex_card(b, mat, center, u_axis, v_axis, su, sv, cell=0.30):
    """Hex overlay card; the texture covers 4 x 4*sqrt3 cells, so tile it at the right aspect."""
    tu = su / (4 * cell)
    tv = sv / (4 * cell * SQ3)
    b.card(mat, center, u_axis, v_axis, su, sv, uv=((0, 0), (tu, 0), (tu, tv), (0, tv)))


# =============================================================================== court
def neon(b, a, c, tone, r=0.011, glow=0.13, pulse=True):
    """A glowing neon tube from a to c: emissive core, two crossed soft halo cards, and an animated energy pulse
    card (court_pulse layer, scrolled by the app). tone: 'white' or 't0' / 't1'."""
    a, c = Vector(a), Vector(c)
    d = c - a
    ln = d.length
    if ln < 1e-4:
        return
    ax = d.normalized()
    mid = (a + c) / 2
    rot = ax.to_track_quat('Z', 'Y').to_matrix().to_4x4()
    core = 'neon_' + tone
    import bmesh
    tmp = bmesh.new()
    bmesh.ops.create_cone(tmp, cap_ends=True, segments=10, radius1=r, radius2=r, depth=ln + r)
    bmesh.ops.transform(tmp, matrix=Matrix.Translation(mid) @ rot, verts=tmp.verts)
    b.add_bmesh(tmp, core, smooth=True)
    side1 = ax.cross(Vector((0, 0, 1)) if abs(ax.z) < 0.9 else Vector((1, 0, 0))).normalized()
    side2 = ax.cross(side1).normalized()
    halo = 'halo' if tone == 'white' else 'halo_' + tone
    for sd in (side1, side2):
        b.card(halo, mid, sd, ax, glow, ln + glow * 0.5, double=True)
    if pulse:
        reps = max(1.0, round(ln / 1.2))
        b.card('pulse_' + tone, mid, side1, ax, glow * 0.55, ln, uv=((0, 0), (1, 0), (1, reps), (0, reps)), double=True)


def build_court(b):
    """A glass arena drawn in neon: transparent walls and roof (spectators see straight in), neon tubes on every
    edge and rib (white-cyan mid court, team colour in the end zones), a polished floor with a faint neon grid."""
    # ---- floor: polished dark tiles; seams are a faint neon grid (lit floor seams + additive glow)
    nx, ny, gap = 16, 4, 0.014
    tx, ty = 2 * HX / nx, 2 * HY / ny
    b.box('floor_seam', (0, 0, -0.02), (2 * HX, 2 * HY, 0.02))
    for i in range(nx):
        for j in range(ny):
            cx, cy = -HX + tx * (i + 0.5), -HY + ty * (j + 0.5)
            if abs(cx) > FOUL:
                name = 'floor_end_' + tm(cx)
            else:
                name = 'floor_runway' if j in (1, 2) else 'floor_tile'
            b.box(name, (cx, cy, -0.006), (tx - gap, ty - gap, 0.012), bevel=0.003, segments=1)
    for i in range(1, nx):
        x = -HX + tx * i
        b.card('grid_' + ('white' if abs(x) < FOUL - 0.01 else tm(x)), (x, 0, 0.001), (1, 0, 0), (0, 1, 0), 0.06, 2 * HY)
    for j in range(1, ny):
        y = -HY + ty * j
        for x0, x1, tone in ((-HX, -FOUL, 't0'), (-FOUL, FOUL, 'white'), (FOUL, HX, 't1')):
            b.card('grid_' + tone, ((x0 + x1) / 2, y, 0.001), (0, 1, 0), (1, 0, 0), 0.06, x1 - x0)
    # floor markings: foul lines, centre line, player areas, centre emblem
    for x in (-FOUL, FOUL):
        b.box('foul_' + tm(x), (x, 0, 0.0015), (0.05, 2 * HY - 0.01, 0.003))
        halo_line(b, 'halo_' + tm(x), (x, -HY + 0.02, 0.002), (x, HY - 0.02, 0.002), (0, 0, 1), 0.05, 0.5)
    for k in range(-5, 6):
        b.box('center_mark', (0, k * 0.42, 0.001), (0.025, 0.24, 0.002))
    for x in (-7.0, 7.0):
        b.ring('area_' + tm(x), (x, 0, 0.0015), 0.86, 0.90, 0.003, segs=96)
        b.ring('halo_' + tm(x), (x, 0, 0.003), 0.78, 0.98, 0.0, segs=96)
    b.disc('emblem_fx', (0, 0, 0.003), 1.05, segs=64)

    # ---- glass: side walls, roof, back walls (team-tinted in the end zones), hex energy on the end-zone glass
    for side in (-1, 1):
        y = side * HY
        for x0, x1 in ((-HX, -FOUL), (-FOUL, FOUL), (FOUL, HX)):
            xc = 0.5 * (x0 + x1)
            g = 'glass' if abs(xc) < FOUL else 'glass_' + tm(xc)
            b.quad(g, ((x0, y, 0.0), (x1, y, 0.0), (x1, y, H), (x0, y, H)))
            if abs(xc) > FOUL:
                hex_card(b, 'hex_' + tm(xc), (xc, y - side * 0.004, H / 2), (side, 0, 0), (0, 0, 1), x1 - x0 - 0.04, H - 0.04)
    for x0, x1 in ((-HX, -FOUL), (-FOUL, FOUL), (FOUL, HX)):
        xc = 0.5 * (x0 + x1)
        g = 'glass' if abs(xc) < FOUL else 'glass_' + tm(xc)
        b.quad(g, ((x0, -HY, H), (x1, -HY, H), (x1, HY, H), (x0, HY, H)))
        if abs(xc) > FOUL:
            hex_card(b, 'hex_' + tm(xc), (xc, 0, H - 0.004), (1, 0, 0), (0, -1, 0), x1 - x0 - 0.04, 2 * HY - 0.04)
    for side in (-1, 1):
        x = side * HX
        t = tm(x)
        b.quad('glass_back_' + t, ((x, -HY, 0), (x, HY, 0), (x, HY, H), (x, -HY, H)))
        b.disc('holo_' + t, (x - side * 0.02, 0, 1.45), 1.35, segs=64, axis='X', flip=side > 0)
        b.disc('holo_' + t, (x - side * 0.02, 0, 1.45), 1.35, segs=64, axis='X', flip=side < 0)

    # ---- neon: the arena's outline
    def zone(x0, x1):
        return 'white' if abs((x0 + x1) / 2) < FOUL else tm((x0 + x1) / 2)
    for y in (-HY, HY):
        for z in (0.0, H):
            for x0, x1 in ((-HX, -FOUL), (-FOUL, FOUL), (FOUL, HX)):
                neon(b, (x0, y, z), (x1, y, z), zone(x0, x1), r=0.014, glow=0.16)
    for x in RIBS:
        tone = 'white' if abs(x) < FOUL - 0.01 else tm(x)
        big = abs(abs(x) - FOUL) < 0.01 or abs(abs(x) - HX) < 0.01
        r, gl = (0.016, 0.2) if big else (0.008, 0.10)
        for y in (-HY, HY):
            neon(b, (x, y, 0), (x, y, H), tone, r=r, glow=gl)
        neon(b, (x, -HY, H), (x, HY, H), tone, r=r, glow=gl)
        if abs(abs(x) - HX) < 0.01:
            neon(b, (x, -HY, 0), (x, HY, 0), tone, r=r, glow=gl)
    # inner portal outline on the back walls (frames the target)
    for side in (-1, 1):
        x = side * (HX - 0.01)
        t = tm(x)
        z0, z1, y0 = 0.45, H - 0.3, HY - 0.3
        for a, c in (((x, -y0, z0), (x, y0, z0)), ((x, -y0, z1), (x, y0, z1)), ((x, -y0, z0), (x, -y0, z1)), ((x, y0, z0), (x, y0, z1))):
            neon(b, a, c, t, r=0.006, glow=0.08, pulse=False)
    # a subtle waist-height line along the mid-court glass
    for y in (-HY, HY):
        neon(b, (-FOUL, y, 1.1), (FOUL, y, 1.1), 'white', r=0.004, glow=0.05, pulse=False)


# =============================================================================== stadium
def build_environment(b):
    # dais: a rim around the court footprint (open underneath, so the floor reflection shows through)
    for y0, y1 in ((-5.5, -2.6), (2.6, 5.5)):
        b.box('dais', (0, (y0 + y1) / 2, -0.30), (25.0, y1 - y0, 0.56), bevel=0.03)
    for x0, x1 in ((-12.5, -9.6), (9.6, 12.5)):
        b.box('dais', ((x0 + x1) / 2, 0, -0.30), (x1 - x0, 5.2, 0.56), bevel=0.03)
    for y in (-5.5, 5.5):
        b.box('light_line', (0, y + (0.02 if y > 0 else -0.02), -0.04), (25.0, 0.02, 0.02))
    for x in (-12.5, 12.5):
        b.box('light_line', (x + (0.02 if x > 0 else -0.02), 0, -0.04), (0.02, 11.0, 0.02))

    # luminous light-slat galleries around the court (what you see through the glass)
    for side in (-1, 1):
        y = side * 7.6
        b.box('structure', (0, y + side * 0.15, 1.7), (28.0, 0.2, 4.8))
        for k in range(7):
            xc = -12.0 + k * 4.0
            b.card('slats' if k % 2 == 0 else 'slats_v', (xc, y, 1.9), (side, 0, 0), (0, 0, 1), 3.7, 3.4,   # faces the court
                   uv=((0, 0), (2.3, 0), (2.3, 1), (0, 1)))
            b.box('structure', (xc + 2.0, y - side * 0.05, 1.7), (0.3, 0.3, 4.8), bevel=0.02)
        b.box('structure', (0, y - side * 0.05, 3.75), (28.0, 0.3, 0.3), bevel=0.02)
        b.box('structure', (0, y - side * 0.05, 0.05), (28.0, 0.3, 0.3), bevel=0.02)
    for side in (-1, 1):
        x = side * 13.6
        b.box('structure', (x + side * 0.15, 0, 1.7), (0.2, 15.2, 4.8))
        for k in range(4):
            yc = -5.7 + k * 3.8
            b.card('slats', (x, yc, 1.9), (0, -side, 0), (0, 0, 1), 3.5, 3.4, uv=((0, 0), (2.2, 0), (2.2, 1), (0, 1)))

    # stands rising behind and above the galleries, with a crowd and their lights
    def seat_row(p0, p1, z, facing, twinkle_team):
        n = max(2, int((Vector(p1) - Vector(p0)).length / 0.62))
        for i in range(n):
            if rnd.random() < 0.28:
                continue
            p = Vector(p0).lerp(Vector(p1), (i + 0.5) / n)
            hgt = 0.48 + rnd.random() * 0.12
            f = Vector(facing)
            body_c = p + Vector((0, 0, z + hgt / 2 + 0.05))
            b.box('crowd', body_c, (0.36 if abs(f.y) > 0.5 else 0.30, 0.30 if abs(f.y) > 0.5 else 0.36, hgt))
            b.sphere('crowd', body_c + Vector((0, 0, hgt / 2 + 0.11)), 0.095, segs=6, rings=4, smooth=True)
            if rnd.random() < 0.33:
                lc = body_c + f * 0.2 + Vector((0, 0, hgt / 2 + rnd.random() * 0.35))
                u = rnd.random()
                tw = rnd.random()
                mat = 'twinkle_' + (twinkle_team if tw < 0.7 else 'w')
                up = Vector((0, 0, 1))
                side_ax = f.cross(up)
                b.card(mat, lc, side_ax, up, 0.06, 0.06, uv=((u, 0), (u, 0), (u, 1), (u, 1)), double=True)

    tiers = 9
    for side in (-1, 1):
        for k in range(tiers):
            y = side * (8.4 + k * 0.95)
            z = 4.2 + k * 0.62
            length = 30.0 + k * 1.6
            b.box('stand', (0, y, z - 0.25), (length, 0.95, 0.5), bevel=0.02)
            b.box('stand_edge', (0, y - side * 0.47, z - 0.02), (length, 0.04, 0.04))
            seat_row((-length / 2 + 0.6, y + side * 0.1, 0), (length / 2 - 0.6, y + side * 0.1, 0), z,
                     (0, -side, 0), 't0' if rnd.random() < 0.5 else 't1')
        b.box('structure', (0, side * (8.4 + tiers * 0.95 + 0.3), 4.2 + tiers * 0.62 / 2), (36.0, 0.4, tiers * 0.62 + 1.0))
    for side in (-1, 1):
        team = 't0' if side < 0 else 't1'
        for k in range(tiers):
            x = side * (14.4 + k * 0.95)
            z = 4.2 + k * 0.62
            width = 15.0 + k * 1.9
            b.box('stand', (x, 0, z - 0.25), (0.95, width, 0.5), bevel=0.02)
            b.box('stand_edge', (x - side * 0.47, 0, z - 0.02), (0.04, width, 0.04))
            seat_row((x + side * 0.1, -width / 2 + 0.6, 0), (x + side * 0.1, width / 2 - 0.6, 0), z, (-side, 0, 0), team)

    # overhead rig: truss ring with spotlights and a four-sided screen above centre court
    zr = 9.0
    for (a, c) in (((-11, -5.5), (11, -5.5)), ((-11, 5.5), (11, 5.5)), ((-11, -5.5), (-11, 5.5)), ((11, -5.5), (11, 5.5))):
        mid = ((a[0] + c[0]) / 2, (a[1] + c[1]) / 2, zr)
        size = (abs(c[0] - a[0]) + 0.5, abs(c[1] - a[1]) + 0.5, 0.45)
        b.box('structure', mid, size, bevel=0.04)
    for i in range(10):
        for yy in (-5.5, 5.5):
            x = -9.9 + i * 2.2
            b.cylinder('structure', (x, yy, zr - 0.35), 0.13, 0.32, segs=12)
            b.disc('light_panel', (x, yy, zr - 0.515), 0.10, segs=12, flip=True)
            # light beam cone toward the court (additive, both faces)
            tgt = Vector((x * 0.55, yy * 0.15, 0.0))
            src = Vector((x, yy, zr - 0.52))
            d = (tgt - src)
            d = d * ((zr - 0.52 - 4.6) / (zr - 0.52))   # stop above the court roof: shafts read from below
            ln = d.length
            ax = d.normalized()
            sideax = ax.cross(Vector((1, 0, 0))).normalized()
            mid = src + ax * (ln / 2)
            b.card('beam', mid, sideax, ax, 0.6, ln, uv=((0, 0), (1, 0), (1, 1), (0, 1)), double=True)
            sideax2 = ax.cross(sideax).normalized()
            b.card('beam', mid, sideax2, ax, 0.6, ln, uv=((0, 0), (1, 0), (1, 1), (0, 1)), double=True)
    # screen block hanging over centre court
    zs = 8.0
    b.box('structure', (0, 0, zs), (3.4, 3.4, 1.9), bevel=0.05)
    for ang in range(4):
        a = ang * math.pi / 2
        n = Vector((math.cos(a), math.sin(a), 0))
        u = Vector((-math.sin(a), math.cos(a), 0))
        b.card('screen', Vector((0, 0, zs)) + n * 1.705, u, (0, 0, 1), 3.2, 1.6)
    b.cylinder('structure', (0, 0, zs + 3.0), 0.06, 4.2, segs=8)

    # sky
    b.sphere('sky', (0, 0, 0), 300.0, segs=48, rings=24, smooth=True)


def flip_sky_normals(objs):
    o = objs.get('prop')
    if o is None:
        return
    import bmesh
    bm = bmesh.new()
    bm.from_mesh(o.data)
    bmesh.ops.reverse_faces(bm, faces=bm.faces[:])
    bm.to_mesh(o.data)
    bm.free()


def main():
    out_dir = common.out_path() or os.getcwd()
    preview = 'preview' in sys.argv
    common.reset_scene()
    court = common.Builder('Court')
    build_court(court)
    court_objs = court.to_objects()
    env = common.Builder('Env')
    build_environment(env)
    env_objs = env.to_objects()
    flip_sky_normals(env_objs)
    size, samples = (1024, 48) if preview else (2048, 384)
    lm_court = common.lightmap_bake(court_objs['lit'], size=size, samples=samples)
    lm_env = common.lightmap_bake(env_objs['lit'], size=size, samples=max(32, samples // 2))
    common.export_mesh(os.path.join(out_dir, 'court.npz'), list(court_objs.values()), lightmap=lm_court)
    common.export_mesh(os.path.join(out_dir, 'environment.npz'), list(env_objs.values()), lightmap=lm_env)


main()
