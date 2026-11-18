# Asset license

The game's art and sound (`app/assets_open`, the template pack, and everything contributed under `contrib/`) are
licensed under the **Creative Commons Attribution 4.0 International License (CC BY 4.0)**:
https://creativecommons.org/licenses/by/4.0/

You may share and adapt them for any purpose, including commercially, as long as you give appropriate credit
("Project Arena" and, for contributed assets, the contributor named in that asset's README), link to the license, and
indicate if changes were made.

The code (game, tools, server) is under the MIT license: see `LICENSE`.

Exceptions:
- The avatar bodies are built from MPFB2 / MakeHuman base meshes, which are CC0 (public domain).
- The licensed music (`app/assets_music`, in the builds only, never in the public repository) was made with Suno and
  is used by CrossGenGaming under Suno's terms: it is **not** covered by CC BY 4.0 and may not be reused apart from the game.
- Project Arena contains no assets from any other game.

- The isolated `experiments/avatar_anatomy` bodies, heads, eyes and eyebrows are derived from Quaternius's
  Universal Base Characters Standard pack, dedicated to CC0 1.0 (see its included `QUATERNIUS-LICENSE.txt`).
  They have been retargeted, clipped and depth-adjusted for this experimental comparison. The generated
  Project Arena garment patterns remain CC BY 4.0, with credit to Project Arena. Experiment code remains MIT.
  These assets are not installed as the game's default avatar assets.

- The additional experimental Vector/Nightforge clothing, helmets, fitted existing headwear,
  opaque shorts, skirts and kilts are Project Arena contributions under CC BY 4.0.
  Fits derived from Quaternius bodies retain their CC0 geometry origin; source game assets
  retain their existing licenses. The bundled inspector's Three.js r169 code is MIT, with
  its copyright notice and full license in `experiments/avatar_anatomy/inspector/THREE-LICENSE.txt`.
  Inspector replay datasets are experimental review fixtures, not a production runtime.
