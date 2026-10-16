"""Render the right-hand gauntlet (Lancer), knuckle guard, held ball and the shield plate in each candidate
placement, from the player's eye, to pick the correct shield side. Mirrors app/src/main.cpp ueLocalToGrip
and Game::shieldLocal. Usage: python -I tools/preview_hand.py <out.png>"""
import math, os, sys
import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(__file__))
import preview_render as pr


def ue_local_to_grip(loc_cm, pyr_deg, scale):
    p, y, r = [math.radians(v) for v in pyr_deg]
    sp, cp, sy, cy, sr, cr = math.sin(p), math.cos(p), math.sin(y), math.cos(y), math.sin(r), math.cos(r)
    R = [[cp * cy, cp * sy, sp], [sr * sp * cy - cr * sy, sr * sp * sy + cr * cy, -sr * cp], [-(cr * sp * cy + sr * sy), cy * sr - cr * sp * sy, cr * cp]]
    T = np.eye(4)
    for c in range(3):
        for rr in range(3):
            T[rr, c] = R[c][rr] * scale[c]
    T[:3, 3] = loc_cm
    S = np.zeros((4, 4)); S[0, 0] = 100; S[2, 1] = 100; S[1, 2] = 100; S[3, 3] = 1
    C = np.zeros((4, 4)); C[2, 0] = -0.01; C[0, 1] = 0.01; C[1, 2] = 0.01; C[3, 3] = 1
    return C @ T @ S


def quat_mat(axis, ang):
    a = np.array(axis, float); a /= np.linalg.norm(a)
    x, y, z = a * math.sin(ang / 2); w = math.cos(ang / 2)
    M = np.eye(4)
    M[:3, :3] = [[1 - 2 * (y * y + z * z), 2 * (x * y - w * z), 2 * (x * z + w * y)],
                 [2 * (x * y + w * z), 1 - 2 * (x * x + z * z), 2 * (y * z - w * x)],
                 [2 * (x * z - w * y), 2 * (y * z + w * x), 1 - 2 * (x * x + y * y)]]
    return M


def trans(v):
    M = np.eye(4); M[:3, 3] = v; return M


def shield_local(place, side=1.0):
    if place == 0: return trans((0, 0.10, -0.20))
    if place == 1: return trans((0, -0.10, -0.20)) @ quat_mat((0, 0, 1), math.pi)
    if place == 2: return trans((side * 0.10, 0, -0.08)) @ quat_mat((0, 0, 1), -side * math.pi / 2)
    return trans((-side * 0.10, 0, -0.08)) @ quat_mat((0, 0, 1), side * math.pi / 2)


def main(out):
    # right hand raised in front of the player, controller pointing forward/up (+X court, tilted 35 deg up), thumb up
    grip = trans((-6.55, 1.35, 0.18)) @ quat_mat((0, 1, 0), -math.pi / 2) @ quat_mat((1, 0, 0), math.radians(35))
    lancer = pr.load_mdl('lancer'); knuckle = pr.load_mdl('knuckleguard'); shield = pr.load_mdl('shield'); ball = pr.load_mdl('projectile')
    L = ue_local_to_grip((1, -0.5, 0), (5, 0, 0), (1, 1, 1))
    K = ue_local_to_grip((10.5, 0, 0), (0, 0, 0), (1, 1, 1))
    P = ue_local_to_grip((0, 0, 0), (0, 0, 180), (10, 10, 10))
    launcher = grip @ np.array([-0.02, 0.02, -0.25, 1])
    names = ['0 ABOVE (original default)', '1 BELOW (current build)', '2 BACK OF HAND (outside)', '3 PALM SIDE (inside)']
    tiles = []
    for place in range(4):
        models = [(lancer, grip @ L), (knuckle, grip @ K), (ball, trans(launcher[:3])), (shield, grip @ shield_local(place) @ P)]
        tmp = os.path.join(os.path.dirname(out), '_hand%d.png' % place)
        pr.W, pr.H = 640, 480
        pr.render(models, (-7.0, 1.62, 0.05), (-6.4, 1.3, 0.18), tmp)
        im = Image.open(tmp)
        ImageDraw.Draw(im).text((10, 10), names[place], fill=(255, 255, 0))
        tiles.append(im)
        os.remove(tmp)
    sheet = Image.new('RGB', (1280, 960))
    for i, t in enumerate(tiles):
        sheet.paste(t, ((i % 2) * 640, (i // 2) * 480))
    sheet.save(out)
    print('wrote', out)


if __name__ == '__main__':
    main(sys.argv[1])
