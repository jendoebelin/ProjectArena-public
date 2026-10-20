# Meta Horizon Store: release channel (private testing)

Goal: friends install Project Arena from the Store app on their Quest (no developer mode), through a private
release channel (ALPHA). Nobody else can see or install it. Public store release is a later, separate step.

## Already done in the repo

- **Manifest** follows Meta's release-manifest rules (checked October 2026). It has:
  - minSdk 32, targetSdk 34 (required for apps created after 1 March 2026), and no compileSdk stamp;
  - installLocation auto, headtracking required, and supportedDevices `quest2|questpro|quest3|quest3s`;
  - the VR launcher category, excludeFromRecents, and no debuggable flag in release builds.

  `python -I tools/check_store_apk.py <apk> --release` checks all of this. `tools/make_release.sh` runs it automatically.
- **versionCode 3** (every upload needs a higher number than the one before).
- **Draft store art** comes from `python -I tools/gen_store_art.py`, which renders `build/store_art/*.png`.
  - It uses Meta's sizes: hero 3000x900, landscape 2560x1440, square 1440x1440, portrait 1008x1440, mini 1080x360, icon 512x512, and 3 screenshots.
  - The game's own renderer draws everything, so there is no outside artwork.
- **Privacy policy**: `docs/PRIVACY.md`, at its public URL in the store listing. It must match what the build
  collects: from 0.3.6 that includes player statistics (`docs/DATA_AND_SECURITY.md`; the answers for the dashboard's
  data questions are in `docs/STORE_LISTING.md`). Before each upload: `docs/RELEASE_CHECKLIST.md`.

## Your steps (the account, keys and secrets stay with you)

1. **Developer account**: sign in at https://developers.meta.com/horizon/ with your Meta account. Then:
   - create an **organization** and accept the developer agreement;
   - add the verification Meta asks for (phone, or payment method).
2. **Create the app**: Developer Dashboard > Create New App > Meta Horizon Store > Meta Quest.
   - **Name:** Project Arena (package `com.projectarena.game`). The package name is tied to the store listing for good after
     the first upload, so it must not change again after that.
3. **Release key** (once; keep it backed up forever: the store only accepts updates signed with the same key):

   ```bash
   keytool -genkeypair -v -keystore projectarena-release.jks -alias projectarena -keyalg RSA -keysize 4096 -validity 10000
   ```

   Keep the `.jks` and its password out of the repo (`.gitignore` already excludes keystores).
4. **Build** the signed release:

   ```bash
   PROJECTARENA_KEYSTORE=/path/projectarena-release.jks PROJECTARENA_KEY_ALIAS=projectarena PROJECTARENA_KEY_PASS='...' ./tools/make_release.sh
   ```

   This runs the tests, builds and checks the APK, builds the PC zip, and writes `build/release-0.2.0/`.
5. **Upload to ALPHA**: Dashboard > your app > Distribution > Release Channels > ALPHA > Upload, and pick the APK.
   - Alternatively use Meta's `ovr-platform-util upload-quest-build` with the App ID and App Secret from the dashboard
     (API page). The App Secret is a password: never paste it into chat or commit it.
6. **Invite testers**: on the ALPHA channel, add each friend's Meta account email. They accept the invite (email or
   the Meta Horizon phone app), then install from the Store on the headset like any other app. Updates arrive the same way.
7. **Store details** the dashboard asks for, even for private channels:
   - age group (self-certified);
   - a short description;
   - the privacy policy URL;
   - the cover art from `build/store_art/`.

## Later, for a public store release (not needed for ALPHA testing)

- **Entitlement check** (VRC.Quest.Security.1) needs Meta's Platform SDK. That's Meta's own library, so it's an
  extra dependency to add when going public.
- **Store review** against the VRCs: performance, comfort, assets and content. The draft art probably needs a designer pass.
- **Voice chat**: the review looks at reporting and moderation for user voice. The relay has no report or ban tools yet (docs/ROADMAP.md).

## PC players (Index, Vive, Quest Link)

The Meta store covers Quest only. PC players get `ProjectArena-<ver>-PC.zip` from the release folder: unzip, start
SteamVR, then run ProjectArena.exe (docs/PC_README.txt). Steam distribution would be a separate Steamworks app.
