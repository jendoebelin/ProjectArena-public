#!/usr/bin/env bash
# Uploads the PC VR build to Steam (SteamPipe) and sets it live on the BETA branch, for testers to try before it goes
# to everyone. Making it the default build is done by hand in Steamworks (App Admin > SteamPipe > Builds).
#
# Usage: ./tools/steam_upload.sh            (after ./tools/make_release.sh --github-only, which builds build/pc/ProjectArena)
#
# Needs, once (the owner's steps):
#   1. Steamworks approval, the app's App ID and its Windows depot ID (Steamworks > App Admin > SteamPipe > Depots),
#      and a branch called "beta" (SteamPipe > Builds > Manage beta branches; a password is optional).
#   2. steamcmd from Valve (partner.steamgames.com/doc/sdk/uploading), unzipped to ~/steamcmd (or set STEAMCMD).
#   3. ~/.projectarena-steam.cfg with three lines (kept out of the repository):
#        APP_ID=<the App ID>
#        DEPOT_ID=<the Windows depot ID>
#        STEAM_USER=<the Steam account that uploads builds>
#   The first upload asks for that account's password and Steam Guard code in steamcmd itself (they are typed there,
#   never stored here); steamcmd remembers the sign-in after that.
#
# The same build as the GitHub zip: the game sees that Steam installed it (its folder is under steamapps) and turns
# its own UPDATE NOW off, and its version line and bug reports say Steam.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CFG="${PROJECTARENA_STEAM_CFG:-$HOME/.projectarena-steam.cfg}"
STEAMCMD="${STEAMCMD:-$HOME/steamcmd/steamcmd.exe}"
VER=$(sed -n 's/.*android:versionName="\([^"]*\)".*/\1/p' "$ROOT/app/AndroidManifest.xml")
CONTENT="$ROOT/build/pc/ProjectArena"
WORK="$ROOT/build/steam"

[ -f "$CFG" ] || { echo "no $CFG yet: see the top of this script (App ID, depot ID, Steam account)" >&2; exit 1; }
APP_ID=$(sed -n 's/^APP_ID=\([0-9]*\).*/\1/p' "$CFG")
DEPOT_ID=$(sed -n 's/^DEPOT_ID=\([0-9]*\).*/\1/p' "$CFG")
STEAM_USER=$(sed -n 's/^STEAM_USER=\(.*\)$/\1/p' "$CFG" | tr -d '\r')
[ -n "$APP_ID" ] && [ -n "$DEPOT_ID" ] && [ -n "$STEAM_USER" ] || { echo "$CFG needs APP_ID, DEPOT_ID and STEAM_USER" >&2; exit 1; }
[ -x "$STEAMCMD" ] || [ -f "$STEAMCMD" ] || { echo "steamcmd not found at $STEAMCMD (set STEAMCMD)" >&2; exit 1; }
[ -f "$CONTENT/ProjectArena.exe" ] || { echo "no PC build at $CONTENT: run ./tools/make_release.sh --github-only first" >&2; exit 1; }
# the build must be this version's (a stale folder would ship old code)
grep -aq "$VER" "$CONTENT/ProjectArena.exe" || { echo "$CONTENT/ProjectArena.exe is not $VER: build it again" >&2; exit 1; }

mkdir -p "$WORK/output"
W() { cygpath -w "$1" | sed 's/\\/\\\\/g'; }
cat > "$WORK/depot_build.vdf" <<EOF
"DepotBuild"
{
	"DepotID" "$DEPOT_ID"
	"FileMapping"
	{
		"LocalPath" "*"
		"DepotPath" "."
		"Recursive" "1"
	}
	"FileExclusion" "*.pdb"
	"FileExclusion" "update_probe.tmp"
	"FileExclusion" "*.log"
}
EOF
cat > "$WORK/app_build.vdf" <<EOF
"AppBuild"
{
	"AppID" "$APP_ID"
	"Desc" "Project Arena $VER"
	"ContentRoot" "$(W "$CONTENT")"
	"BuildOutput" "$(W "$WORK/output")"
	"SetLive" "beta"
	"Depots"
	{
		"$DEPOT_ID" "$(W "$WORK/depot_build.vdf")"
	}
}
EOF
echo "uploading Project Arena $VER (app $APP_ID, depot $DEPOT_ID) to Steam's beta branch as $STEAM_USER ..."
"$STEAMCMD" +login "$STEAM_USER" +run_app_build "$(cygpath -w "$WORK/app_build.vdf")" +quit
echo "done: Project Arena $VER is live on the beta branch. Test it (Steam > Project Arena > Properties > Betas), then"
echo "make it the default build in Steamworks (App Admin > SteamPipe > Builds) when it's good."
