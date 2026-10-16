# Contributing to Project Arena

Thanks for helping! There are three easy ways in.

## 1. Make art: outfits, hair, headgear, masks, gauntlets, props

1. Open the **asset catalog** (linked from the README). Every model has a **Download** button (.glb, opens in
   Blender 4+ and most 3D tools), and the **template pack** has the bodies, heads and hand frame to build on.
2. Read `README.md` inside the template pack: units, the space each kind of part lives in, triangle budgets, and the
   **material names** the game understands (`av_primary`, `av_glow`, `gaunt_shell`, ...). Keep those names.
3. Export as glTF Binary (.glb) and put it in `contrib/<kind>/<asset-name>/` with a `README.md` saying what it is,
   which bodies or heads it fits, and how you want to be credited.
4. Open a pull request. We try it in the headset (`tools/import_glb.py` turns it into a game model) and reply with
   screenshots or fixes.

Art you contribute is published under **CC BY 4.0** (`LICENSE-ASSETS.md`): anyone may use it, with credit to you.
Only submit work you made yourself (or that is CC0 / CC BY and credited). No assets from other games, please,
including Sparc.

## 2. Report bugs

In the game: **MENU > REPORT BUG**. It sends a note, your settings and the game's recent log to the community server.
Or open a GitHub issue: what you did, what happened, what you expected, and the headset (Quest 2 / 3 / 3S, or PC and
which headset).

## 3. Code

- Game: `app/src` (C++17, OpenXR, OpenGL ES 3). Build: `./build.sh debug` (Quest APK, needs the Android NDK) or
  `tools/build_pc.bat` (Windows, Visual Studio 2022). See `docs/BUILD.md`.
- Tests: `tools/run_tests.bat` (gameplay), `python tools/run_room_test.py` (multiplayer rooms). Please keep them
  passing and add one for what you change.
- Generated art: `tools/build_open_assets.py` (Blender 4.x/5.x, MPFB2 for the bodies).
- Keep pull requests small and focused; describe how you tested.

Code contributions are under the MIT license (`LICENSE`).
