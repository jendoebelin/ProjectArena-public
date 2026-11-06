# Project Arena on Steam

Steam gets the **PC VR build**, the same one as the GitHub zip. Steam can't install on a standalone Quest: Quest
owners play the Steam copy as PC VR (Link cable, Air Link or Virtual Desktop), and the standalone Quest build stays on
the Meta store, SideQuest and GitHub. Everyone plays together on the same server.

The game knows when Steam installed it (its folder is under `steamapps`, or Steam started it): it turns its own
UPDATE NOW off (Steam updates it), and says **STEAM** after its version and in bug reports.

It is released as **Early Access** (free), the Steam equivalent of the Quest ALPHA, and each new build goes to a
**beta branch** for testers before it goes to everyone.

## Once, when Steamworks approves the app (the owner)
1. **App and depot:** Steamworks > App Admin > SteamPipe > Depots: note the App ID and the Windows depot ID.
2. **Beta branch:** SteamPipe > Builds > Manage beta branches: add `beta` (a password is optional: give it to testers).
3. **Launch option:** Installation > General: executable `ProjectArena.exe`, Windows, 64-bit.
4. **VR:** mark the game VR only; headsets: Valve Index, HTC Vive, Windows Mixed Reality, Meta Quest (via PC);
   input: tracked motion controllers; play area: standing / room-scale. It runs on OpenXR (SteamVR or Meta's runtime).
5. **steamcmd:** download Valve's steamcmd (partner.steamgames.com/doc/sdk/uploading) and unzip it to `~/steamcmd`.
6. **Upload settings:** `~/.projectarena-steam.cfg` (outside the repository), three lines:
   ```
   APP_ID=...
   DEPOT_ID=...
   STEAM_USER=...
   ```
7. **Store page** (drafts from Claude): the capsule art, 5+ screenshots (1920 x 1080), the trailer, descriptions, tags,
   system requirements, price (free), the content questionnaire, and the **Early Access questionnaire** below.
8. **Coming Soon:** the page must be public for **at least 2 weeks** before the game can be released.
9. Steam reviews the store page and the build (a few working days), then the owner presses **Release**.

### Store art sizes (Steamworks > Store page > Graphical assets)
| Asset | Size |
|---|---|
| Header capsule | 920 x 430 |
| Small capsule | 462 x 174 |
| Main capsule | 1232 x 706 |
| Vertical capsule | 748 x 896 |
| Screenshots (at least 5) | 1920 x 1080 |
| Library capsule | 600 x 900 |
| Library hero | 3840 x 1240 |
| Library logo | 1280 x 720 (transparent PNG) |

All from real in-game pictures (the PC build's picture mode), never the illustrated key art.

### Early Access questionnaire (draft answers)
- **Why Early Access?** Project Arena is a free community game built in the open: players shape it through Discord
  and bug reports sent from inside the game, and new modes and fixes come out every few days.
- **How long?** Roughly a few months, while the online modes, the first-time player experience and performance are
  finished and tested with players.
- **The full version:** the court sport with its modes (1-v-1 Basic and Advanced, FOUR CORNERS, FIREBALL BLITZ),
  TRAINING, spectating and streaming, online play across Quest and PC, polished and stable.
- **The Early Access version now:** all of the above playable; online rooms for up to eight; bots from ROOKIE to
  UNBEATABLE; regular updates.
- **Will the price change?** No: it is free and stays free.
- **Community:** the Discord, in-game REPORT BUG and REPORT PLAYER, and public release notes for every update.

## Each release (after the usual build)
1. `./tools/make_release.sh --github-only` (as now: also builds `build/pc/ProjectArena`).
2. `./tools/steam_upload.sh`: uploads that build and sets it live on the **beta** branch.
3. Testers (and you) try it: Steam > Project Arena > Properties > Betas > `beta`.
4. Steamworks > SteamPipe > Builds: make it the **default** build (everyone gets it).
5. Then the server's `latest_version.txt`, so no Steam player is told to update before Steam has the update.
6. GitHub release, public repo, Discord (and a Steam news post if wanted).

## SteamVR controller check (each headset family once before launch)
- [ ] Grab the ball (grip), throw it, curve it (a flick), catch it.
- [ ] Shield up, knuckle guards, a punch.
- [ ] The menu: open it, point and pull the trigger to select; A for READY in a room for four.
- [ ] Push to talk (Valve Index: a firm squeeze, mapped by a tester; Vive and WMR: the grip).
- [ ] Snap / smooth turning on the thumbstick or trackpad.
