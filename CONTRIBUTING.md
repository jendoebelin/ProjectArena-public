# Contributing to Project Arena

Thanks for helping! Project Arena is a free, open-source 1-v-1 VR court sport for Meta Quest and PC VR. You can help
with art, sounds, testing, ideas or code; you do not need to be a programmer.

Questions, or not sure where something fits? Open an issue, or email vrsportsfan@gmail.com.

## 1. Play and report

- **In the game:** MENU > **REPORT BUG** (pick a category, add a note). It sends the note, your settings and the
  game's recent log to the community server, which is the fastest way for us to see what happened.
- **On GitHub:** open an issue with the **Bug report** form: what you did, what happened, what you expected, your
  headset (Quest 2 / 3 / 3S / Pro, or PC and which headset) and the game version (MENU > SETTINGS, or the release).
- **Someone behaving badly online?** Use MENU > **REPORT PLAYER** in the game (not GitHub): it goes privately to the
  people who run the server, with what is needed to act.
- **Want to test new builds early?** Email vrsportsfan@gmail.com for the Meta Horizon Store test channel.

## 2. Make art: outfits, hair, headgear, masks, gauntlets, props

1. Open the **asset catalog** (linked from the README). Every model has a **Download** button (.glb, opens in
   Blender 4+ and most 3D tools), and the **template pack** has the bodies, heads and hand frame to build on.
2. Read `README.md` inside the template pack: units, the space each kind of part lives in, triangle budgets, and the
   **material names** the game understands (`av_primary`, `av_glow`, `gaunt_shell`, ...). Keep those names.
3. Export as glTF Binary (.glb) and put it in `contrib/<kind>/<asset-name>/` with a `README.md` saying what it is,
   which bodies or heads it fits, and how you want to be credited (see `contrib/README.md`).
4. Open a pull request using the **asset** checklist. We try it in the headset (`tools/import_glb.py` turns it into a
   game model) and reply with screenshots or fixes.

## 3. Make sounds

`docs/SOUND_BRIEF.md` lists every sound the game plays: when it plays, how long and how loud it should be, and the
style we are after (subtle, short, clean sci-fi sport; no music or voices in effects). To propose a sound:

1. Make it (record, synthesize, or generate with a tool whose terms let you give it away under CC BY 4.0).
2. Name the WAV after its slot (`Grab.wav`, `ShieldBlock.wav`, ...) and try it in your build:
   `python -I tools/import_sounds.py <folder>` fits it to the slot (trim, length, loudness), then build and play.
3. Put the original WAV in `contrib/sound/<slot>/` with a `README.md` (who made it, how, the licence) and open a pull
   request. Variations are welcome: say which you like best.

## 4. Ideas

Open an issue with the **Idea** form: what you would like and why (a mode, a rule, a menu change, an outfit theme).
`docs/RELEASE_NOTES_*.md` shows what changed recently.

## 5. Code

- Game: `app/src` (C++17, OpenXR, OpenGL ES 3 / OpenGL 4.3). Build: `./build.sh debug` (Quest APK, needs the
  Android NDK) or `tools/build_pc.bat` (Windows, Visual Studio 2022). See `docs/BUILD.md`.
- Community server: `server/projectarena_relay.py` (Python 3, no dependencies); `docs/SERVER.md` to run your own.
- Tests, please keep them passing and add one for what you change:
  - `tools/run_tests.bat`: gameplay rules, avatars, the match start
  - `python -I tools/run_room_test.py`: multiplayer rooms, queue, names, voice routing (starts a local server)
  - `python -I server/test_relay.py` and `python -I server/test_relay_moderation.py`: the server
- Generated art: `tools/build_open_assets.py` (Blender 4.x / 5.x, MPFB2 for the bodies).
- Keep pull requests small and focused, and say how you tested (desktop tests, PC VR, which Quest).
- Multiplayer messages: new fields go at the end of a message so older builds still read it (see `app/src/net.cpp`).

## Rules of the road

- **Only your own work.** No assets, sounds or code from other games, Sparc included, and nothing you do not have
  the right to give away. Credit anything CC0 / CC BY you build on.
- **Licences:** code under MIT (`LICENSE`); art and sound under CC BY 4.0 (`LICENSE-ASSETS.md`), credited to you.
- **Be kind.** We follow `CODE_OF_CONDUCT.md` here and in the game.
