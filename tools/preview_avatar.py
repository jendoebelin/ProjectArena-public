"""Offline renders of Project Arena avatars using the game's own rig and part selection (tools/avatar_pose.cpp,
built by tools/build_avatar_pose.bat) and the shader mirror in tools/preview_render.py.

Usage: python -I tools/preview_avatar.py <out.png> <look> [pose] [team] [yaw] [--close] [--size WxH]
  look: preset index, seed:N or k=v,k=v (avatar.cfg keys); pose: ready | guard | throw | rest
As a module: render_look(out, look, pose, team, yaw, close, size) for the catalog.
"""
import importlib.util
import json
import os
import subprocess
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
POSE_EXE = os.path.join(ROOT, 'build', 'tests', 'avatar_pose.exe')
_mdl = {}


def mdl(name):
    if name not in _mdl:
        _mdl[name] = pr.load_mdl(name)
    return _mdl[name]


def draw_list(look, pose='ready', team=0, yaw=0.0):
    out = subprocess.run([POSE_EXE, str(look), pose, str(team), str(yaw)], capture_output=True, text=True, check=True).stdout
    return json.loads(out)


def entries(dl, offset=(0, 0, 0), hands=True, knuckles=True, gauntlet=None):
    ch = np.array(dl['channels'], float)
    team = np.array(dl['team'], float)
    pat = pr.load_tex(dl['pattern']) if dl['pattern'] else None
    T = np.eye(4)
    T[:3, 3] = offset
    rim = tuple(team) + (0.5,)
    out = []
    for p in dl['parts']:
        M = T @ np.array(p['m'], float).reshape(4, 4).T
        em = ch[3] * 0.35 if p['neon'] else np.zeros(3)
        bones = [np.array(m, float).reshape(4, 4).T for m in p['skin']] if 'skin' in p else None
        out.append((mdl(p['model']), M, 1.0, np.ones(3), em, dict(channels=ch, rim=rim, pattern=pat, mirrored=bool(p['mirrored']),
                                                                  bones=bones)))
    if hands:
        for h in dl['hands']:
            G = T @ np.array(h['grip'], float).reshape(4, 4).T
            mir = np.diag([-1.0, 1, 1, 1]) if h['h'] == 0 else np.eye(4)
            out.append((mdl(gauntlet or dl.get('gauntlet', 'lancer')), G @ mir @ frames.LANCER, 1.0, np.ones(3), np.zeros(3),
                        dict(channels=ch, rim=rim, mirrored=h['h'] == 0)))
            if knuckles:   # Basic rules: the glass knuckle guard is always up (main.cpp kp)
                out.append((mdl('knuckleguard'), G @ mir @ frames.KNUCKLE, 0.35, 0.75 + 0.25 * team, team * 0.06,
                            dict(mirrored=h['h'] == 0)))
    return out


def render_look(out, look, pose='ready', team=0, yaw=0.0, close=False, size=(640, 720), bg=(0.035, 0.045, 0.07), supersample=2):
    dl = draw_list(look, pose, team, yaw)
    W0, H0 = pr.W, pr.H
    pr.W, pr.H = size[0] * supersample, size[1] * supersample
    if close == 'hands':   # the right gauntlet, framed on its grip
        g = np.array(dl['hands'][1]['grip'], float).reshape(4, 4).T[:3, 3]
        tgt = list(g + np.array([0.0, -0.01, 0.0]))
        eye, fov = list(g + np.array([-0.30, 0.12, 0.26])), 36   # the back of the hand, knuckles and cuff
    elif close:
        eye, tgt, fov = [0.0, 1.62, 0.62], [0.0, 1.60, 0.0], 30
    else:
        eye, tgt, fov = [0.0, 1.16, 2.55], [0.0, 1.10, 0.0], 40   # head to the thigh fade
    tmp = out + '.tmp.png'
    pr.render(entries(dl), eye, tgt, tmp, fov=fov, bg=bg)
    pr.W, pr.H = W0, H0
    Image.open(tmp).convert('RGB').resize(size, Image.LANCZOS).save(out, quality=90)
    os.remove(tmp)
    return dl


if __name__ == '__main__':
    a = [x for x in sys.argv[1:] if not x.startswith('--')]
    size = (640, 720)
    if '--size' in sys.argv:
        w, h = sys.argv[sys.argv.index('--size') + 1].split('x')
        size = (int(w), int(h))
        a = [x for x in a if x != sys.argv[sys.argv.index('--size') + 1]]
    out, look = a[0], a[1]
    pose = a[2] if len(a) > 2 else 'ready'
    team = int(a[3]) if len(a) > 3 else 0
    yaw = float(a[4]) if len(a) > 4 else 0.0
    dl = render_look(out, look, pose, team, yaw, '--close' in sys.argv, size)
    print(dl['desc'])
