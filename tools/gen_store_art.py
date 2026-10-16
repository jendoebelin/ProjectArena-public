"""Draft Meta Horizon Store art, rendered by the PC build itself (ProjectArena.exe --selftest --view ... --title ...),
so every pixel is our own content. Sizes follow Meta's asset guidelines (24-bit PNG).

    tools\\build_pc.bat && python -I tools/gen_store_art.py      ->  build/store_art/*.png
"""
import os
import subprocess
import sys

from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
EXE = os.path.join(ROOT, 'build', 'pc', 'ProjectArena', 'ProjectArena.exe')
OUT = os.path.join(ROOT, 'build', 'store_art')
TITLE = 'PROJECT ARENA'
DOWN_COURT = (-7.6, 2.0, 0.0, -90.0, -4.0)     # just behind the blue player, looking down the court
SIDE = (-11.0, 3.5, 4.5, -67.7, -12.0)        # high, from the side
LOW = (-3.0, 1.4, -2.5, -60.0, -3.0)          # low near the blue player

# name, width, height, camera, vertical fov, title height (fraction), title offset (-1..1, + up)
ART = [
    ('hero_cover', 3000, 900, DOWN_COURT, 34, 0.20, 0.0),
    ('cover_landscape', 2560, 1440, DOWN_COURT, 52, 0.12, 0.0),
    ('cover_square', 1440, 1440, DOWN_COURT, 64, 0.07, 0.0),
    ('cover_portrait', 1008, 1440, DOWN_COURT, 72, 0.048, 0.3),
    ('mini_landscape', 1080, 360, DOWN_COURT, 30, 0.22, 0.0),
    ('icon', 512, 512, DOWN_COURT, 64, 0.0, 0.0),
    ('screenshot_1', 2560, 1440, SIDE, 60, 0.0, 0.0),
    ('screenshot_2', 2560, 1440, LOW, 70, 0.0, 0.0),
    ('screenshot_3', 2560, 1440, (-6.4, 1.7, 0.0, -90.0, -2.0), 70, 0.0, 0.0),
]


def main():
    if not os.path.exists(EXE):
        sys.exit('build the PC version first: tools\\build_pc.bat')
    os.makedirs(OUT, exist_ok=True)
    for name, w, h, cam, fov, th, ty in ART:
        ppm = os.path.join(OUT, name + '.ppm')
        cmd = [EXE, '--selftest', ppm, '--view', str(w), str(h)] + ['%g' % c for c in cam] + ['%g' % fov]
        if th > 0:
            cmd += ['--title', TITLE, '%g' % th, '%g' % ty]
        r = subprocess.run(cmd)
        if r.returncode != 0 or not os.path.exists(ppm):
            sys.exit('%s: render failed (%d)' % (name, r.returncode))
        Image.open(ppm).convert('RGB').save(os.path.join(OUT, name + '.png'), optimize=True)
        os.remove(ppm)
        print('%-16s %dx%d' % (name, w, h))
    print('wrote', OUT)


if __name__ == '__main__':
    main()
