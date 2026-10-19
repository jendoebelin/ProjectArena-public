"""Contributor templates: reference meshes and layouts for making new Project Arena assets.

Usage: python -I tools/make_templates.py OUT_DIR

Writes:
  body_masculine.glb / body_feminine.glb   the whole skinned body (rig + weights + UVs): outfits are made on these
  uv_layout_masculine.png / _feminine.png   the body's UV layout (1024 px) coloured by bone, for painting designs
  head_masculine.glb / head_feminine.glb   the bald head in head space: fit hair, helmets, face shields, masks to it
  gauntlet_grip.glb                        the controller in a glove, in the hand frame every gauntlet is built in
  examples/                                one finished asset of each kind
  README.md                                how it all fits together
"""
import os
import re
import shutil
import sys

import numpy as np
from PIL import Image, ImageDraw

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import export_glb as E  # noqa: E402
from glb import Glb  # noqa: E402

BONE_COL = [(90, 110, 140), (120, 150, 120), (170, 140, 110), (150, 90, 90), (90, 90, 150), (190, 110, 110), (110, 110, 200)]


def load_body(key):
    src = open(os.path.join(ROOT, 'tools', 'build_body_parts.py'), encoding='utf-8').read()
    src = src[:src.find('\ndef main')] if '\ndef main' in src else src
    ns = {'__file__': os.path.join(ROOT, 'tools', 'build_body_parts.py'), '__name__': 'bbp'}
    exec(compile(src, 'build_body_parts', 'exec'), ns)
    return ns['Body'](key)


def eye_of(body):
    rows = [l for l in open(os.path.join(ROOT, 'app', 'src', 'avatar_bodies.inc')).read().splitlines() if l.strip().startswith('{{')]
    return np.array([float(x) for x in re.findall(r'-?\d+\.\d+', rows[body])][:3])


def body_glb(b, body, path):
    g = Glb('Project Arena tools/make_templates.py')
    skin = g.add_skin(E.BONES, E.bind_joints(body), E.PARENTS)
    W = b.Wd
    order = np.argsort(-W, axis=1)[:, :4]
    top = np.take_along_axis(W, order, 1)
    top /= np.maximum(top.sum(1, keepdims=True), 1e-9)
    w = np.clip(np.round(top * 255), 0, 255)
    w[:, 0] += 255 - w.sum(1)
    mat = g.material('av_skin', color=(0.62, 0.43, 0.33, 1), roughness=0.8,
                     extras={'projectArena': {'palette': 'av_skin', 'channel': 'skin', 'note': 'reference body, not exported'}})
    g.add_mesh('body_' + ('feminine' if body else 'masculine'), [dict(
        pos=b.P.astype(np.float32), nrm=b.N.astype(np.float32), uv0=b.UV.astype(np.float32), idx=b.T.reshape(-1).astype(np.uint32),
        joints=order.astype(np.uint8), weights=w.astype(np.uint8), material=mat)], skin=skin)
    g.save(path)


def uv_layout(b, path, size=1024):
    im = Image.new('RGB', (size, size), (24, 26, 32))
    dr = ImageDraw.Draw(im)
    dom = np.argmax(b.Wd, axis=1)
    for t in b.T:
        uv = b.UV[t]
        pts = [(float(u) * size, (1 - float(v)) * size) for u, v in uv]
        c = BONE_COL[int(np.bincount(dom[t], minlength=7).argmax())]
        dr.polygon(pts, fill=c)
    for t in b.T:
        uv = b.UV[t]
        pts = [(float(u) * size, (1 - float(v)) * size) for u, v in uv]
        dr.line(pts + [pts[0]], fill=(20, 20, 24), width=1)
    im.save(path)


README = """# Project Arena asset templates

Thanks for making something for Project Arena! Everything here is a reference you build on; nothing in this folder
ends up in the game as-is. Art you contribute is published under **CC BY 4.0** (see LICENSE-ASSETS.md in the repo):
anyone may use it with credit to you.

## Space and units (all files)

- Metres, **+Y up**, the avatar faces **-Z** (Blender's glTF importer turns this into Z up, facing -Y; export back with
  the default glTF settings and it comes out the same way).
- Keep the **material names** from the palette (list below). They are how the game knows which part takes the
  player's primary colour, which part glows, what is skin, and so on. Colours in the files are only previews.

## What goes where

| You are making | Start from | Notes |
|---|---|---|
| A top / outfit / shorts | `body_masculine.glb` and `body_feminine.glb` | Model on the body in its bind pose (arms out). Keep the armature: the game bends the outfit with the same 7 bones, so every vertex needs weights (copy them from the body with Blender's Data Transfer modifier). Make one version per body. Max ~8,000 triangles each. |
| (top: what it covers) | | The top **replaces the body from the waist up**: chest, back and both arms to the wrists (the gauntlets cover the hands, the game draws the head). Any skin it leaves bare is part of your file (material `av_skin`, follow the template body's surface). It ends at the waist: Y = -0.417 m (feminine) / -0.478 m (masculine). |
| (shorts: what they cover) | | The shorts **replace the body from the waist down**: they start at the same waist line and carry the legs on to where the avatar's legs end, Y = -0.864 m (feminine) / -0.991 m (masculine), finished with the glowing fade ring (material `av_fade`). Bare leg below the hem is part of your file (`av_skin`). Without the legs the avatar stops at the hem. Copy the legs and the fade ring from `examples/av_bottom_1_f.glb` / `av_bottom_1_m.glb`. |
| (fit) | | Cloth follows the body's surface closely (3-15 mm off the skin, shrinkwrap it); hard plates up to ~25 mm. Fit each version to its own body (the feminine and masculine bodies differ in shape, not only in size). Use the template's coordinates exactly as they are: numbers from the game's source code are in other spaces. |
| A pattern or design on an outfit | `uv_layout_*.png` | Paint over the layout at 512 or 1024 px. Design masks use channels: **red** = the trim colour, **green** = glowing lines, **blue** = white accents. |
| Hair, a helmet, a cap, glasses, a face shield, a mask | `head_masculine.glb` / `head_feminine.glb` | Head space: the origin is between the eyes. Leave 3-6 mm between the part and the skin so nothing pokes through. One version per head. Max ~8,000 triangles. |
| A gauntlet / glove | `gauntlet_grip.glb` | The controller held in a glove, in the hand frame. Your gauntlet must enclose the controller (players hold a real one there). Right hand only; the game mirrors it for the left. Max ~8,000 triangles. |

The `examples/` folder has one finished game asset of each kind, exported from the game, so you can see how ours
are set up.

## Materials (palette names)

Avatars: `av_primary` (player's main colour, takes patterns), `av_primary_np` (main colour, no pattern),
`av_primary_dk` (darker main), `av_secondary`, `av_secondary_dk`, `av_glow` (glows in the glow colour), `av_trim`
(metallic trim colour), `av_skin`, `av_hair`, `av_hair_lock` (thin two-sided hair strands), `av_visor` (glass, takes
the visor tint), `av_dark`, `av_armor`, `av_armor_white`, `av_piping`, `av_cloth` / `av_cloth_dk` (matte fabric),
`av_fade` (the glowing ring where the avatar's legs end: shorts only).

Gauntlets: `gaunt_shell` (main colour plates), `gaunt_trim`, `gaunt_dark`, `gaunt_glow`, `glove`.

## Submitting

1. Export your model as **.glb** (File > Export > glTF 2.0, format "glTF Binary").
2. Put it in `contrib/<kind>/<your-asset-name>/` with a short `README.md`: what it is, your name or handle for the
   credit, and which bodies / heads it fits.
3. Open a pull request. We check it in the headset; `tools/import_glb.py` turns it into a game model.
"""


def main():
    out = sys.argv[1] if len(sys.argv) > 1 else os.path.join(ROOT, 'build', 'templates')
    if os.path.isdir(out):
        shutil.rmtree(out)
    os.makedirs(os.path.join(out, 'examples'))
    for key, body, nm in (('m', 0, 'masculine'), ('f', 1, 'feminine')):
        b = load_body(key)
        body_glb(b, body, os.path.join(out, 'body_%s.glb' % nm))
        uv_layout(b, os.path.join(out, 'uv_layout_%s.png' % nm))
        E.export('av_bhead_' + key, os.path.join(out, 'head_%s.glb' % nm), offset=-eye_of(body), rigid=True)
        print('  body + head', nm)
    E.export('lancer', os.path.join(out, 'gauntlet_grip.glb'))
    for m in ('av_top_9_f', 'av_top_9_m', 'av_bottom_1_f', 'av_bottom_1_m', 'av_hair_8_m', 'av_gear_6_f', 'av_visor_4_m',
              'av_mask_4_f', 'gauntlet_0'):
        E.export(m, os.path.join(out, 'examples', m + '.glb'))
    open(os.path.join(out, 'README.md'), 'w', encoding='utf-8', newline='\n').write(README)
    print('wrote', out)


if __name__ == '__main__':
    main()
