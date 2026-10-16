"""Avatar section of the asset catalog (tools/gen_catalog.py): every suit, build, pattern, hair style, headgear, face
shield, mask and preset with an ID (V01...), rendered with the game's own rig (tools/preview_avatar.py), plus the
colour palettes. Renders are cached in build/catalog_av_cache and redone when the parts or the rig change.
"""
import hashlib
import html
import json
import os
import shutil
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import preview_avatar as pa
import catalog_dl  # noqa: E402

CACHE = os.path.join(ROOT, 'build', 'catalog_av_cache')
BASE = dict(suit=0, build=1, pattern=0, primary=0, secondary=1, glow=0, hair=0, haircolor=2, headgear=0, visor=0,
            visortint=0, mask=0, skin=2, lower=0, body=0, gauntlet=0)

SUIT_DESC = [
    'Long-sleeve bodysuit with glowing circuit piping, a high collar and cuff rings (Tron).',
    'Loose crew-neck sports tee: short sleeves with trim bands, trim collar and side stripes.',
    'Dark undersuit with raised armour: chest and back plates, pauldrons, vambraces and a glowing core.',
    'Padded long-sleeve jacket with a high collar, zipper line, trim cuffs and waistband.',
    'Thick hoodie with a kangaroo pocket, trim cuffs and waistband.',
    'Sleek long-sleeve racer suit: high collar, glowing V lines and an identity disc on the back.',
    'Tank top with a round neckline and wide straps, silver-plated forearm bracers (reference sheet 1).',
    'Flight suit with harness straps, a belt and a chest light.',
    'Sleeveless compression vest: mock collar, glowing chest Y, side and back lines, dark circuit bracers.',
    'Fitted training tee with raglan and side panels in the trim colour, grey-plated bracers (reference sheet 2).',
]
LOWER_DESC = ['The body ends at the waist in an energy fade.',
              'Athletic shorts with a split hem and white piping (reference sheet 1); thighs fade above the knee.',
              'Long circuit shorts with glowing lines down the legs and on the waistband.',
              'Bike shorts with side panels in the trim colour (reference sheet 2).']
BODY_DESC = ['MPFB2 masculine base body: athletic, bald (hair is a separate part).',
             'MPFB2 feminine base body: athletic, bald (hair is a separate part).']
BUILD_DESC = ['Slimmer shoulders and arms.', 'The base proportions.', 'Broader shoulders and chest, thicker arms.',
              'Slightly broader, stronger arms.']
PATTERN_DESC = ['', 'Circuit traces with pads, in the glow colour (Tron).', 'Twin racing stripes front and back, pinstripes on the sides.',
                'Chevrons across the chest.', 'Honeycomb lines with trim-colour cells.',
                'Left half in the trim colour.', 'Digital camo blocks in the trim colour.',
                'Horizontal bands in the trim colour.']
HAIR_DESC = ['', 'Close-cropped.', 'Volume on top, swept back in layers.', 'Combed back to a high tie, a full wavy tail and face-framing strands.',
             'Shaved sides with a crest from brow to nape.', 'Short spikes swept back.', 'Chin-length bob with a fringe.',
             'Shaved with a bun on top.', 'Short and layered, swept up and to the side, ends flicking out.']
GEAR_DESC = ['', 'Full helmet with a glowing face rim, midline and side lines (Tron).',
             'Rounded racing helmet with twin stripes and a rear spoiler.', 'Soft fabric hood with a trim edge.',
             'Helmet with a crest fin and cheek guards.', 'Over-ear headphones with glowing rings (hair stays visible).',
             'Cap with a curved brim.']
VISOR_DESC = ['', 'Wraparound glass band over the eyes, glowing edges.', 'Smooth glass dome over the whole face.',
              'Sunglasses.', 'Round goggles on a strap.', 'A VR headset on a strap (a nod to the real thing).',
              'A glowing ring orbiting the head, with a slim eye line.', 'Opaque plate with a glowing eye slit.']
MASK_DESC = ['', 'Plate over the mouth and chin with vents.', 'Armoured jaw from ear to ear with glowing vents.',
             'Respirator: snout, side filters and straps.', 'Cloth over nose and mouth, knotted at the back.',
             'Smooth full-face plate with glowing eye slits.']


def look(**kw):
    d = dict(BASE)
    d.update(kw)
    return ','.join('%s=%d' % kv for kv in d.items())


def options():
    out = subprocess.run([pa.POSE_EXE, '--options'], capture_output=True, text=True, check=True).stdout
    return json.loads(out)


def items(opt):
    """Every avatar option on the MPFB2 bodies (IDs renumbered for the skinned avatars, October 2026)."""
    F = {f['name']: [o['name'] for o in f['options']] for f in opt['fields']}
    v = []
    n = 0

    def add(name, desc, models, lk, pose='ready', close=False, team=0, yaw=0.0, group=''):
        nonlocal n
        n += 1
        v.append(dict(id='V%02d' % n, name=name, desc=desc, models=models, look=lk, pose=pose, close=close, team=team,
                      yaw=yaw, group=group))
    for k, name in enumerate(F['BODY TYPE']):
        b = 'mf'[k]
        add(name.title() + ' body', BODY_DESC[k], 'av_bhead_%s, av_top_6_%s, av_bottom_1_%s' % (b, b, b),
            look(body=k, suit=6, primary=14, secondary=4, lower=1, skin=3), pose='rest', group='body')
        add(name.title() + ' body (back)', BODY_DESC[k], 'av_bhead_%s, av_top_6_%s, av_bottom_1_%s' % (b, b, b),
            look(body=k, suit=6, primary=14, secondary=4, lower=1, skin=3), pose='rest', yaw=180.0, group='body')
    for k, name in enumerate(F['SUIT']):
        add(name.title(), SUIT_DESC[k], 'av_top_%d_m, av_top_%d_f' % (k, k),
            look(suit=k, body=k % 2, primary=4 if k not in (6, 9) else 14, secondary=2, glow=2, lower=1 + k % 3, hair=2 if k % 2 == 0 else 3,
                 haircolor=2), group='suit')
    add('Echo Suit (back)', 'The identity disc on the back of the Echo Suit.', 'av_top_5_f', look(suit=5, body=1, primary=4, secondary=2, glow=2, hair=3),
        yaw=180.0, group='suit')
    for k, name in enumerate(F['LOWER BODY']):
        add(name.title() if k else 'No lower body', LOWER_DESC[k], ('av_bottom_%d_m, av_bottom_%d_f' % (k, k)) if k else 'av_wfade_m, av_wfade_f',
            look(suit=9 if k != 2 else 8, body=1 if k != 2 else 0, primary=13, secondary=15 if k == 3 else 4, glow=2, lower=k, hair=3 if k != 2 else 8,
                 haircolor=2), group='lower')
    GAUNTLET_DESC = ['Gothic plate: pointed finger scales, knuckle gadlings, a fluted back plate, a pointed bell cuff.',
                     'Heavy plate: thick scales, a spiked knuckle ridge, a riveted back plate, a wide bell cuff.',
                     'Light plate over a dark glove with glowing inlays and a slim cuff.',
                     'The classic look: the VR controller in an armoured glove.']
    for k, name in enumerate(F['GAUNTLET']):
        add(name.title() + ' gauntlet', "GAUNTLET = %s. %s" % (name, GAUNTLET_DESC[k] if k < len(GAUNTLET_DESC) else ''),
            ('gauntlet_%d' % k) if k < 3 else 'lancer', look(suit=8, primary=4, secondary=0, glow=2, gauntlet=k, lower=2, hair=8),
            close='hands', group='gauntlet')
    for k, name in enumerate(F['BUILD']):
        add(name.title() + ' build', BUILD_DESC[k], 'av_top_0_m (scaled)',
            look(suit=0, build=k, primary=13, secondary=0, glow=2, hair=1, haircolor=0, lower=2), group='build')
    for k, name in enumerate(F['PATTERN']):
        if k:
            add(name.title() + ' pattern', PATTERN_DESC[k], 'tex/open_pat_%s.tex' % name.lower(),
                look(suit=5, primary=2, secondary=10, glow=2, pattern=k, hair=1, lower=3), group='pattern')
    for k, name in enumerate(F['HAIR']):
        if k:
            add(name.title(), HAIR_DESC[k], 'av_hair_%d_m, av_hair_%d_f' % (k, k), look(hair=k, haircolor=2, body=1 if k in (3, 6, 7) else 0),
                close=True, yaw=35.0, group='hair')
    add('Ponytail (back)', 'The ponytail from behind.', 'av_hair_3_f', look(hair=3, haircolor=2, body=1, suit=9, primary=14), yaw=200.0,
        close=True, group='hair')
    for k, name in enumerate(F['HEADGEAR']):
        if k:
            add(name.title(), GEAR_DESC[k], 'av_gear_%d_m, av_gear_%d_f' % (k, k), look(headgear=k, primary=4, secondary=2, hair=2),
                close=True, yaw=30.0, group='gear')
    for k, name in enumerate(F['FACE SHIELD']):
        if k:
            add(name.title(), VISOR_DESC[k], 'av_visor_%d_m, av_visor_%d_f' % (k, k), look(visor=k, hair=1), close=True, group='visor')
    for k, name in enumerate(F['MASK']):
        if k:
            add(name.title(), MASK_DESC[k], 'av_mask_%d_m, av_mask_%d_f' % (k, k), look(mask=k, primary=10, secondary=1, hair=1), close=True,
                group='mask')
    for k, name in enumerate(F['BODY TYPE']):
        add(name.title() + ' head', 'MPFB2 head with our eyes, brows and lips; hair, helmets, visors and masks are fitted to it.',
            'av_bhead_%s' % 'mf'[k], look(body=k), close=True, group='head')
    for k, name in enumerate(opt['presets']):
        add(name.title() + ' (preset)', 'Builder preset %d.' % (k + 1), 'app/src/avatar.cpp kPresets', str(k), pose='ready',
            team=k % 2, group='preset')
    return v


def stamp():
    """Newest input the renders depend on."""
    paths = [os.path.join(pa.pr.ASSETS, f) for f in os.listdir(pa.pr.ASSETS) if f.startswith(('av_', 'gauntlet_')) or f == 'lancer.mdl']
    paths += [os.path.join(pa.pr.ASSETS, 'tex', f) for f in os.listdir(os.path.join(pa.pr.ASSETS, 'tex')) if f.startswith('open_pat_')]
    paths += [pa.POSE_EXE, os.path.join(ROOT, 'tools', 'preview_render.py'), os.path.join(ROOT, 'tools', 'preview_avatar.py')]
    return max(os.path.getmtime(p) for p in paths)


def hexcol(rgb):
    return '#%02x%02x%02x' % tuple(int(round(255 * min(1.0, max(0.0, c)) ** (1 / 2.2))) for c in rgb)


def build(out_dir):
    """Render (or reuse) the avatar thumbnails into out_dir/img and return the section HTML."""
    os.makedirs(CACHE, exist_ok=True)
    opt = options()
    newest = stamp()
    cards = []
    groups = {'body': [], 'suit': [], 'lower': [], 'gauntlet': [], 'build': [], 'pattern': [], 'hair': [], 'gear': [], 'visor': [], 'mask': [], 'head': [],
              'preset': []}
    for it in items(opt):
        key = ('%s_%s_%s_%d_%d_full' % (it['look'], it['pose'], it['close'], it['team'], it['yaw'])).replace('=', '').replace(',', '_')
        cached = os.path.join(CACHE, hashlib.md5(key.encode()).hexdigest()[:12] + '.jpg')
        meta = cached + '.key'
        fresh = os.path.exists(cached) and os.path.exists(meta) and open(meta).read() == key and os.path.getmtime(cached) > newest
        if not fresh:
            print('  avatar', it['id'], it['name'], flush=True)
            size = (840, 840) if it['close'] else (840, 1040)   # same render cost as before, saved without downscaling
            pa.render_look(cached, it['look'], it['pose'], it['team'], it['yaw'], it['close'], size, supersample=1)
            open(meta, 'w').write(key)
        img = 'img/%s.jpg' % it['id']
        shutil.copy2(cached, os.path.join(out_dir, img))
        search = html.escape((it['id'] + ' ' + it['name'] + ' ' + it['desc'] + ' ' + it['models']).lower(), quote=True)
        kind = 'avc' if it['close'] else 'av'
        dl = catalog_dl.link(out_dir, it['id'], it['models'])
        groups[it['group']].append(
            f'<article class="card {kind}" data-search="{search}"><div class="img"><img src="{img}" alt="{html.escape(it["name"])}" '
            f'loading="lazy"></div><div class="meta"><div class="id">{it["id"]}</div><div class="name">{html.escape(it["name"])}</div>'
            f'<div class="desc">{html.escape(it["desc"])}</div><div class="path">{html.escape(it["models"])}</div>'
            f'<div class="btns"><button class="copy" type="button" data-copy="{it["id"]} {html.escape(it["name"])}">Copy ID</button>{dl}</div>'
            f'</div></article>')
    first = lambda g: groups[g][0].split('"id">')[1][:3] if groups[g] else ''
    last = lambda g: groups[g][-1].split('"id">')[1][:3] if groups[g] else ''
    sub = [('body', 'Base bodies', 'MPFB2 (MakeHuman for Blender, CC0) bodies with one continuous skin bent by the game rig; '
                                   'the thighs fade out above the knee, the hands are under the gauntlets.'),
           ('suit', 'Suits', 'Each suit is cut from the body surface for both body types; trim, panels and glowing lines are painted '
                             'into design textures. Colours come from the builder.'),
           ('lower', 'Lower body', 'Shorts cut from the hips and thighs, or a fade at the waist.'),
           ('gauntlet', 'Gauntlets', 'The hands (GAUNTLET row): medieval-style plate gauntlets in the avatar colours; P10-P12 in the props show them alone.'),
           ('build', 'Builds', 'The same suit at each build (scaling of the skinned body).'),
           ('pattern', 'Patterns', 'Masks on the main colour in the body UV layout: trim-colour regions and glowing lines.'),
           ('hair', 'Hair', 'Hair takes the HAIR COLOR (NEON makes it glow in the glow colour); fitted to each head. Helmets and the hood hide it.'),
           ('gear', 'Headgear', 'Fitted to each head.'), ('visor', 'Face shields', 'Glass parts take the SHIELD TINT and reflect the arena.'),
           ('mask', 'Masks', ''), ('head', 'Base heads', ''), ('preset', 'Presets', 'Ready-made looks in the builder (PRESET row).')]
    parts = []
    for g, title, note in sub:
        if not groups[g]:
            continue
        parts.append(f'<h3>{title} <small>{first(g)}&ndash;{last(g)}</small></h3>' + (f'<p class="sub">{html.escape(note)}</p>' if note else '') +
                     f'<div class="grid avgrid">{"".join(groups[g])}</div>')
    pal = []
    for f in opt['fields']:
        if not f['color']:
            continue
        sw = ''.join(f'<span class="sw"><i style="background:{hexcol(o["rgb"])}"></i>{html.escape(o["name"])}</span>' for o in f['options'])
        pal.append(f'<div class="pal"><div class="pname">{html.escape(f["name"])}</div><div class="sws">{sw}</div></div>')
    n = sum(len(v) for v in groups.values())
    return (f'<section id="avatar"><h2>Avatars <small>V01 to V{n:02d} &middot; models</small></h2>'
            '<p class="sub">Waist-up humans (no legs, like the original) built by tools/blender/make_avatar.py. In the game the '
            'arms follow your real hands, and a rim light in your team colour outlines every player. Open the builder from the '
            'menu: CUSTOMIZE AVATAR (practice only). Your look is saved in avatar.cfg and sent to the other player.</p>'
            + ''.join(parts) +
            '<h3>Colour options</h3><p class="sub">TEAM follows your team colour (blue or orange). Swatches are approximate.</p>'
            + ''.join(pal) + '</section>')
