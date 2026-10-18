"""Steam art (docs/STEAM.md), rendered by the PC build like the Meta art (tools/gen_store_art.py): the same staged
rally, cameras and title treatment, at Steam's sizes.

    tools\\build_pc.bat && python -I tools/gen_steam_art.py      ->  build/steam_art/

Library assets (the Playtest's review checks these and the icons): library_capsule 600x900, library_header 920x430,
library_hero 3840x1240 (no text: Steam lays the logo over it), library_logo 1280x720 (title only, transparent),
community_icon 184x184 (JPG), client_icon.ico. Store page assets (for the store page later): header_capsule 920x430,
small_capsule 462x174, main_capsule 1232x706, vertical_capsule 748x896, page_background 1438x810, and 1920x1080
screenshots.
"""
import os
import sys

import numpy as np
from PIL import Image, ImageFilter

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import gen_store_art as meta  # noqa: E402

OUT = os.path.join(meta.ROOT, 'build', 'steam_art')
SIDE = meta.SIDEBANNER                 # the side view of the arena at centre court (the owner's choice)
SIDE_LOW = (0.0, 2.0, 6.5, 0.0, -5.0)  # meta.SIDEVIEW
COVER = meta.COVER                     # inside blue's strike ring: tall formats

# name, width, height, camera, vertical fov, scene, title height (fraction of the image), title offset (-1..1, + up)
ART = [
    ('library_capsule', 600, 900, COVER, 78, 'action', 0.05, 0.74),
    ('library_header', 920, 430, SIDE, 60, 'action', 0.12, 0.45),
    ('library_hero', 3840, 1240, SIDE_LOW, 44, 'action', 0.0, 0.0),
    ('header_capsule', 920, 430, SIDE, 60, 'action', 0.12, 0.45),
    ('small_capsule', 462, 174, SIDE, 56, 'action', 0.20, 0.40),
    ('main_capsule', 1232, 706, SIDE, 66, 'action', 0.10, 0.45),
    ('vertical_capsule', 748, 896, COVER, 74, 'action', 0.055, 0.72),
    ('page_background', 1438, 810, meta.OVERVIEW, 60, 'action', 0.0, 0.0),
    ('screenshot_1', 1920, 1080, meta.SHOULDER, 60, 'action', 0.0, 0.0),
    ('screenshot_2', 1920, 1080, meta.SIDE, 60, 'action', 0.0, 0.0),
    ('screenshot_3', 1920, 1080, SIDE_LOW, 60, 'action', 0.0, 0.0),
    ('screenshot_4', 1920, 1080, meta.OVERVIEW, 60, 'action', 0.0, 0.0),
    ('screenshot_5', 1920, 1080, meta.MENU, 96, 'menu:title', 0.0, 0.0),
]


def cmd_for(name, w, h, cam, fov, scene):
    ppm = os.path.join(OUT, name + '.ppm')
    cmd = [meta.EXE, '--selftest', ppm, '--view', str(w), str(h)] + ['%g' % c for c in cam] + ['%g' % fov]
    if scene.startswith('menu:'):
        cmd += ['--menu', scene[5:]]
    elif scene:
        cmd += ['--scene', scene]
    return cmd, ppm


def logo(path, w=1280, h=720):
    """The title alone on transparency (white letters, the cyan glow as soft alpha) for Steam's library logo."""
    cmd, ppm = cmd_for('logo', w, h, SIDE, 60, 'action')
    th = 0.15   # 13 letters at 0.75 x the height: about 1050 px wide
    base = meta.render(cmd, ppm)
    lettered = meta.render(cmd + ['--title', meta.TITLE, '%g' % -th, '0'], ppm)
    mask = np.clip(((lettered - base).max(axis=2) - 0.12) * 1.8, 0, 1)   # (the scene's faint differences dropped)
    m = Image.fromarray((mask * 255).astype(np.uint8), 'L')
    hp = th * h
    wide = np.asarray(m.filter(ImageFilter.GaussianBlur(hp * 0.35))).astype(np.float32) / 255
    near = np.asarray(m.filter(ImageFilter.GaussianBlur(hp * 0.10))).astype(np.float32) / 255
    glow = np.clip(wide * 1.6 + near * 0.9, 0, 1) * 0.85
    a = np.clip(mask + glow * (1 - mask), 0, 1)
    rgb = np.array([0.15, 0.70, 1.0], np.float32) * (1 - mask[..., None]) + mask[..., None]   # white letters, cyan glow
    img = np.dstack([rgb, a[..., None]])
    out = Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGBA')
    bb = out.getbbox()   # trim to the glow, keeping a small margin
    if bb:
        pad = 16
        out = out.crop((max(0, bb[0] - pad), max(0, bb[1] - pad), min(w, bb[2] + pad), min(h, bb[3] + pad)))
        wide = Image.new('RGBA', (w, out.height), (0, 0, 0, 0))   # Steam: 1280 px wide (or 720 tall)
        wide.alpha_composite(out, ((w - out.width) // 2, 0))
        out = wide
    out.save(path, optimize=True)
    return out.size


def main():
    if not os.path.exists(meta.EXE):
        sys.exit('build the PC version first: tools\\build_pc.bat')
    os.makedirs(OUT, exist_ok=True)
    meta.OUT = OUT   # meta.icon() writes its masters next to ours
    for name, w, h, cam, fov, scene, th, ty in ART:
        cmd, ppm = cmd_for(name, w, h, cam, fov, scene)
        img = meta.render(cmd, ppm)
        if th > 0:
            img = meta.with_title(img, meta.render(cmd + ['--title', meta.TITLE, '%g' % -th, '%g' % ty], ppm), th * h)
        Image.fromarray((np.clip(img, 0, 1) * 255 + 0.5).astype(np.uint8), 'RGB').save(os.path.join(OUT, name + '.png'), optimize=True)
        print('%-18s %dx%d' % (name, w, h))
    lw, lh = logo(os.path.join(OUT, 'library_logo.png'))
    print('%-18s %dx%d (transparent)' % ('library_logo', lw, lh))
    meta.icon(os.path.join(OUT, 'icon_512.png'))
    big = Image.open(os.path.join(OUT, 'icon_1024.png')).convert('RGB')
    big.resize((184, 184), Image.LANCZOS).save(os.path.join(OUT, 'community_icon.jpg'), quality=92)
    big.save(os.path.join(OUT, 'client_icon.ico'), sizes=[(16, 16), (32, 32), (48, 48), (64, 64), (256, 256)])
    for f in ('tile_background.png', 'tile_foreground.png'):   # Meta-only
        p = os.path.join(OUT, f)
        if os.path.exists(p):
            os.remove(p)
    print('community_icon     184x184 (JPG), client_icon.ico, icon_512.png / icon_1024.png')
    print('wrote', OUT)


if __name__ == '__main__':
    main()
