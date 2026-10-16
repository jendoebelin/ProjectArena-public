# Installing Project Arena on a Meta Quest

About 10 minutes the first time. You need a Quest 3 (Quest 2, Pro and 3S should work but are not tested yet),
a USB-C cable, and a Windows, Mac or Linux computer.

## 1. Turn on developer mode (once)
1. On your phone, open the **Meta Horizon** app and sign in with the account your headset uses.
2. If asked, create a developer organization at https://developer.oculus.com/manage/organizations/create/
   (free, any name).
3. In the app: **Devices** → your headset → **Headset settings** → **Developer mode** → turn it on.
4. Restart the headset.

## 2. Install the game
**Easiest: SideQuest**
1. Install SideQuest (Advanced installer) from https://sidequestvr.com/setup-howto on your computer.
2. Plug the headset in. Put it on and choose **Allow USB debugging** (tick "Always allow from this computer").
3. In SideQuest, the dot at the top left turns green. Click the **install APK** icon (box with a down arrow)
   and pick `ProjectArena-release-open.apk`.

**Or with adb** (if you already have Android platform-tools):

    adb install -r ProjectArena-release-open.apk

## 3. Play
1. In the headset: **Library** → the drop-down at the top right → **Unknown Sources** → **Project Arena**.
2. Stand in a clear space about 2 x 2 m. The first launch shows **HOW TO PLAY**.
3. **B** or a thumbstick click re-centres you. **MENU** (left controller) opens the lobby, CUSTOMIZE AVATAR and
   CHANGE NAME.

## Playing with a friend on the same Wi-Fi
1. Both headsets on the same network, both with the same version of the game.
2. One player: **MENU → HOST GAME**. The other: **MENU** → pick the host's name under GAMES ON YOUR NETWORK.
3. Anyone else on the network can **SPECTATE GAMES**.

If the game does not appear: check both are on the same Wi-Fi (guest networks often block players from seeing each
other), and that both have the same game version.

## Playing online (community server)
1. **MENU → SETTINGS → SERVER**: type the server address you were given (for example `PLAY.EXAMPLE.NET:47780`), then
   **DONE**. The game connects right away.
2. In the **MENU**, **ONLINE: ON** shows games hosted on that server. Host, join and spectate as on Wi-Fi.
3. **ONLINE: ON** again switches back to your home network.

## Updating
Install the new APK the same way; your avatar, name and settings are kept.

## Uninstalling
Settings → Apps → Installed apps → Project Arena → Uninstall (or remove it in SideQuest).
