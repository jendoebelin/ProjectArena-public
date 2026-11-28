"""The DRAGON SILK QIPAO's and the CHERRY POODLE SKIRT's country versions (0.3.27).

Usage: python -I tools/build_country_kits.py [--preview DIR]

The two outfits come in their own colours (the silk, the felt: the owner's outfit kits, contrib/outfit), their colours
painted into their textures, so MAIN COLOR, TRIM COLOR and a country's colours didn't reach them (the owner,
2026-11-27: "the new outfits not changing to the country colors"). Under a COUNTRY KIT of MY OUTFIT the game draws
these instead (avatar.cpp drawList, av_top_19c_* / av_top_20c_*), the same shapes with:
  - the silk / the felt in MAIN (the flag's main colour): its texture made neutral (its shading kept, as grey);
  - the qipao's gold (the dragon, its clouds, the lines at the hem: a design mask's R) and its frog buttons in TRIM
    (the flag's second colour), its dark piping a dark MAIN;
  - the poodle skirt's neck scarf in TRIM; the poodle, its leash, the belt and the petticoat stay black and the blouse
    white (the poodle's white collar and eye: the design mask's B, white accents).
Their own look, the textures as made, is unchanged. --preview writes the textures as PNGs for a look.
Then: cd app/assets_open && ../../build/tools/make_lods.exe av_top_19c_m.mdl av_top_19c_f.mdl av_top_20c_m.mdl av_top_20c_f.mdl
"""
import os
import shutil
import struct
import sys

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, 'app', 'assets_open')
TEX = os.path.join(ASSETS, 'tex')
TWOSIDED, DESIGN = 1, 32
CH_PRIMARY, CH_SECONDARY = 1, 2


def lin(c):
    c = c / 255.0
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def srgb8(x):
    x = np.clip(x, 0, 1)
    return np.round(255 * np.where(x <= 0.0031308, x * 12.92, 1.055 * x ** (1 / 2.4) - 0.055)).astype(np.uint8)


def write_tex(name, rgba):
    h, w = rgba.shape[:2]
    with open(os.path.join(TEX, name + '.tex'), 'wb') as f:
        f.write(b'STEX' + struct.pack('<3I', w, h, 0))
        f.write(np.ascontiguousarray(rgba, np.uint8).tobytes())


def smooth(a, b, x):
    t = np.clip((x - a) / (b - a), 0, 1)
    return t * t * (3 - 2 * t)


def hsv(rgb8):
    c = rgb8 / 255.0
    mx, mn = c.max(-1), c.min(-1)
    d = mx - mn
    r, g, b = c[..., 0], c[..., 1], c[..., 2]
    hue = np.zeros_like(mx)
    k = d > 1e-6
    hr = (mx == r) & k
    hg = (mx == g) & k & ~hr
    hb = k & ~hr & ~hg
    hue[hr] = ((g - b)[hr] / d[hr]) % 6
    hue[hg] = (b - r)[hg] / d[hg] + 2
    hue[hb] = (r - g)[hb] / d[hb] + 4
    return hue * 60.0, np.where(mx > 0, d / np.maximum(mx, 1e-6), 0), mx


def neutral(rgb8, cloth, dark=None):
    """the texture's shading as grey, its cloth's brightest (95%) at white: MAIN x it is the cloth in MAIN. `dark` (0..1):
    what is black in the picture (the poodle, its leash) kept as dark as it is, not made relative to the cloth"""
    L = (lin(rgb8.astype(np.float64)) * [0.2126, 0.7152, 0.0722]).sum(-1)
    ref = np.percentile(L[cloth], 95)
    n = L / ref
    if dark is not None:
        n = n * (1 - dark) + L * dark
    g = srgb8(n)
    return np.dstack([g, g, g, np.full(g.shape, 255, np.uint8)]), ref


def qipao(preview):
    px = np.array(Image.open(os.path.join(ROOT, 'contrib', 'outfit', 'dragon-silk-qipao', 'dragon_silk.png')).convert('RGB'))
    h, s, v = hsv(px)
    # the gold: hue 20-60 (orange to yellow), saturated, not dark; the dragon's brown outlines partly (a dark MAIN there)
    gold = smooth(12, 24, h) * (1 - smooth(55, 70, h)) * smooth(0.25, 0.45, s) * smooth(0.25, 0.55, v)
    silk = gold < 0.05
    tex, ref = neutral(px, silk)
    mask = np.zeros(px.shape[:2] + (4,), np.uint8)
    mask[..., 0] = np.round(255 * gold)
    mask[..., 3] = 255
    write_tex('qipao_dragon_kit', tex)
    write_tex('design_qipao_kit', mask)
    if preview:
        Image.fromarray(tex).save(os.path.join(preview, 'qipao_dragon_kit.png'))
        Image.fromarray(mask[..., :3]).save(os.path.join(preview, 'design_qipao_kit.png'))
    return ref


def poodle(preview):
    px = np.array(Image.open(os.path.join(ROOT, 'contrib', 'outfit', 'cherry-poodle-skirt', 'poodle_felt.png')).convert('RGB'))
    h, s, v = hsv(px)
    white = (1 - smooth(0.15, 0.35, s)) * smooth(0.55, 0.8, v)   # (the poodle's collar, its eye)
    felt = (s > 0.5) & (v > 0.4)
    black = 1 - smooth(0.18, 0.40, v)   # (the poodle and its leash: black on any colour)
    tex, ref = neutral(px, felt, black)
    mask = np.zeros(px.shape[:2] + (4,), np.uint8)
    mask[..., 2] = np.round(255 * white)
    mask[..., 3] = 255
    write_tex('poodle_felt_kit', tex)
    write_tex('design_poodle_kit', mask)
    if preview:
        Image.fromarray(tex).save(os.path.join(preview, 'poodle_felt_kit.png'))
        Image.fromarray(mask[..., :3]).save(os.path.join(preview, 'design_poodle_kit.png'))
    return ref


def materials(path):
    b = bytearray(open(path, 'rb').read())
    assert b[:4] == b'SMDL'
    ver, nv, ni, ns, nm = struct.unpack_from('<5I', b, 4)
    start = len(b) - nm * 172
    out = []
    for i in range(nm):
        o = start + 172 * i
        sh = struct.unpack_from('<I', b, o)[0]
        tint = list(struct.unpack_from('<4f', b, o + 4))
        flags = struct.unpack_from('<I', b, o + 40)[0]
        dif = bytes(b[o + 44:o + 108]).split(b'\0')[0].decode()
        out.append({'o': o, 'shader': sh, 'tint': tint, 'flags': flags, 'diffuse': dif})
    return b, out


def set_mat(b, m, tint=None, channel=None, design=None, diffuse=None):
    o = m['o']
    if tint is not None:
        struct.pack_into('<4f', b, o + 4, *tint)
    flags = m['flags']
    if channel is not None:
        flags = (flags & ~0xF00) | (channel << 8)
    if design is not None:
        flags |= DESIGN
        b[o + 108:o + 172] = design.encode().ljust(64, b'\0')
    struct.pack_into('<I', b, o + 40, flags)
    if diffuse is not None:
        b[o + 44:o + 108] = diffuse.encode().ljust(64, b'\0')


def lum(rgb):
    return 0.2126 * rgb[0] + 0.7152 * rgb[1] + 0.0722 * rgb[2]


def main():
    preview = sys.argv[sys.argv.index('--preview') + 1] if '--preview' in sys.argv else None
    if preview:
        os.makedirs(preview, exist_ok=True)
    silk_ref, felt_ref = qipao(preview), poodle(preview)
    for sex in 'mf':
        # the qipao: the silk (its texture) in MAIN, its gold in TRIM, the piping a dark MAIN, the frogs TRIM
        b, mats = materials(os.path.join(ASSETS, 'av_top_19_%s.mdl' % sex))
        for m in mats:
            if m['diffuse'] == 'qipao_dragon':
                set_mat(b, m, tint=[1, 1, 1, 1], channel=CH_PRIMARY, design='design_qipao_kit', diffuse='qipao_dragon_kit')
            elif m['shader'] == 0 and (m['flags'] >> 8) & 15 == 0 and not m['diffuse']:
                t = m['tint']
                if t[0] > 0.5 and t[1] > 0.3:   # the gold frog buttons
                    set_mat(b, m, tint=[1, 1, 1, 1], channel=CH_SECONDARY)
                else:                           # the dark crimson piping: as dark against the silk, in MAIN
                    k = min(1.0, lum(t[:3]) / silk_ref)
                    set_mat(b, m, tint=[k, k, k, 1], channel=CH_PRIMARY)
        open(os.path.join(ASSETS, 'av_top_19c_%s.mdl' % sex), 'wb').write(bytes(b))
        # the poodle skirt: the felt in MAIN, the scarf TRIM; the blouse, the black parts and the buckle as they are
        b, mats = materials(os.path.join(ASSETS, 'av_top_20_%s.mdl' % sex))
        for m in mats:
            t = m['tint']
            if m['diffuse'] == 'poodle_felt':
                set_mat(b, m, tint=[1, 1, 1, 1], channel=CH_PRIMARY, design='design_poodle_kit', diffuse='poodle_felt_kit')
            elif m['shader'] == 0 and (m['flags'] >> 8) & 15 == 0 and not m['diffuse'] and t[0] > 0.3 and t[1] < 0.1:
                set_mat(b, m, tint=[1, 1, 1, 1], channel=CH_SECONDARY)   # the cherry neck scarf
        open(os.path.join(ASSETS, 'av_top_20c_%s.mdl' % sex), 'wb').write(bytes(b))
        shutil.copyfile(os.path.join(ASSETS, 'av_top_20_%s.swish' % sex), os.path.join(ASSETS, 'av_top_20c_%s.swish' % sex))
    print('silk 95%%: %.3f, felt 95%%: %.3f (linear luminance); av_top_19c_m/f, av_top_20c_m/f (+ .swish) and four textures written'
          % (silk_ref, felt_ref))


if __name__ == '__main__':
    main()
