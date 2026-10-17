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
OVERVIEW = (-15.0, 6.5, 0.0, -90.0, -18.0)      # a spectator's view from high behind blue: the whole court
SIDEVIEW = (0.0, 2.0, 6.5, 0.0, -5.0)           # from the side at centre court, both players (owner's choice)
SIDEBANNER = (0.0, 2.2, 6.9, 0.0, -4.0)         # the same for banners: players and title inside Meta's safe area
# Meta's hero safe area (dashboard preview): about x 17.5..82.5 %, y 13.5..83 %; the strip below stays clear

# name, width, height, camera, vertical fov, scene, title height (fraction), title offset (-1..1, + up)
ART = [
    ('hero_cover', 3000, 900, SIDEBANNER, 52, 'action', 0.13, 0.42),
    ('cover_landscape', 2560, 1440, (0.0, 2.2, 6.9, 0.0, -3.0), 72, 'action', 0.08, 0.42),   # title well below the top
    ('cover_square', 1440, 1440, COVER, 66, 'action', 0.07, 0.70),
    ('cover_portrait', 1008, 1440, COVER, 76, 'action', 0.046, 0.72),
    ('mini_landscape', 1080, 360, SIDEBANNER, 57, 'action', 0.14, 0.42),
    ('screenshot_1', 2560, 1440, SHOULDER, 60, 'action', 0.0, 0.0),
    ('screenshot_2', 2560, 1440, SIDE, 60, 'action', 0.0, 0.0),
    ('screenshot_3', 2560, 1440, MENU, 96, 'menu:title', 0.0, 0.0),
    ('screenshot_4', 2560, 1440, OVERVIEW, 60, 'action', 0.0, 0.0),
    ('screenshot_5', 2560, 1440, MENU, 96, 'menu:help', 0.0, 0.0),
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
    tile_bg = img.convert('RGB')                     # spatialized tile: the field alone ...
    img.alpha_composite(glow)
    img.alpha_composite(glow)
    img.alpha_composite(layer)
    img = img.convert('RGB')
    img.save(os.path.join(OUT, 'icon_1024.png'), optimize=True)
    img.resize((size, size), Image.LANCZOS).save(path, optimize=True)
    tile_bg.resize((512, 512), Image.LANCZOS).save(os.path.join(OUT, 'tile_background.png'), optimize=True)
    # ... and the emblem with its glow on transparency, inside the 392 px safe area (it comes forward on hover)
    fg = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    fg.alpha_composite(glow)
    fg.alpha_composite(glow)
    fg.alpha_composite(layer)
    out = Image.new('RGBA', (512, 512), (0, 0, 0, 0))
    out.alpha_composite(fg.resize((392, 392), Image.LANCZOS), (60, 60))
    out.save(os.path.join(OUT, 'tile_foreground.png'), optimize=True)


def render(cmd, ppm):
    r = subprocess.run(cmd, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
    if r.returncode != 0 or not os.path.exists(ppm):
        sys.exit('render failed (%d): %s' % (r.returncode, ' '.join(cmd)))
    a = np.asarray(Image.open(ppm).convert('RGB')).astype(np.float32) / 255
    os.remove(ppm)
    return a


def with_title(base, lettered, height_px):
    """The white title letters (lettered - base) with a soft cyan glow behind them."""
    mask = np.clip((lettered - base).max(axis=2) * 1.5, 0, 1)
    m = Image.fromarray((mask * 255).astype(np.uint8), 'L')
    wide = np.asarray(m.filter(ImageFilter.GaussianBlur(height_px * 0.35))).astype(np.float32) / 255
    near = np.asarray(m.filter(ImageFilter.GaussianBlur(height_px * 0.10))).astype(np.float32) / 255
    glow = np.clip(wide * 1.6 + near * 0.9, 0, 1)[..., None] * np.array([0.15, 0.70, 1.0], np.float32)
    out = 1 - (1 - base) * (1 - glow * 0.85)          # screen the glow onto the scene
    return out * (1 - mask[..., None]) + lettered * mask[..., None]


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
        img = render(cmd, ppm)
        if th > 0:   # the same view again with the plain white title; the glow is made here
            img = with_title(img, render(cmd + ['--title', TITLE, '%g' % -th, '%g' % ty], ppm), th * h)
        Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGB').save(os.path.join(OUT, name + '.png'), optimize=True)
        print('%-16s %dx%d' % (name, w, h))
    icon(os.path.join(OUT, 'icon.png'))
    print('icon             512x512 (+ icon_1024.png)')
    print('wrote', OUT)


if __name__ == '__main__':
    main()
