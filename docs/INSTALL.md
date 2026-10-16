# Installing Project Arena

## Meta Quest (Quest 2, 3, 3S, Pro)

**From the Meta Horizon Store (test channel, no developer mode):**
1. Email **vrsportsfan@gmail.com** (subject "Project Arena test access") with the email address of the Meta account
   your headset is signed in to.
2. Accept the invite Meta sends you (email, or the Meta Horizon phone app).
3. Find **Project Arena** in the Store on your headset and install it. Updates arrive automatically.

The release page has a step-by-step `README-Quest.txt` for both ways.

**Sideloading the APK (developer mode):**
1. Turn on developer mode once: Meta Horizon phone app → **Devices** → your headset → **Headset settings** →
   **Developer mode** (you may be asked to create a free developer organization first). Restart the headset.
2. Install with SideQuest (https://sidequestvr.com/setup-howto: plug the headset in, allow USB debugging, click the
   install-APK icon and pick `ProjectArena-<version>.apk`), or with adb: `adb install -r ProjectArena-<version>.apk`.
3. In the headset: **Library** → **Unknown Sources** → **Project Arena**.

## PC VR (SteamVR: Valve Index, Vive, Windows Mixed Reality; or Meta Quest Link)

1. Download `ProjectArena-<version>-PC.zip` from the GitHub release and unzip it anywhere (keep the `assets` folder
   next to the exe).
2. Start SteamVR (or Quest Link) and put your headset on.
3. Run `ProjectArena.exe`. Settings, avatar and log live in `%APPDATA%\ProjectArena`.

Quest and PC players meet in the same rooms online.

## Playing

- Stand in a clear space of about 2 x 2 m. The first launch shows **HOW TO PLAY**.
- **MENU** (left controller) opens the menus. **B** or a thumbstick click re-centres you; holding the Meta button
  does too.
- **PLAY PRACTICE**: you against the bot. **CUSTOMIZE AVATAR**: your look, which other players see.
- **MULTIPLAYER**:
  - **ONLINE: ON** shows rooms on the community server (the game connects to it by default); **ONLINE: OFF**
    shows rooms on your home Wi-Fi.
  - **HOST A ROOM** opens a room. You start in its waiting area: press **JOIN QUEUE** (the red button in front of
    you) to play. Others pick your room and press **JOIN**, then **JOIN QUEUE**.
  - **FORFEIT MATCH** takes you back to the waiting area. **SPECTATE GAMES** shows every live match.
- Voice chat in a room: hold the **left grip** to talk (SETTINGS → VOICE AND MUTES for open mic and mutes).
- Something wrong? **MENU → REPORT BUG** sends a short note and the game's recent log to us.

## Updating and uninstalling

Store installs update themselves; sideloaded APKs update by installing the new APK the same way (your avatar, name
and settings are kept). Uninstall: Settings → Apps → Installed apps → Project Arena.
