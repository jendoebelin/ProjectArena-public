"""Generate an original (CCP-asset-free) art set for the Quest port, styled after Sparc's look:
dark glass court, team-coloured end zones, light-strip stands. Every mesh is built procedurally here.

Writes app/assets_clean/{arena,environment,projectile,projectile_shell,spawner,shield,lancer,knuckleguard,
head,torso,strikezone}.mdl and arena.col (same formats as tools/build_open_assets.py, so the app is unchanged).

Gameplay geometry (court 19 x 5 x 4 m, foul lines at +-4.73 m, spawners at +-6.48 m, zone tags) follows the
values documented in the developers' research notes; the meshes themselves are new.

Usage: python -I tools/gen_clean_assets.py
"""
import math, os, struct
import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
OUT = os.path.join(ROOT, 'app', 'assets_clean')

SH_LIT, SH_COURTMASK, SH_ADDITIVE, SH_UNLIT, SH_TRANSLUCENT = 0, 1, 2, 3, 4
TWOSIDED = 1
TAG_ARENA, TAG_RETURN, TAG_FOUL, TAG_TEAM0, TAG_TEAM1 = 1, 2, 4, 8, 16

BLUE = (0.10, 0.55, 1.00)
ORANGE = (1.00, 0.42, 0.10)

# court dimensions (app space: x along court, y up, z across)
HX, HZ, H = 9.5, 2.5, 4.0
FOUL_X = 4.73
CR = 0.45            # corner radius of the court tube
CSEG = 4


def mat(shader, tint=(1, 1, 1, 1), emissive=(0, 0, 0), flags=0):
    t = tuple(tint) + (1.0,) * (4 - len(tint))
    return dict(shader=shader, tint=t[:4], emissive=tuple(emissive) + (0.0,), uv=0, flags=flags, diffuse='', mask='')


class Mesh:
    def __init__(self):
        self.mats = []
        self.parts = {}   # mat index -> (verts list, idx list)

    def m(self, material):
        key = repr(sorted(material.items()))
        for i, mm in enumerate(self.mats):
            if repr(sorted(mm.items())) == key:
                return i
        self.mats.append(material)
        return len(self.mats) - 1

    def tri(self, mi, a, b, c, n=None):
        a, b, c = np.array(a, float), np.array(b, float), np.array(c, float)
        if n is None:
            n = np.cross(b - a, c - a)
            l = np.linalg.norm(n)
            n = n / l if l > 1e-9 else np.array([0, 1, 0.0])
        v, ix = self.parts.setdefault(mi, ([], []))
        base = len(v)
        for p in (a, b, c):
            v.append((p, n))
        ix += [base, base + 1, base + 2]

    def quad(self, mi, a, b, c, d):
        self.tri(mi, a, b, c)
        self.tri(mi, a, c, d)

    def box(self, mi, center, half, rot=None):
        c = np.array(center, float); hx, hy, hz = half
        R = np.eye(3) if rot is None else rot
        P = lambda x, y, z: c + R @ np.array([x, y, z])
        f = [((-1, -1, 1), (1, -1, 1), (1, 1, 1), (-1, 1, 1)), ((1, -1, -1), (-1, -1, -1), (-1, 1, -1), (1, 1, -1)),
             ((1, -1, 1), (1, -1, -1), (1, 1, -1), (1, 1, 1)), ((-1, -1, -1), (-1, -1, 1), (-1, 1, 1), (-1, 1, -1)),
             ((-1, 1, 1), (1, 1, 1), (1, 1, -1), (-1, 1, -1)), ((-1, -1, -1), (1, -1, -1), (1, -1, 1), (-1, -1, 1))]
        for face in f:
            self.quad(mi, *[P(x * hx, y * hy, z * hz) for x, y, z in face])

    def sphere(self, mi, center, r, seg=16, ring=10, scale=(1, 1, 1)):
        c = np.array(center, float)
        pts = [[c + np.array([r * scale[0] * math.sin(math.pi * i / ring) * math.cos(2 * math.pi * j / seg),
                              r * scale[1] * math.cos(math.pi * i / ring),
                              r * scale[2] * math.sin(math.pi * i / ring) * math.sin(2 * math.pi * j / seg)])
                for j in range(seg + 1)] for i in range(ring + 1)]
        for i in range(ring):
            for j in range(seg):
                a, b, cc, d = pts[i][j], pts[i][j + 1], pts[i + 1][j + 1], pts[i + 1][j]
                na = lambda p: (p - c) / max(np.linalg.norm(p - c), 1e-9)
                self._tri_n(mi, a, d, cc, [na(a), na(d), na(cc)])
                self._tri_n(mi, a, cc, b, [na(a), na(cc), na(b)])

    def _tri_n(self, mi, a, b, c, ns):
        v, ix = self.parts.setdefault(mi, ([], []))
        base = len(v)
        for p, n in zip((a, b, c), ns):
            v.append((np.array(p, float), np.array(n, float)))
        ix += [base, base + 1, base + 2]

    def torus(self, mi, center, axis, R, r, seg=48, tseg=8):
        c = np.array(center, float); ax = np.array(axis, float); ax /= np.linalg.norm(ax)
        u = np.cross(ax, [0, 1, 0] if abs(ax[1]) < 0.9 else [1, 0, 0]); u /= np.linalg.norm(u)
        w = np.cross(ax, u)
        def P(i, j):
            a, b = 2 * math.pi * i / seg, 2 * math.pi * j / tseg
            rc = u * math.cos(a) + w * math.sin(a)
            n = rc * math.cos(b) + ax * math.sin(b)
            return c + rc * R + n * r, n
        for i in range(seg):
            for j in range(tseg):
                (a, na), (b, nb), (cc, nc), (d, nd) = P(i, j), P(i + 1, j), P(i + 1, j + 1), P(i, j + 1)
                self._tri_n(mi, a, b, cc, [na, nb, nc])
                self._tri_n(mi, a, cc, d, [na, nc, nd])

    def disc(self, mi, center, normal, r0, r1, seg=48):
        c = np.array(center, float); ax = np.array(normal, float); ax /= np.linalg.norm(ax)
        u = np.cross(ax, [0, 1, 0] if abs(ax[1]) < 0.9 else [1, 0, 0]); u /= np.linalg.norm(u)
        w = np.cross(ax, u)
        for i in range(seg):
            a0, a1 = 2 * math.pi * i / seg, 2 * math.pi * (i + 1) / seg
            d0, d1 = u * math.cos(a0) + w * math.sin(a0), u * math.cos(a1) + w * math.sin(a1)
            self._tri_n(mi, c + d0 * r0, c + d1 * r1 if r0 == 0 else c + d0 * r1, c + d1 * r1, [ax] * 3) if r0 == 0 else \
                self.quad_n(mi, c + d0 * r0, c + d0 * r1, c + d1 * r1, c + d1 * r0, ax)

    def quad_n(self, mi, a, b, c, d, n):
        self._tri_n(mi, a, b, c, [n] * 3)
        self._tri_n(mi, a, c, d, [n] * 3)

    def write(self, path):
        verts, idx, secs = [], [], []
        for mi in sorted(self.parts):
            v, ix = self.parts[mi]
            first = len(idx)
            base = len(verts)
            verts += v
            idx += [base + i for i in ix]
            secs.append((first, len(ix), mi))
        with open(path, 'wb') as f:
            f.write(b'SMDL' + struct.pack('<5I', 2, len(verts), len(idx), len(secs), len(self.mats)))
            for p, n in verts:
                f.write(struct.pack('<3f3f2f2f4B', *p, *n, 0, 0, 0, 0, 255, 255, 255, 255))
            f.write(struct.pack('<%dI' % len(idx), *idx))
            for s in secs:
                f.write(struct.pack('<3I', *s))
            for mm in self.mats:
                f.write(struct.pack('<I4f4f2I', mm['shader'], *mm['tint'], *mm['emissive'], mm['uv'], mm['flags']))
                f.write(b'\0' * 128)
        P = np.array([p for p, _ in verts])
        print('  %-18s %6d verts %6d tris %2d mats  bounds %s..%s' % (os.path.basename(path), len(verts), len(idx) // 3,
              len(self.mats), P.min(0).round(2), P.max(0).round(2)))


# ---------------------------------------------------------------------------------------------------------- court
def section_profile():
    """Inner cross-section of the court tube (z, y) going around, with rounded corners."""
    pts = []
    corners = [((HZ - CR, H - CR), 0), ((-HZ + CR, H - CR), 90), ((-HZ + CR, CR), 180), ((HZ - CR, CR), 270)]
    for (cz, cy), a0 in corners:
        for k in range(CSEG + 1):
            a = math.radians(a0 + 90 * k / CSEG)
            pts.append((cz + CR * math.cos(a), cy + CR * math.sin(a)))
    pts.append(pts[0])   # close the loop (the z+ side wall)
    return pts


def build_arena():
    m = Mesh()
    floor = m.m(mat(SH_LIT, (0.30, 0.37, 0.42)))
    floor_dark = m.m(mat(SH_LIT, (0.16, 0.20, 0.23)))
    stripe = m.m(mat(SH_UNLIT, (0.55, 0.62, 0.66)))
    ceiling = m.m(mat(SH_LIT, (0.30, 0.33, 0.36)))
    light = m.m(mat(SH_UNLIT, (2.2, 2.1, 1.9)))
    frame = m.m(mat(SH_LIT, (0.05, 0.06, 0.07)))
    glass = m.m(mat(SH_TRANSLUCENT, (0.55, 0.72, 0.85, 0.12), flags=TWOSIDED))
    backwall = m.m(mat(SH_LIT, (0.02, 0.025, 0.035)))
    team_panel = [m.m(mat(SH_LIT, tuple(c * 0.75 for c in BLUE) + (1,), tuple(c * 0.12 for c in BLUE), flags=TWOSIDED)),
                  m.m(mat(SH_LIT, tuple(c * 0.75 for c in ORANGE) + (1,), tuple(c * 0.12 for c in ORANGE), flags=TWOSIDED))]
    team_dark = [m.m(mat(SH_LIT, tuple(c * 0.22 for c in BLUE) + (1,), flags=TWOSIDED)),
                 m.m(mat(SH_LIT, tuple(c * 0.22 for c in ORANGE) + (1,), flags=TWOSIDED))]
    team_glow = [m.m(mat(SH_ADDITIVE, tuple(c * 0.9 for c in BLUE) + (1,), flags=TWOSIDED)),
                 m.m(mat(SH_ADDITIVE, tuple(c * 0.9 for c in ORANGE) + (1,), flags=TWOSIDED))]
    foul = [m.m(mat(SH_ADDITIVE, tuple(c * 0.8 for c in BLUE) + (1,), flags=TWOSIDED)),
            m.m(mat(SH_ADDITIVE, tuple(c * 0.8 for c in ORANGE) + (1,), flags=TWOSIDED))]

    prof = section_profile()
    # floor (flat part) as tiles with a darker centre lane and white stripes
    for i in range(-9, 9):
        x0, x1 = i * 1.0 + 0.5 - 0.5, i * 1.0 + 0.5 + 0.5
        x0, x1 = max(-HX, i * 1.055), min(HX, (i + 1) * 1.055)
    xs = np.linspace(-HX, HX, 19)
    for x0, x1 in zip(xs[:-1], xs[1:]):
        m.quad(floor, (x0, 0, -HZ + CR), (x0, 0, HZ - CR), (x1, 0, HZ - CR), (x1, 0, -HZ + CR))
    # centre lane + stripes (slightly above floor)
    m.quad(floor_dark, (-HX, 0.003, -0.55), (-HX, 0.003, 0.55), (HX, 0.003, 0.55), (HX, 0.003, -0.55))
    for z in (-0.62, 0.62):
        m.quad(stripe, (-HX, 0.004, z - 0.06), (-HX, 0.004, z + 0.06), (HX, 0.004, z + 0.06), (HX, 0.004, z - 0.06))
    # ceiling flat part + light panels
    for x0, x1 in zip(xs[:-1], xs[1:]):
        m.quad(ceiling, (x0, H, -HZ + CR), (x1, H, -HZ + CR), (x1, H, HZ - CR), (x0, H, HZ - CR))
    for x in np.arange(-8.0, 8.5, 2.0):
        for z in (-1.2, 1.2):
            m.quad(light, (x - 0.35, H - 0.01, z - 0.25), (x + 0.35, H - 0.01, z - 0.25), (x + 0.35, H - 0.01, z + 0.25), (x - 0.35, H - 0.01, z + 0.25))
    # rounded corners + side walls: glass in the middle, team panels in the end zones
    for k in range(len(prof) - 1):
        (z0, y0), (z1, y1) = prof[k], prof[k + 1]
        if abs(y0) < 1e-6 and abs(y1) < 1e-6 or abs(y0 - H) < 1e-6 and abs(y1 - H) < 1e-6:
            continue
        side_wall = abs(abs(z0) - HZ) < 1e-6 and abs(abs(z1) - HZ) < 1e-6
        for x0, x1 in zip(xs[:-1], xs[1:]):
            xm = 0.5 * (x0 + x1)
            if side_wall:
                if abs(xm) > FOUL_X + 0.3:
                    # end zone: solid team-coloured wall (as in the original's player areas)
                    t = 0 if xm < 0 else 1
                    m.quad(team_panel[t], (x0, y0, z0), (x1, y0, z0), (x1, y1, z1), (x0, y1, z1))
                else:
                    m.quad(glass, (x0, y0, z0), (x1, y0, z0), (x1, y1, z1), (x0, y1, z1))
            else:
                m.quad(ceiling if y0 > H / 2 else floor, (x0, y0, z0), (x1, y0, z0), (x1, y1, z1), (x0, y1, z1))
    # dark cut-out shapes on the team walls (stylised swoosh: a slanted parallelogram band)
    for t, sx in ((0, -1), (1, 1)):
        for s in (-1, 1):
            z = s * (HZ - 0.01)
            xa, xb = sx * (FOUL_X + 0.6), sx * (HX - 0.3)
            pts = [(xa, 1.4), (xb, 2.2), (xb, 3.0), (xa, 2.0)]
            P = [(px, py, z) for px, py in pts]
            if (s > 0) == (sx > 0):
                m.quad(team_dark[t], *P)
            else:
                m.quad(team_dark[t], *P[::-1])
    # mullions (vertical frame posts) and horizontal rails on both side walls
    for x in np.linspace(-HX, HX, 9):
        for s in (-1, 1):
            m.box(frame, (x, H / 2, s * (HZ + 0.03)), (0.05, H / 2, 0.05))
    for s in (-1, 1):
        for y in (1.05, H - 0.05):
            m.box(frame, (0, y, s * (HZ + 0.03)), (HX, 0.035, 0.05))
    # top frame along the ceiling edges
    for s in (-1, 1):
        m.box(frame, (0, H + 0.04, s * (HZ - 0.2)), (HX, 0.04, 0.06))
    # back walls: near-black with a glowing team frame and a ceiling light ring above the player
    for t, x in ((0, -HX), (1, HX)):
        sgn = -1 if x < 0 else 1
        for k in range(len(prof)):
            pass
        poly = section_profile()
        c = np.array([x, H / 2, 0])
        for k in range(len(poly) - 1):
            (z0, y0), (z1, y1) = poly[k], poly[k + 1]
            a, b = np.array([x, y0, z0]), np.array([x, y1, z1])
            if sgn < 0:
                m.tri(backwall, c, a, b)
            else:
                m.tri(backwall, c, b, a)
        fx = x - sgn * 0.02
        bw = 0.05
        for (za, ya, zb, yb) in ((-1.6, 0.6, 1.6, 0.6), (-1.6, 3.4, 1.6, 3.4), (-1.6, 0.6, -1.6, 3.4), (1.6, 0.6, 1.6, 3.4)):
            if za == zb:
                m.quad(team_glow[t], (fx, ya, za - bw), (fx, yb, za - bw), (fx, yb, za + bw), (fx, ya, za + bw))
            else:
                m.quad(team_glow[t], (fx, ya - bw, za), (fx, ya + bw, za), (fx, yb + bw, zb), (fx, yb - bw, zb))
        m.torus(team_glow[t], (sgn * 7.0, H - 0.02, 0), (0, 1, 0), 0.55, 0.03, seg=40, tseg=6)
    # foul lines: glowing bands on floor, walls and ceiling at x = +-FOUL_X
    for t, x in ((0, -FOUL_X), (1, FOUL_X)):
        poly = section_profile()
        for k in range(len(poly) - 1):
            (z0, y0), (z1, y1) = poly[k], poly[k + 1]
            m.quad(foul[t], (x - 0.03, y0, z0), (x + 0.03, y0, z0), (x + 0.03, y1, z1), (x - 0.03, y1, z1))
        m.quad(foul[t], (x - 0.04, 0.006, -HZ + CR), (x + 0.04, 0.006, -HZ + CR), (x + 0.04, 0.006, HZ - CR), (x - 0.04, 0.006, HZ - CR))
    # exterior under-shell so the court reads as a floating box from outside
    shell = m.m(mat(SH_LIT, (0.08, 0.09, 0.10)))
    m.box(shell, (0, -0.15, 0), (HX + 0.1, 0.15, HZ + 0.1))
    return m


def build_collision():
    """Tagged collision triangles: tube walls (arena), end zones (foul, team), back walls (return, team)."""
    tris = []
    prof = section_profile()
    xs = [-HX, -FOUL_X - 0.02, FOUL_X + 0.02, HX]
    for k in range(len(prof) - 1):
        (z0, y0), (z1, y1) = prof[k], prof[k + 1]
        for x0, x1 in zip(xs[:-1], xs[1:]):
            xm = 0.5 * (x0 + x1)
            tags = TAG_ARENA
            if abs(xm) > FOUL_X:
                tags |= TAG_FOUL | (TAG_TEAM0 if xm < 0 else TAG_TEAM1)
            a, b, c, d = (x0, y0, z0), (x1, y0, z0), (x1, y1, z1), (x0, y1, z1)
            tris.append((a, b, c, tags)); tris.append((a, c, d, tags))
    # the flat floor & ceiling spans (profile covers corners+sides; add top/bottom spans explicitly)
    for y, (za, zb) in ((0.0, (-HZ + CR, HZ - CR)), (H, (HZ - CR, -HZ + CR))):
        for x0, x1 in zip(xs[:-1], xs[1:]):
            xm = 0.5 * (x0 + x1)
            tags = TAG_ARENA | ((TAG_FOUL | (TAG_TEAM0 if xm < 0 else TAG_TEAM1)) if abs(xm) > FOUL_X else 0)
            a, b, c, d = (x0, y, za), (x1, y, za), (x1, y, zb), (x0, y, zb)
            tris.append((a, b, c, tags)); tris.append((a, c, d, tags))
    for t, x in ((0, -HX), (1, HX)):
        tags = TAG_ARENA | TAG_RETURN | (TAG_TEAM0 if t == 0 else TAG_TEAM1)
        c = (x, H / 2, 0)
        for k in range(len(prof) - 1):
            (z0, y0), (z1, y1) = prof[k], prof[k + 1]
            tris.append((c, (x, y0, z0), (x, y1, z1), tags))
    with open(os.path.join(OUT, 'arena.col'), 'wb') as f:
        f.write(b'SCOL' + struct.pack('<I', len(tris)))
        for a, b, c, tags in tris:
            f.write(struct.pack('<9fI', *a, *b, *c, tags))
    print('  arena.col          %d tris' % len(tris))


def build_environment():
    m = Mesh()
    void = m.m(mat(SH_UNLIT, (0.012, 0.016, 0.022), flags=TWOSIDED))
    m.sphere(void, (0, 0, 0), 400, seg=24, ring=12)
    dais = m.m(mat(SH_LIT, (0.035, 0.07, 0.08)))
    m.disc(dais, (0, -1.0, 0), (0, 1, 0), 0, 26, seg=64)
    rim = m.m(mat(SH_ADDITIVE, (0.25, 0.55, 0.6, 1), flags=TWOSIDED))
    m.torus(rim, (0, -1.0, 0), (0, 1, 0), 26, 0.06, seg=96, tseg=6)
    # stands: rows of glowing light bands on both sides of the court (as in the original's courtside)
    band = m.m(mat(SH_UNLIT, (0.85, 1.0, 1.1)))
    seat = m.m(mat(SH_LIT, (0.05, 0.06, 0.07)))
    for s in (-1, 1):
        for row in range(6):
            z = s * (9.0 + row * 1.4)
            y = 0.2 + row * 1.0
            m.box(seat, (0, y - 0.25, z), (14, 0.25, 0.6))
            for x in np.arange(-13, 13.01, 0.9):
                m.quad(band, (x - 0.35, y, z - s * 0.61), (x + 0.35, y, z - s * 0.61), (x + 0.35, y + 0.5, z - s * 0.61), (x - 0.35, y + 0.5, z - s * 0.61)) \
                    if s > 0 else m.quad(band, (x - 0.35, y, z - s * 0.61), (x - 0.35, y + 0.5, z - s * 0.61), (x + 0.35, y + 0.5, z - s * 0.61), (x + 0.35, y, z - s * 0.61))
    # overhead stadium light bars
    for x in (-12, -4, 4, 12):
        m.box(band, (x, 14, 0), (1.5, 0.08, 9))
    return m


# ---------------------------------------------------------------------------------------------------------- props
def build_projectile():
    m = Mesh()
    core = m.m(mat(SH_ADDITIVE, (1.0, 1.0, 1.0, 1), flags=TWOSIDED))
    m.sphere(core, (0, 0, 0), 0.08, seg=24, ring=14)
    return m


def build_shell():
    m = Mesh()
    band = m.m(mat(SH_UNLIT, (1.6, 1.6, 1.6)))
    for ax in ((1, 0, 0), (0.5, 0.86, 0), (-0.5, 0.86, 0)):
        m.torus(band, (0, 0, 0), ax, 0.095, 0.006, seg=40, tseg=5)
    return m


def build_spawner():
    m = Mesh()
    base = m.m(mat(SH_LIT, (0.08, 0.09, 0.1)))
    glow = m.m(mat(SH_ADDITIVE, (0.7, 0.85, 0.9, 1), flags=TWOSIDED))
    m.disc(base, (0, 0.012, 0), (0, 1, 0), 0, 0.36, seg=48)
    m.disc(glow, (0, 0.016, 0), (0, 1, 0), 0.24, 0.30, seg=48)
    m.torus(glow, (0, 0.03, 0), (0, 1, 0), 0.36, 0.012, seg=48, tseg=6)
    return m


def build_shield():
    """Plate in the same convention as Shield_01 after the app's g_plateLocal (scale 10): authored at 1/10 size,
    lying in the glTF XZ plane (normal along glTF Y), so the runtime transform is unchanged."""
    m = Mesh()
    glass = m.m(mat(SH_TRANSLUCENT, (0.75, 0.9, 1.0, 0.35), (0.05, 0.08, 0.1), flags=TWOSIDED))
    edge = m.m(mat(SH_ADDITIVE, (0.8, 0.9, 1.0, 1), flags=TWOSIDED))
    # hexagon ~ 64 x 40 cm final size -> 6.4 x 4.0 cm authored
    pts = [(math.cos(math.radians(a)) * 0.032, 0, math.sin(math.radians(a)) * 0.020) for a in range(0, 360, 60)]
    for i in range(6):
        m._tri_n(glass, (0, 0, 0), pts[i], pts[(i + 1) % 6], [np.array([0, 1, 0])] * 3)
    for i in range(6):
        a, b = np.array(pts[i]), np.array(pts[(i + 1) % 6])
        d = b - a
        n = np.array([d[2], 0, -d[0]]); n /= np.linalg.norm(n)
        m.quad_n(edge, a, b, b + n * 0.002, a + n * 0.002, np.array([0, 1, 0]))
    return m


def build_gauntlet():
    """Glove + wrist pod, authored in glTF-style space so the app's g_lancerLocal maps it onto the hand:
    glTF +X = forward along the controller, +Y = up (thumb), +Z = right."""
    m = Mesh()
    glove = m.m(mat(SH_LIT, (0.28, 0.12, 0.55)))
    pod = m.m(mat(SH_LIT, (0.55, 0.20, 0.75)))
    trim = m.m(mat(SH_ADDITIVE, (0.6, 0.6, 0.7, 1), flags=TWOSIDED))
    # fist (palm block + knuckle row) around the controller handle
    m.sphere(glove, (0.035, -0.005, 0.0), 0.045, seg=16, ring=10, scale=(1.3, 1.0, 0.85))
    for k in range(4):
        m.sphere(glove, (0.075, 0.012, -0.03 + k * 0.02), 0.014, seg=8, ring=6)
    m.sphere(glove, (0.045, 0.035, 0.03), 0.015, seg=8, ring=6, scale=(1.6, 0.9, 1))   # thumb
    # wrist cuff + pod on the back of the wrist
    m.sphere(glove, (-0.03, -0.01, 0.0), 0.04, seg=14, ring=8, scale=(1.2, 0.9, 0.9))
    m.sphere(pod, (-0.035, 0.04, 0.0), 0.035, seg=16, ring=10, scale=(1.3, 0.8, 1.0))
    m.torus(trim, (-0.035, 0.045, 0.0), (0, 1, 0), 0.03, 0.004, seg=24, tseg=5)
    return m


def build_knuckleguard():
    """Clear shell over the front of the fist. Authored so that after g_knuckleLocal (1.5 cm) it spans the
    gameplay box (right-hand grip centre (-0.022, 0.010, -0.137), half (0.064, 0.048, 0.029))."""
    m = Mesh()
    glass = m.m(mat(SH_TRANSLUCENT, (0.8, 0.92, 1.0, 0.3), flags=TWOSIDED))
    # curved shell: part of an ellipsoid, glTF space (x fwd ~ 0.09..0.15 before the 1.5 cm offset)
    cx = 0.10
    for i in range(10):
        for j in range(12):
            def P(ii, jj):
                th = -0.9 + 1.8 * ii / 10
                ph = -1.0 + 2.0 * jj / 12
                return np.array([cx + 0.03 * math.cos(th) * math.cos(ph), 0.01 + 0.048 * math.sin(th), -0.02 + 0.064 * math.sin(ph)])
            a, b, c, d = P(i, j), P(i + 1, j), P(i + 1, j + 1), P(i, j + 1)
            m.quad(glass, a, b, c, d)
    return m


def build_head():
    m = Mesh()
    skin = m.m(mat(SH_LIT, (0.85, 0.86, 0.88)))
    visor = m.m(mat(SH_LIT, (0.05, 0.05, 0.07), (0.05, 0.05, 0.08)))
    m.sphere(skin, (0, 1.66, 0.0), 0.11, seg=18, ring=12, scale=(0.85, 1.05, 0.95))
    m.sphere(visor, (0, 1.68, 0.06), 0.075, seg=16, ring=8, scale=(1.15, 0.5, 0.7))
    return m


def build_torso():
    m = Mesh()
    suit = m.m(mat(SH_LIT, (0.12, 0.13, 0.16)))
    accent = m.m(mat(SH_LIT, (0.75, 0.75, 0.2)))
    m.sphere(suit, (0, 1.36, 0), 0.19, seg=18, ring=12, scale=(1.0, 1.15, 0.65))
    m.sphere(accent, (0, 1.38, 0.08), 0.1, seg=12, ring=8, scale=(1.0, 1.3, 0.35))
    m.sphere(suit, (0, 1.53, 0), 0.05, seg=10, ring=6)   # neck
    for s in (-1, 1):
        m.sphere(suit, (s * 0.21, 1.47, 0), 0.07, seg=10, ring=6)   # shoulders
    return m


def build_strikezone():
    m = Mesh()
    ring = m.m(mat(SH_UNLIT, (1, 1, 1)))
    m.torus(ring, (0, 0, 0), (1, 0, 0), 0.70, 0.012, seg=64, tseg=5)
    for k in range(8):
        a = 2 * math.pi * k / 8 + math.pi / 8
        m.sphere(ring, (0, 0.74 * math.sin(a), 0.74 * math.cos(a)), 0.02, seg=6, ring=4)
    return m


def main():
    os.makedirs(OUT, exist_ok=True)
    print('generating original art set ->', OUT)
    build_arena().write(os.path.join(OUT, 'arena.mdl'))
    build_collision()
    build_environment().write(os.path.join(OUT, 'environment.mdl'))
    build_projectile().write(os.path.join(OUT, 'projectile.mdl'))
    build_shell().write(os.path.join(OUT, 'projectile_shell.mdl'))
    build_spawner().write(os.path.join(OUT, 'spawner.mdl'))
    build_shield().write(os.path.join(OUT, 'shield.mdl'))
    build_gauntlet().write(os.path.join(OUT, 'lancer.mdl'))
    build_knuckleguard().write(os.path.join(OUT, 'knuckleguard.mdl'))
    build_head().write(os.path.join(OUT, 'head.mdl'))
    build_torso().write(os.path.join(OUT, 'torso.mdl'))
    build_strikezone().write(os.path.join(OUT, 'strikezone.mdl'))


if __name__ == '__main__':
    main()
