"""Project Arena material palette, shared by the Blender generators (bake setup) and tools/build_open_assets.py
(app shaders). Plain Python so it also imports outside Blender.

kind:
  'lit'   diffuse surface, lighting baked by Cycles into the model's lightmap; app SH_LIGHTMAP
          albedo, f0 = specular colour (metal ~ its tint, dielectric ~0.04), gloss = reflection strength
  'emit'  light source: emission in the bake (strength), app SH_UNLIT showing `show` (display colour 0..1)
          tex = optional emission texture (app: textured UNLIT on uv0)
  'glass' transparent pane: transparent in the bake, app SH_TRANSLUCENT (rgb, alpha)
  'fx'    additive glow layer (halos, holograms, beams): hidden from the bake, app SH_ADDITIVE (show, tex)
  'prop'  props that are not lightmapped (ball, gauntlet, avatar...): app SH_LIT / SH_UNLIT / SH_ADDITIVE via `shader`
"""
T0 = (0.10, 0.70, 1.00)   # team 0 (blue end, -X)
T1 = (1.00, 0.40, 0.10)   # team 1 (orange end, +X)


def _s(c, k):
    return tuple(min(1.0, x * k) for x in c)


P = {
    # ---------------------------------------------------------------- court surfaces
    'floor_tile':     dict(kind='lit', albedo=(0.055, 0.06, 0.07), f0=(0.04, 0.04, 0.04), gloss=0.10),
    'floor_runway':   dict(kind='lit', albedo=(0.13, 0.14, 0.155), f0=(0.04, 0.04, 0.04), gloss=0.10),
    'floor_end_t0':   dict(kind='lit', albedo=(0.07, 0.09, 0.12), f0=(0.04, 0.04, 0.04), gloss=0.10),
    'floor_end_t1':   dict(kind='lit', albedo=(0.12, 0.085, 0.07), f0=(0.04, 0.04, 0.04), gloss=0.10),
    'floor_seam':     dict(kind='lit', albedo=(0.02, 0.022, 0.026), f0=(0.04, 0.04, 0.04), gloss=0.2),
    'frame':          dict(kind='lit', albedo=(0.30, 0.33, 0.37), f0=(0.40, 0.45, 0.52), gloss=0.5),
    'frame_dark':     dict(kind='lit', albedo=(0.07, 0.075, 0.085), f0=(0.25, 0.27, 0.30), gloss=0.7),
    'wall_end_t0':    dict(kind='lit', albedo=(0.05, 0.065, 0.085), f0=(0.06, 0.06, 0.06), gloss=0.25),
    'wall_end_t1':    dict(kind='lit', albedo=(0.085, 0.06, 0.05), f0=(0.06, 0.06, 0.06), gloss=0.25),
    'backwall_t0':    dict(kind='lit', albedo=(0.020, 0.026, 0.040), f0=(0.04, 0.04, 0.04), gloss=0.4),
    'backwall_t1':    dict(kind='lit', albedo=(0.040, 0.026, 0.020), f0=(0.04, 0.04, 0.04), gloss=0.4),
    'ceiling':        dict(kind='lit', albedo=(0.20, 0.21, 0.23), f0=(0.05, 0.05, 0.05), gloss=0.2),
    'glass':          dict(kind='glass', rgb=(0.02, 0.035, 0.05), alpha=0.10),
    'glass_t0':       dict(kind='glass', rgb=(0.0, 0.03, 0.06), alpha=0.16),
    'glass_t1':       dict(kind='glass', rgb=(0.06, 0.025, 0.0), alpha=0.16),
    'glass_back_t0':  dict(kind='glass', rgb=(0.0, 0.02, 0.05), alpha=0.30),
    'glass_back_t1':  dict(kind='glass', rgb=(0.05, 0.018, 0.0), alpha=0.30),
    # neon outline of the arena (light sources in the bake)
    'neon_white':     dict(kind='emit', color=(0.55, 0.85, 1.0), strength=14.0, show=(0.80, 0.95, 1.0)),
    'neon_t0':        dict(kind='emit', color=T0, strength=14.0, show=(0.45, 0.88, 1.0)),
    'neon_t1':        dict(kind='emit', color=T1, strength=14.0, show=(1.0, 0.62, 0.35)),
    'grid_white':     dict(kind='fx', show=(0.03, 0.055, 0.075), tex='soft_strip'),
    'grid_t0':        dict(kind='fx', show=_s(T0, 0.08), tex='soft_strip'),
    'grid_t1':        dict(kind='fx', show=_s(T1, 0.08), tex='soft_strip'),
    'pulse_white':    dict(kind='fx', show=(0.55, 0.85, 1.0), tex='pulse'),
    'pulse_t0':       dict(kind='fx', show=_s(T0, 1.0), tex='pulse'),
    'pulse_t1':       dict(kind='fx', show=_s(T1, 1.0), tex='pulse'),
    # emitters (the lights that bake onto everything)
    'light_panel':    dict(kind='emit', color=(1.0, 0.98, 0.95), strength=5.0, show=(1.0, 0.99, 0.97)),
    'light_line':     dict(kind='emit', color=(0.85, 0.92, 1.0), strength=4.0, show=(0.85, 0.92, 1.0)),
    'foul_t0':        dict(kind='emit', color=T0, strength=10.0, show=T0),
    'foul_t1':        dict(kind='emit', color=T1, strength=10.0, show=T1),
    'inlay_t0':       dict(kind='emit', color=T0, strength=5.0, show=_s(T0, 0.9)),
    'inlay_t1':       dict(kind='emit', color=T1, strength=5.0, show=_s(T1, 0.9)),
    'inlay_white':    dict(kind='emit', color=(0.8, 0.88, 1.0), strength=3.0, show=(0.7, 0.8, 0.92)),
    'center_mark':    dict(kind='emit', color=(0.7, 0.78, 0.9), strength=1.5, show=(0.45, 0.5, 0.58)),
    'area_t0':        dict(kind='emit', color=T0, strength=3.0, show=_s(T0, 0.75)),
    'area_t1':        dict(kind='emit', color=T1, strength=3.0, show=_s(T1, 0.75)),
    'slats':          dict(kind='emit', color=(0.6, 0.8, 1.0), strength=1.6, show=(0.18, 0.28, 0.40), tex='slats'),
    'slats_v':        dict(kind='emit', color=(0.75, 0.55, 1.0), strength=1.6, show=(0.24, 0.16, 0.40), tex='slats'),
    'screen':         dict(kind='emit', color=(1.0, 1.0, 1.0), strength=2.0, show=(1.0, 1.0, 1.0), tex='screen'),
    # additive glow layers
    'halo':           dict(kind='fx', show=(0.12, 0.20, 0.27), tex='soft_strip'),
    'halo_t0':        dict(kind='fx', show=_s(T0, 0.30), tex='soft_strip'),
    'halo_t1':        dict(kind='fx', show=_s(T1, 0.30), tex='soft_strip'),
    'holo_t0':        dict(kind='fx', show=_s(T0, 0.55), tex='holo_ring'),
    'holo_t1':        dict(kind='fx', show=_s(T1, 0.55), tex='holo_ring'),
    'hex_t0':         dict(kind='fx', show=_s(T0, 0.40), tex='hex'),
    'hex_t1':         dict(kind='fx', show=_s(T1, 0.40), tex='hex'),
    'emblem_fx':      dict(kind='fx', show=(0.35, 0.35, 0.35), tex='emblem'),
    'beam':           dict(kind='fx', show=(0.022, 0.026, 0.032), tex='soft_strip'),
    'twinkle_t0':     dict(kind='fx', show=_s(T0, 0.9), tex='twinkle'),
    'twinkle_t1':     dict(kind='fx', show=_s(T1, 0.9), tex='twinkle'),
    'twinkle_w':      dict(kind='fx', show=(0.85, 0.85, 0.85), tex='twinkle'),
    # ---------------------------------------------------------------- stadium
    'stand':          dict(kind='lit', albedo=(0.09, 0.10, 0.12), f0=(0.05, 0.05, 0.05), gloss=0.2),
    'stand_edge':     dict(kind='lit', albedo=(0.30, 0.32, 0.35), f0=(0.5, 0.52, 0.56), gloss=0.7),
    # the aisles' stairs through the stands (owner, 2026-10-21): slightly tinted, with light step edges
    # (owner: "a little more visible ... pinks and blues but subtle": the aisles alternate a soft blue and a soft pink,
    # with a gentle glow on each step's edge)
    'stairs_b':       dict(kind='lit', albedo=(0.16, 0.24, 0.42), f0=(0.06, 0.07, 0.10), gloss=0.3),
    'stairs_p':       dict(kind='lit', albedo=(0.40, 0.18, 0.32), f0=(0.10, 0.06, 0.08), gloss=0.3),
    'stairs_edge_b':  dict(kind='emit', color=(0.35, 0.65, 1.0), strength=0.9, show=(0.12, 0.24, 0.42)),
    'stairs_edge_p':  dict(kind='emit', color=(1.0, 0.45, 0.80), strength=0.9, show=(0.40, 0.16, 0.30)),
    'seat':           dict(kind='lit', albedo=(0.05, 0.055, 0.065), f0=(0.04, 0.04, 0.04), gloss=0.2),
    'crowd':          dict(kind='lit', albedo=(0.06, 0.06, 0.07), f0=(0.04, 0.04, 0.04), gloss=0.1),
    'dais':           dict(kind='lit', albedo=(0.06, 0.065, 0.075), f0=(0.3, 0.32, 0.35), gloss=0.6),
    'structure':      dict(kind='lit', albedo=(0.16, 0.17, 0.19), f0=(0.4, 0.42, 0.46), gloss=0.6),
    'sky':            dict(kind='prop', shader='unlit', rgb=(1.0, 1.0, 1.0), tex='sky'),
}

# avatar colour channels (material flags bits 8..11, app/src/render.h) and the suit-pattern flag
CH = {'primary': 1, 'secondary': 2, 'glow': 3, 'skin': 4, 'visor': 5, 'hair': 6, 'trim': 7}
TWOSIDED, PATTERN = 1, 16

# ---------------------------------------------------------------- props (drawn with per-player tints in the app)
#   shader: 'lit' (SH_LIT: base * lighting + spec, em = emissive), 'unlit', 'add' (additive), 'trans'
P.update({
    'ball_plasma':   dict(kind='prop', shader='add', rgb=(0.55, 0.55, 0.55), tex='veins', flags=1),
    'ball_filament': dict(kind='prop', shader='add', rgb=(0.45, 0.45, 0.45), flags=1),
    'ball_spark':    dict(kind='prop', shader='add', rgb=(0.8, 0.8, 0.8), flags=1),
    'ctrl_body':     dict(kind='prop', shader='lit', rgb=(0.045, 0.047, 0.052)),
    'ctrl_face':     dict(kind='prop', shader='lit', rgb=(0.085, 0.088, 0.095)),
    'ctrl_button':   dict(kind='prop', shader='lit', rgb=(0.14, 0.145, 0.155)),
    'glove':         dict(kind='prop', shader='lit', rgb=(0.30, 0.30, 0.32), flags=CH['secondary'] << 8),
    'pad_metal':     dict(kind='prop', shader='lit', rgb=(0.10, 0.11, 0.13)),
    'pad_rim':       dict(kind='prop', shader='lit', rgb=(0.45, 0.48, 0.53)),
    'pad_glow':      dict(kind='prop', shader='unlit', rgb=(0.10, 0.11, 0.12)),
    'pad_lens':      dict(kind='prop', shader='unlit', rgb=(0.25, 0.27, 0.30)),
    'gaunt_shell':   dict(kind='prop', shader='lit', rgb=(0.78, 0.80, 0.84), gloss=0.25, flags=CH['primary'] << 8),
    'gaunt_dark':    dict(kind='prop', shader='lit', rgb=(0.07, 0.075, 0.085)),
    'gaunt_trim':    dict(kind='prop', shader='lit', rgb=(1, 1, 1), gloss=0.6, flags=CH['trim'] << 8),
    'gaunt_glow':    dict(kind='prop', shader='unlit', rgb=(0.95, 0.97, 1.0), flags=CH['glow'] << 8),
    'knuckle_glass': dict(kind='prop', shader='trans', rgb=(0.8, 0.9, 1.0), alpha=0.35, flags=1),
    'shield_hex':    dict(kind='prop', shader='add', rgb=(0.40, 0.40, 0.40), tex='hex', flags=1),
    'shield_rim':    dict(kind='prop', shader='add', rgb=(1.0, 1.0, 1.0), flags=1),
    'zone_holo':     dict(kind='prop', shader='add', rgb=(1.0, 1.0, 1.0), tex='holo_ring', flags=1),
    'zone_edge':     dict(kind='prop', shader='add', rgb=(0.9, 0.9, 0.9), flags=1),
    'avatar_shell':  dict(kind='prop', shader='lit', rgb=(0.82, 0.84, 0.88)),
    'avatar_dark':   dict(kind='prop', shader='lit', rgb=(0.06, 0.065, 0.075)),
    'avatar_visor':  dict(kind='prop', shader='unlit', rgb=(0.75, 0.88, 1.0)),
    'avatar_glow':   dict(kind='prop', shader='unlit', rgb=(0.95, 0.97, 1.0)),
})

# ---------------------------------------------------------------- avatars (tools/blender/make_avatar.py): colours come from
# the avatar's channels at draw time and rgb scales them; gloss = arena reflections (SH_LIT emissive.a)
P.update({
    'av_primary':      dict(kind='prop', shader='lit', rgb=(1, 1, 1), gloss=0.10, flags=CH['primary'] << 8 | PATTERN),
    'av_primary_np':   dict(kind='prop', shader='lit', rgb=(1, 1, 1), gloss=0.35, flags=CH['primary'] << 8),
    'av_primary_dk':   dict(kind='prop', shader='lit', rgb=(0.45, 0.45, 0.45), gloss=0.08, flags=CH['primary'] << 8),
    'av_secondary':    dict(kind='prop', shader='lit', rgb=(1, 1, 1), gloss=0.22, flags=CH['secondary'] << 8),
    'av_cloth':        dict(kind='prop', shader='lit', rgb=(1, 1, 1), gloss=-0.6, flags=CH['primary'] << 8 | TWOSIDED),   # matte
    'av_cloth_dk':     dict(kind='prop', shader='lit', rgb=(0.40, 0.40, 0.40), gloss=-0.6, flags=CH['primary'] << 8 | TWOSIDED),
    'av_secondary_dk': dict(kind='prop', shader='lit', rgb=(0.42, 0.42, 0.42), gloss=0.08, flags=CH['secondary'] << 8),
    'av_glow':         dict(kind='prop', shader='unlit', rgb=(1, 1, 1), flags=CH['glow'] << 8),
    'av_iris':         dict(kind='prop', shader='lit', rgb=(0.16, 0.09, 0.05), gloss=0.12),   # low: a mirror-like iris washed out in the arena lights
    'av_eye_white':    dict(kind='prop', shader='lit', rgb=(0.70, 0.67, 0.63), gloss=0.2),
    'av_skin':         dict(kind='prop', shader='lit', rgb=(1, 1, 1), gloss=0.05, flags=CH['skin'] << 8),
    'av_visor':        dict(kind='prop', shader='lit', rgb=(1, 1, 1), gloss=1.0, flags=CH['visor'] << 8 | TWOSIDED),
    'av_hair':         dict(kind='prop', shader='lit', rgb=(1, 1, 1), gloss=-0.85, flags=CH['hair'] << 8),   # matte
    'av_hair_lock':    dict(kind='prop', shader='lit', rgb=(1, 1, 1), gloss=-0.85, flags=CH['hair'] << 8 | TWOSIDED),
    'av_trim':         dict(kind='prop', shader='lit', rgb=(1, 1, 1), gloss=0.7, flags=CH['trim'] << 8),
    'av_dark':         dict(kind='prop', shader='lit', rgb=(0.035, 0.037, 0.042), gloss=0.25),
    'av_piping':       dict(kind='prop', shader='lit', rgb=(0.80, 0.80, 0.82), gloss=0.10),
    'av_armor':        dict(kind='prop', shader='lit', rgb=(0.045, 0.047, 0.052), gloss=0.45),
    'av_armor_white':  dict(kind='prop', shader='lit', rgb=(0.78, 0.79, 0.81), gloss=0.55),
    'av_eye':          dict(kind='prop', shader='lit', rgb=(0.012, 0.012, 0.016), gloss=0.35),
    'av_fade':         dict(kind='prop', shader='add', rgb=(0.85, 0.85, 0.85), tex='soft_strip',
                            flags=CH['glow'] << 8 | TWOSIDED),
})
