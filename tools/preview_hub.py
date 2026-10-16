"""Offline mock-up of the spectator hub: the stadium with live miniature arenas on pedestals (no players).
Usage: python -I tools/preview_hub.py <assets_dir> <out.png> [n_minis]
"""
import importlib.util
import math
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
spec = importlib.util.spec_from_file_location('pr', os.path.join(ROOT, 'tools', 'preview_render.py'))
pr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pr)


def mat(pos, yaw, s):
    a = yaw
    M = np.eye(4)
    M[:3, :3] = np.array([[math.cos(a), 0, math.sin(a)], [0, 1, 0], [-math.sin(a), 0, math.cos(a)]]) * s
    M[:3, 3] = pos
    return M


def main():
    pr.ASSETS = sys.argv[1]
    out = sys.argv[2]
    n = int(sys.argv[3]) if len(sys.argv) > 3 else 3
    models = [(pr.load_mdl('environment'), np.eye(4))]
    names = ['floor', 'arena', 'court_fx', 'court_pulse']
    mdl = {k: pr.load_mdl(k) for k in names}
    for i in range(n):
        ang = 0.0 if n == 1 else math.radians(-40 + 80 * i / (n - 1))
        at = (1.35 * math.cos(ang), 0.92, 1.35 * math.sin(ang))
        # app: Quat::axisAngle(y, -ang + pi/2); the preview's yaw matrix uses the opposite sign convention
        M = mat(at, -(-ang + math.pi / 2), 1 / 17)
        for k in names:
            models.append((mdl[k], M))
    pr.render(models, [-0.35, 1.55, 0.0], [1.3, 0.85, 0.0], out)


if __name__ == '__main__':
    main()
