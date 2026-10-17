"""Build the Project Arena asset catalog: a numbered page of every original asset (models, textures, sounds,
code-drawn effects) with thumbnails and sound players, so each piece can be referenced by ID.

Writes build/catalog/index.html plus img/ and snd/ (published as an Artifact).
Usage: python -I tools/gen_catalog.py
"""
import html
import importlib.util
import math
import os
import shutil
import subprocess
import sys
import wave

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, 'app', 'assets_open')
TEXPNG = os.path.join(ROOT, 'build', 'open_src', 'tex')
OUT = os.path.join(ROOT, 'build', 'catalog')
FFMPEG = os.path.join(ROOT, 'third_party', 'pylibs', 'imageio_ffmpeg', 'binaries', 'ffmpeg-win64-v4.2.2.exe')
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import frames  # noqa: E402
import catalog_avatar  # noqa: E402
import catalog_dl  # noqa: E402

spec = importlib.util.spec_from_file_location('pr', os.path.join(ROOT, 'tools', 'preview_render.py'))
pr = importlib.util.module_from_spec(spec)
spec.loader.exec_module(pr)
pr.ASSETS = ASSETS

TEAM0 = np.array([0.1, 0.7, 1.0])
TEAM1 = np.array([1.0, 0.4, 0.1])


# ---------------------------------------------------------------- what is in the catalog
MODELS = [
    # id, file, name, made by, description, render recipe
    ('A01', 'arena', 'Neon glass court', 'tools/blender/make_arena.py: build_court',
     'Transparent glass walls, roof and back walls with the neon tube outline, floor markings and player rings.', 'arena'),
    ('A02', 'floor', 'Polished floor', 'make_arena.py: build_court (floor)',
     'Dark polished tiles with the pale runway and team-tinted end zones. Lit by the Cycles bake (lightmap T13).', 'floor'),
    ('A03', 'court_fx', 'Glow, holograms and hex energy', 'make_arena.py: halo_line / hex_card / neon',
     'Soft halos around every neon tube, the holographic back-wall targets, hex energy on the end-zone glass, the faint floor grid, the overhead light shafts.', 'fx'),
    ('A04', 'court_pulse', 'Energy pulses', 'make_arena.py: neon (pulse cards)',
     'Cards along each neon tube that the game scrolls so energy pulses race around the outline (texture T12). Shown without the tubes, brightened.', 'pulse'),
    ('A05', 'floor_reflect', 'Floor reflection layer', 'tools/build_open_assets.py: mirrored()',
     'Mirror image of every light source below the floor; seen through the glossy floor it gives the reflections.', 'reflect'),
    ('A06', 'environment', 'Stadium', 'make_arena.py: build_environment',
     'Dais rim, the slat light galleries, tiered stands with the crowd, overhead rig with spotlights, the screen block and the sky dome.', 'env'),
    ('A07', 'env_fx', 'Crowd lights', 'make_arena.py: seat_row (twinkle cards)',
     'Twinkling phone and light-stick glows in the crowd (texture T07, scrolled by the game).', 'envfx'),
    ('P01', 'projectile', 'Plasma ball', 'tools/blender/make_props.py: ball',
     'The energy sphere (additive, swirling veins T08). The game adds the white core, trails and electric arcs (C03).', 'ball'),
    ('P02', 'projectile_shell', 'Filament orbits', 'make_props.py: shell',
     'Three hair-thin glowing orbits with sparks that spin with the ball and show its curve.', 'shell'),
    ('P03', 'spawner', 'Spawn pad', 'make_props.py: spawner',
     'Floor pad in front of each player, glowing in the team colour; the ball rises from it.', 'pad'),
    ('P04', 'lancer', 'Controller gauntlet (classic)', 'make_props.py: lancer',
     'GAUNTLET = CONTROLLER: a VR controller held in an armoured glove; the glass knuckle guard floats in front of it.', 'hand'),
    ('P05', 'knuckleguard', 'Knuckle guard', 'make_props.py: knuckleguard',
     'Clear glass bumper in front of the fist (matches the punch collision box). Drawn translucent.', 'knuckle'),
    ('P06', 'shield', 'Hex shield', 'make_props.py: shield',
     'Energy shield that forms while you hold a charged ball (hex texture T04, bright rim).', 'shield'),
    ('P07', 'strikezone', 'Strike ring', 'make_props.py: strikezone',
     'Holographic ring behind each player; a ball through it is a strike (texture T03).', 'zone'),
    ('P08', 'head', 'Old android head (fallback)', 'make_props.py: head',
     'The first placeholder head. Replaced by the avatar parts (V section); only used if those are missing.', 'head'),
    ('P09', 'torso', 'Old android torso (fallback)', 'make_props.py: torso',
     'The first placeholder torso. Replaced by the avatar parts (V section); only used if those are missing.', 'torso'),
    ('P10', 'gauntlet_0', 'Striker gauntlet', 'make_props.py: gauntlet(0)',
     'GAUNTLET = STRIKER (default): a gothic plate gauntlet. Overlapping scales on every finger with pointed tips, '
     'a knuckle ridge with gadlings, a fluted back plate, a thumb wing and a pointed bell cuff with rolled rims.', 'hand'),
    ('P11', 'gauntlet_1', 'Titan gauntlet', 'make_props.py: gauntlet(1)',
     'GAUNTLET = TITAN: heavy plate. Thick finger scales, a spiked knuckle ridge, a ridged and riveted back plate and a '
     'wide bell cuff.', 'hand'),
    ('P12', 'gauntlet_2', 'Phantom gauntlet', 'make_props.py: gauntlet(2)',
     'GAUNTLET = PHANTOM: light plate over a dark glove. Thin finger scales, a smooth knuckle ridge and back plate with '
     'glowing inlays, a slim cuff.', 'hand'),
]

TEXTURES = [
    ('T01', 'soft_strip', 'Soft strip', 'Gaussian band used for every halo and glow line.'),
    ('T02', 'soft_dot', 'Soft dot', 'Round glow sprite.'),
    ('T03', 'holo_ring', 'Holo ring', 'Back-wall targets and the strike ring.'),
    ('T04', 'hex', 'Hex grid', 'End-zone energy and the shield.'),
    ('T05', 'slats', 'Light slats', 'The stadium light galleries.'),
    ('T06', 'sky', 'Night sky', 'Starfield and violet nebula on the sky dome. Shown 2.5x brighter.'),
    ('T07', 'twinkle', 'Twinkle strip', 'Crowd light brightness over time.'),
    ('T08', 'veins', 'Plasma veins', 'Energy veins on the ball.'),
    ('T09', 'emblem', 'Project Arena emblem', 'Centre-court logo, screens.'),
    ('T10', 'screen', 'Screen face', 'The hanging screen block.'),
    ('T11', 'font', 'Stroke font', 'Every letter in the scoreboard, menu and hub.'),
    ('T12', 'pulse', 'Energy pulse', 'Pulses racing along the neon tubes.'),
]
LIGHTMAPS = [
    ('T13', 'open_court_lm', 'Court lightmap', 'Cycles-baked light on the floor (from the neon, galleries and rig).'),
    ('T14', 'open_env_lm', 'Stadium lightmap', 'Cycles-baked light on the stands, dais and rig.'),
]

SOUNDS = [   # the subtle set (tools/gen_open_audio.py); the classic set is kept in sounds/classic
    ('S01', 'Spawn', 'Ball appears', 'A small soft bloom with a faint glint as a new ball appears on its launcher.'),
    ('S02', 'SpawnRise', 'Launcher rises', 'A gentle airy swell as the launcher lifts out of the floor.'),
    ('S03', 'Grab', 'Grab', 'A muted thump and a brief warm hum when you take the ball.'),
    ('S04', 'Caught', 'Catch', 'A padded catch with a quick soft rise.'),
    ('S05', 'ThrowSlow', 'Throw (slow)', 'A soft low whoosh, the gentlest throw.'),
    ('S06', 'ThrowMed', 'Throw (medium)', 'A clean whoosh, a little brighter.'),
    ('S07', 'ThrowFast', 'Throw (fast)', 'A quicker, brighter whoosh with a faint streak.'),
    ('S08', 'ThrowFull', 'Throw (full power)', 'The fullest whoosh with a soft low push.'),
    ('S09', 'ThrowFail', 'Dropped throw', 'A soft falling tone and faint fizzle when a release is too slow.'),
    ('S10', 'Bounce', 'Wall bounce', 'A short soft tok off the glass (the quietest sound: it plays most).'),
    ('S11', 'ShieldBlock', 'Shield block', 'A resonant soft thoom with a short glassy shimmer.'),
    ('S12', 'ShieldBreak', 'Shield break', 'A soft crystalline crack fading into glitter.'),
    ('S13', 'KnuckleBlock', 'Knuckle punch', 'A tight padded metallic knock.'),
    ('S14', 'HitPlayerOpponent', 'You scored', 'A soft deep thud, a gentle discharge and a small rising two-note chime.'),
    ('S15', 'HitPlayerSelf', 'You were hit', 'A muffled heavy thud with a low downward wobble.'),
    ('S16', 'Returned', 'Ball returns', 'An airy whoosh moving away as the ball flies home.'),
    ('S17', 'Foul', 'Foul', 'Two rounded falling notes.'),
    ('S18', 'Countdown', 'Countdown beep', 'One calm low beep for each count.'),
    ('S19', 'RoundStart', 'Round start', 'A short bright swell landing on a soft chord.'),
    ('S20', 'RoundEndWin', 'Round won', 'A small crowd cheering and clapping.'),
    ('S21', 'RoundEndLose', 'Round lost', 'A small crowd "ohh" with a few claps.'),
    ('S22', 'MatchEndWin', 'Match won', 'Crowd cheers and applause over a soft major chord.'),
    ('S23', 'MatchEndLose', 'Match lost', 'Warm applause over a gentle falling chord.'),
    ('S24', 'Recharge', 'Recharge', 'A short soft rise and a gentle ding when your ball recharges.'),
    ('S25', 'Strike', 'Strike', 'A clear two-note ding-ding over a soft swell.'),
    ('S26', 'Overtime', 'Sudden death', 'A deep pulsing swell with two heartbeat thumps.'),
    ('S27', 'Ambience', 'Arena ambience', 'A quiet 30-second loop: low hum, air and a distant crowd.'),
    ('S28', 'FistBump', 'Fist bump', 'A friendly metallic knock and a warm pulse when two players bump fists to start.'),
]

CODE_FX = [
    ('C01', 'Hit plasma burst', 'app/src/main.cpp: drawHitFx',
     'Flash, shock ring, swirling plasma puffs, curling wisps and lingering sparks in the scorer\'s colour; a colour wash for the player who was hit.'),
    ('C02', 'Spawn glow and rise', 'main.cpp: drawSpawnFx',
     'Glow gathers on the pad, then the ball floats up with twisting tracers.'),
    ('C03', 'Electric arcs', 'main.cpp: drawWorld (ball)',
     'Jagged arcs crackling over the ball surface, re-rolled 18 times a second.'),
    ('C04', 'Ball trails', 'main.cpp: trail ribbons',
     'Soft-edged trail plus two helix streaks that show the spin.'),
    ('C05', 'Ball light pool and reflection', 'main.cpp: drawWorld (floor)',
     'Coloured light on the floor under the ball and its reflection in the floor.'),
    ('C06', 'Holographic scoreboard', 'main.cpp: HUD',
     'Translucent board over centre court with team-coloured scores, clock, strikes and charge.'),
    ('C07', 'Multiplayer menu', 'main.cpp: drawMenu',
     'Floating panel with the laser pointer: host, join, spectate, rules, tuning.'),
    ('C08', 'Spectator hub', 'main.cpp: drawHub',
     'Live miniature arenas on pedestals; point and pull the trigger to watch one.'),
    ('C09', 'Avatar builder', 'main.cpp: drawBuilder',
     'A mirror that copies your movements, with a pointer panel beside it: suit, build, pattern, colours, hair, headgear, face shield, mask, skin, presets, random.'),
    ('C10', 'Avatar rig and rim light', 'app/src/avatar.cpp: solveRig; render.cpp: uRim',
     'Torso hung below the neck, two-bone arms reaching your real hands, the team-colour rim light and the waist energy fade.'),
]


# ---------------------------------------------------------------- rendering
def T(p):
    M = np.eye(4)
    M[:3, 3] = p
    return M


def S(s):
    M = np.eye(4)
    M[0, 0] = M[1, 1] = M[2, 2] = s
    return M


def rot_y(deg):
    a = math.radians(deg)
    M = np.eye(4)
    M[:3, :3] = [[math.cos(a), 0, math.sin(a)], [0, 1, 0], [-math.sin(a), 0, math.cos(a)]]
    return M


THUMB = (1280, 720)   # saved at the render size: cards scale it down, the viewer shows it full
pr.W, pr.H = THUMB   # rendered at the size it is saved (the enlarged view needs the detail)


def world_points(entries, limit=40000):
    pts = []
    for e in entries:
        (v, idx, secs, mats), M = e[0], e[1]
        P = (np.c_[v['p'], np.ones(len(v))] @ M.T)[:, :3]
        pts.append(P)
    P = np.concatenate(pts)
    if len(P) > limit:
        P = P[np.linspace(0, len(P) - 1, limit).astype(int)]
    return P


def fit_camera(P, view_dir, fov, fill=0.84):
    """Eye on view_dir (from the target) at the distance where P just fills `fill` of the frame, centred."""
    aspect = pr.W / pr.H
    t = math.tan(math.radians(fov) / 2)
    d = np.array(view_dir, float)
    d /= np.linalg.norm(d)
    c = (P.min(0) + P.max(0)) / 2
    eye = c + d
    for _ in range(3):
        lo, hi = 1e-3, 1e4
        for _ in range(60):
            mid = math.sqrt(lo * hi)
            q = (np.c_[P, np.ones(len(P))] @ pr.look_at(c + d * mid, c).T)[:, :3]
            z = -q[:, 2]
            if (z <= 0.02).any():
                lo = mid
                continue
            x, y = q[:, 0] / (z * t * aspect), q[:, 1] / (z * t)
            if max(np.abs(x).max(), np.abs(y).max()) <= fill:
                hi = mid
            else:
                lo = mid
        eye = c + d * hi
        V = pr.look_at(eye, c)
        q = (np.c_[P, np.ones(len(P))] @ V.T)[:, :3]
        z = -q[:, 2]
        x, y = q[:, 0] / (z * t * aspect), q[:, 1] / (z * t)
        mx, my = (x.min() + x.max()) / 2, (y.min() + y.max()) / 2
        c = c + V[0, :3] * mx * hi * t * aspect + V[1, :3] * my * hi * t   # re-centre the projected extents
    return list(eye), list(c)


def team(k, base=0.0):
    return base + k * TEAM0


def recipe(name, kind):
    """(draw entries, entries to frame or None, view dir, fov, manual eye/target or None)"""
    m = pr.load_mdl(name)
    I = np.eye(4)
    ghost = lambda: (pr.load_mdl('arena'), I, 1.0, np.full(3, 0.22))
    if kind == 'arena':
        return [(m, I)], None, (-0.55, 0.42, 0.72), 38, None
    if kind == 'floor':
        return [(m, I)], None, (-0.30, 0.80, 0.52), 38, None
    if kind == 'fx':
        return [ghost(), (m, I, 1.0, np.full(3, 1.6))], [(m, I)], (-0.55, 0.42, 0.72), 38, None
    if kind == 'reflect':
        return [(m, I)], None, (-0.55, 0.30, 0.78), 38, None
    if kind == 'env':
        return [(m, I)], None, None, 75, ([-17.0, 9.5, 12.5], [4.0, 4.0, -6.0])
    if kind == 'pulse':   # close-up of one end: the pulses are short comets racing along the tubes
        return [(m, I, 1.0, np.full(3, 6.0))], None, None, 62, \
            ([-6.2, 2.6, 4.6], [-9.5, 1.6, -0.6])
    if kind == 'envfx':   # looking into one long stand from above the court
        return [(pr.load_mdl('environment'), I, 1.0, np.full(3, 0.6)), (m, I, 1.0, np.full(3, 3.0))], None, None, 22, \
            ([-3.0, 6.0, 3.0], [1.0, 7.2, -15.5])
    if kind == 'ball':
        return [(m, I, 1.0, team(0.6))], None, (0.5, 0.35, 0.8), 30, None
    if kind == 'shell':
        return [(m, I, 1.0, team(2.0))], None, (0.5, 0.35, 0.8), 30, None
    if kind == 'pad':
        return [(m, I, 1.0, np.ones(3), team(0.16))], None, (0.40, 0.62, 0.70), 32, None
    if kind == 'hand':
        return [(m, frames.LANCER, 1.0, team(0.4, 0.6))], None, (0.80, 0.32, -0.50), 32, None
    if kind == 'knuckle':
        e = [(pr.load_mdl('lancer'), frames.LANCER, 1.0, team(0.4, 0.6)),
             (m, frames.KNUCKLE, 0.35, team(0.25, 0.75), team(0.06))]
        return e, None, (0.62, 0.38, -0.70), 32, None
    if kind == 'shield':
        return [(m, frames.PLATE, 0.7, team(1.0), team(0.4))], None, (0.0, 0.87, 0.49), 32, None
    if kind == 'zone':
        return [(m, I, 1.0, team(0.7))], None, (0.90, 0.18, 0.40), 32, None
    if kind in ('head', 'torso'):
        return [(m, I, 1.0, team(0.3, 0.7))], None, (0.50, 0.18, 0.85), 30, None
    raise ValueError(kind)


def render_thumb(entries, out, eye, tgt, fov):
    tmp = os.path.join(OUT, '_shot.png')
    pr.render(entries, eye, tgt, tmp, fov=fov)
    Image.open(tmp).convert('RGB').resize(THUMB, Image.LANCZOS).save(out, quality=88)
    os.remove(tmp)


def shot(name, kind, out):
    entries, frame, view, fov, manual = recipe(name, kind)
    if manual:
        eye, tgt = manual
    else:
        eye, tgt = fit_camera(world_points(frame or entries), view, fov)
    render_thumb(entries, out, eye, tgt, fov)


def full_arena_shot(out):
    I = np.eye(4)
    models = [(pr.load_mdl('floor_reflect'), I), (pr.load_mdl('floor'), I, 0.8),
              (pr.load_mdl('environment'), I), (pr.load_mdl('arena'), I),
              (pr.load_mdl('env_fx'), I), (pr.load_mdl('court_fx'), I), (pr.load_mdl('court_pulse'), I)]
    for x, tm in ((-6.48, TEAM0), (6.48, TEAM1)):
        models.append((pr.load_mdl('spawner'), T((x, 0.0, 0.0)), 1.0, np.ones(3), tm * 0.16))
    tmp = os.path.join(OUT, '_shot.png')
    W0, H0 = pr.W, pr.H
    pr.W, pr.H = 1920, 1080
    pr.render(models, [0.0, 2.9, 6.8], [0.0, 1.1, 0.0], tmp, fov=70)   # beside the court, inside the light galleries (z 7.6)
    pr.W, pr.H = W0, H0
    Image.open(tmp).convert('RGB').resize((1280, 720), Image.LANCZOS).save(out, quality=88)
    os.remove(tmp)


def texture_thumb(src, out, name):
    im = Image.open(src).convert('RGBA')
    bg = Image.new('RGBA', im.size, (8, 11, 18, 255))
    im = Image.alpha_composite(bg, im).convert('RGB')
    if name == 'font':   # 59 glyphs of 48x64 in one row -> a specimen of 4 rows
        cw, ch, per = 48, 64, 15
        n = im.size[0] // cw
        rows = (n + per - 1) // per
        sheet = Image.new('RGB', (per * cw + 32, rows * ch + 32), (8, 11, 18))
        for i in range(n):
            sheet.paste(im.crop((i * cw, 0, (i + 1) * cw, ch)), (16 + (i % per) * cw, 16 + (i // per) * ch))
        im = sheet
    elif name == 'twinkle':   # brightness over time: one row per light, stretched to read as bands
        im = im.resize((THUMB[0], THUMB[1] // 2), Image.NEAREST)
    elif name == 'pulse':     # the comet runs along v; lay it on its side
        im = im.transpose(Image.ROTATE_90)
    elif name == 'sky':
        im = im.point(lambda v: min(255, int(v * 2.5)))
    w, h = im.size
    sc = min(THUMB[0] / w, THUMB[1] / h)
    # small textures are blown up with hard pixels so the enlarged view shows them as they are
    im = im.resize((max(1, int(w * sc)), max(1, int(h * sc))), Image.NEAREST if sc > 1.5 else Image.LANCZOS)
    im.save(out, quality=88)
    return im.size


def to_mp3(wav, mp3):
    subprocess.run([FFMPEG, '-y', '-loglevel', 'error', '-i', wav, '-codec:a', 'libmp3lame', '-q:a', '4', mp3], check=True)


def wav_seconds(path):
    with wave.open(path) as w:
        return w.getnframes() / w.getframerate()


# ---------------------------------------------------------------- page
CSS = r'''
:root {
  /* layout: a studio contact sheet; one sticky section rail, cards in an auto-fill grid */
  --bg: #090d14; --panel: #101724; --panel-2: #152033; --line: #22314a;
  --fg: #dde7f3; --muted: #8b9ab0; --cyan: #5fd8ff; --amber: #ff9a4d;
  --display: "Chakra Petch", "Bahnschrift", "Segoe UI", sans-serif;
  --body: "Barlow", "Segoe UI", system-ui, sans-serif;
  --mono: "IBM Plex Mono", ui-monospace, "Cascadia Mono", Consolas, monospace;
  color-scheme: dark;
}
* { box-sizing: border-box; }
html, body { background: var(--bg); color: var(--fg); }
body { font: 15px/1.5 var(--body); padding-inline: 16px; padding-block: 0 48px; }
.wrap { max-width: 1240px; margin: 0 auto; }
header { padding-block: 32px 18px; border-bottom: 1px solid var(--line); display: grid; gap: 10px; }
h1 { font: 600 clamp(28px, 4vw, 44px)/1.05 var(--display); letter-spacing: 0.02em; margin: 0; text-wrap: balance; }
h1 span { color: var(--cyan); }
.lede { color: var(--muted); max-width: 70ch; margin: 0; }
.hero { margin: 22px 0 6px; border: 1px solid var(--line); border-radius: 6px; overflow: hidden; background: #000; }
.hero img { display: block; width: 100%; height: auto; max-height: min(56vh, 520px); object-fit: cover; }
.hero figcaption { padding: 8px 12px; color: var(--muted); font-size: 13px; background: var(--panel); }
nav { position: sticky; top: env(safe-area-inset-top, 0px); z-index: 5; background: color-mix(in srgb, var(--bg) 92%, transparent);
  backdrop-filter: blur(6px); border-bottom: 1px solid var(--line); padding-block: 10px; display: flex; flex-wrap: wrap; gap: 8px; align-items: center; }
nav a, .chip { font: 500 13px/1 var(--display); letter-spacing: 0.06em; text-transform: uppercase; color: var(--fg); text-decoration: none;
  border: 1px solid var(--line); border-radius: 999px; padding: 8px 12px; background: var(--panel); }
nav a:hover, nav a:focus-visible { border-color: var(--cyan); color: var(--cyan); outline: none; }
nav input { margin-left: auto; min-width: 0; flex: 1 1 200px; max-width: 320px; background: var(--panel); color: var(--fg);
  border: 1px solid var(--line); border-radius: 999px; padding: 8px 14px; font: 14px var(--body); }
nav input:focus-visible { outline: 2px solid var(--cyan); outline-offset: 1px; }
section { padding-block: 28px 4px; scroll-margin-top: 64px; }
h2 { font: 600 20px/1.2 var(--display); letter-spacing: 0.08em; text-transform: uppercase; margin: 0 0 4px; display: flex; gap: 10px; align-items: baseline; }
h2 small { font: 400 13px var(--body); letter-spacing: 0; text-transform: none; color: var(--muted); }
.sub { color: var(--muted); margin: 0 0 14px; max-width: 75ch; }
.grid { display: grid; grid-template-columns: repeat(auto-fill, minmax(260px, 1fr)); gap: 14px; }
.card { background: var(--panel); border: 1px solid var(--line); border-radius: 6px; overflow: hidden; display: flex; flex-direction: column; min-width: 0; }
.card .img { aspect-ratio: 16 / 9; background: #05070c; display: grid; place-items: center; overflow: hidden; }
.card .img img { width: 100%; height: 100%; object-fit: cover; display: block; }
.card .img.contain img { object-fit: contain; }
.card .img img, .hero img { cursor: zoom-in; }
/* enlarged view: click any picture; arrows, swipe or the side buttons step through what the filter shows */
dialog#lb { border: 0; padding: 0; margin: 0; background: transparent; width: 100vw; height: 100dvh; max-width: 100vw; max-height: 100dvh; color: var(--fg); }
dialog#lb::backdrop { background: rgba(3, 5, 9, 0.94); }
#lb .stage { position: absolute; inset: 0; display: grid; grid-template-rows: 1fr auto; justify-items: center; align-items: center; gap: 10px; padding: 52px 16px 16px; }
#lb img { max-width: 100%; max-height: calc(100dvh - 120px); object-fit: contain; display: block; border-radius: 6px; box-shadow: 0 0 0 1px var(--line); background: #05070c; }
#lb .cap { font: 500 14px/1.4 var(--body); text-align: center; max-width: 900px; }
#lb .cap b { font: 600 14px var(--mono); color: var(--cyan); margin-right: 8px; }
#lb .cap small { display: block; color: var(--muted); font-size: 13px; }
#lb button { position: absolute; background: var(--panel-2); color: var(--fg); border: 1px solid var(--line); border-radius: 999px;
  width: 44px; height: 44px; font: 600 20px/1 var(--display); cursor: pointer; display: grid; place-items: center; }
#lb button:hover, #lb button:focus-visible { border-color: var(--cyan); color: var(--cyan); outline: none; }
#lb .close { top: 8px; right: 12px; }
#lb .prev { left: 12px; top: 50%; transform: translateY(-50%); }
#lb .next { right: 12px; top: 50%; transform: translateY(-50%); }
#lb .open { top: 8px; left: 12px; width: auto; padding: 0 14px; font: 500 13px var(--body); text-decoration: none; }
@media (max-width: 640px) { #lb .prev, #lb .next { top: auto; bottom: 10px; transform: none; } #lb .stage { padding-bottom: 64px; } }
.card .meta { padding: 10px 12px 12px; display: grid; gap: 4px; }
.id { font: 600 13px var(--display); letter-spacing: 0.08em; color: var(--cyan); }
.card.tex .id { color: var(--amber); }
.name { font: 600 16px/1.25 var(--display); }
.desc { color: var(--muted); font-size: 13.5px; }
.path { font: 12px/1.4 var(--mono); color: #9fb3cc; overflow-wrap: anywhere; }
.copy { justify-self: start; margin-top: 4px; font: 500 12px var(--display); letter-spacing: 0.05em; text-transform: uppercase;
  color: var(--fg); background: var(--panel-2); border: 1px solid var(--line); border-radius: 4px; padding: 5px 9px; cursor: pointer; }
.copy:hover, .copy:focus-visible { border-color: var(--cyan); color: var(--cyan); outline: none; }
.btns { display: flex; flex-wrap: wrap; gap: 6px; }
a.copy.dl { text-decoration: none; display: inline-block; }
.contrib { margin: 6px 0 0; display: flex; flex-wrap: wrap; gap: 10px; align-items: center; }
.contrib a.copy { font-size: 13px; padding: 8px 12px; border-color: var(--cyan); color: var(--cyan); }
.sounds { display: grid; gap: 8px; }
.snd { display: grid; grid-template-columns: 54px minmax(0, 1.2fr) minmax(0, 2fr) minmax(220px, 1fr); gap: 12px; align-items: center;
  background: var(--panel); border: 1px solid var(--line); border-radius: 6px; padding: 10px 12px; }
.snd .id { color: #b98cff; }
.snd audio { width: 100%; height: 34px; }
.snd .len { font: 12px var(--mono); color: var(--muted); font-variant-numeric: tabular-nums; }
.fx { display: grid; grid-template-columns: repeat(auto-fill, minmax(300px, 1fr)); gap: 10px; }
h3 { font: 600 15px/1.2 var(--display); letter-spacing: 0.08em; text-transform: uppercase; margin: 22px 0 6px; color: #ffb3d9; }
h3 small { font: 400 12px var(--body); letter-spacing: 0; text-transform: none; color: var(--muted); margin-left: 6px; }
.avgrid { grid-template-columns: repeat(auto-fill, minmax(190px, 1fr)); }
.card.av .img { aspect-ratio: 42 / 52; } .card.avc .img { aspect-ratio: 1; }
.card.av .id, .card.avc .id { color: #ff8cc6; }
.pal { display: grid; grid-template-columns: 120px minmax(0, 1fr); gap: 10px; align-items: start; padding: 8px 0; border-top: 1px solid var(--line); }
.pname { font: 600 13px var(--display); letter-spacing: 0.06em; color: var(--muted); padding-top: 4px; }
.sws { display: flex; flex-wrap: wrap; gap: 6px 12px; }
.sw { display: inline-flex; align-items: center; gap: 6px; font-size: 13px; }
.sw i { width: 18px; height: 18px; border-radius: 3px; border: 1px solid #ffffff33; display: inline-block; }
@media (max-width: 520px) { .pal { grid-template-columns: minmax(0, 1fr); } }
.fx .card { padding: 12px; gap: 4px; display: grid; }
.fx .id { color: #7dffb0; }
footer { margin-top: 36px; border-top: 1px solid var(--line); padding-top: 14px; color: var(--muted); font-size: 13px; }
@media (max-width: 760px) { .snd { grid-template-columns: 48px minmax(0, 1fr); } .snd .d, .snd audio { grid-column: 1 / -1; }
  nav { position: static; } nav a { padding: 7px 10px; } nav input { margin-left: 0; max-width: none; } }
@media (prefers-reduced-motion: reduce) { * { scroll-behavior: auto !important; } }
'''

JS = r'''
document.querySelectorAll('.copy').forEach(b => b.addEventListener('click', () => {
  const t = b.dataset.copy;
  const done = () => { const o = b.textContent; b.textContent = 'Copied'; setTimeout(() => b.textContent = o, 1200); };
  if (navigator.clipboard && navigator.clipboard.writeText) {
    navigator.clipboard.writeText(t).then(done).catch(() => window.getSelection().selectAllChildren(b.previousElementSibling));
  } else { window.getSelection().selectAllChildren(b.previousElementSibling); }
}));
const q = document.getElementById('search');
q.addEventListener('input', () => {
  const s = q.value.trim().toLowerCase();
  document.querySelectorAll('[data-search]').forEach(el => { el.hidden = s && !el.dataset.search.includes(s); });
});
const lb = document.getElementById('lb');
const lbImg = lb.querySelector('img'), lbCap = lb.querySelector('.cap'), lbOpen = lb.querySelector('.open');
const PICS = '.hero img, .card .img img';
let lbList = [], lbIdx = 0;
function lbShow(i) {
  lbIdx = (i + lbList.length) % lbList.length;
  const im = lbList[lbIdx], card = im.closest('.card');
  lbImg.src = im.src; lbImg.alt = im.alt; lbOpen.href = im.src;
  const id = card && card.querySelector('.id') ? card.querySelector('.id').textContent : '';
  const name = card && card.querySelector('.name') ? card.querySelector('.name').textContent : im.alt;
  const desc = card && card.querySelector('.desc') ? card.querySelector('.desc').textContent : '';
  lbCap.innerHTML = '';
  if (id) { const b = document.createElement('b'); b.textContent = id; lbCap.append(b); }
  lbCap.append(name + '  (' + (lbIdx + 1) + ' of ' + lbList.length + ')');
  if (desc) { const s = document.createElement('small'); s.textContent = desc; lbCap.append(s); }
}
document.addEventListener('click', e => {
  const im = e.target.closest(PICS);
  if (!im || lb.open) return;
  lbList = [...document.querySelectorAll(PICS)].filter(x => !x.closest('[hidden]'));
  lbShow(lbList.indexOf(im));
  lb.showModal();
});
lb.querySelector('.close').addEventListener('click', () => lb.close());
lb.querySelector('.prev').addEventListener('click', () => lbShow(lbIdx - 1));
lb.querySelector('.next').addEventListener('click', () => lbShow(lbIdx + 1));
lb.addEventListener('click', e => { if (e.target === lb || e.target.classList.contains('stage')) lb.close(); });
document.addEventListener('keydown', e => {
  if (!lb.open) return;
  if (e.key === 'ArrowLeft') lbShow(lbIdx - 1);
  else if (e.key === 'ArrowRight') lbShow(lbIdx + 1);
});
let lbX = null;
lb.addEventListener('touchstart', e => { lbX = e.touches[0].clientX; }, {passive: true});
lb.addEventListener('touchend', e => {
  if (lbX === null) return;
  const dx = e.changedTouches[0].clientX - lbX;
  if (Math.abs(dx) > 50) lbShow(lbIdx + (dx < 0 ? 1 : -1));
  lbX = null;
});
document.addEventListener('play', e => {
  document.querySelectorAll('audio').forEach(a => { if (a !== e.target) a.pause(); });
}, true);
'''


def esc(s):
    return html.escape(s, quote=True)


def card(id_, name, desc, path, img, kind='', contain=False):
    search = esc((id_ + ' ' + name + ' ' + desc + ' ' + path).lower())
    dl = catalog_dl.link(OUT, id_, path)
    return (f'<article class="card {kind}" data-search="{search}"><div class="img{" contain" if contain else ""}">'
            f'<img src="{esc(img)}" alt="{esc(name)}" loading="lazy"></div><div class="meta"><div class="id">{id_}</div>'
            f'<div class="name">{esc(name)}</div><div class="desc">{esc(desc)}</div><div class="path">{esc(path)}</div>'
            f'<div class="btns"><button class="copy" type="button" data-copy="{esc(id_ + " " + name)}">Copy ID</button>{dl}</div>'
            f'</div></article>')


def main():
    if os.path.isdir(OUT):
        shutil.rmtree(OUT)
    os.makedirs(os.path.join(OUT, 'img'))
    os.makedirs(os.path.join(OUT, 'snd'))
    print('hero')
    full_arena_shot(os.path.join(OUT, 'img', 'hero.jpg'))
    model_cards = []
    for id_, f, name, by, desc, kind in MODELS:
        print('model', id_, f)
        img = 'img/%s.jpg' % id_
        shot(f, kind, os.path.join(OUT, img))
        model_cards.append((id_, card(id_, name, desc, 'app/assets_open/%s.mdl  |  %s' % (f, by), img)))
    tex_cards = []
    for id_, f, name, desc in TEXTURES:
        img = 'img/%s.jpg' % id_
        texture_thumb(os.path.join(TEXPNG, f + '.png'), os.path.join(OUT, img), f)
        tex_cards.append(card(id_, name, desc, 'app/assets_open/tex/open_%s.tex  |  tools/gen_open_textures.py: %s()' % (f, f),
                              img, 'tex', contain=True))
    for id_, f, name, desc in LIGHTMAPS:
        img = 'img/%s.jpg' % id_
        src = os.path.join(ROOT, 'build', 'open_src', f + '.png')
        im = Image.open(src).convert('RGB')
        im = im.point(lambda v: min(255, int(v * 3.0)))   # brightened for viewing
        im.resize((THUMB[1], THUMB[1]), Image.LANCZOS).save(os.path.join(OUT, img), quality=88)
        tex_cards.append(card(id_, name, desc + ' Shown 3x brighter.', 'app/assets_open/tex/%s.tex  |  Cycles bake in make_arena.py' % f,
                              img, 'tex', contain=True))
    rows = []
    for id_, key, name, desc in SOUNDS:
        wav = os.path.join(ASSETS, 'audio', 'oa_%s.wav' % key)
        mp3 = 'snd/%s.mp3' % id_
        to_mp3(wav, os.path.join(OUT, mp3))
        secs = wav_seconds(wav)
        search = esc((id_ + ' ' + name + ' ' + desc + ' ' + key).lower())
        rows.append(f'<div class="snd" data-search="{search}"><div class="id">{id_}</div><div><div class="name">{esc(name)}</div>'
                    f'<div class="len">oa_{esc(key)}.wav &middot; {secs:.1f} s</div></div><div class="desc d">{esc(desc)}</div>'
                    f'<audio controls preload="none" src="{mp3}"></audio></div>')
    fx = []
    for id_, name, where, desc in CODE_FX:
        search = esc((id_ + ' ' + name + ' ' + desc).lower())
        fx.append(f'<article class="card" data-search="{search}"><div class="id">{id_}</div><div class="name">{esc(name)}</div>'
                  f'<div class="desc">{esc(desc)}</div><div class="path">{esc(where)}</div></article>')
    arena = ''.join(c for i, c in model_cards if i.startswith('A'))
    props = ''.join(c for i, c in model_cards if i.startswith('P'))
    print('avatars')
    extra = catalog_avatar.build(OUT)
    print('template pack')
    tpl = catalog_dl.templates(OUT)
    page = f'''<title>Project Arena Asset Catalog</title>
<link rel="preconnect" href="https://fonts.googleapis.com"><link rel="preconnect" href="https://fonts.gstatic.com" crossorigin>
<link rel="stylesheet" href="https://fonts.googleapis.com/css2?family=Barlow:wght@400;500;600&family=Chakra+Petch:wght@500;600&family=IBM+Plex+Mono&display=swap">
<style>{CSS}</style>
<div class="wrap">
<header>
  <h1>Project Arena <span>Asset Catalog</span></h1>
  <p class="lede">Every model, texture and sound made for Project Arena to replace CCP's library. All of it is generated by our own scripts; none of it comes from Sparc. Refer to anything by its ID, for example "P04 needs thinner fingers", "V14 hair too flat" or "S12 is too harsh", or send a replacement file for that ID.</p>
  <div class="contrib"><a class="copy dl" href="{tpl}" download>Download the template pack</a>
  <span class="sub">Every card has a Download button for its model (.glb, opens in Blender) or texture. The template pack has the
  bodies, heads and hand frame to build new outfits, hair, masks and gauntlets on, plus how to submit them. Contributed
  art is published under CC BY 4.0.</span></div>
  <figure class="hero"><img src="img/hero.jpg" alt="The neon glass arena seen from beside the court"><figcaption>All arena layers together (A01 to A07), rendered on the PC from the spectator seat beside the court.</figcaption></figure>
</header>
<nav aria-label="Sections"><a href="#arena">Arena</a><a href="#props">Props</a>{('<a href="#avatar">Avatar</a>' if extra else '')}<a href="#textures">Textures</a><a href="#sounds">Sounds</a><a href="#code">Code effects</a>
<input id="search" type="search" placeholder="Filter by ID or name" aria-label="Filter assets by ID or name"></nav>
<section id="arena"><h2>Arena and stadium <small>A01 to A07 &middot; models</small></h2>
<p class="sub">The court and the stadium are built together in one Blender scene so the neon really lights the floor (lightmaps T13, T14). Each layer is a separate file.</p>
<div class="grid">{arena}</div></section>
<section id="props"><h2>Props <small>P01 to P09 &middot; models</small></h2>
<p class="sub">Shown in their in-game orientation and team colour (blue team). Hand pieces are drawn mirrored for the left hand.</p>
<div class="grid">{props}</div></section>
{extra}
<section id="textures"><h2>Textures <small>T01 to T14</small></h2>
<p class="sub">Procedural textures from tools/gen_open_textures.py, plus the two baked lightmaps.</p>
<div class="grid">{''.join(tex_cards)}</div></section>
<section id="sounds"><h2>Sounds <small>S01 to S27</small></h2>
<p class="sub">All synthesized by tools/gen_open_audio.py (no samples). Press play to listen; starting one stops the others.</p>
<div class="sounds">{''.join(rows)}</div></section>
<section id="code"><h2>Code effects <small>C01 to C10 &middot; drawn by the game, no files</small></h2>
<p class="sub">These are generated live by the game engine, so there is no file to swap; describe changes by ID.</p>
<div class="fx">{''.join(fx)}</div></section>
<dialog id="lb" aria-label="Enlarged picture"><div class="stage"><img alt=""><div class="cap"></div></div>
<a class="open" href="#" target="_blank" rel="noopener">Open full size</a>
<button class="close" type="button" aria-label="Close">&times;</button>
<button class="prev" type="button" aria-label="Previous picture">&lsaquo;</button>
<button class="next" type="button" aria-label="Next picture">&rsaquo;</button></dialog>
<footer>Regenerate this page with <span class="path">python -I tools/gen_catalog.py</span>. Thumbnails come from the offline preview renderer and are close to, but not exactly, what the headset shows.</footer>
</div>
<script>{JS}</script>
'''
    with open(os.path.join(OUT, 'index.html'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(page)
    files = sorted(os.listdir(os.path.join(OUT, 'img'))) + sorted(os.listdir(os.path.join(OUT, 'snd')))
    print('wrote', os.path.join(OUT, 'index.html'), len(files), 'files')


if __name__ == '__main__':
    main()
