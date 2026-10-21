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


def main():
    os.makedirs(TEX, exist_ok=True)
    for name, fn in (('fx_heart', heart), ('fx_star', star)):
        im = shape(fn)
        write_tex(os.path.join(TEX, name + '.tex'), im)
        im.save(os.path.join(ROOT, 'build', name + '_preview.png'))
    print('fx_heart, fx_star -> %s' % TEX)


if __name__ == '__main__':
    main()
