"""Usage: python -I tools/make_camera_guides.py streaming/camera_guides
Camera-match guides for stream backgrounds: for each PC spectator preset camera (main.cpp desktopPreset, 66 deg
vertical view, 16:9), a 1920 x 1080 picture with the horizon, a 1 m floor grid, the court's centre line and two
1.75 m players on their start spots (x = -7 / +7 m). A background drawn to match lines up with the game."""
import math
import os
import sys

from PIL import Image, ImageDraw, ImageFont

W, H, FOV = 1920, 1080, 66.0
CAMS = [('1_side', 'SIDE (the default camera)', (0, 2.7, 7.3), (0, 1.0, 0)),
        ('2_high_side', 'HIGH SIDE', (0, 5.2, 6.6), (0, 0.6, 0)),
        ('3_blue_end', 'BLUE END', (-12.0, 2.8, 0), (0, 1.3, 0)),
        ('4_orange_end', 'ORANGE END', (12.0, 2.8, 0), (0, 1.3, 0))]
TY = math.tan(math.radians(FOV / 2))
TX = TY * W / H


def basis(pos, tgt):
    f = [tgt[i] - pos[i] for i in range(3)]
    n = math.sqrt(sum(c * c for c in f))
    f = [c / n for c in f]
    r = [-f[2], 0.0, f[0]]   # f x (0, 1, 0)
    n = math.sqrt(sum(c * c for c in r))
    r = [c / n for c in r]
    u = [r[1] * f[2] - r[2] * f[1], r[2] * f[0] - r[0] * f[2], r[0] * f[1] - r[1] * f[0]]
    return f, r, u


def project(cam, p):
    pos, (f, r, u) = cam
    d = [p[i] - pos[i] for i in range(3)]
    z = sum(d[i] * f[i] for i in range(3))
    if z < 0.1:
        return None
    x = sum(d[i] * r[i] for i in range(3))
    y = sum(d[i] * u[i] for i in range(3))
    return (W / 2 + x / z / TX * W / 2, H / 2 - y / z / TY * H / 2)


def line(dr, cam, a, b, fill, width):
    # clip the segment to in front of the camera (split into short pieces)
    pts = []
    for k in range(41):
        t = k / 40
        q = project(cam, [a[i] + (b[i] - a[i]) * t for i in range(3)])
        if q:
            pts.append(q)
        elif len(pts) > 1:
            dr.line(pts, fill=fill, width=width)
            pts = []
    if len(pts) > 1:
        dr.line(pts, fill=fill, width=width)


def guide(name, title, pos, tgt, out):
    cam = (pos, basis(pos, tgt))
    im = Image.new('RGB', (W, H), (18, 20, 28))
    dr = ImageDraw.Draw(im)
    try:
        font = ImageFont.truetype('arial.ttf', 26)
        small = ImageFont.truetype('arial.ttf', 20)
    except OSError:
        font = small = ImageFont.load_default()
    R = 40
    for i in range(-R, R + 1):   # the 1 m floor grid (every 5 m brighter)
        c = (90, 100, 120) if i % 5 else (150, 165, 190)
        line(dr, cam, (i, 0, -R), (i, 0, R), c, 1 if i % 5 else 2)
        line(dr, cam, (-R, 0, i), (R, 0, i), c, 1 if i % 5 else 2)
    line(dr, cam, (0, 0, -R), (0, 0, R), (255, 255, 255), 3)   # the centre line between the two halves
    f = cam[1][0]
    pitch = math.asin(f[1])
    hy = H / 2 - math.tan(-pitch) / TY * H / 2   # the horizon (eye level, far away)
    if 0 <= hy <= H:
        dr.line([(0, hy), (W, hy)], fill=(255, 210, 60), width=3)
        dr.text((12, hy - 32), 'HORIZON  y = %d px (%d%% down)' % (round(hy), round(100 * hy / H)), fill=(255, 210, 60), font=font)
    for px, col, label in ((-7, (60, 170, 255), 'BLUE'), (7, (255, 150, 60), 'ORANGE')):
        feet, head = project(cam, (px, 0, 0)), project(cam, (px, 1.75, 0))
        if not feet or not head:
            continue
        h = feet[1] - head[1]
        w = h * 0.28
        dr.rounded_rectangle([feet[0] - w / 2, head[1], feet[0] + w / 2, feet[1]], radius=max(1, w / 2 - 2), fill=col)
        dr.text((feet[0] - 70, feet[1] + 8), '%s 1.75 m = %d px' % (label, round(h)), fill=col, font=small)
    dr.text((12, H - 70), 'Project Arena spectator camera %s: eye %.1f m up, looking at (%g, %g, %g); %g deg vertical view, 16:9'
            % (title, pos[1], tgt[0], tgt[1], tgt[2], FOV), fill=(220, 220, 230), font=small)
    dr.text((12, H - 40), 'Floor grid 1 m (bright lines every 5 m); white = the court centre line; players on their start spots, 14 m apart',
            fill=(220, 220, 230), font=small)
    path = os.path.join(out, 'camera_guide_%s.png' % name)
    im.save(path)
    print(path, 'horizon %d px' % round(hy))


if __name__ == '__main__':
    out = sys.argv[1]
    os.makedirs(out, exist_ok=True)
    for c in CAMS:
        guide(*c, out)
