"""PELURU (ProjectArena-public pull request #30): its tuxedo and layered dress as the game's own clothing and
accessories, fitted to the game's avatars and worn in the player's colours (the owner, 2026-11-19: "build and fit it
correctly along with choosing what should actually come over such as colours, not necessary to be fixed"; "the
sugarskull should appear to be a mask ... clothing and accessories in a full outfit").

The pull request drew each costume as one model in one fixed-colour picture, over a painted copy of the head. Here
each piece is a part of its own, on the player's own head and body:

  av_top_17_m/f       SUIT 17 PELURU TUXEDO: the charro jacket (the base suit's cloth with its shirt front, bow tie,
                      bandolier and embroidery; lapels, tails and a boutonniere), over the player's LOWER BODY
  av_top_18_m/f       SUIT 18 PELURU LAYERED DRESS: the off-shoulder bodice, its flounces and the layered skirt, which
                      sways (.swish) and takes the LOWER BODY's place
  av_top_18e_m/f      the dress's hoop earrings, which swing (.swing)
  av_gear_10_m/f      HEADGEAR 10 SOMBRERO, its crown keeping the hair inside (.tuck)
  av_mask_6_<face>    MASK 6 SUGAR SKULL, a mask fitted to each of the eight faces, with eye holes where the eyes are
  tex/open_design_peluru_{tux,dress,skull}.tex   the design masks: R the TRIM COLOR, G the GLOW, B white

Colours: the cloth is the MAIN COLOR, its embroidery the TRIM COLOR and its flowers the GLOW; the lapels and the
outer skirt are the TRIM COLOR, the inner skirt, the bow and the flowers the GLOW colour, the gold the trim metal, the
lace ivory and the bare shoulders and arms the skin. The sugar skull is white with its decorations in the TRIM COLOR
and the GLOW. Its shapes are the pull request's, unchanged unless noted.

    py -3.10 -I tools/build_peluru.py        (writes into app/assets_open)
"""
import math
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, str(Path(__file__).resolve().parent))
import preview_render as reader  # noqa: E402  (load_mdl)
from build_open_assets import write_model, write_rig_text, write_tuck  # noqa: E402

ASSETS = Path(__file__).resolve().parents[1] / 'app' / 'assets_open'
TAU = math.tau
# head space (the head pieces' models) is the body's model space less the eyes' midpoint (avatar_bodies.inc)
EYE = {'m': np.array((0.0, .17797, -.11099)), 'f': np.array((0.0, .17103, -.10375))}
FACES = ('m', 'm1', 'm2', 'm3', 'f', 'f1', 'f2', 'f3')

# ---------------------------------------------------------------- materials
SH_LIT = 0
TWOSIDED, DESIGN, CLOTH = 1, 32, 64
CH_NONE, CH_PRIMARY, CH_SECONDARY, CH_GLOW, CH_SKIN, CH_TRIM = 0, 1, 2, 3, 4, 7


def material(ch, tint=1.0, gloss=0.1, flags=0, mask=''):
    t = (tint, tint, tint) if np.isscalar(tint) else tuple(tint)
    return dict(shader=SH_LIT, tint=t + (1.0,), em=(0.0, 0.0, 0.0, gloss),
                flags=(ch << 8) | flags | (DESIGN if mask else 0), mask=mask)


def materials(design):
    """the pieces' materials (by name); `design` the design mask the cloth uses"""
    return {
        'main': material(CH_PRIMARY, 1.0, 0.1, CLOTH, design),                 # the base suit's cloth
        'main2': material(CH_PRIMARY, 1.0, 0.1, CLOTH | TWOSIDED, design),     # the cloth's own pieces
        'inner': material(CH_SECONDARY, 1.0, 0.22, TWOSIDED),                  # the base suit's lining
        'sec': material(CH_SECONDARY, 1.0, 0.22, CLOTH | TWOSIDED, design),    # the outer skirt, the swag
        'lapel': material(CH_PRIMARY, 0.55, 0.3, TWOSIDED, design),            # the jacket's colour, darker, in satin
        'bright': material(CH_GLOW, 0.80, 0.1, CLOTH | TWOSIDED, design),      # the inner skirt, the bow
        'bright_dk': material(CH_GLOW, 0.50, 0.1, CLOTH | TWOSIDED, design),   # its lining
        'bright_dkr': material(CH_GLOW, 0.30, 0.1, CLOTH | TWOSIDED, design),  # under the skirt, the sash
        'lace': material(CH_NONE, (.88, .85, .78), 0.1, CLOTH | TWOSIDED, design),
        'skin': material(CH_SKIN, 1.0, 0.05),
        'skin2': material(CH_SKIN, 1.0, 0.05, TWOSIDED),   # (the lining's lip at the neck and cuffs, bare: seen from both sides)
        'metal': material(CH_TRIM, 1.0, 0.7, TWOSIDED),                        # the gold: the trim metal
        'petal': material(CH_GLOW, 0.95, 0.1, TWOSIDED),
        'petal_mid': material(CH_GLOW, 0.75, 0.1, TWOSIDED),
        'petal_dk': material(CH_GLOW, 0.55, 0.1, TWOSIDED),
        'leaf': material(CH_SECONDARY, 0.70, 0.1, TWOSIDED),
    }


# the pull request's picture tiles (its colours) and the piece each became
TILE_MAT = {0: 'main2', 1: 'sec', 2: 'bright', 3: 'lace', 4: 'metal', 5: 'bright_dk', 6: 'skin', 7: 'main2',
            8: 'leaf', 9: 'bright_dkr', 10: 'main2', 11: 'lace', 12: 'petal', 13: 'lapel', 14: 'sec', 15: 'main2'}
PETAL_MAT = {12: 'petal', 2: 'petal_mid', 5: 'petal_dk'}


def model_bytes(ver, V, I, secs, mats):
    out = [b'SMDL' + struct.pack('<5I', ver, len(V), len(I), len(secs), len(mats)), V.tobytes(), I.tobytes()]
    out += [struct.pack('<3I', *s) for s in secs]
    for m in mats:
        out.append(struct.pack('<I4f4f2I', m['shader'], *m['tint'], *m['em'], 0, m['flags'])
                   + b''.ljust(64, b'\0') + m['mask'].encode().ljust(64, b'\0'))
    return b''.join(out)


def write_tex(name, im):
    if im.size[0] > 512:   # (drawn at 1024, kept at the 512 the game's other designs are)
        im = im.resize((512, 512), Image.LANCZOS)
    rgba = np.asarray(im.convert('RGBA'))
    (ASSETS / 'tex').mkdir(exist_ok=True)
    (ASSETS / 'tex' / (name + '.tex')).write_bytes(b'STEX' + struct.pack('<3I', rgba.shape[1], rgba.shape[0], 0)
                                                   + np.ascontiguousarray(rgba).tobytes())


# ---------------------------------------------------------------- the design masks (the pull request's pictures)
# inks: R the TRIM COLOR, G the GLOW, B white; black leaves the piece's own colour
K, R, G, B = (0, 0, 0), (255, 0, 0), (0, 255, 0), (0, 0, 255)


def flower(d, x, y, r, outer=G, mid=R, inner=G, centre=B):
    for ring, scale, petals, fill in [(0, 1, 7, outer), (.35, .76, 6, mid), (.7, .43, 5, inner)]:
        for j in range(petals):
            t = TAU * j / petals + ring
            a, b = x + r * scale * .58 * math.cos(t), y + r * scale * .58 * math.sin(t)
            k = r * scale * .45
            d.ellipse((a - k, b - k, a + k, b + k), fill)
    k = max(1, r * .18)
    d.ellipse((x - k, y - k, x + k, y + k), centre)


def sprig(d, x, y, r, stem=R, mid=R):
    for sign in (-1, 1):
        pts = [(x + sign * r * t, y + r * (t * t - .7 * t)) for t in np.linspace(0, 1.7, 20)]
        d.line(pts, fill=stem, width=max(1, int(r * .10)))
        for t in (.45, .85, 1.25):
            a, b = x + sign * r * t, y + r * (t * t - .7 * t)
            d.polygon([(a, b), (a + sign * r * .35, b - r * .62), (a + sign * r * .32, b - r * .1)], fill=stem)
    flower(d, x, y, r, mid=mid)


def design_top(dress):
    """the jacket's or the bodice's front and back panels and the sixteen trim tiles, as the pull request laid them out"""
    im = Image.new('RGB', (1024, 1024), K)
    for back in (False, True):
        panel = Image.new('RGB', (512, 512), K)
        d = ImageDraw.Draw(panel)
        if not back and not dress:   # the shirt front, the lapels' gold edges, the studs and the bow tie
            d.polygon([(204, 0), (305, 0), (256, 285)], fill=B)
            for sign in (-1, 1):
                d.line([(256 + sign * 65, 32), (256 + sign * 78, 64), (256 + sign * 52, 96), (256, 301)], fill=R, width=3)
                d.polygon([(256, 65), (256 + sign * 37, 48), (256 + sign * 32, 82)], fill=K)
            for y in range(100, 210, 26):
                d.ellipse((253, y, 259, y + 6), fill=K)
        if dress:   # the stitched boning, embroidered sprigs and the waistband
            for x in (179, 210, 302, 333):
                d.line([(x, 160), ((x + 256) // 2, 306), (x, 421)], fill=R, width=3)
            for x in (199, 313):
                for y in (206, 256, 306):
                    sprig(d, x, y, 14)
            d.rectangle((110, 362, 402, 423), fill=R)
            d.line((110, 375, 402, 375), fill=B, width=5)
        elif not back:   # the belt's buckle
            d.rounded_rectangle((235, 390, 278, 422), 6, fill=R, outline=B, width=3)
        for x in (136, 376):
            for y in (116, 182, 247, 311, 455):
                sprig(d, x, y, 10)
        # the bandolier: trim-coloured edges, beads with white highlights, across the back too
        start, end = ((151, 39), (363, 405)) if back else ((361, 39), (149, 405))
        d.line([start, end], fill=R, width=40)
        d.line([start, end], fill=K, width=35)
        for t in np.linspace(.025, .975, 19):
            x, y = np.array(start) * (1 - t) + np.array(end) * t
            sign = 1 if back else -1
            d.line((x - 10, y + 6 * sign, x + 10, y - 6 * sign), fill=R, width=12)
            d.line((x - 9, y + 6 * sign - 4, x + 9, y - 6 * sign - 4), fill=B, width=3)
        sprig(d, 345 if not back else 254, 100 if not back else 322, 18)
        im.paste(panel, (512 * back, 0))
    for k in range(16):
        tile = Image.new('RGB', (128, 128), K)
        d = ImageDraw.Draw(tile)
        sec = TILE_MAT[k] == 'sec'   # (on the TRIM COLOR, its lines are white)
        line = B if sec else R
        plain = dress and k in (1, 2, 5, 9, 14)
        if k in (0, 1, 2, 3, 13) and not plain:   # gold edges with a light zigzag, a sprig and two flowers
            for y in (7, 118):
                d.line((0, y, 128, y), fill=line, width=4)
                for x in range(-8, 129, 16):
                    d.line([(x, y), (x + 8, y + 7), (x + 16, y)], fill=B, width=2)
            sprig(d, 64, 83 if k == 1 else 60, 16 if k == 1 else 22, stem=line, mid=line)
            if k == 1:
                d.arc((18, 39, 73, 92), 10, 163, fill=line, width=2)
                d.arc((59, 39, 114, 92), 20, 166, fill=line, width=2)
                flower(d, 64, 39, 6, mid=line)
            for x in (12, 114):
                flower(d, x, 83, 9, mid=line)
        if k == 13:   # the lapels: a gold and a green line down the edge (here the trim colour and the glow)
            tile = Image.new('RGB', (128, 128), K)
            d = ImageDraw.Draw(tile)
            d.line((120, 0, 120, 128), fill=R, width=4)
            d.line((114, 0, 114, 128), fill=G, width=2)
        if k == 3:   # the lace: scalloped ovals
            tile = Image.new('RGB', (128, 128), K)
            d = ImageDraw.Draw(tile)
            for x in range(-16, 144, 16):
                d.ellipse((x, 34, x + 13, 103), outline=R, width=2)
        if k == 14 and not dress:
            tile = Image.new('RGB', (128, 128), K)
            d = ImageDraw.Draw(tile)
            for y in (45, 83):
                sprig(d, 62, y, 11 if y < 80 else 17, stem=B, mid=B)
            for x in range(-16, 140, 16):
                d.arc((x, 106, x + 16, 121), 0, 180, fill=B, width=3)
            d.line((0, 123, 128, 123), fill=B, width=3)
        if k == 15:
            tile = Image.new('RGB', (128, 128), K)
            d = ImageDraw.Draw(tile)
            d.line((58, 0, 58, 128), fill=R, width=3)
            for y in (24, 64, 104):
                sprig(d, 64, y, 9)
        if plain:
            tile = Image.new('RGB', (128, 128), K)
            d = ImageDraw.Draw(tile)
            if k in (1, 9):
                for y in (8, 118):
                    d.line((0, y, 128, y), fill=B if k == 1 else R, width=2)
        im.paste(tile, (512 + k % 4 * 128, 512 + k // 4 * 128))
    return im


# the sugar skull's picture: 512 x 512 over the face (skull_uv), the eyes at SKULL_EYE
SKULL_EYE = (169, 236)   # the left eye (the right one mirrored)
SKULL_W, SKULL_H = 0.20, 0.252   # metres across and down the picture (the masculine eyes 6.8 cm apart)


def design_skull():
    im = Image.new('RGB', (512, 512), B)   # the skull's white
    d = ImageDraw.Draw(im)
    for x in (SKULL_EYE[0], 512 - SKULL_EYE[0]):
        y = SKULL_EYE[1]
        for j in range(14):   # a ring of dots round each socket
            t = TAU * j / 14
            a, b = x + 58 * math.cos(t), y + 46 * math.sin(t)
            d.ellipse((a - 9, b - 9, a + 9, b + 9), fill=R)
            d.ellipse((a - 3, b - 3, a + 3, b + 3), fill=G)
        d.ellipse((x - 46, y - 34, x + 46, y + 34), fill=K)   # the socket (the eye shows through its hole)
        sprig(d, x, y + 66, 9)
    d.polygon([(256, 270), (232, 315), (249, 321), (256, 309), (266, 321), (281, 315)], fill=K)   # the nose
    d.arc((157, 298, 355, 373), 0, 180, fill=K, width=9)   # the smile and its stitches
    for x in range(183, 333, 13):
        y = 367 - .0038 * (x - 256) ** 2
        d.line((x - 3, y - 12, x + 3, y + 13), fill=K, width=4)
    for side in (-1, 1):
        sprig(d, 256 + side * 125, 299, 13)
    sprig(d, 256, 150, 21)   # the forehead's flower
    return im


def skull_uv(sex, x, y):
    """the sugar skull's picture over the face: the eyes where the picture has them, on both bodies"""
    ex = 0.034 if sex == 'm' else 0.035   # the eyes' half spacing (av_bhead_*)
    u = 0.5 + x / (SKULL_W * ex / 0.034)
    v = SKULL_EYE[1] / 512 + (EYE[sex][1] - y) / SKULL_H
    return u, v


# ---------------------------------------------------------------- the pieces (the pull request's shapes)
class Piece:
    """vertices and triangles of one model, each run of triangles with its material's name"""

    def __init__(self, sex, dtype):
        self.sex, self.dtype = sex, dtype
        self.verts, self.indices, self.mats = [], [], []
        self.offset = 0

    def add(self, v, idx, mat):
        self.verts.append(v)
        self.indices.append(np.asarray(idx, dtype=np.uint32).ravel() + self.offset)
        self.mats.append(mat)
        self.offset += len(v)

    @staticmethod
    def tile(k, u, v):
        return ((512 + 128 * (k % 4) + 2 + 124 * u) / 1024, (512 + 128 * (k // 4) + 2 + 124 * v) / 1024)

    def loft(self, rows, tile, bone=0, slices=36, repeats=6, center=(0, 0), wave=0, lift=0, hem=False, mat=None):
        # each ring is (height, half-width, half-depth); the front is -Z
        xyz, uv, idx = [], [], []
        for j, (y, rx, rz) in enumerate(rows):
            v = j / (len(rows) - 1)
            for i in range(slices):
                for k in (i, i + 1):
                    t = TAU * k / slices
                    fl = 1 + wave * v * math.cos(12 * t)
                    brim_curl = max(0, rx - .10) * (.25 * math.sin(t) ** 2 + .10 * math.cos(t)) if bone == 2 else 0
                    xyz.append((center[0] + rx * fl * math.sin(t), y + brim_curl + lift * v * (1 + math.cos(t)) / 2,
                                center[1] - rz * fl * math.cos(t)))
                    u = (i * repeats % slices + (k - i) * repeats) / slices
                    uv.append(self.tile(tile, u, v if not hem else ((.02, .08, .83, .98)[j] if len(rows) == 4 else (.02, .70, .98)[j])))
                if j:
                    a = (j - 1) * 2 * slices + i * 2
                    b = a + 2 * slices
                    idx.extend([a, a + 1, b, b, a + 1, b + 1])
        p = np.array(xyz)
        tri = np.array(idx).reshape(-1, 3)
        n = np.zeros_like(p)
        normals = np.cross(p[tri[:, 1]] - p[tri[:, 0]], p[tri[:, 2]] - p[tri[:, 0]])
        for col in tri.T:
            np.add.at(n, col, normals)
        vtx = np.zeros(len(p), self.dtype)
        vtx['p'] = p
        vtx['n'] = n / np.maximum(np.linalg.norm(n, axis=1)[:, None], 1e-10)
        vtx['u0'] = uv
        vtx['c'] = 255
        if 'bi' in self.dtype.names:
            vtx['bi'][:, 0] = bone
            vtx['bw'][:, 0] = 255
        self.add(vtx, idx, mat or TILE_MAT[tile])

    def fabric(self, point, tile, columns, rows, bone=0, repeat=1, mat=None, band=(0.0, 1.0)):
        """one cloth pattern piece, split only where its motif repeats (band: the part of the tile it shows, top to
        bottom)"""
        for panel in range(repeat):
            uvs = [(u / columns, v / rows) for v in range(rows + 1) for u in range(columns + 1)]
            points = np.array([point((panel + u) / repeat, v) for u, v in uvs])
            faces = []
            for j in range(rows):
                for i in range(columns):
                    a = j * (columns + 1) + i
                    b = a + columns + 1
                    faces.extend([(a, a + 1, b), (b, a + 1, b + 1)])
            faces = np.array(faces)
            normals = np.zeros_like(points)
            cross = np.cross(points[faces[:, 1]] - points[faces[:, 0]], points[faces[:, 2]] - points[faces[:, 0]])
            for corner in faces.T:
                np.add.at(normals, corner, cross)
            mesh = np.zeros(len(points), self.dtype)
            mesh['p'] = points
            mesh['n'] = normals / np.maximum(np.linalg.norm(normals, axis=1)[:, None], 1e-10)
            mesh['u0'] = [self.tile(tile, u, band[0] + (band[1] - band[0]) * v) for u, v in uvs]
            if 'bi' in self.dtype.names:
                mesh['bi'][:, 0] = bone
                mesh['bw'][:, 0] = 255
            mesh['c'] = 255
            self.add(mesh, faces, mat or TILE_MAT[tile])

    def bloom(self, x, y, z, r, bone, color):
        # overlapping cupped petals spiral round an upright, forward-facing corolla: GLOW petals, a gold heart, leaves
        for ring, count, radius, shade in [(0, 8, 1, color), (1, 6, .70, color), (2, 4, .44, 4)]:
            for petal in range(count):
                angle = TAU * petal / count + ring * .54

                def petal_point(u, v, angle=angle, ring=ring, radius=radius):
                    spread = (u - .5) * (1.1 - .72 * v)
                    rad = r * radius * (1 - .65 * v) * (1 - .21 * (2 * u - 1) ** 2)
                    a = angle + spread + .30 * v
                    return (x + rad * math.cos(a), y + rad * math.sin(a), z - r * (.16 * ring + .25 * math.sin(math.pi * u) + .18 * v))
                self.fabric(petal_point, shade, 4, 2, bone, mat='metal' if shade == 4 else PETAL_MAT.get(shade, 'petal'))
        for sign in (-1, 1):
            def leaf(u, v, sign=sign):
                return (x + sign * r * (.3 + 1.2 * v), y + r * (.34 + .81 * v + (u - .5) * .65 * math.sin(math.pi * v)),
                        z + .004 - r * .17 * math.sin(math.pi * v))
            self.fabric(leaf, 8, 2, 4, bone)

    def arrays(self):
        V = np.concatenate(self.verts)
        order = sorted(range(len(self.mats)), key=lambda i: self.mats[i])   # one section a material
        I, secs, names = [], [], []
        n = 0
        for name in sorted(set(self.mats)):
            run = [self.indices[i] for i in order if self.mats[i] == name]
            idx = np.concatenate(run).astype('<u4')
            secs.append((n, len(idx), len(names)))
            names.append(name)
            I.append(idx)
            n += len(idx)
        I = np.concatenate(I)
        assert I.max() < len(V) and np.all(np.isfinite(V['p']))
        return V, I, secs, names

    def write(self, name, mats, ver):
        V, I, secs, names = self.arrays()
        if ver == 2:   # (rigid: no bones)
            V2 = np.zeros(len(V), dtype=[('p', '<f4', 3), ('n', '<f4', 3), ('u0', '<f4', 2), ('u1', '<f4', 2), ('c', 'u1', 4)])
            for f in V2.dtype.names:
                V2[f] = V[f]
            V = V2
        else:
            assert np.all(V['bw'].astype(int).sum(1) == 255)
        how = write_model(str(ASSETS / (name + '.mdl')), model_bytes(ver, V, I, secs, [mats[m] for m in names]))
        print('  %-16s %6d tris  %s  (%s)' % (name, len(I) // 3, how, ', '.join(names)))
        return V, I


class Garment(Piece):
    """SUIT 17 and 18: the base suit re-cut, with the pull request's own pieces"""

    def __init__(self, sex, dress):
        super().__init__(sex, None)
        self.dress = dress

    def neckline(self, px, py):
        """the dress's sweetheart neckline in the panels' pixels: above it, bare skin (the pull request's picture)"""
        edge = np.interp(px, [0, 156, 218, 256, 296, 355, 512], [171, 146, 132, 159, 132, 146, 171])
        return py < edge

    def neck_ring(self, v, idx, sections, mats):
        """the suit's own neck skin where it starts above the collar: its centre (x, z) and its radius round it by angle"""
        p = []
        for start, count, mi in sections:
            if (mats[mi]['flags'] >> 8) & 15 == CH_SKIN:
                q = v['p'][np.unique(idx[start:start + count])].astype(float)
                p.append(q[(np.abs(q[:, 0]) < .09) & (q[:, 1] > .044) & (q[:, 1] < .054)])
        p = np.concatenate(p)
        cz = (p[:, 2].min() + p[:, 2].max()) / 2
        ang = np.arctan2(p[:, 0], p[:, 2] - cz)
        r = np.hypot(p[:, 0], p[:, 2] - cz)
        bins = np.linspace(-np.pi, np.pi, 37)
        rb = np.array([r[(ang >= bins[i]) & (ang < bins[i + 1])].max() if ((ang >= bins[i]) & (ang < bins[i + 1])).any() else np.nan
                       for i in range(36)])
        ok = np.isfinite(rb)
        mid = (bins[:-1] + bins[1:]) / 2
        rb = np.interp(mid, mid[ok], rb[ok], period=2 * np.pi)
        return cz, mid, rb

    def bare_neck(self, part, ring):
        """the dress's bare neck: the suit's collar, made skin, drawn in at its top onto the neck's own outline (it stood
        some millimetres out from it: a kink at the back, a line in front)"""
        cz, mid, rb = ring
        x, y, z = part['p'][:, 0].astype(float), part['p'][:, 1].astype(float), part['p'][:, 2].astype(float)
        near = (np.abs(x) < .09) & (y > .0)
        ang = np.arctan2(x, z - cz)
        r = np.hypot(x, z - cz)
        # its top runs on 7 mm up under the neck skin and 2 mm inside it, so the neck covers the join: without the
        # suit's lip it left a crack there, with it a line
        lift = np.where(near, np.clip((y - .035) / .012, 0, 1) * .007, 0.0)
        y2 = y + lift
        target = np.interp(ang, mid, rb, period=2 * np.pi) - np.clip((y2 - .043) / .011, 0, 1) * .002
        w = np.clip(y / .040, 0, 1)
        w = w * w * (3 - 2 * w)
        new_r = np.where(near, r + (target - r) * w, r)
        k = np.where(r > 1e-6, new_r / np.maximum(r, 1e-6), 1.0)
        part['p'][:, 0] = (x * k).astype('<f4')
        part['p'][:, 1] = y2.astype('<f4')
        part['p'][:, 2] = (cz + (z - cz) * k).astype('<f4')

    def surface(self):
        v, idx, sections, mats = reader.load_mdl('av_top_0_' + self.sex)
        self.dtype = v.dtype
        ring = self.neck_ring(v, idx, sections, mats) if self.dress else None
        for start, count, material in sections:
            tris = idx[start:start + count].reshape(-1, 3)
            channel = (mats[material]['flags'] >> 8) & 15
            if channel == CH_SKIN:   # the suit's own skin (the neck, the hands)
                ids, inv = np.unique(tris, return_inverse=True)
                part = v[ids].copy()
                part['u1'] = 0
                self.add(part, inv, 'skin')
                continue
            for back in (False, True):
                pos = v['p'][tris].mean(axis=1)
                group = tris[(pos[:, 2] > -.033) == back]
                if not len(group):
                    continue
                x, y, z = v['p'][group].mean(axis=1).T
                region = np.zeros(len(group), int)
                region[abs(x) > .204] = 1
                if self.dress:   # bare arms and neck (the tuxedo keeps the suit's collar: its shirt collar, the jacket's)
                    region[abs(x) > .23] = 2
                    region[(y > .018) & (abs(x) < .083)] = 2
                if self.dress:   # above the sweetheart neckline: the shoulders bare
                    u = np.clip(.5 + (x if not back else -x) / .56, .004, .996)
                    vv = np.clip((.035 - y) / .50, .004, .996)
                    region[(region == 0) & self.neckline(u * 512, vv * 512)] = 2
                if self.dress and channel != CH_PRIMARY:   # (the lining's lip at the bare neck: left out, it shaded dark;
                    lip = (region == 2) & (y > .018) & (abs(x) < .083)   # the collar and the neck overlap there anyway)
                    group, region = group[~lip], region[~lip]
                    if not len(group):
                        continue
                for zone in np.unique(region):
                    ids, inv = np.unique(group[region == zone], return_inverse=True)
                    part = v[ids].copy()
                    if self.dress and zone == 2 and channel == CH_PRIMARY:
                        self.bare_neck(part, ring)
                    x, y, z = part['p'].T
                    if zone == 1:   # the sleeves: plain (the trim tile only caught part of a flower at the shoulder)
                        uv = np.array(self.tile(7, np.clip((abs(x) - .20) / .32, 0, 1), np.clip((.04 - y) / .38, 0, 1))).T
                    else:
                        u = np.clip(.5 + (x if not back else -x) / .56, .004, .996)
                        uv = np.stack([.5 * (u + back), .5 * np.clip((.035 - y) / .50, .004, .996)], axis=1)
                    part['u0'] = uv
                    part['u1'] = 0   # (the skirt's sway is the skirt's alone)
                    if zone == 2:   # (the lining's lip, two-sided in the suit, stays two-sided: one-sided it left a dark line)
                        mat = 'skin' if channel == CH_PRIMARY else 'skin2'
                    else:
                        mat = 'main' if channel == CH_PRIMARY else 'inner'
                    self.add(part, inv, mat)

    def dress_panels(self):
        # a lining sweeps down one hip inside the skirt; the outer wrap is an open pattern gathered at the right hip
        waist_mesh_start = len(self.verts)

        def skirt_point(t, y, offset):
            fall = max(0, -.345 - y)
            bias = (1 - math.cos(t)) * .5
            release = min(1., fall / .14)
            gathering = (.25 + .75 * release) * (math.sin(11 * t + 3 * fall) + .40 * math.sin(17 * t - 4 * fall))
            depth = .178 + .193 * fall + (.010 + .027 * fall) * gathering + offset
            width = .172 + .253 * fall + (.010 + .032 * fall) * gathering + offset
            return (width * math.sin(t) - .095 * fall ** 1.4, y, -.045 - depth * math.cos(t) + .18 * fall ** 1.7 * bias)

        def lining_hem(t):
            return -.645 + .157 * math.sin(t + .28) + .013 * math.cos(11 * t)
        for lower, upper, tile in [(1, .68, 2), (.73, 0, 5)]:
            def lining(u, v, lower=lower, upper=upper):
                t = TAU * u
                f = upper + (lower - upper) * v
                return skirt_point(t, -.345 + (lining_hem(t) + .345) * f, .008 * (1 - upper))
            self.fabric(lining, tile, 4, 4, repeat=12)   # (inside the wraps: fewer triangles than the pull request's)

        def underside(u, v):
            t = TAU * u
            x, y, z = skirt_point(t, lining_hem(t), .0025)
            return (x * (1 - v), y + (-.467 - y) * v, -.045 + (z + .045) * (1 - v))
        self.fabric(underside, 9, 4, 2, repeat=12)
        # both wrap edges rise toward the gathered hip and clear the hands
        for start, arc, drop, tile, offset in [(-1.1, 5.65, .82, 2, .014), (1.20, 5.95, .66, 14, .026)]:
            def wrap(u, v, start=start, arc=arc, drop=drop, offset=offset):
                t = start + arc * u + .42 * v * math.sin(math.pi * u)
                top = -.355 - .030 * math.sin(math.pi * u) ** 2
                span = (top - lining_hem(t)) * (drop + .09 * math.sin(TAU * u))
                y = top - span * v + .007 * v * math.cos(8 * t - 1.7 * v)
                return skirt_point(t, y, offset * (.5 + .5 * v))
            self.fabric(wrap, tile, 7, 8, repeat=12)

            def border(u, v, wrap=wrap):
                x, y, z = wrap(u, .78 + .215 * v)
                return (x * 1.010, y, z * 1.012)
            if tile == 14:
                self.fabric(border, 0, 8, 3, repeat=15)

            def lace(u, v, wrap=wrap):
                x, y, z = wrap(u, .975 + .025 * v)
                return (x * 1.003, y - .024 * v * (.45 + .55 * abs(math.sin(48 * math.pi * u))), z * 1.003)
            self.fabric(lace, 3, 10, 2, repeat=12)

        # silk swags across the front into a rosette
        def swag(u, v):
            t = -1.45 + 2.62 * u
            y = -.351 - .102 * (1 - u) - .024 * math.sin(math.pi * u) - (.018 + .038 * math.sin(math.pi * u)) * v
            return skirt_point(t, y, .029 + .003 * math.sin(math.pi * u) * math.sin(3 * math.pi * v))
        self.fabric(swag, 1, 32, 8)

        def sash(u, v):
            t = TAU * u
            return ((.173 + .003 * math.sin(4 * math.pi * v)) * math.sin(t), -.30 - .083 * v + .025 * math.sin(t),
                    -.04 - (.149 + .003 * math.sin(4 * math.pi * v)) * math.cos(t))
        self.fabric(sash, 9, 4, 6, repeat=8)
        for x, y, z, r, c in [(.138, -.352, -.141, .055, 12), (.18, -.369, -.117, .039, 2), (.117, -.378, -.174, .028, 5)]:
            self.bloom(x, y, z, r, 0, c)
        for sign in (-1, 1):
            def bow(u, v, sign=sign):
                return (.008 + sign * .13 * v, -.361 + .07 * v + (u - .5) * (.023 + .105 * math.sin(math.pi * v)),
                        .124 + .077 * math.sin(math.pi * v) + .019 * math.cos(TAU * u))
            self.fabric(bow, 2, 8, 8)

            def tail(u, v, sign=sign):
                return (.018 + sign * (.07 * v + .041 * math.sin(4 * v)) + (u - .5) * .072,
                        -.377 - .18 * v + .020 * u * v, .125 + .089 * v + .026 * math.sin(5 * v + u))
            self.fabric(tail, 2, 6, 10)
        # take in the waistband, pleats and trim together; let it out above the high-side hem
        for mesh in self.verts[waist_mesh_start:]:
            pos = mesh['p']
            fit = np.clip((pos[:, 1] + .485) / .115, 0, 1)
            fit = fit * fit * (3 - 2 * fit)
            pos[:, 0] *= 1 - .140 * fit
            pos[:, 2] = -.04 + (pos[:, 2] + .04) * (1 - .100 * fit)
        waist_y, hem_y = -.390, -.806
        for mesh in self.verts[waist_mesh_start:]:   # (the sway's weight, down the skirt)
            hang = np.clip((waist_y - mesh['p'][:, 1]) / (waist_y - hem_y), 0, 1)
            mesh['u1'][:, 0] = hang * hang * (3 - 2 * hang)
        # the footless avatar has no legs in the skirt: only the upper hip needs room
        profile = np.repeat(np.array([[0.0], [0.0], [0.0], [.105]], dtype='<f4'), 16, axis=1)
        header = struct.pack('<I7f2I', 0, waist_y, hem_y, 0., -.04, .055, hem_y, (waist_y - hem_y) / 3, 4, 16)
        (ASSETS / f'av_top_18_{self.sex}.swish').write_bytes(b'SWSH' + header + profile.tobytes())
        # the wide flounced neckline across the chest, the back and the upper arms
        for tile, offset, lip in [(3, .011, .055), (0, .015, .027)]:
            def neckline(u, v, offset=offset, lip=lip):
                t = TAU * u
                pleat = .015 * math.sin(18 * t + .8 * v) * v
                return ((.183 + offset + .039 * v + pleat) * math.sin(t),
                        -.047 - .049 * math.cos(t) ** 2 - (.043 + lip) * v + .012 * v * math.cos(18 * t),
                        -.033 - (.132 + offset + .035 * v + pleat) * math.cos(t))
            self.fabric(neckline, tile, 7, 4, repeat=12)

    def tailoring(self):
        vertices, indices, _, _ = reader.load_mdl('av_top_0_' + self.sex)
        triangles = vertices['p'][indices.reshape(-1, 3)].astype(float)

        def front(x, y):
            """the chest's front surface at (x, y); where the body is narrower than the lapel (the feminine one's at the
            collar), the nearest point inward (the pull request's fixed fallback depth threw a lapel's corner out)"""
            a, b, c = triangles.transpose(1, 0, 2)
            ba, ca = b - a, c - a
            det = ba[:, 0] * ca[:, 1] - ba[:, 1] * ca[:, 0]
            det = np.where(abs(det) > 1e-10, det, np.inf)
            for k in np.linspace(1.0, 0.0, 21):
                dx, dy = x * k - a[:, 0], y - a[:, 1]
                u = (dx * ca[:, 1] - dy * ca[:, 0]) / det
                v = (dy * ba[:, 0] - dx * ba[:, 1]) / det
                keep = (u >= 0) & (v >= 0) & (u + v <= 1) & np.isfinite(det)
                if keep.any():
                    return np.min((a[:, 2] + u * ba[:, 2] + v * ca[:, 2])[keep])
            return -.11
        for side in (-1, 1):
            def lapel(u, v, side=side):
                # a peaked notch and a rolled inner edge follow the chest
                y = .019 - .279 * v
                inner = .047 * (1 - v) + .003
                width = np.interp(v, [0, .21, .27, .33, .52, 1], [.017, .062, .032, .058, .039, .002])
                x = side * (inner + width * u)
                return (x, y, front(x, y) - .006 - .009 * math.sin(math.pi * u))
            self.fabric(lapel, 13, 5, 20)

            def tail(u, v, side=side):
                t = side * (.26 + u * (math.pi - .31))
                radius = .148 + .044 * v
                end = -.453 - .068 * (1 - math.cos(t)) - .025 * math.sin(t) ** 2
                return (radius * math.sin(t), -.332 + (end + .332) * v, -.040 - (.128 + .049 * v) * math.cos(t))
            self.fabric(tail, 7, 18, 6)   # (plain: one trim tile stretched over a tail made its flowers huge)

            def hem(u, v, tail=tail):   # a band of the gold edge and its zigzag along each tail's hem
                x, y, z = tail(u, .86 + .14 * v)
                return (x * 1.006, y, -.04 + (z + .04) * 1.006)
            self.fabric(hem, 0, 2, 2, repeat=14, band=(0.02, 0.15))
        self.bloom(-.088, -.089, front(-.088, -.089) - .019, .026, 0, 12)

    def build(self):
        self.surface()
        if self.dress:
            self.dress_panels()
            for sign in (-1, 1):   # the off-shoulder flounces round the upper arms
                shoulder = .205 if self.sex == 'm' else .169
                for drop, r, k in [(0, .080, 3), (.019, .086, 0)]:
                    self.loft([(-.045 + drop, .046, .055), (-.087 + drop, r, .084), (-.103 + drop, r * 1.03, .088)], k,
                              3 if sign < 0 else 4, 36, 6, (sign * shoulder, .008), wave=.06)
        else:
            self.tailoring()
        name = 'av_top_%d_%s' % (18 if self.dress else 17, self.sex)
        self.write(name, materials('open_design_peluru_' + ('dress' if self.dress else 'tux')), 3)


# how the sombrero sits (the owner, 2026-11-19: "the male sugar skull's hat is sitting too high on his head ... it's
# floating"). The pull request's crown is round, 19 cm across and 18 cm front to back, and the game's heads are long
# (about 15 by 20 cm), so it could sit only on top of the head: its band 7 cm above the eyes, where the game's other
# hats sit at the eyes (the cap's peak 2.3 cm above them), and even there the forehead met its front. Made as deep as
# a head (front to back by z, centred over the head at y = cz) and seated 3.5 cm lower, tipped back 4 degrees as a hat
# sits, its band lower at the nape than at the brow: every face's skin stays 4 mm and the masks that reach the
# forehead or the temples (1, 2, 5, 6) 3 mm inside the crown
SOMBRERO_FIT = {'m': (1.02, 1.25, -.044), 'f': (1.01, 1.29, -.042)}   # (x, z, cz)
SOMBRERO_DROP, SOMBRERO_TILT = .035, 4.0


def seat_sombrero(piece, sex, h, z):
    """the hat built round (0, z) at band height h, made the head's shape and seated (still in model space)"""
    sx, sz, cz = SOMBRERO_FIT[sex]
    a = math.radians(SOMBRERO_TILT)
    R = np.array(((1, 0, 0), (0, math.cos(a), -math.sin(a)), (0, math.sin(a), math.cos(a))))   # (the front up)
    pivot = np.array((0, h, cz))
    for m in piece.verts:
        p = m['p'].astype(np.float64)
        p = np.stack((p[:, 0] * sx, p[:, 1], cz + (p[:, 2] - z) * sz), axis=1)
        m['p'] = (p - pivot) @ R.T + pivot - np.array((0, SOMBRERO_DROP, 0))
        n = m['n'].astype(np.float64) / np.array((sx, 1, sz))   # (a stretch turns normals by its inverse)
        n = n @ R.T
        m['n'] = n / np.maximum(np.linalg.norm(n, axis=1)[:, None], 1e-10)


def sombrero(sex, dtype):
    """HEADGEAR 10: the pull request's hat (crown, brim, band and its flower cluster), on the head"""
    hat = Piece(sex, dtype)
    z = -.029
    h = .246 if sex == 'm' else .241
    hat.loft([(h + .127, .008, .008), (h + .119, .067, .062), (h + .086, .078, .075), (h + .035, .094, .088),
              (h + .012, .103, .096), (h + .018, .17, .15), (h + .044, .229, .192), (h + .035, .226, .19),
              (h + .007, .17, .15), (h + .002, .095, .09)], 7, 2, 48, 8, (0, z))
    hat.loft([(h + .045, .230, .193), (h + .035, .23, .193), (h + .029, .222, .187)], 0, 2, 48, 12, (0, z))
    hat.loft([(h + .026, .218, .183), (h + .001, .161, .145)], 0, 2, 48, 12, (0, z))
    hat.loft([(h + .045, .093, .087), (h + .021, .102, .096)], 2, 2, 36, 6, (0, z))
    # (the pull request's dark band from inside the crown down to the eyes, hair for its painted head, is left out:
    # on a real head it showed through at the temples as a black mark; the player's own hair is under the hat)
    for x, y, zz, r, k in [(-.062, .050, -.065, .029, 2), (-.018, .052, -.084, .027, 12), (.026, .042, -.088, .023, 5),
                           (-.09, .037, -.04, .018, 4)]:
        hat.bloom(x, h + y + .043, z + zz - .014, r * 1.42, 2, k)
    seat_sombrero(hat, sex, h, z)
    for m in hat.verts:   # into head space
        m['p'] -= EYE[sex].astype('<f4')
    hat.write('av_gear_10_' + sex, materials('open_design_peluru_tux'), 2)
    # what hair under it keeps inside (round the centre the game's own helmets use): the crown alone. The brim presses
    # no hair, and with it in the map a ponytail's or the pigtails' roots went "just inside the rim" at the brim's edge,
    # 7 cm off the head (the owner, 2026-11-19: "the hair is flying out of the hat")
    crown = Piece(sex, dtype)
    crown.loft([(h + .127, .008, .008), (h + .119, .067, .062), (h + .086, .078, .075), (h + .035, .094, .088),
                (h + .012, .103, .096), (h + .002, .095, .09)], 7, 2, 48, 8, (0, z))
    seat_sombrero(crown, sex, h, z)
    for m in crown.verts:
        m['p'] -= EYE[sex].astype('<f4')
    V, I, _, _ = crown.arrays()
    with open(ASSETS / ('av_gear_1_%s.tuck' % sex), 'rb') as f:
        C = np.array(struct.unpack('<3f', f.read(24)[12:24]), dtype=np.float64)
    write_tuck(str(ASSETS / ('av_gear_10_%s.tuck' % sex)), {'all': {'pos': V['p'][I].astype(np.float32)}}, {'C': C})


def earlobe(sex):
    """the bottom of the base head's ear (model space, the ear on +x): its lowest skin point beside the head"""
    v, idx, sections, mats = reader.load_mdl('av_bhead_' + sex)
    p = []
    for start, count, mi in sections:
        if (mats[mi]['flags'] >> 8) & 15 == CH_SKIN:
            p.append(v['p'][np.unique(idx[start:start + count])])
    p = np.concatenate(p).astype(float)
    ear = p[(np.abs(p[:, 0]) > .078) & (p[:, 1] > .07) & (p[:, 1] < .21)]
    lobe = ear[np.argmin(ear[:, 1])]
    return np.array((abs(lobe[0]), lobe[1], lobe[2]))


def hoops(sex, dtype):
    """the dress's hoop earrings: a head-space part, each hoop hanging from its earlobe on a spring of its own (drawn and
    swung as hair is). (The pull request hung them 4 cm below and 3-5 cm in front of the lobes: they floated.)"""
    eye = EYE[sex]
    lobe = earlobe(sex)
    piece = Piece(sex, dtype)
    chains = []
    for index, side in enumerate((-1, 1)):
        pivot = np.array((side * (lobe[0] + .002), lobe[1] + .004, lobe[2])) - eye   # (through the lobe)

        def hoop(u, v, pivot=pivot):
            a, b = TAU * u, TAU * v
            return pivot + np.array(((.022 + .003 * math.cos(b)) * math.cos(a), (.031 + .003 * math.cos(b)) * math.sin(a) - .031,
                                     .003 * math.sin(b)))
        piece.fabric(hoop, 4, 24, 6, index + 1, mat='metal')
        chains.append(((180, 11, .12, 30), [pivot, pivot + np.array((0, -.062, 0))]))
    piece.write('av_top_18e_' + sex, materials(''), 3)
    write_rig_text(str(ASSETS / ('av_top_18e_' + sex)), np.array((0, .19, -.024)) - eye, chains)


def sugar_skull(fk, dtype):
    """MASK 6 on one face: a shell 3 mm off the face's skin over an oval from the forehead to the chin, holes where the
    eyes are, the sugar skull's picture on it"""
    sex = fk[0]
    e = EYE[sex]
    v, idx, sections, mats = reader.load_mdl('av_bhead_' + fk)
    tris, eyes = [], []
    for start, count, mi in sections:
        t = idx[start:start + count].reshape(-1, 3)
        ch = (mats[mi]['flags'] >> 8) & 15
        if ch in (CH_SKIN, 6):   # (the skin and the brows: the mask goes over both)
            tris.append(v['p'][t].astype(float))
        elif ch == CH_NONE:   # the eyes (their own picture)
            eyes.append(v['p'][t].reshape(-1, 3))
    T = np.concatenate(tris)
    E = np.concatenate(eyes)
    eye_c = [E[E[:, 0] < 0].mean(0), E[E[:, 0] > 0].mean(0)]
    a, b, c = T.transpose(1, 0, 2)
    ba, ca = b - a, c - a
    det = ba[:, 0] * ca[:, 1] - ba[:, 1] * ca[:, 0]
    det = np.where(abs(det) > 1e-12, det, np.inf)
    nrm = np.cross(ba, ca)
    nrm /= np.maximum(np.linalg.norm(nrm, axis=1)[:, None], 1e-12)
    nrm[nrm[:, 2] > 0] *= -1   # (outward: toward the front)

    def front(x, y):
        dx, dy = x - a[:, 0], y - a[:, 1]
        u = (dx * ca[:, 1] - dy * ca[:, 0]) / det
        w = (dy * ba[:, 0] - dx * ba[:, 1]) / det
        keep = (u >= -1e-6) & (w >= -1e-6) & (u + w <= 1 + 1e-6) & np.isfinite(det)
        if not keep.any():
            return None
        z = a[:, 2] + u * ba[:, 2] + w * ca[:, 2]
        i = np.where(keep)[0][np.argmin(z[keep])]
        return np.array((x, y, z[i])) + nrm[i] * .003
    # the oval: polar rings round the face's middle
    cy = e[1] - .013
    ax, ay = .064, .092
    rings, spokes = 26, 96
    pts, ok = [], []
    centre = front(0.0, cy)
    for i in range(1, rings + 1):
        r = i / rings
        for j in range(spokes):
            t = TAU * j / spokes
            p = front(ax * r * math.cos(t), cy + ay * r * math.sin(t))
            pts.append(p if p is not None else np.zeros(3))
            ok.append(p is not None)
    P = np.array([centre] + pts)
    ok = np.array([True] + ok)

    def vid(i, j):
        return 0 if i == 0 else 1 + (i - 1) * spokes + j % spokes
    faces = []
    for i in range(rings):
        for j in range(spokes):
            if i == 0:
                faces.append((0, vid(1, j + 1), vid(1, j)))
            else:
                faces.append((vid(i, j), vid(i, j + 1), vid(i + 1, j)))
                faces.append((vid(i, j + 1), vid(i + 1, j + 1), vid(i + 1, j)))
    faces = np.array(faces)
    faces = faces[ok[faces].all(1)]
    # the eye holes: an ellipse round each eye (its lids' opening, a little wider)
    cen = P[faces].mean(1)
    hole = np.zeros(len(faces), bool)
    for ec in eye_c:
        hole |= ((cen[:, 0] - ec[0]) / .0165) ** 2 + ((cen[:, 1] - ec[1]) / .0105) ** 2 < 1
    faces = faces[~hole]
    used, inv = np.unique(faces, return_inverse=True)
    P = P[used]
    faces = inv.reshape(-1, 3)
    n = np.zeros_like(P)
    cross = np.cross(P[faces[:, 1]] - P[faces[:, 0]], P[faces[:, 2]] - P[faces[:, 0]])
    for col in faces.T:
        np.add.at(n, col, cross)
    n /= np.maximum(np.linalg.norm(n, axis=1)[:, None], 1e-12)
    if np.mean(n[:, 2]) > 0:   # (facing out: the front is -Z)
        faces = faces[:, ::-1]
        n = -n
    piece = Piece(sex, dtype)
    mesh = np.zeros(len(P), dtype)
    uv = np.array([skull_uv(sex, x, y) for x, y in P[:, :2]])
    mesh['p'] = P - e
    mesh['n'] = n
    mesh['u0'] = uv
    mesh['c'] = 255
    piece.add(mesh, faces, 'skull')
    mats = {'skull': material(CH_NONE, (.06, .06, .07), 0.25, 0, 'open_design_peluru_skull')}
    piece.write('av_mask_6_' + fk, mats, 2)


def main():
    reader.ASSETS = str(ASSETS)
    write_tex('open_design_peluru_tux', design_top(False))
    write_tex('open_design_peluru_dress', design_top(True))
    write_tex('open_design_peluru_skull', design_skull())
    for sex in 'mf':
        for dress in (False, True):
            Garment(sex, dress).build()
        v, _, _, _ = reader.load_mdl('av_top_0_' + sex)
        hoops(sex, v.dtype)
        sombrero(sex, v.dtype)
    for fk in FACES:
        v, _, _, _ = reader.load_mdl('av_bhead_' + fk)
        sugar_skull(fk, np.dtype([(f, v.dtype[f]) for f in ('p', 'n', 'u0', 'u1', 'c')]))


if __name__ == '__main__':
    main()
