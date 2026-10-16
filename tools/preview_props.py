"""Offline preview of the open props placed the way the app draws them (grip / head / body frames).
Usage: python -I tools/preview_props.py <props_dir> <out.png> [arena_assets_dir]
"""
import importlib.util
import math
import os
import sys

import numpy as np

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import frames  # noqa: E402

spec = importlib.util.spec_from_file_location('pr', os.path.join(ROOT, 'tools', 'preview_render.py'))
pr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pr)


def rot_y(deg):
    a = math.radians(deg)
    M = np.eye(4)
    M[:3, :3] = [[math.cos(a), 0, math.sin(a)], [0, 1, 0], [-math.sin(a), 0, math.cos(a)]]
    return M


def rot_z(deg):
    a = math.radians(deg)
    M = np.eye(4)
    M[:3, :3] = [[math.cos(a), -math.sin(a), 0], [math.sin(a), math.cos(a), 0], [0, 0, 1]]
    return M


def T(p):
    M = np.eye(4)
    M[:3, 3] = p
    return M


def S(s):
    M = np.eye(4)
    M[0, 0] = M[1, 1] = M[2, 2] = s
    return M


def main():
    props_dir, out = sys.argv[1], sys.argv[2]
    arena_dir = sys.argv[3] if len(sys.argv) > 3 else None
    models = []
    if arena_dir:
        pr.ASSETS = arena_dir
        if os.path.exists(os.path.join(arena_dir, 'floor.mdl')):
            models += [(pr.load_mdl('floor_reflect'), np.eye(4)), (pr.load_mdl('floor'), np.eye(4), 0.8)]
        models += [(pr.load_mdl('environment'), np.eye(4)), (pr.load_mdl('arena'), np.eye(4))]
    pr.ASSETS = props_dir
    L = lambda n: pr.load_mdl(n)
    # right hand held out, pointing down the court (+X), slightly raised
    grip = T((-6.55, 1.30, 0.22)) @ rot_y(-90) @ rot_z(-10)
    team = np.array([0.1, 0.7, 1.0])
    hand = 0.6 + 0.4 * team
    models.append((L('lancer'), grip @ frames.LANCER, 1.0, hand))
    models.append((L('knuckleguard'), grip @ frames.KNUCKLE, 0.35, 0.75 + 0.25 * team))
    ball_at = grip @ np.array([-0.02, 0.02, -0.25, 1.0])
    models.append((L('projectile'), T(ball_at[:3]) @ S(1.25), 1.0, team * 2.0))
    models.append((L('projectile_shell'), T(ball_at[:3]) @ S(1.25), 1.0, team * 2.0))
    # left hand with the shield (placement "below": rotZ 180 at (0, -0.10, -0.20))
    lgrip = T((-6.50, 1.15, -0.35)) @ rot_y(-90) @ rot_z(15)
    mirror = np.diag([-1.0, 1, 1, 1])
    models.append((L('lancer'), lgrip @ mirror @ frames.LANCER, 1.0, hand))
    models.append((L('shield'), lgrip @ T((0, -0.10, -0.20)) @ rot_z(180) @ frames.PLATE, 0.7, team))
    # opponent avatar further down the court, facing the camera
    hp = np.array([-3.2, 1.62, 0.0])
    head = T(hp) @ rot_y(90) @ rot_y(180) @ T((0, -1.66, 0))
    models.append((L('head'), head))
    models.append((L('torso'), T((hp[0], hp[1] - 1.66, hp[2])) @ rot_y(math.degrees(math.pi / 2 + math.pi))))
    models.append((L('spawner'), T((-4.6, 0.0, 0.9))))
    models.append((L('strikezone'), T((-2.6, 1.28, -0.4)), 1.0, np.array([1.0, 0.4, 0.1]) * 0.35))
    cam = os.environ.get('PROPS_CAM')   # "ex,ey,ez,tx,ty,tz" to frame a close-up
    if cam:
        c = [float(t) for t in cam.split(',')]
        pr.render(models, c[:3], c[3:], out)
    else:
        pr.render(models, [-7.0, 1.55, 0.0], [-5.0, 1.15, 0.0], out)


if __name__ == '__main__':
    main()
