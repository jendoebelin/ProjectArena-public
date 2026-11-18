"""The four rooms' pictures for CHOOSE A ROOM (ROOMS AND THEIR OPTIONS, the owner's design 2026-11-17): each room seen
from straight above, with its players, taken by the PC app's picture tool and packed into one texture.

    py -3.10 tools/make_room_pictures.py [path/to/ProjectArena.exe]

Writes app/assets_open/tex/room_pictures.tex: 640 x 400, the SQUARE top left, DIAMOND top right, FOUR CORNERS bottom
left, FIREBALL BLITZ bottom right (each 320 x 200; Renderer::drawTexturedPanel draws one with a quarter of the UVs).
Needs the PC app built (tools/build_pc.bat; the default is build/pc_glass's test copy, then build/pc's).
"""
import os
import struct
import subprocess
import sys
import tempfile

import numpy as np
from PIL import Image

ROOT = os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), '..'))
OUT = os.path.join(ROOT, 'app', 'assets_open', 'tex', 'room_pictures.tex')
CELL = (320, 200)

# (scene, camera height, vertical field of view): straight down over the centre, the room filling the picture
ROOMS = [
    ('action', 16.0, 56.0),   # SQUARE: two players mid-rally
    ('exp', 16.0, 56.0),      # DIAMOND
    ('four', 16.0, 76.0),     # FOUR CORNERS
    ('blitz', 16.0, 72.0),    # FIREBALL BLITZ
]


def find_exe():
    if len(sys.argv) > 1:
        return sys.argv[1]
    for rel in ('build/pc_glass/ProjectArena/ProjectArena.exe', 'build/pc/ProjectArena/ProjectArena.exe'):
        p = os.path.join(ROOT, rel)
        if os.path.exists(p):
            return p
    sys.exit('the PC app is not built (tools/build_pc.bat)')


def shoot(exe, scene, height, fov, out):
    w, h = CELL[0] * 2, CELL[1] * 2   # (drawn at twice the size, then scaled down: smoother lines)
    cmd = [exe, '--selftest', out, '--scene', scene,
           '--view', str(w), str(h), '0', str(height), '0.01', '0', '-89', str(fov)]
    subprocess.run(cmd, cwd=os.path.dirname(exe), stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, timeout=240)
    return Image.open(out).convert('RGB').resize(CELL, Image.Resampling.LANCZOS)


def main():
    exe = find_exe()
    atlas = Image.new('RGB', (CELL[0] * 2, CELL[1] * 2))
    with tempfile.TemporaryDirectory() as tmp:
        for k, (scene, height, fov) in enumerate(ROOMS):
            im = shoot(exe, scene, height, fov, os.path.join(tmp, 'room%d.ppm' % k))
            atlas.paste(im, ((k % 2) * CELL[0], (k // 2) * CELL[1]))
    rgba = np.asarray(atlas.convert('RGBA'), dtype=np.uint8)
    os.makedirs(os.path.dirname(OUT), exist_ok=True)
    with open(OUT, 'wb') as f:
        f.write(b'STEX' + struct.pack('<3I', rgba.shape[1], rgba.shape[0], 0))
        f.write(np.ascontiguousarray(rgba).tobytes())
    atlas.save(os.path.join(tempfile.gettempdir(), 'room_pictures.png'))
    print('wrote', OUT, '(a copy to look at:', os.path.join(tempfile.gettempdir(), 'room_pictures.png') + ')')


if __name__ == '__main__':
    main()
