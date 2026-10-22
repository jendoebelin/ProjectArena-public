"""The Crowd's cheer bursts: a heart and a star (app/assets_open/tex/fx_heart.tex, fx_star.tex; 64 x 64 RGBA, white
shapes with a soft edge, tinted by the game). Drawn here from simple shapes (no emoji fonts, no outside art).
Usage: python -I tools/gen_crowd_fx.py"""
import math
import os
import struct

import numpy as np
from PIL import Image, ImageDraw, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TEX = os.path.join(ROOT, 'app', 'assets_open', 'tex')
N, S = 64, 8   # size, supersampling


def write_tex(path, im):
    a = np.asarray(im.convert('RGBA'))
    with open(path, 'wb') as fh:
        fh.write(b'STEX' + struct.pack('<3I', im.width, im.height, 0))
        fh.write(a.tobytes())


def heart(d, s):
    pts = []
    for i in range(200):
        t = 2 * math.pi * i / 200
        x = 16 * math.sin(t) ** 3
        y = 13 * math.cos(t) - 5 * math.cos(2 * t) - 2 * math.cos(3 * t) - math.cos(4 * t)
        pts.append((s / 2 + x * s / 38, s / 2 - y * s / 38 - s * 0.03))
    d.polygon(pts, fill=255)


def star(d, s):
    pts = []
    for i in range(10):
        r = s * (0.47 if i % 2 == 0 else 0.2)
        t = -math.pi / 2 + i * math.pi / 5
        pts.append((s / 2 + r * math.cos(t), s / 2 + r * math.sin(t) + s * 0.03))
    d.polygon(pts, fill=255)


def shape(fn):
    m = Image.new('L', (N * S, N * S), 0)
    fn(ImageDraw.Draw(m), N * S)
    m = m.resize((N, N), Image.LANCZOS)
    glow = m.filter(ImageFilter.GaussianBlur(2.5)).point(lambda v: v * 0.55)
    a = np.maximum(np.asarray(m, float), np.asarray(glow, float)).astype(np.uint8)
    core = np.asarray(m, float) / 255.0
    rgb = (200 + 55 * core).astype(np.uint8)   # white, the glow a touch darker (the game tints it)
    out = np.dstack([rgb, rgb, rgb, a])
    return Image.fromarray(out, 'RGBA')


def coloured(draw_fn, size=64):
    """a full-colour picture (drawn at S x and scaled down; the game draws it untinted)"""
    im = Image.new('RGBA', (size * S, size * S), (0, 0, 0, 0))
    draw_fn(ImageDraw.Draw(im), size * S)
    return im.resize((size, size), Image.LANCZOS)


def hotdog(d, s):   # HOTDOG MODE's cheer: a hot dog in a bun with a mustard squiggle
    u = s / 64.0
    d.rounded_rectangle((6 * u, 24 * u, 58 * u, 46 * u), radius=11 * u, fill=(214, 160, 84, 255), outline=(150, 100, 45, 255), width=int(2 * u))
    d.rounded_rectangle((2 * u, 21 * u, 62 * u, 35 * u), radius=7 * u, fill=(178, 62, 38, 255), outline=(120, 38, 22, 255), width=int(2 * u))
    pts = [((8 + 6 * i) * u, (26 if i % 2 else 30) * u) for i in range(9)]
    d.line(pts, fill=(255, 214, 40, 255), width=int(3 * u), joint='curve')
    d.rounded_rectangle((10 * u, 38 * u, 54 * u, 50 * u), radius=6 * u, fill=(230, 180, 100, 255))


def smiley(d, s):
    u = s / 64.0
    d.ellipse((4 * u, 4 * u, 60 * u, 60 * u), fill=(255, 212, 40, 255), outline=(200, 140, 0, 255), width=int(2 * u))
    d.ellipse((20 * u, 20 * u, 27 * u, 30 * u), fill=(70, 40, 10, 255))
    d.ellipse((37 * u, 20 * u, 44 * u, 30 * u), fill=(70, 40, 10, 255))
    d.arc((16 * u, 22 * u, 48 * u, 50 * u), 20, 160, fill=(70, 40, 10, 255), width=int(4 * u))


def foam_finger(d, s):   # a foam "#1" hand, white and greys: the game tints it with the end's colour
    # (owner, 2026-10-21: the first one, a finger straight up from the middle of the fist, read as the wrong finger.
    # Now the pointing finger rises from the side of the mitt, the other fingers fold over the top as knuckles and
    # the thumb wraps across the front, as on a stadium foam finger.)
    u = s / 128.0
    edge = (140, 140, 140, 255)
    white = (255, 255, 255, 255)
    d.rounded_rectangle((26 * u, 56 * u, 104 * u, 122 * u), radius=20 * u, fill=white, outline=edge, width=int(3 * u))   # the mitt
    d.rounded_rectangle((28 * u, 4 * u, 52 * u, 74 * u), radius=12 * u, fill=white, outline=edge, width=int(3 * u))      # index, at the side
    for x in (54, 70, 86):   # three folded fingers: knuckles along the top
        d.rounded_rectangle((x * u, 50 * u, (x + 17) * u, 70 * u), radius=8 * u, fill=white, outline=edge, width=int(3 * u))
    d.rounded_rectangle((44 * u, 72 * u, 96 * u, 88 * u), radius=8 * u, fill=white, outline=edge, width=int(3 * u))      # the thumb across
    t = (115, 115, 115, 255)   # "#1" on the mitt, a darker shade of the colour
    w = int(4 * u)
    d.line(((52 * u, 94 * u), (49 * u, 116 * u)), fill=t, width=w)
    d.line(((61 * u, 94 * u), (58 * u, 116 * u)), fill=t, width=w)
    d.line(((45 * u, 101 * u), (66 * u, 101 * u)), fill=t, width=w)
    d.line(((44 * u, 109 * u), (65 * u, 109 * u)), fill=t, width=w)
    d.line(((80 * u, 93 * u), (80 * u, 117 * u)), fill=t, width=int(5 * u))
    d.line(((73 * u, 99 * u), (80 * u, 93 * u)), fill=t, width=w)


def main():
    os.makedirs(TEX, exist_ok=True)
    for name, fn in (('fx_heart', heart), ('fx_star', star)):
        im = shape(fn)
        write_tex(os.path.join(TEX, name + '.tex'), im)
        im.save(os.path.join(ROOT, 'build', name + '_preview.png'))
    for name, fn, size in (('fx_hotdog', hotdog, 64), ('fx_smile', smiley, 64), ('fx_foam', foam_finger, 128)):
        im = coloured(fn, size)
        write_tex(os.path.join(TEX, name + '.tex'), im)
        im.save(os.path.join(ROOT, 'build', name + '_preview.png'))
    print('fx_heart, fx_star, fx_hotdog, fx_smile, fx_foam -> %s' % TEX)


if __name__ == '__main__':
    main()
