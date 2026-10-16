"""Bind-pose turnaround of the skinned MPFB avatars (no rig needed): front, side and back.
Usage: python -I tools/preview_skinned.py <out.png> <body m|f> <top N> <bottom K|0> [primary r,g,b] [secondary r,g,b]"""
import importlib.util
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location('pr', os.path.join(ROOT, 'tools', 'preview_render.py'))
pr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pr)
pr.ASSETS = os.path.join(ROOT, 'app', 'assets_open')


def lin(c):
    return np.power(np.array(c, float) / 255.0, 2.2)


def main():
    out, body, top, bottom = sys.argv[1], sys.argv[2], int(sys.argv[3]), int(sys.argv[4])
    prim = [int(x) for x in sys.argv[5].split(',')] if len(sys.argv) > 5 else (92, 56, 36)
    sec = [int(x) for x in sys.argv[6].split(',')] if len(sys.argv) > 6 else (30, 82, 196)
    ch = np.ones((8, 3))
    ch[1], ch[2], ch[3] = lin(prim), lin(sec), lin((40, 230, 255)) * 1.15
    ch[4], ch[6], ch[7] = lin((214, 160, 122)), lin((54, 36, 26)), np.array([0.5, 0.52, 0.56]) * 0.7 + lin(sec) * 0.3
    names = ['av_bhead_' + body, 'av_top_%d_%s' % (top, body)] + (['av_bottom_%d_%s' % (bottom, body)] if bottom else ['av_wfade_' + body])
    bones = np.tile(np.eye(4), (7, 1, 1))
    tiles = []
    pr.W, pr.H = 520, 900
    for yaw in (0, 90, 180):
        a = np.radians(yaw)
        eye = (2.3 * np.sin(a), -0.30, -2.3 * np.cos(a))
        models = [(pr.load_mdl(n), np.eye(4), 1.0, np.ones(3), np.zeros(3), dict(channels=ch, bones=bones)) for n in names]
        tmp = out + '.%d.png' % yaw
        pr.render(models, eye, (0, -0.32, 0), tmp, fov=38, bg=(0.80, 0.81, 0.84))
        with Image.open(tmp) as im:
            tiles.append(im.copy())
        os.remove(tmp)
    m = Image.new('RGB', (pr.W * 3, pr.H))
    for i, t in enumerate(tiles):
        m.paste(t, (i * pr.W, 0))
    m.save(out)
    print('wrote', out)


main()
