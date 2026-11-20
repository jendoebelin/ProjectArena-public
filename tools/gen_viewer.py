"""Build the Project Arena asset viewer: one three.js page with every model, texture and sound of the game, each with an
ID (A arena, P props, V avatar parts, T textures, S sounds, C effects the game draws in code, F the countries' flags:
the builder's COUNTRY puts on their team kit, the flag printed on it as main.cpp does), in the style of the
body inspector from public pull request #25 (choices in a sidebar beside a view you can turn and zoom).

The page reads the game's own files: models/ holds every app/assets_open .mdl (gzipped), tex/ the textures as PNG,
snd/ the sounds, and data.json the catalog, the avatar builder's options and presets, and the rig for each body, build
and pose (tools/avatar_pose.cpp: the game's own solveRig and part placing). The page draws them with a copy of the
game's shader (app/src/render.cpp) and the game's part selection (app/src/avatar.cpp drawList).

Usage: python -I tools/gen_viewer.py [OUT_DIR] [--text-models]    (default build/viewer; needs build/tests/avatar_pose.exe,
made by tools/build_avatar_pose.bat). --text-models: each model as its gzipped bytes in base64, a few to a bundle
(models/bundle_K.json), for hosts that serve only web types and few files (the Claude artifact); otherwise
models/NAME.mdl.gz, and dl/ holds the downloads
(--no-downloads leaves them out): each item's models as .glb (tools/export_glb.py: Blender's format, the 9-bone rig)
with the textures they use and a README, in dl/ID.zip; a texture or sound is its own file; dl/project-arena-templates.zip
is the contributor template pack (tools/make_templates.py, made from the game's bodies).
"""
import base64
import gzip
import json
import os
import re
import shutil
import struct
import subprocess
import sys
import zipfile

import numpy as np
from PIL import Image

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ASSETS = os.path.join(ROOT, 'app', 'assets_open')
POSE_EXE = os.path.join(ROOT, 'build', 'tests', 'avatar_pose.exe')
PAGE = os.path.join(ROOT, 'tools', 'viewer')
sys.path.insert(0, os.path.join(ROOT, 'tools'))
import frames  # noqa: E402

POSES = ['rest', 'ready', 'guard', 'throw', 'reach']
# the knuckle glass at the bumper (main.cpp knuckleShift: game.h Tunables::knuckleOffset, from 0.3.21's matches on,
# less the glass model's own centre)
KNUCKLE_SHIFT = np.eye(4)
KNUCKLE_SHIFT[:3, 3] = np.array([0.0, -0.010, -0.005]) - np.array([-0.022, 0.010, -0.137])
TEAMS = [[0.1, 0.7, 1.0], [1.0, 0.4, 0.1]]

# ---------------------------------------------------------------- the catalog (IDs as tools/gen_catalog.py has them)
ARENA = [
    ('A01', 'arena', 'Neon glass court', 'Transparent glass walls, roof and back walls with the neon tube outline, floor '
     'markings and player rings.', 'tools/blender/make_arena.py: build_court'),
    ('A02', 'floor', 'Polished floor', 'Dark polished tiles with the pale runway and team-tinted end zones, lit by the '
     'baked lightmap (T13).', 'make_arena.py: build_court (floor)'),
    ('A03', 'court_fx', 'Glow, holograms and hex energy', 'Soft halos around every neon tube, the holographic back-wall '
     'targets, hex energy on the end-zone glass, the faint floor grid, the overhead light shafts.',
     'make_arena.py: halo_line / hex_card / neon'),
    ('A04', 'court_pulse', 'Energy pulses', 'Cards along each neon tube that the game scrolls so energy pulses race '
     'around the outline (texture T12).', 'make_arena.py: neon (pulse cards)'),
    ('A05', 'floor_reflect', 'Floor reflection', 'The court mirrored below the floor; seen through the glossy floor it '
     'makes the reflections.', 'tools/build_open_assets.py: mirrored()'),
    ('A06', 'environment', 'Stadium', 'Dais rim, the slat light galleries, tiered stands with the crowd, the overhead rig '
     'with spotlights, the screen block and the sky dome.', 'make_arena.py: build_environment'),
    ('A07', 'env_fx', 'Crowd lights', 'Twinkling phone and light-stick glows in the crowd (texture T07, scrolled by the '
     'game).', 'make_arena.py: seat_row (twinkle cards)'),
    ('A08', 'stadium_fx', 'Stadium glow', "The stadium's light beams and halos (hidden in PASSTHROUGH).",
     'tools/build_open_assets.py'),
    ('A09', 'floor_reflect_env', 'Stadium reflection', 'The stadium mirrored below the floor.',
     'tools/build_open_assets.py: mirrored()'),
    ('A10', 'court_outline', 'Court outline', "PASSTHROUGH > OUTLINE: only the court's neon edges and floor lines, drawn "
     'over your room.', 'tools/build_open_assets.py'),
]
PROPS = [
    ('P01', ['projectile'], 'Plasma ball', 'The energy sphere (additive, swirling veins T08). The game adds the core, '
     'trails and electric arcs (C03).', 'tools/blender/make_props.py: ball'),
    ('P02', ['projectile_shell'], 'Filament orbits', 'Three hair-thin glowing orbits with sparks that spin with the ball '
     'and show its curve.', 'make_props.py: shell'),
    ('P03', ['spawner'], 'Spawn pad', 'The floor pad in front of each player, glowing in the team colour; the ball rises '
     'from it.', 'make_props.py: spawner'),
    ('P04', ['lancer'], 'Controller gauntlet', 'GAUNTLET = CONTROLLER: a VR controller held in an armoured glove; the '
     'glass knuckle guard floats in front of it.', 'make_props.py: lancer'),
    ('P05', ['knuckleguard'], 'Knuckle guard', 'The clear glass bumper in front of the fist (it matches the punch '
     'collision box). Drawn translucent.', 'make_props.py: knuckleguard'),
    ('P06', ['shield'], 'Hex shield', 'The energy shield that forms while you hold a charged ball (hex texture T04, '
     'bright rim).', 'make_props.py: shield'),
    ('P07', ['strikezone'], 'Strike ring', 'The holographic ring behind each player; a ball through it is a strike '
     '(texture T03). The game draws it at the room\'s STRIKE ZONE size.', 'make_props.py: strikezone'),
    ('P08', ['head'], 'Old android head', 'The first placeholder head, used only if the avatar parts are missing.',
     'make_props.py: head'),
    ('P09', ['torso'], 'Old android torso', 'The first placeholder torso, used only if the avatar parts are missing.',
     'make_props.py: torso'),
    ('P10', ['gauntlet_0_hand', 'gauntlet_0_cuff'], 'Striker gauntlet', 'GAUNTLET = STRIKER (the default): gothic plate. '
     'Overlapping scales on every finger with pointed tips, a knuckle ridge with gadlings, a fluted back plate, a thumb '
     'wing and a pointed bell cuff. Drawn as the fist and the cuff (gauntlet_0 is the whole piece).',
     'make_props.py: gauntlet(0)'),
    ('P11', ['gauntlet_1_hand', 'gauntlet_1_cuff'], 'Titan gauntlet', 'GAUNTLET = TITAN: heavy plate. Thick finger '
     'scales, a spiked knuckle ridge, a ridged and riveted back plate and a wide bell cuff.', 'make_props.py: gauntlet(1)'),
    ('P12', ['gauntlet_2_hand', 'gauntlet_2_cuff'], 'Phantom gauntlet', 'GAUNTLET = PHANTOM: light plate over a dark '
     'glove. Thin finger scales, a smooth knuckle ridge and back plate with glowing inlays, a slim cuff.',
     'make_props.py: gauntlet(2)'),
]
TEXTURES = [
    ('T01', 'open_soft_strip', 'Soft strip', 'The soft band used for every halo and glow line.'),
    ('T02', 'open_soft_dot', 'Soft dot', 'Round glow sprite.'),
    ('T03', 'open_holo_ring', 'Holo ring', 'Back-wall targets and the strike ring.'),
    ('T04', 'open_hex', 'Hex grid', 'End-zone energy and the shield.'),
    ('T05', 'open_slats', 'Light slats', 'The stadium light galleries.'),
    ('T06', 'open_sky', 'Night sky', 'Starfield and violet nebula on the sky dome.'),
    ('T07', 'open_twinkle', 'Twinkle strip', 'Crowd light brightness over time.'),
    ('T08', 'open_veins', 'Plasma veins', 'Energy veins on the ball.'),
    ('T09', 'open_emblem', 'Project Arena emblem', 'The centre-court logo and the screens.'),
    ('T10', 'open_screen', 'Screen face', 'The hanging screen block.'),
    ('T11', 'open_font', 'Stroke font', 'Every letter on the scoreboard, the menus and the hub.'),
    ('T12', 'open_pulse', 'Energy pulse', 'Pulses racing along the neon tubes.'),
    ('T13', 'open_court_lm', 'Court lightmap', 'Baked light on the court and floor (from the neon, galleries and rig).'),
    ('T14', 'open_env_lm', 'Stadium lightmap', 'Baked light on the stands, dais and rig.'),
    ('T15', 'open_fabric', 'Fabric knit', 'The knit every garment shows up close (repeated across the cloth).'),
]
CODE_FX = [
    ('C01', 'Hit plasma burst', 'Flash, shock ring, swirling plasma puffs, curling wisps and lingering sparks in the '
     "scorer's colour; a colour wash for the player who was hit.", 'app/src/main.cpp: drawHitFx'),
    ('C02', 'Spawn glow and rise', 'Glow gathers on the pad, then the ball floats up with twisting tracers.',
     'main.cpp: drawSpawnFx'),
    ('C03', 'Electric arcs', 'Jagged arcs crackling over the ball, re-rolled 18 times a second.', 'main.cpp: drawWorld'),
    ('C04', 'Ball trails', 'A soft-edged trail plus two helix streaks that show the spin.', 'main.cpp: trail ribbons'),
    ('C05', 'Ball light pool and reflection', 'Coloured light on the floor under the ball, and its reflection in the '
     'floor.', 'main.cpp: drawWorld (floor)'),
    ('C06', 'Holographic scoreboard', 'The translucent board over centre court with team-coloured scores, clock, strikes '
     'and charge.', 'main.cpp: HUD'),
    ('C07', 'Menus', 'Floating panels with the laser pointer: play, rooms and their options, settings.',
     'main.cpp: drawMenu'),
    ('C08', 'Spectator hub', 'Live miniature arenas on pedestals; point and pull the trigger to watch one.',
     'main.cpp: drawHub'),
    ('C09', 'Avatar builder', 'A mirror that copies your movements, with a pointer panel beside it for every choice in '
     'the AVATAR section here.', 'main.cpp: drawBuilder'),
    ('C10', 'Avatar rig and rim light', 'The torso hung below the neck, two-bone arms reaching your real hands, the '
     'team-colour rim light and the waist fade. This viewer poses avatars with the same rig.',
     'app/src/avatar.cpp: solveRig; render.cpp'),
    ('C11', "The fans' flags", "With The Crowd cheering from their phones, a flag on a pole waves in an end's stands for "
     "each phone (up to 40), faster as the hype rises, and cheer bursts throw hearts, stars and little flags up out of the "
     "stands: the end's country flag (the F IDs), or with no country a foam finger in the end's colour.",
     'main.cpp: drawFanFlags, drawFlag'),
]
# tools/gen_catalog.py SOUNDS (the codes the SOUND STUDIO and tools/add_sound.py use): the open set's sounds
# (app/assets_open/audio); a fifth item names the licensed sound the game plays there instead (app/assets_music,
# NOTICE.txt: not in the public repository, so not in the viewer either)
SOUNDS = [
    ('S01', 'Spawn', 'Ball appears', 'A small soft bloom with a faint glint as a new ball appears on its launcher.'),
    ('S02', 'SpawnRise', 'Launcher rises', 'A gentle airy swell as the launcher lifts out of the floor.'),
    ('S03', 'Grab', 'Grab', 'A muted thump and a brief warm hum when you take the ball.'),
    ('S04', 'Caught', 'Catch', 'A padded catch with a quick soft rise.'),
    ('S05', 'ThrowSlow', 'Throw (slow)', 'A soft low whoosh, the gentlest throw.'),
    ('S06', 'ThrowMed', 'Throw (medium)', 'A clean whoosh, a little brighter.'),
    ('S07', 'ThrowFast', 'Throw (fast)', 'A quicker, brighter whoosh with a faint streak.'),
    ('S08', 'ThrowFull', 'Throw (full power)', 'The fullest whoosh with a soft low push.'),
    ('S09', 'ThrowFail', 'Dropped throw', 'A soft falling tone and faint fizzle when a release is too slow.'),
    ('S10', 'Bounce', 'Wall bounce', 'A short soft tok off the glass.', 'a licensed rubber bounce'),
    ('S11', 'ShieldBlock', 'Shield block', 'A resonant soft thoom with a short glassy shimmer.'),
    ('S12', 'ShieldBreak', 'Shield break', 'The shield shattering: a community contribution, layered from three sound effects.'),
    ('S13', 'KnuckleBlock', 'Knuckle punch', 'A tight padded metallic knock.'),
    ('S14', 'HitPlayerOpponent', 'You scored', 'A soft deep thud, a gentle discharge and a small rising two-note chime.'),
    ('S15', 'HitPlayerSelf', 'You were hit', 'A muffled heavy thud with a low downward wobble.'),
    ('S16', 'Returned', 'Ball returns', 'An airy whoosh moving away as the ball flies home.'),
    ('S17', 'Foul', 'Foul', 'Two rounded falling notes.'),
    ('S18', 'Countdown', 'Countdown beep', 'One calm low beep for each count.'),
    ('S19', 'RoundStart', 'Round start', 'A short bright swell landing on a soft chord.'),
    ('S20', 'RoundEndWin', 'Round won', 'A small crowd cheering and clapping.', 'the licensed arena cheer'),
    ('S21', 'RoundEndLose', 'Round lost', 'A small crowd "ohh" with a few claps.', "a licensed crowd's disappointment"),
    ('S22', 'MatchEndWin', 'Match won', 'Crowd cheers and applause over a soft major chord.', 'the licensed arena cheer'),
    ('S23', 'MatchEndLose', 'Match lost', 'Warm applause over a gentle falling chord.', "a licensed crowd's disappointment"),
    ('S24', 'Recharge', 'Recharge', 'A short soft rise and a gentle ding when your ball recharges.'),
    ('S25', 'Strike', 'Strike', 'A clear two-note ding-ding over a soft swell.'),
    ('S26', 'Overtime', 'Sudden death', 'A voice calls SUDDEN DEATH.'),
    ('S27', 'Ambience', 'Arena ambience', 'A quiet 30-second loop: low hum, air and a distant crowd.'),
    ('S28', 'FistBump', 'Fist bump', 'A friendly metallic knock and a warm pulse when two players bump fists to start.'),
    ('S29', 'Crowd', 'Crowd', 'A short, quiet crowd blip each time the fans cheer on their phones (The Crowd).'),
    ('S30', 'Cheer', 'Crowd cheer', 'The crowd erupts: a rising cheer with applause.', 'the licensed arena cheer'),
    ('S31', 'ClockTick', 'Clock tick', 'A very soft wooden tick once a second in the last 10 seconds of a timed match.'),
    ('S32', 'LeadChange', 'Lead change', 'A bright rising three-note chime when someone new leads (FOUR CORNERS).'),
    ('S33', 'TargetHit', 'Target hit', 'A bright glassy ping when a TRAINING target is taken.'),
    ('S34', 'KnucklePunch', 'Hard punch', 'A hard crack when a knuckle guard punch sends the ball back at 12 m/s or more '
     '(new in 0.3.21; a SOUND STUDIO slot from 0.3.22).'),
]

# the avatar builder's parts, in its own order (app/src/avatar.cpp: kSuit, kLower, ...), each with a V ID
DESC = {
    'BODY TYPE': ['The masculine body: every suit, lower body and head part is fitted to it.',
                  'The feminine body: every suit, lower body and head part is fitted to it.'],
    'FACE': ['The base face.', 'A face of its own; face shields and masks are fitted to each face.',
             'A face of its own; face shields and masks are fitted to each face.',
             'A face of its own; face shields and masks are fitted to each face.'],
    'SUIT': ['Long-sleeve bodysuit with glowing circuit piping, a high collar and cuff rings.',
             'Loose crew-neck sports tee: short sleeves with trim bands, trim collar and side stripes. Also the team kit '
             'of every COUNTRY (COUNTRY KIT: TEAM JERSEY; MY OUTFIT keeps your own suit in its colours).',
             'Dark undersuit with raised armour: one smooth chest plate, back plate, pauldrons, vambraces and a glowing core.',
             'Padded long-sleeve jacket with a high collar, zipper line, trim cuffs and waistband.',
             'Thick hoodie with a kangaroo pocket, trim cuffs and waistband.',
             'Sleek long-sleeve racer suit: high collar, glowing V lines and an identity disc on the back.',
             'Tank top with a round neckline and wide straps, silver-plated forearm bracers.',
             'Flight suit with harness straps, a belt and a chest light.',
             'Sleeveless compression vest: mock collar, glowing chest Y, side and back lines, dark circuit bracers.',
             'Fitted training tee with raglan and side panels in the trim colour, grey-plated bracers.',
             'Body suit with modelled circuit traces, pulses running along them.',
             'The Grid Runner with its glow lines raised from the suit, pulsing.',
             'The Echo Suit with its glow lines raised from the suit, pulsing.',
             'The Circuit Vest with its glow lines raised from the suit, pulsing.',
             'Athletic kit made with a member of the community, with shorts of its own for the first four LOWER BODY '
             'choices.',
             'Armoured kit made with a member of the community, with shorts of its own for the first four LOWER BODY '
             'choices.',
             'Wrap jacket crossed to the left hip over a dark undershirt, a raised obi with a tied knot, three layered '
             'shoulder lames, long sleeves with glowing cuffs.',
             'A charro jacket made with a member of the community: white shirt front and bow tie, an embroidered '
             'bandolier, satin lapels and tails, flowers in the glow colour.',
             'An off-shoulder folk dress made with a member of the community: a flounced neckline, an embroidered bodice '
             'and a layered skirt that sways (the viewer shows it at rest), with hoop earrings that swing.'],
    'LOWER BODY': ['The body ends at the waist in an energy fade.',
                   'Athletic shorts with a split hem and white piping; the thighs fade above the knee.',
                   'Long circuit shorts with glowing lines down the legs and on the waistband.',
                   'Bike shorts with side panels in the trim colour.',
                   'Opaque long shorts that cover the thighs.',
                   'A tennis skirt over undershorts; it swings as you move (the viewer shows it at rest).',
                   'A kilt over undershorts; it swings as you move (the viewer shows it at rest).'],
    'GAUNTLET': ['Gothic plate: pointed finger scales, knuckle gadlings, a fluted back plate, a pointed bell cuff.',
                 'Heavy plate: thick scales, a spiked knuckle ridge, a riveted back plate, a wide bell cuff.',
                 'Light plate over a dark glove with glowing inlays and a slim cuff.',
                 'The classic look: the VR controller in an armoured glove.',
                 'Heavy plating with hooked talons and hydraulic pistons; in the game its fingers open on the trigger '
                 'and a plasma tether runs to the ball.',
                 'Slim plate with glowing joint nodes and lines; in the game its fingers open on the trigger.',
                 'Gold plates with black filigree and a faceted jewel in the glow colour; in the game its fingers open on '
                 'the trigger.'],
    'BUILD': ['Slimmer shoulders and arms.', 'The base proportions.', 'Broader shoulders and chest, thicker arms.',
              'Slightly broader, stronger arms.'],
    'PATTERN': ['', 'Circuit traces with pads, in the glow colour.', 'Twin racing stripes front and back, pinstripes on the sides.',
                'Chevrons across the chest.', 'Honeycomb lines with trim-colour cells.', 'Left half in the trim colour.',
                'Digital camo blocks in the trim colour.', 'Horizontal bands in the trim colour.'],
    'HAIR': ['', 'Close-cropped.', 'Volume on top, swept back in layers.',
             'Combed back to a high tie, a full wavy tail and face-framing strands.', 'Shaved sides with a crest from brow to nape.',
             'Short spikes swept back.', 'Chin-length bob with a fringe.', 'Shaved with a bun on top.',
             'Short and layered, swept up and to the side, ends flicking out.',
             'Two high ties, each tail springing up and out before it falls; they swing as you move.',
             'Locs in two staggered rows with beads and cuffs, swept back behind the ears; they swing as you move.'],
    'HEADGEAR': ['', 'Full helmet with a glowing face rim, midline and side lines.',
                 'Rounded racing helmet with twin stripes and a rear spoiler.', 'Soft fabric hood with a trim edge.',
                 'Helmet with a crest fin and cheek guards.', 'Over-ear headphones with glowing rings (hair stays visible).',
                 'Cap with a curved brim.', 'A horned helmet, made with a member of the community.',
                 "The Vector kit's helmet, made with a member of the community.",
                 'An open-faced samurai-style helmet: a ribbed bowl, a laced neck guard, cheek wings and a glowing '
                 'crescent crest.',
                 'A wide-brimmed sombrero with embroidered bands and a flower cluster, made with a member of the '
                 'community.'],
    'FACE SHIELD': ['', 'Wraparound glass band over the eyes, glowing edges.', 'Smooth glass dome over the whole face.',
                    'Sunglasses.', 'Round goggles on a strap.', 'A VR headset on a strap.',
                    'A glowing ring orbiting the head, with a slim eye line.', 'Opaque plate with a glowing eye slit.'],
    'MASK': ['', 'Plate over the mouth and chin with vents.', 'Armoured jaw from ear to ear with glowing vents.',
             'Respirator: snout, side filters and straps.',
             'Cloth over nose and mouth, knotted at the back; its point and its tails swing.',
             'Smooth full-face plate with glowing eye slits.',
             'A sugar-skull mask fitted to each face, made with a member of the community: eye holes, dotted rings and '
             'flowers in the trim and glow colours.'],
}
# field -> (look key, models for body b / face f, where the view looks, the part shown on its own look)
PARTS = [
    ('BODY TYPE', 'body', lambda i: ['av_bhead_%s' % 'mf'[i], 'av_top_6_%s' % 'mf'[i]], 'full', {'suit': 6, 'lower': 1}),
    ('FACE', 'face', lambda i: ['av_bhead_m%s' % (i or ''), 'av_bhead_f%s' % (i or '')], 'head', {}),
    ('SUIT', 'suit', lambda i: ['av_top_%d_m' % i, 'av_top_%d_f' % i] + (['av_top_18e_m', 'av_top_18e_f'] if i == 18 else []),
     'full', {}),
    ('LOWER BODY', 'lower', lambda i: ['av_bottom_%d_m' % i, 'av_bottom_%d_f' % i] if i else ['av_wfade_m', 'av_wfade_f'], 'full', {}),
    ('GAUNTLET', 'gauntlet', lambda i: ['lancer'] if i == 3 else ['gauntlet_%d_hand' % i, 'gauntlet_%d_cuff' % i] +
     (['gauntlet_%d_fist' % i] + ['gauntlet_%d_f%d' % (i, k) for k in range(5)] if i >= 4 else []), 'hands', {}),
    ('BUILD', 'build', lambda i: ['av_top_0_m (sized by the rig)'], 'full', {}),
    ('PATTERN', 'pattern', lambda i: ['tex/open_pat_%s_m' % ['', 'circuit', 'stripes', 'chevron', 'hex', 'split', 'pixel', 'bands'][i],
                                      'tex/open_pat_%s_f' % ['', 'circuit', 'stripes', 'chevron', 'hex', 'split', 'pixel', 'bands'][i]],
     'full', {'suit': 5}),
    ('HAIR', 'hair', lambda i: ['av_hair_%d_m' % i, 'av_hair_%d_f' % i] + (['av_hair_%dt_m' % i, 'av_hair_%dt_f' % i] if i in (3, 7, 9) else []),
     'head', {'headgear': 0}),
    ('HEADGEAR', 'headgear', lambda i: ['av_gear_%d_m' % i, 'av_gear_%d_f' % i], 'head', {}),
    ('FACE SHIELD', 'visor', lambda i: ['av_visor_%d_m' % i, 'av_visor_%d_f' % i], 'head', {}),
    ('MASK', 'mask', lambda i: ['av_mask_%d_m' % i, 'av_mask_%d_f' % i] + (['av_mask_4t_m', 'av_mask_4t_f'] if i == 4 else []),
     'head', {}),
]


def pose_tool(look, pose, team=0):
    out = subprocess.run([POSE_EXE, look, pose, str(team)], capture_output=True, text=True, check=True).stdout
    return json.loads(out)


def mat_rows(m):
    """a column-major Mat4 (16 floats) -> a numpy 4x4"""
    return np.array(m, float).reshape(4, 4).T


def col_major(M):
    return [round(float(x), 6) for x in np.asarray(M).T.reshape(16)]


def rig_data():
    """for each body (0, 1), build (0-3) and pose: the skin matrices, the waist, the forearm twist, the head's matrix,
    the bones, and each hand's gauntlet (fist and cuff as main.cpp drawGauntlet places them, the classic controller
    and the knuckle guard as tools/preview_avatar.py does)"""
    rig = {}
    for body in (0, 1):
        for build in range(4):
            for pose in POSES:
                d = pose_tool('body=%d,build=%d,suit=0,lower=0,hair=0,headgear=0,visor=1,mask=0,gauntlet=0' % (body, build), pose)
                skinned = [p for p in d['parts'] if 'skin' in p]
                head = [p for p in d['parts'] if p['model'].startswith('av_visor_')]
                assert skinned and head, 'the pose tool gave no skinned part or no head part'
                hands = []
                for h in d['hands']:
                    G = mat_rows(h['grip'])
                    mir = np.diag([-1.0, 1, 1, 1]) if h['h'] == 0 else np.eye(4)
                    hands.append({'fist': col_major(mat_rows(h['fist']) @ frames.LANCER),
                                  'cuff': col_major(mat_rows(h['cuff']) @ frames.LANCER),
                                  'lancer': col_major(G @ mir @ frames.LANCER),
                                  'knuckle': col_major(G @ mir @ KNUCKLE_SHIFT @ frames.KNUCKLE)})
                e = {'skin': skinned[0]['skin'], 'head': head[0]['m'], 'bones': d['bones'], 'hands': hands}
                if 'pelvis' in d:
                    e['pelvis'] = d['pelvis']
                if 'twist' in d:
                    e['twist'] = d['twist']
                rig['%d%d_%s' % (body, build, pose)] = e
    return rig


def gauntlet_local():
    """the plate gauntlet's fist and cuff against the grip (the right hand, as main.cpp drawGauntlet places them), for
    the props on their own"""
    d = pose_tool('body=0,build=1,gauntlet=0', 'ready')
    h = d['hands'][1]
    G = np.linalg.inv(mat_rows(h['grip']))
    return {'fist': col_major(G @ mat_rows(h['fist']) @ frames.LANCER), 'cuff': col_major(G @ mat_rows(h['cuff']) @ frames.LANCER)}


def presets(opt):
    """each builder preset as {look key: option}, from the pose tool's description of it"""
    keys = {'SUIT': 'suit', 'BUILD': 'build', 'PATTERN': 'pattern', 'MAIN COLOR': 'primary', 'TRIM COLOR': 'secondary',
            'GLOW': 'glow', 'HAIR': 'hair', 'HAIR COLOR': 'haircolor', 'HEADGEAR': 'headgear', 'FACE SHIELD': 'visor',
            'SHIELD TINT': 'visortint', 'MASK': 'mask', 'SKIN': 'skin', 'LOWER BODY': 'lower', 'BODY TYPE': 'body',
            'GAUNTLET': 'gauntlet', 'FACE': 'face'}
    names = {f['name']: [o['name'] for o in f['options']] for f in opt['fields']}
    out = []
    for i, name in enumerate(opt['presets']):
        d = pose_tool(str(i), 'ready')
        look = {}
        for kv in d['desc'].split(' | '):
            f, v = kv.split(': ', 1)
            if f in keys:
                look[keys[f]] = names[f].index(v)
        out.append({'name': name, 'look': look})
        if i >= PUBLISHED_PRESETS and UNRELEASED:
            out[-1]['since'] = UNRELEASED
    return out


def nations():
    """the country table (app/src/nations.inc, tools/gen_flags.py; append-only: a country's place + 1 is its number):
    F001-F205 in that order, each with its code, name, flag texture, team kit (sRGB: jersey, trim, accent) and its end's
    colours (linear: first and second choice)"""
    out = []
    for line in open(os.path.join(ROOT, 'app', 'src', 'nations.inc'), encoding='utf-8'):
        m = re.match(r'\s*\{"([A-Z]+)", "([^"]+)", "(flag_[a-z]+)", (.*)$', line)
        if not m:
            continue
        ints = [int(x) for x in re.findall(r'(?<![.\d])\d+(?![.\d])', m.group(4))[:9]]
        floats = [float(x) for x in re.findall(r'-?\d+\.\d+', m.group(4))[:6]]
        assert len(ints) == 9 and len(floats) == 6, line
        n = len(out) + 1
        out.append({'id': 'F%03d' % n, 'n': n, 'code': m.group(1), 'name': m.group(2), 'flag': m.group(3),
                    'kit': [ints[0:3], ints[3:6], ints[6:9]], 'team': [floats[0:3], floats[3:6]]})
    return out


# each field's options as first published (2026-11-18, V01-V72); options added since take the IDs after V72, in the
# order of PARTS, so a published ID never changes (people refer to parts by them)
PUBLISHED = {'BODY TYPE': 2, 'FACE': 4, 'SUIT': 16, 'LOWER BODY': 7, 'GAUNTLET': 4, 'BUILD': 4, 'PATTERN': 8, 'HAIR': 9,
             'HEADGEAR': 9, 'FACE SHIELD': 8, 'MASK': 6}
PUBLISHED_PRESETS = 14
# what the release doesn't have yet says "from" this version in the viewer (as COUNTRY KIT does); None once it's out
UNRELEASED = '0.3.22'


def avatar_ids(opt):
    names = {f['name']: [o['name'] for o in f['options']] for f in opt['fields']}
    out, n = [], 0
    for later in (False, True):
        for field, key, models, view, base in PARTS:
            for i, oname in enumerate(names[field]):
                if field in ('PATTERN', 'HAIR', 'HEADGEAR', 'FACE SHIELD', 'MASK') and i == 0:
                    continue   # (NONE)
                if (i >= PUBLISHED[field]) != later:
                    continue
                n += 1
                look = dict(base)
                look[key] = i
                out.append({'id': 'V%02d' % n, 'field': field, 'key': key, 'value': i, 'name': oname.title(),
                            'desc': DESC[field][i], 'models': models(i), 'view': view, 'look': look})
                if later and UNRELEASED:
                    out[-1]['since'] = UNRELEASED
    order = {p[0]: k for k, p in enumerate(PARTS)}
    out.sort(key=lambda a: (order[a['field']], a['value']))   # (listed as the builder has them)
    return out


def model_info(name):
    b = open(os.path.join(ASSETS, name + '.mdl'), 'rb').read()
    ver, nv, ni, ns, nm = struct.unpack_from('<5I', b, 4)
    return {'v': nv, 't': ni // 3, 'kb': round(len(b) / 1024)}


def textures_of(name):
    """the diffuse and mask textures a model's materials name (as the game loads them: '/' -> '_')"""
    b = open(os.path.join(ASSETS, name + '.mdl'), 'rb').read()
    ver, nv, ni, ns, nm = struct.unpack_from('<5I', b, 4)
    off = 24 + nv * (52 if ver >= 3 else 44) + ni * 4 + ns * 12
    out = set()
    for _ in range(nm):
        off += 44
        for _k in range(2):
            t = b[off:off + 64].split(b'\0')[0].decode()
            off += 64
            if t:
                out.add(t.replace('/', '_'))
    return out


def write_png(tex, dst):
    b = open(os.path.join(ASSETS, 'tex', tex + '.tex'), 'rb').read()
    assert b[:4] == b'STEX', tex
    w, h = struct.unpack_from('<2I', b, 4)
    px = np.frombuffer(b[16:16 + w * h * 4], np.uint8).reshape(h, w, 4)
    img = Image.fromarray(px, 'RGBA')
    if px[:, :, 3].min() == 255:
        img = img.convert('RGB')   # (opaque: smaller)
    img.save(dst, optimize=False, compress_level=9)
    return w, h


def app_version():
    m = re.search(r'kAppVersion = "([0-9.]+)"', open(os.path.join(ROOT, 'app', 'src', 'main.cpp'), encoding='utf-8').read())
    return m.group(1) if m else '?'


DL_README = """{id} {name}: {desc}

Project Arena {version}, from the asset viewer: {url}#{id}
{files}
The models are .glb (Blender: File > Import > glTF 2.0). Skinned parts carry the game's 9-bone avatar rig in its bind
pose; head parts are in head space (the origin between the eyes); gauntlets in the hand frame. Keep the material
names: they tell the game how to colour and light each part, and when you export from Blender (File > Export >
glTF 2.0, glTF Binary) tick Include > Custom Properties: the materials carry the game's exact settings there.

To make your own, start from the template pack (dl/project-arena-templates.zip on the viewer's page): its README says
what goes where. Send your file with the ID it replaces or adds to: a pull request (contrib/) or the Discord.

Art and sound: CC BY 4.0 (LICENSE-ASSETS.md in https://github.com/jendoebelin/ProjectArena-public).
"""
VIEWER_URL = 'https://jendoebelin.github.io/ProjectArena-public/'


def downloads(out, data, tex_use):
    """dl/: a zip for each item with models (its .glb files, the textures they use, a README); a texture or sound: the
    file the page already has, under a name with its ID; the template pack. Returns {ID: {f, name, what, kb}}."""
    sys.path.insert(0, os.path.join(ROOT, 'tools', 'blender'))
    import export_glb
    import make_templates
    dl = os.path.join(out, 'dl')
    if os.path.isdir(dl):
        shutil.rmtree(dl)
    os.makedirs(dl)
    version = app_version()
    res = {}

    def zipped(id_, name, desc, models, extra_tex=()):
        models = [m.split()[0] for m in models]
        models = [m for i, m in enumerate(models) if m not in models[:i] and os.path.exists(os.path.join(ASSETS, m + '.mdl'))]
        texs = sorted({t for m in models for t in tex_use.get(m, [])} | set(extra_tex))
        texs = [t for t in texs if os.path.exists(os.path.join(out, 'tex', t + '.png'))]
        if not models and not texs:
            return
        rel = 'dl/%s.zip' % id_
        lines = ['', 'In this file:'] + ['  %s.glb' % m for m in models] + ['  %s.png' % t for t in texs] + ['']
        with zipfile.ZipFile(os.path.join(out, rel), 'w', zipfile.ZIP_DEFLATED) as z:
            for m in models:
                tmp = os.path.join(dl, '_tmp.glb')
                export_glb.export(m, tmp)
                z.write(tmp, '%s/%s.glb' % (id_, m))
                os.remove(tmp)
            for t in texs:
                z.write(os.path.join(out, 'tex', t + '.png'), '%s/%s.png' % (id_, t))
            z.writestr('%s/README.txt' % id_, DL_README.format(id=id_, name=name, desc=desc, version=version,
                                                               url=VIEWER_URL, files='\n'.join(lines)))
        what = ', '.join(x for x in ('%d model%s' % (len(models), '' if len(models) == 1 else 's') if models else '',
                                     '%d texture%s' % (len(texs), '' if len(texs) == 1 else 's') if texs else '') if x)
        res[id_] = {'f': rel, 'name': '%s.zip' % id_, 'what': '.glb for Blender: ' + what if models else what,
                    'kb': max(1, round(os.path.getsize(os.path.join(out, rel)) / 1024))}

    for a in data['avatar']:
        extra = ()
        if a['field'] == 'PATTERN':   # (a pattern is its two textures, one a body)
            extra = [t for t in data['textures'] if t.startswith('open_pat_%s_' % a['name'].lower())]
        if a['field'] == 'BUILD':
            continue   # (a build sizes the body: nothing of its own; the template pack has the bodies)
        zipped(a['id'], a['name'], a['desc'], a['models'], extra)
    for x in data['props']:
        zipped(x['id'], x['name'], x['desc'], x['models'])
    for x in data['arena']:
        zipped(x['id'], x['name'], x['desc'], [x['model']])
    for x in data['tex']:
        if x['file'] in data['textures']:
            res[x['id']] = {'f': 'tex/%s.png' % x['file'], 'name': '%s_%s.png' % (x['id'], x['file']), 'what': 'PNG',
                            'kb': max(1, round(os.path.getsize(os.path.join(out, 'tex', x['file'] + '.png')) / 1024))}
    for t in data['textures']:   # (every other texture: by its file name)
        res.setdefault('tex:' + t, {'f': 'tex/%s.png' % t, 'name': t + '.png', 'what': 'PNG',
                                    'kb': max(1, round(os.path.getsize(os.path.join(out, 'tex', t + '.png')) / 1024))})
    for x in data['nations']:   # (a country's flag: its PNG, 192 x 128)
        res[x['id']] = {'f': 'tex/%s.png' % x['flag'], 'name': '%s_%s.png' % (x['id'], x['flag']), 'what': 'PNG, 192 x 128',
                        'kb': max(1, round(os.path.getsize(os.path.join(out, 'tex', x['flag'] + '.png')) / 1024))}
    for x in data['sounds']:
        res[x['id']] = {'f': 'snd/' + x['file'], 'name': '%s_%s' % (x['id'], x['file']), 'what': 'WAV, 48 kHz',
                        'kb': max(1, round(os.path.getsize(os.path.join(out, 'snd', x['file'])) / 1024))}
    # the template pack, made fresh from the game's bodies
    src = os.path.join(ROOT, 'build', 'templates')
    argv, sys.argv = sys.argv, ['make_templates.py', src]
    try:
        make_templates.main()
    finally:
        sys.argv = argv
    rel = 'dl/project-arena-templates.zip'
    with zipfile.ZipFile(os.path.join(out, rel), 'w', zipfile.ZIP_DEFLATED) as z:
        for base, _, files in os.walk(src):
            for f in sorted(files):
                q = os.path.join(base, f)
                z.write(q, os.path.join('project-arena-templates', os.path.relpath(q, src)))
    res['templates'] = {'f': rel, 'name': 'project-arena-templates.zip', 'what': 'bodies, heads, the glove, examples',
                        'kb': max(1, round(os.path.getsize(os.path.join(out, rel)) / 1024)), 'version': version}
    return res


def main():
    args = [a for a in sys.argv[1:] if not a.startswith('--')]
    text_models = '--text-models' in sys.argv
    out = os.path.abspath(args[0] if args else os.path.join(ROOT, 'build', 'viewer'))
    if not os.path.exists(POSE_EXE):
        raise SystemExit('build/tests/avatar_pose.exe is missing: run tools/build_avatar_pose.bat first')
    for sub in ('models', 'tex', 'snd'):
        os.makedirs(os.path.join(out, sub), exist_ok=True)
    opt = json.loads(subprocess.run([POSE_EXE, '--options'], capture_output=True, text=True, check=True).stdout)

    # every model, gzipped as it is (the page reads the game's own format). --text-models: base64 text, a few models to a
    # bundle (models/bundle_K.json, at most ~9 MB each: the Claude artifact holds at most 512 files)
    names = sorted(f[:-4] for f in os.listdir(ASSETS) if f.endswith('.mdl'))
    models, texs, tex_use = {}, set(), {}
    bundles, bundle_of = [{}], {}
    for f in os.listdir(os.path.join(out, 'models')):   # (an earlier build's files go)
        os.remove(os.path.join(out, 'models', f))
    for n in names:
        models[n] = model_info(n)
        used = textures_of(n)
        texs |= used
        if used:
            tex_use[n] = sorted(used)
        raw = open(os.path.join(ASSETS, n + '.mdl'), 'rb').read()
        packed = gzip.compress(raw, 9, mtime=0)
        if text_models:
            if sum(len(v) for v in bundles[-1].values()) > 9_000_000:
                bundles.append({})
            bundles[-1][n] = base64.b64encode(packed).decode()
            bundle_of[n] = len(bundles) - 1
        else:
            with open(os.path.join(out, 'models', n + '.mdl.gz'), 'wb') as f:
                f.write(packed)
    if text_models:
        for k, b in enumerate(bundles):
            with open(os.path.join(out, 'models', 'bundle_%d.json' % k), 'w', encoding='ascii') as f:
                json.dump(b, f, separators=(',', ':'))
    pats = ['open_pat_%s_%s' % (p, b) for p in ('circuit', 'stripes', 'chevron', 'hex', 'split', 'pixel', 'bands') for b in 'mf']
    countries = nations()
    texs |= set(pats) | {t for _, t, _, _ in TEXTURES} | {c['flag'] for c in countries}
    textures = {}
    for t in sorted(texs):
        if not os.path.exists(os.path.join(ASSETS, 'tex', t + '.tex')):
            print('  (no texture %s)' % t)
            continue
        dst = os.path.join(out, 'tex', t + '.png')
        src = os.path.join(ASSETS, 'tex', t + '.tex')
        if os.path.exists(dst) and os.path.getmtime(dst) >= os.path.getmtime(src):
            with Image.open(dst) as im:
                textures[t] = im.size
            continue
        textures[t] = write_png(t, dst)
    sounds = []
    for sid, ev, name, desc, *lic in SOUNDS:
        src = os.path.join(ASSETS, 'audio', 'oa_%s.wav' % ev)
        if not os.path.exists(src):
            print('  (no sound %s)' % ev)
            continue
        shutil.copyfile(src, os.path.join(out, 'snd', 'oa_%s.wav' % ev))
        sounds.append({'id': sid, 'file': 'oa_%s.wav' % ev, 'name': name, 'desc': desc, 'event': ev,
                       'licensed': lic[0] if lic else None})

    # hair kept inside headgear and face shields: the tuck maps ('TUCK', u32 w, h, f32 centre[3] in head space, u32, w x h
    # bytes; main.cpp loadTuck)
    tuck = {}
    for f in sorted(os.listdir(ASSETS)):
        if f.endswith('.tuck'):
            b = open(os.path.join(ASSETS, f), 'rb').read()
            w, h = struct.unpack_from('<2I', b, 4)
            tuck[f[:-5]] = {'w': w, 'h': h, 'c': [round(x, 6) for x in struct.unpack_from('<3f', b, 12)],
                            'v': base64.b64encode(b[24:24 + w * h]).decode()}
    rig = rig_data()
    data = {
        'models': models,
        'modelExt': '.mdl.gz',
        'modelBundle': bundle_of if text_models else None,   # (--text-models: each model's bundle)
        'texUse': tex_use,
        'tuck': tuck,
        'textures': {k: list(v) for k, v in textures.items()},
        'fields': opt['fields'],
        'presets': presets(opt),
        'avatar': avatar_ids(opt),
        'arena': [{'id': i, 'model': m, 'name': n, 'desc': d, 'made': mk} for i, m, n, d, mk in ARENA],
        'props': [{'id': i, 'models': m, 'name': n, 'desc': d, 'made': mk} for i, m, n, d, mk in PROPS],
        'tex': [{'id': i, 'file': f, 'name': n, 'desc': d} for i, f, n, d in TEXTURES],
        'sounds': sounds,
        'code': [{'id': i, 'name': n, 'desc': d, 'where': w} for i, n, d, w in CODE_FX],
        'teams': TEAMS,
        'nations': countries,
        'rig': rig,
        'gauntletLocal': gauntlet_local(),
    }
    # the downloads (not where the page can't offer them: the Claude artifact's frame refuses downloads)
    data['dl'] = None if text_models or '--no-downloads' in sys.argv else downloads(out, data, tex_use)
    data['dlHome'] = VIEWER_URL
    with open(os.path.join(out, 'data.json'), 'w', encoding='utf-8', newline='\n') as f:
        json.dump(data, f, ensure_ascii=False, separators=(',', ':'))
    # the page: tools/viewer/index.html with tools/viewer/viewer.js put inside it (one file to publish)
    page = open(os.path.join(PAGE, 'index.html'), encoding='utf-8').read()
    js = open(os.path.join(PAGE, 'viewer.js'), encoding='utf-8').read()
    assert '/*VIEWER_JS*/' in page
    page = page.replace('/*VIEWER_JS*/', js)
    with open(os.path.join(out, 'index.html'), 'w', encoding='utf-8', newline='\n') as f:
        f.write(page)
    total = sum(os.path.getsize(os.path.join(dp, n)) for dp, _, ns in os.walk(out) for n in ns)
    count = sum(len(ns) for _, _, ns in os.walk(out))
    print('asset viewer: %d models, %d textures, %d sounds, %d avatar parts; %d files, %.1f MB -> %s'
          % (len(models), len(textures), len(sounds), len(data['avatar']), count, total / 1e6, out))


if __name__ == '__main__':
    main()
