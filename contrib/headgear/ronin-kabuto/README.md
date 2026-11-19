# Ronin Kabuto
by: jendoebelin
kind: headgear (a new HEADGEAR; it replaces no existing ID). Made to go with the Ronin Wrap suit (`contrib/outfit/ronin-wrap/`).
fits: masculine and feminine heads (two files: ronin_kabuto_m.glb, ronin_kabuto_f.glb), in head space; every face
notes: an open-faced samurai-style helmet:

- **Bowl:** a ribbed, dark-lacquer bowl with 16 raised ribs, a proud trim rim band and a trim crown ring.
- **Neck guard:** four lames flaring out and down round the back and sides. Each sits over the one below, laced
  with cords in the secondary colour and hemmed in trim, like the suit's shoulder lames. The lower lames start a
  little further back.
- **Cheek wings:** the top lames' front ends folded back into solid plates in the bowl's own lacquer and gilt.
  Each follows the guard's curve and turns out a little toward its back edge, closed onto the guard all round by
  gilt edges, with a gilt border and a gilt crest (a ring round a diamond).
- **Brow peak:** angled down, with a glowing edge.
- **Crest:** a tall trim crescent with a glowing inlay, on a primary-colour mount with a glowing emblem.

## Materials

| Material | Where |
|---|---|
| `av_armor` | bowl, lames, cheek wings, brow peak |
| `av_trim` | rim band, crown, lame hems, wing borders and crests, crescent |
| `av_secondary` | lacing |
| `av_primary_np` | crest mount |
| `av_glow` | brow edge, crescent inlay, the mount's emblem |

The lames, wings, peak, rim band and crescent are two-sided (`twoSided` in the custom properties; Blender names
them `*.2s`, and `tools/import_glb.py` strips the suffix). No textures.

## Fit and size

- **Triangles:** 5,602 per head.
- **Clearance:** at least 11.5 mm off the masculine head's skin and 10.9 mm off the feminine's (measured on the
  template heads), with nothing inside the head.
- **Size:** about 31 cm across and 25 cm tall, and 35 cm from the brow peak to the back of the neck guard (the
  guard's flare sets the width).
- **The bowl** is a smooth dome fitted to clear each head by 12 mm. Its rim sits over the brows, stays level back
  past the ears and is lower at the back.

## With the other head parts

Measured against the game's models:

- **Masks and face shields that clear it:** every mask, including the Bandana with its swinging tails, and the
  Band Visor, Full Shield, Shades, Goggles and Slit Visor.
- **Faceplate:** sits wholly under the bowl's rim, like a face mask under a kabuto.
- **Headset:** two points of its strap touch the rim band at the back.
- **Halo:** its floating ring passes through the helmet.
- **Hair:** goes under it, the way the game tucks hair into every helmet. The game needs a `.tuck` map made for it
  (`tools/build_open_assets.py write_tuck`).

## Previews

- `preview_m.png` / `preview_f.png`: on the template heads (front, three-quarter, side, back).
- `preview_set_m.png` / `preview_set_f.png`: the full look with the Ronin Wrap and the Bandana.

Preview colours: primary blue, secondary orange, trim gold, glow cyan.

## Source

`make_ronin_kabuto.py` builds both files from the template pack's heads:

```
blender -b -P make_ronin_kabuto.py -- m ronin_kabuto_m.glb
```

Set `PA_TEMPLATES` to the unpacked template pack. It prints the triangle count and the fit check.

## Checked

`python -I tools/import_glb.py ronin_kabuto_m.glb av_gear_9_m` (and `_f`): both become rigid models with no
material warnings. Not tried in the headset.

To make it a HEADGEAR choice, the game needs:

- its name in the headgear list
- the preload loop in `main.cpp` raised to 9
- `tuckGear` grown to 10
