"""Turnaround of the GAUNTLET styles (gauntlet_0..2 and the classic lancer), in avatar colours.
Usage: python -I tools/preview_gauntlets.py [out.png]"""
import importlib.util
import os
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import frames  # noqa: E402

spec = importlib.util.spec_from_file_location('pr', os.path.join(ROOT, 'tools', 'preview_render.py'))
pr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pr)
pr.ASSETS = os.path.join(ROOT, 'app', 'assets_open')


def lin(c):
    return np.power(np.array(c, float) / 255.0, 2.2)


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'gauntlets.png')
    ch = np.ones((8, 3))
    ch[1], ch[2], ch[3] = lin((30, 82, 196)), lin((52, 56, 64)), lin((40, 230, 255)) * 1.15
    ch[7] = np.array([0.5, 0.52, 0.56]) * 0.7 + ch[2] * 0.3
    names = ['gauntlet_0', 'gauntlet_1', 'gauntlet_2', 'lancer']
    # camera directions (grip frame: +X back of the hand, +Y forward, +Z up)
    views = [(0.85, -0.15, 0.50), (0.25, 0.90, 0.35), (-0.80, 0.30, 0.50)]
    S = 360
    sheet = Image.new('RGB', (S * len(views), S * len(names)))
    for r, n in enumerate(names):
        m = pr.load_mdl(n)
        M = frames.LANCER
        P = np.c_[m[0]['p'], np.ones(len(m[0]))] @ M.T
        c = (P[:, :3].min(0) + P[:, :3].max(0)) / 2
        for k, d in enumerate(views):
            d = np.array(d) / np.linalg.norm(d)
            # grip frame (Blender: +Y forward, +Z up) -> app (x, z, -y)
            dapp = np.array([d[0], d[2], -d[1]])
            eye = c + dapp * 0.42
            pr.W, pr.H = S, S
            tmp = os.path.join(ROOT, 'build', '_g.png')
            pr.render([(m, M, 1.0, np.ones(3), np.zeros(3), dict(channels=ch, rim=(0.1, 0.7, 1.0, 0.35)))], eye, c, tmp, fov=34,
                      bg=(0.06, 0.07, 0.10))
            with Image.open(tmp) as im:
                sheet.paste(im.copy(), (k * S, r * S))
            os.remove(tmp)
    sheet.save(out)
    print('wrote', out)


main()
