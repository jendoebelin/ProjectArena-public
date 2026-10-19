"""The PC build's icon (taskbar, title bar, Explorer): app/src/pc/ProjectArena.ico, 16..256 px.

Usage: python -I tools/gen_pc_icon.py
The Project Arena emblem (tools/gen_open_textures.py emblem: the orbit ring round a ball, blue and orange halves) on
the deep navy tile of the store icon (tools/gen_store_art.py icon), filling more of the tile at the small sizes so
it still reads at 16 px. The .ico is kept in the repo, so every PC build embeds it (app/src/pc/ProjectArena.rc).
"""
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageFilter

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EMBLEM = os.path.join(ROOT, 'build', 'open_src', 'tex', 'emblem.png')
OUT = os.path.join(ROOT, 'app', 'src', 'pc', 'ProjectArena.ico')
SIZES = (16, 20, 24, 32, 40, 48, 64, 96, 128, 256)


def tile(size, fill):
    big = max(256, size * 4)   # drawn large, then reduced (smooth edges)
    yy, xx = np.mgrid[0:big, 0:big] / (big - 1) - 0.5
    r = np.hypot(xx, yy)
    k = np.clip(1 - r * 1.6, 0, 1)
    bg = np.dstack([0.012 + 0.05 * k, 0.020 + 0.09 * k, 0.045 + 0.16 * k, np.ones_like(k)])
    # rounded corners, like the other platform icons
    rad = 0.18 * big
    cx = np.clip(np.abs(xx * (big - 1)) - (big / 2 - rad), 0, None)
    cy = np.clip(np.abs(yy * (big - 1)) - (big / 2 - rad), 0, None)
    bg[..., 3] = np.clip(rad - np.hypot(cx, cy) + 0.5, 0, 1)
    img = Image.fromarray((np.clip(bg, 0, 1) * 255).astype(np.uint8), 'RGBA')
    em = Image.open(EMBLEM).convert('RGBA').resize((int(big * fill),) * 2, Image.LANCZOS)
    layer = Image.new('RGBA', (big, big), (0, 0, 0, 0))
    off = (big - em.width) // 2
    layer.alpha_composite(em, (off, off))
    if size >= 48:   # the soft glow only where there is room for it
        glow = layer.filter(ImageFilter.GaussianBlur(big * 0.027))
        img.alpha_composite(glow)
    img.alpha_composite(layer)
    return img.resize((size, size), Image.LANCZOS)


def main():
    if not os.path.exists(EMBLEM):   # the emblem comes from the open texture set
        subprocess.run([sys.executable, '-I', os.path.join(ROOT, 'tools', 'gen_open_textures.py')], check=True,
                       stdout=subprocess.DEVNULL)
    imgs = [tile(s, 0.82 if s >= 64 else 0.94) for s in SIZES]
    imgs[-1].save(OUT, format='ICO', sizes=[(s, s) for s in SIZES], append_images=imgs[:-1])
    print('wrote', OUT, os.path.getsize(OUT), 'bytes,', len(SIZES), 'sizes')


if __name__ == '__main__':
    main()
