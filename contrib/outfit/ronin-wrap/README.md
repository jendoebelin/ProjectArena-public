# Ronin Wrap
by: Project Arena authors
kind: outfit (a new SUIT; it replaces no existing ID)
fits: masculine and feminine bodies (two files: ronin_wrap_m.glb, ronin_wrap_f.glb)
notes: a wrap jacket crossed over to the left hip, with a raised obi sash, a trim stripe and a tied knot with two
short tails; three layered shoulder lames on each arm with trim hems; long sleeves with cuffs and a glowing cuff line.

- Materials: `av_primary` (jacket and sleeves, takes patterns), `av_secondary` (lapels, collar, cuffs),
  `av_secondary_dk` (obi, knot, tails), `av_trim` (obi stripe, lame hems and edges), `av_armor` (the lames),
  `av_glow` (the top lapel's edge, the cuff lines), `av_dark` (the undershirt in the V), `av_skin` (the neck).
  No design mask textures: every colour region is its own geometry.
- Triangles: 7,409 (masculine), 7,312 (feminine). Skinned to the template rig, every vertex weighted.
- Fit: cloth 4-6 mm off the skin, the obi 13 mm, the lames 8-24 mm. Ends at the waist (Y = -0.440 m / -0.431 m) and
  the neck (Y = 0.099 m / 0.079 m) like the existing suits.
- The knot's two tails hang about 9 cm below the waist over the shorts (they follow the torso).
- `preview_m.png` / `preview_f.png`: front, three-quarter, side, back, in preview colours (primary blue, secondary
  orange), on the base head and the Athletic Shorts.
- Checked with `python -I tools/import_glb.py ronin_wrap_m.glb av_top_16_m` (and _f): converts with no material
  warnings (only the usual note that clavicle weights go to the torso, as for the shipped suits).
