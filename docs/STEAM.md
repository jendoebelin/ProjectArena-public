# Project Arena on Steam

How to get the PC build (VR plus the spectator window for streamers) onto Steam. First as a free **Playtest** for
test releases, later as a free **Early Access** game. Checked against the Steamworks documentation on 2026-10-17
(Playtest, Early Access, Uploading, Releasing, App fee, VR settings). Valve changes these pages, so re-read a step's
page if something looks different.

Steam gets the newest release (0.3.5 as of 2026-10-19). Any release from 0.3.2 on has the spectator window for PCs
without a headset.

## What it costs and how long it takes

- **Steam Direct fee:** US$100 for each new app. It is not refundable, but you get it back after the game earns
  $1,000 on Steam. A free game never earns that, so for us it is simply $100. Only a Steamworks Admin can pay it.
- **Playtest:** a free child app of the main game. Valve's review "only consists of capsule images and icons".
- **Store page review:** typically 3-5 business days. Valve suggests submitting at least 7 days before you want the
  page live.
- **Release:** the store page has to be approved and shown as *Coming Soon* for about 2 weeks, and the build has to
  be reviewed. Approved games don't release by themselves: you press **Release App**.

## Part 1: account and app (owner, in the browser)

1. Join the Steamworks partner program at <https://partner.steamgames.com>. Use the project's contact address,
   vrsportsfan@gmail.com, as the public contact. Steamworks asks for a legal name, a bank account and a tax interview
   before you can pay. The account owner enters those personally; never share them.
2. Pay the Steam Direct fee for one app, **Project Arena**. Search Steam for that name first, in case another game
   already uses it.
3. Optional but useful: make a separate Steam account for uploads, for example `projectarena_build`. Give it only
   "Edit App Metadata" and "Publish App Changes To Steam". Attach a phone or the Steam Mobile app, because Steam needs
   a confirmation to set builds live on a released app. After any security change, Steam makes the account wait
   3 days before it can set a build live.

## Part 2: the Playtest (test releases)

1. Open the game's **Associated Packages & DLC** page and create a **Playtest**. You get a new app ID with its own
   depot.
2. In the Playtest's Steamworks settings:
   - **Installation > General > Launch Options.** Add two:
     | Label | Executable | Arguments | Launch type |
     |---|---|---|---|
     | Play in VR | `ProjectArena.exe` | (none) | Launch SteamVR App / Launch in VR |
     | Spectator window (no headset, for streaming) | `ProjectArena.exe` | `--spectate` | Launch (default) |
     - Set the operating system to **Windows, 64-bit** for both.
     - Steam needs at least one VR launch option before it turns on VR support.
     - The second option lets people pick the spectator window from their library.
   - **Application > Virtual Reality.**
     - Project Arena is a native **OpenXR** app. It declares its own bindings for Quest Touch, Index, Vive, Windows
       Mixed Reality and the simple controller.
     - Choose the input mode that keeps the game's own OpenXR bindings. Don't make SteamVR Input bindings.
   - **Library assets and icons.** These are the only items in the Playtest review checklist. They're ready in
     `build/steam_art/` (`python -I tools/gen_steam_art.py` after `tools\build_pc.bat`), rendered by the game
     from the same staged rally as the Meta art:
     - library capsule 600x900, library header 920x430, library hero 3840x1240 (no text: Steam puts the logo on it);
     - library logo 1280 wide, transparent; community icon 184x184 JPG; client icon `.ico`;
     - for the store page later: header 920x430, small 462x174, main 1232x706 and vertical 748x896 capsules, a page
       background 1438x810, and five 1920x1080 screenshots.
3. On the Builds page, create a branch called **testing** (Manage branches). Uploads go there automatically. You set
   a build on the default branch yourself.
4. Upload a build (Part 4).
5. Finish the Playtest release checklist and mark it ready for review.
6. Choose who gets in:
   - **Keys:** you can request **Standard Release** keys for a Playtest *before the store page is live*. This is the
     way to invite our testers now.
   - **Store sign-ups:** these appear as a section on the main game's store page, so they need that page (Part 3).
     *Limited* lets you grant access in batches. *Open* lets everyone in. Turn sign-ups on under Edit Store Page >
     Special Settings.
7. Rules from Valve:
   - A Playtest must be free.
   - Don't sell its keys or put them in bundles.
   - Turn sign-ups off before setting it to Not Playable to end it.
   - Players who got access keep it if it is opened again.

## Part 3: the store page and Early Access (later, when we want it public)

1. **Edit Store Page.**
   - Descriptions: reuse docs/STORE_LISTING.md, where the Meta answers are already written.
   - Screenshots and a gameplay trailer. Early Access requires "at minimum a trailer showing gameplay".
   - **Basic Info > Supported Platforms > VR Devices and Support:**
     - SDK: OpenXR.
     - Headsets: Valve Index, HTC Vive, Windows Mixed Reality, Meta Quest (Link / Air Link / Steam Link / Virtual
       Desktop).
     - Tracked motion controllers; about 2 x 2 m of standing space.
     - Say that the game itself needs VR, and that without a headset it opens only the spectator window.
2. **Early Access tab.** Tick *Early Access* and answer the Q&A:
   - why Early Access;
   - how long (no specific promises);
   - what the full version adds;
   - the current state;
   - pricing (free during and after);
   - how the community takes part (the Discord, MENU > REPORT BUG, GitHub issues).
   The Early Access page doesn't say anything about free games. Pick *Free* on the pricing step and see what
   Steamworks allows.
3. **Content survey.** Answer it like the Meta IARC questionnaire (docs/STORE_LISTING.md): online play with voice
   chat, player reporting and mute. Link the privacy policy (`docs/PRIVACY.md`'s public URL) wherever Steamworks asks
   for one (it applies to Steam players too: their statistics go to the same server); the data facts are in
   `docs/STORE_LISTING.md` > Data use and privacy answers.
   - It also asks about AI-generated content. Read its current wording carefully: the code and the procedural art
     scripts were written with an AI assistant.
4. Mark the store presence ready for review. Once it's approved, set it to *Coming Soon* for at least 2 weeks. Then
   submit the build checklist, using a default-branch build that has the features on the page. Then **Release App**.
5. Leaving Early Access later:
   - Publish the build and use *View Release Options*.
   - A released game can't go back to Coming Soon or Early Access.

## Part 4: uploading a build (each release)

One-time setup on this PC:
1. Download the **Steamworks SDK** from the partner site (you have to be logged in). Unzip it, for example to
   `C:\steamworks_sdk`. Run `tools\ContentBuilder\builder\steamcmd.exe` once so it updates itself.
2. Create `steam.local.cfg` in the project root. It is gitignored. The app and depot IDs are the Playtest's while we
   test:
   ```
   STEAM_APP_ID=<playtest app id>
   STEAM_DEPOT_ID=<its windows depot id>
   STEAM_USER=<build account name>
   STEAMCMD=/c/steamworks_sdk/tools/ContentBuilder/builder/steamcmd.exe
   STEAM_BRANCH=testing
   ```

Each release:
```bash
./tools/steam_upload.sh --preview
./tools/steam_upload.sh
```
- The script builds the PC version (`tools/build_pc.bat`, with the open asset set only). It also checks that the exe
  carries the release version.
- It copies the build to `build/steam/content` and writes the SteamPipe scripts to `build/steam/scripts`.
- Then it runs `steamcmd +login <user> +run_app_build ... +quit`. steamcmd asks for the password and the Steam Guard
  code itself. Nothing is stored in the project.
- `--preview` checks everything without sending anything. Read `build/steam/output/*.log`: Valve says errors show up
  there, not in the console.
- The build goes live on **testing**. To test it: in your library, open Project Arena > Properties > Betas >
  *testing*.
- When it's good, set it live on **default** from the Builds page.

Settings and logs are kept in `%APPDATA%\ProjectArena`, outside the Steam folder, so Steam updates never overwrite
them.

## Before the first Steam build goes to testers

- **Test the release in a PC VR headset through SteamVR**: start it from Steam's "Play in VR" option. The spectator
  window has been tested on this PC without a headset. Quest Link / SteamVR play of this release hasn't been tested
  yet.
- Test the "Spectator window" launch option from the Steam library.
- Steam builds don't use the Steamworks API (no achievements, no Steam login). The game runs the same as the GitHub
  zip. Adding Steam features later would need the redistributable `steam_api64.dll` and its licence terms.
