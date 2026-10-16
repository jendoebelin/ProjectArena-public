"""Meta Horizon Store art, rendered by the PC build itself (ProjectArena.exe --selftest --view ... --scene action
--title ...), so every pixel is our own content. Sizes follow Meta's asset guidelines (24-bit PNG).

    tools\\build_pc.bat && python -I tools/gen_store_art.py      ->  build/store_art/*.png

Covers show a staged rally (blue throws, orange blocks with a shield) with the title above the court; the icon is
the emblem on a dark field (readable at small sizes).
"""
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build', 'pc', 'ProjectArena', 'ProjectArena.exe')
OUT = os.path.join(ROOT, 'build', 'store_art')
EMBLEM = os.path.join(ROOT, 'build', 'open_src', 'tex', 'emblem.png')
TITLE = 'PROJECT ARENA'
BANNER = (-7.5, 2.0, 0.0, -90.0, 1.0)           # wide banners: level view, the scoreboard lower in the frame
SHOULDER = (-9.0, 2.0, 0.85, -93.0, -5.0)       # over blue's right shoulder, down the court
COVER = (-7.5, 2.4, 0.0, -90.0, -9.0)           # just inside blue's strike ring, centred: the title above the scoreboard
SIDE = (-11.0, 3.5, 4.5, -67.7, -12.0)          # high, from the side
MENU = (-7.6, 1.7, 0.0, -90.0, 6.0)             # the main menu as the player sees it, avatar on its pedestal

# name, width, height, camera, vertical fov, scene, title height (fraction), title offset (-1..1, + up)
ART = [
    ('hero_cover', 3000, 900, BANNER, 46, 'action', 0.15, 0.80),
    ('cover_landscape', 2560, 1440, COVER, 58, 'action', 0.095, 0.70),
    ('cover_square', 1440, 1440, COVER, 66, 'action', 0.07, 0.76),
    ('cover_portrait', 1008, 1440, COVER, 76, 'action', 0.046, 0.80),
    ('mini_landscape', 1080, 360, BANNER, 42, 'action', 0.18, 0.78),
    ('screenshot_1', 2560, 1440, SHOULDER, 60, 'action', 0.0, 0.0),
    ('screenshot_2', 2560, 1440, SIDE, 60, 'action', 0.0, 0.0),
    ('screenshot_3', 2560, 1440, MENU, 96, 'menu:title', 0.0, 0.0),
]


def icon(path, size=512):
    """The emblem on a deep navy field with a soft glow (512 px, plus a 1024 px master)."""
    big = 1024
    yy, xx = np.mgrid[0:big, 0:big] / (big - 1) - 0.5
    r = np.hypot(xx, yy)
    bg = np.dstack([0.012 + 0.05 * np.clip(1 - r * 1.6, 0, 1), 0.020 + 0.09 * np.clip(1 - r * 1.6, 0, 1),
                    0.045 + 0.16 * np.clip(1 - r * 1.6, 0, 1)])
    img = Image.fromarray((np.clip(bg, 0, 1) * 255).astype(np.uint8), 'RGB').convert('RGBA')
    em = Image.open(EMBLEM).convert('RGBA').resize((int(big * 0.82),) * 2, Image.LANCZOS)
    layer = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    off = (big - em.width) // 2
    layer.alpha_composite(em, (off, off))
    glow = layer.filter(ImageFilter.GaussianBlur(28))
    img.alpha_composite(glow)
    img.alpha_composite(glow)
    img.alpha_composite(layer)
    img = img.convert('RGB')
    img.save(os.path.join(OUT, 'icon_1024.png'), optimize=True)
    img.resize((size, size), Image.LANCZOS).save(path, optimize=True)


def main():
    if not os.path.exists(EXE):
        sys.exit('build the PC version first: tools\\build_pc.bat')
    os.makedirs(OUT, exist_ok=True)
    for name, w, h, cam, fov, scene, th, ty in ART:
        ppm = os.path.join(OUT, name + '.ppm')
        cmd = [EXE, '--selftest', ppm, '--view', str(w), str(h)] + ['%g' % c for c in cam] + ['%g' % fov]
        if scene.startswith('menu:'):
            cmd += ['--menu', scene[5:]]
        elif scene:
            cmd += ['--scene', scene]
        if th > 0:
            cmd += ['--title', TITLE, '%g' % th, '%g' % ty]
        r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        if r.returncode != 0 or not os.path.exists(ppm):
            sys.exit('%s: render failed (%d)' % (name, r.returncode))
        Image.open(ppm).convert('RGB').save(os.path.join(OUT, name + '.png'), optimize=True)
        os.remove(ppm)
        print('%-16s %dx%d' % (name, w, h))
    icon(os.path.join(OUT, 'icon.png'))
    print('icon             512x512 (+ icon_1024.png)')
    print('wrote', OUT)


if __name__ == '__main__':
    main()
