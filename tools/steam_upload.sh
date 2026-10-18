#!/usr/bin/env bash
# Uploads the PC build to Steam with SteamPipe (docs/STEAM.md). Builds build/pc/ProjectArena (tools/build_pc.bat),
# copies it to build/steam/content, writes the app / depot build scripts and runs steamcmd.
#
# Settings: steam.local.cfg in the project root (gitignored), for example
#   STEAM_APP_ID=1234560        # the Playtest's app ID (or the main game's)
#   STEAM_DEPOT_ID=1234561      # its Windows depot
#   STEAM_USER=projectarena_build
#   STEAMCMD=/c/steamworks_sdk/tools/ContentBuilder/builder/steamcmd.exe
#   STEAM_BRANCH=testing        # set live on this branch after the upload ("" = don't set live)
#
# steamcmd asks for the password and the Steam Guard code itself: never put them in this file or on the command line.
# Steam does not let a script set the "default" branch live; do that on the app's Builds page.
# Usage: ./tools/steam_upload.sh [--preview] [--no-build]
#   --preview   check the upload (logs and manifest in build/steam/output) without sending anything
#   --no-build  upload build/pc/ProjectArena as it is
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
CFG="$ROOT/steam.local.cfg"
[ -f "$CFG" ] || { echo "missing $CFG (see the top of this script)" >&2; exit 1; }
# shellcheck disable=SC1090
. "$CFG"
: "${STEAM_APP_ID:?set STEAM_APP_ID in steam.local.cfg}" "${STEAM_DEPOT_ID:?set STEAM_DEPOT_ID}" "${STEAM_USER:?set STEAM_USER}"
: "${STEAMCMD:?set STEAMCMD (steamcmd.exe in the Steamworks SDK)}"
STEAM_BRANCH="${STEAM_BRANCH-testing}"
PREVIEW=0; BUILD=1
for a in "$@"; do
  case "$a" in --preview) PREVIEW=1 ;; --no-build) BUILD=0 ;; *) echo "unknown option $a" >&2; exit 1 ;; esac
done
[ "$STEAM_BRANCH" = "default" ] && { echo "Steam can't set the default branch live from a script: use STEAM_BRANCH=testing, then the Builds page" >&2; exit 1; }
[ -x "$STEAMCMD" ] || [ -f "$STEAMCMD" ] || { echo "steamcmd not found at $STEAMCMD" >&2; exit 1; }

VER=$(sed -n 's/.*android:versionName="\([^"]*\)".*/\1/p' "$ROOT/app/AndroidManifest.xml")
if [ "$BUILD" = 1 ]; then cmd.exe //c "$(cygpath -w "$ROOT/tools/build_pc.bat")" | tail -1; fi
PC="$ROOT/build/pc/ProjectArena"
[ -f "$PC/ProjectArena.exe" ] && [ -d "$PC/assets" ] || { echo "no PC build in $PC" >&2; exit 1; }
# the exe must carry this version (kAppVersion in app/src/main.cpp)
grep -aq "$VER" "$PC/ProjectArena.exe" || { echo "ProjectArena.exe does not contain version $VER: rebuild" >&2; exit 1; }

S="$ROOT/build/steam"
rm -rf "$S/content"; mkdir -p "$S/content" "$S/output" "$S/scripts"
cp -r "$PC/." "$S/content/"
W() { cygpath -w "$1"; }
cat > "$S/scripts/depot_build_$STEAM_DEPOT_ID.vdf" <<EOF
"DepotBuild"
{
	"DepotID" "$STEAM_DEPOT_ID"
	"FileMapping"
	{
		"LocalPath" "*"
		"DepotPath" "."
		"Recursive" "1"
	}
	"FileExclusion" "*.pdb"
}
EOF
cat > "$S/scripts/app_build_$STEAM_APP_ID.vdf" <<EOF
"AppBuild"
{
	"AppID" "$STEAM_APP_ID"
	"Desc" "Project Arena $VER (PC VR + spectator window)"
	"Preview" "$PREVIEW"
	"ContentRoot" "$(W "$S/content")\\"
	"BuildOutput" "$(W "$S/output")\\"
	"SetLive" "$STEAM_BRANCH"
	"Depots"
	{
		"$STEAM_DEPOT_ID" "depot_build_$STEAM_DEPOT_ID.vdf"
	}
}
EOF
echo "uploading Project Arena $VER to app $STEAM_APP_ID depot $STEAM_DEPOT_ID$([ "$PREVIEW" = 1 ] && echo ' (preview: nothing is sent)')"
echo "steamcmd will ask for the password and Steam Guard code of $STEAM_USER"
"$STEAMCMD" +login "$STEAM_USER" +run_app_build "$(W "$S/scripts/app_build_$STEAM_APP_ID.vdf")" +quit
echo "done: check the logs in $S/output (steamcmd's console can say OK when a depot failed)"
[ -n "$STEAM_BRANCH" ] && [ "$PREVIEW" = 0 ] && echo "set live on branch '$STEAM_BRANCH'; testers pick it under Properties > Betas"
exit 0
