# Building Project Arena

## Quest (Android APK)

Needs: Android SDK with NDK 26.3 and build-tools 34, a JDK 17+, Python 3, and the Khronos OpenXR loader AAR unpacked
into `third_party/openxr_aar` (from https://github.com/KhronosGroup/OpenXR-SDK releases, `openxr_loader_for_android`).

```
./build.sh debug open          # build/ProjectArena-debug-open.apk (debug-signed)
adb install -r build/ProjectArena-debug-open.apk
```

Release builds are signed with your own key: set `PROJECTARENA_KEYSTORE`, `PROJECTARENA_KEY_ALIAS` and
`PROJECTARENA_KEY_PASS`, then `./build.sh release open`.

## PC (Windows, SteamVR / Quest Link)

Needs Visual Studio 2022 (C++ workload) and Python 3. `tools\build_pc.bat` builds `build\pc\ProjectArena\` (the exe
and its assets). `ProjectArena.exe --selftest out.ppm` renders a frame without a headset.

## Server address

Builds read `servers.local.cfg` (repo root, not in git) for the community server players connect to by default:

```
PROJECTARENA_SERVER=your.server:47780
```

Without it, players type a server address in SETTINGS > SERVER. Running one: `docs/SERVER_SETUP.md`.

## Tests

`tools\run_tests.bat` (gameplay and protocol tests), `python tools/run_room_test.py` (multiplayer rooms and player
statistics through a local relay), `python server/test_relay.py`, `python server/test_relay_stats.py`,
`python server/test_relay_moderation.py`.

## Art

`python tools/build_open_assets.py` regenerates `app/assets_open` with Blender 4.x / 5.x (the avatar bodies need the
MPFB2 add-on). `tools/export_glb.py` and `tools/import_glb.py` convert between game models and .glb;
`tools/gen_catalog.py` builds the asset catalog with downloads. `tools/gen_flags.py` writes the country table
(`app/src/nations.inc`) and the flag textures: its list is append-only (a country's position is the number sent over
the network).
