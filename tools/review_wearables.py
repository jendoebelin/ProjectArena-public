"""Fit check for the head wearables: every hair style / headgear / face shield / mask on both MPFB heads, from the
front, three-quarter and side, rendered with the game's rig (tools/preview_avatar.py). One sheet per family.

Usage: python -I tools/review_wearables.py <mask|visor|gear|hair> [out.png] [k ...]
"""
import os
import sys

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import preview_avatar as pa  # noqa: E402

pr = pa.pr
COUNT = {'mask': 5, 'visor': 7, 'gear': 6, 'hair': 8}
FIELD = {'mask': 'mask', 'visor': 'visor', 'gear': 'headgear', 'hair': 'hair'}
VIEWS = ((0, 'front'), (35, '3/4'), (90, 'side'), (160, 'back'))


def look(fam, k, body):
    d = dict(suit=0, build=1, pattern=0, primary=4, secondary=2, glow=2, hair=1 if fam in ('mask', 'visor') else 0,
             haircolor=2, headgear=0, visor=0, visortint=0, mask=0, skin=3, lower=0, body=body)
    d[FIELD[fam]] = k
    if fam == 'hair':
        d['hair'] = k
    return ','.join('%s=%d' % kv for kv in d.items())


def render(fam, k, body, yaw, size=300):
    dl = pa.draw_list(look(fam, k, body), 'rest', 0, yaw)
    dl['parts'] = [p for p in dl['parts'] if not p['model'].startswith(('av_bottom', 'av_wfade'))]
    pr.W, pr.H = size, size
    tmp = os.path.join(ROOT, 'build', 'rw_%s_%d_%d_%d.png' % (fam, k, body, yaw))
    eye_y = 1.62
    pr.render(pa.entries(dl, hands=False), [0.0, eye_y + 0.0, 0.52], [0.0, eye_y - 0.03, 0.0], tmp, fov=30,
              bg=(0.80, 0.81, 0.84))
    with Image.open(tmp) as im:
        out = im.copy()
    os.remove(tmp)
    return out


def main():
    fam = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.join(ROOT, 'build', 'review_%s.png' % fam)
    ks = [int(x) for x in sys.argv[3:]] or list(range(1, COUNT[fam] + 1))
    S = 300
    sheet = Image.new('RGB', (S * len(VIEWS) * 2, S * len(ks)), (40, 40, 40))
    dr = ImageDraw.Draw(sheet)
    for r, k in enumerate(ks):
        for body in (0, 1):
            for c, (yaw, name) in enumerate(VIEWS):
                im = render(fam, k, body, yaw, S)
                sheet.paste(im, ((body * len(VIEWS) + c) * S, r * S))
        dr.text((6, r * S + 4), '%s %d' % (fam, k), fill=(0, 0, 0))
    sheet.save(out)
    print('wrote', out)


main()
