#!/usr/bin/env bash
# Builds the signed release APK and a release folder for GitHub: APK, SHA-256, release notes, install guide.
# Needs the release key (docs/META_STORE.md). Default key: ~/projectarena-release.jks, alias projectarena; the password is
# asked for when the script runs (never put it on the command line). Override with PROJECTARENA_KEYSTORE / PROJECTARENA_KEY_ALIAS.
# Usage: ./tools/make_release.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
export PROJECTARENA_KEYSTORE="${PROJECTARENA_KEYSTORE:-$HOME/projectarena-release.jks}"
export PROJECTARENA_KEY_ALIAS="${PROJECTARENA_KEY_ALIAS:-projectarena}"
if [ ! -f "$PROJECTARENA_KEYSTORE" ]; then
  echo "release key not found at $PROJECTARENA_KEYSTORE; refusing to publish a debug-signed build" >&2; exit 1
fi
if [ -z "${PROJECTARENA_KEY_PASS:-}" ]; then
  read -rsp "Password for $PROJECTARENA_KEYSTORE: " PROJECTARENA_KEY_PASS; echo
  export PROJECTARENA_KEY_PASS
fi
VER=$(sed -n 's/.*android:versionName="\([^"]*\)".*/\1/p' "$ROOT/app/AndroidManifest.xml")
cmd.exe //c "$(cygpath -w "$ROOT/tools/run_tests.bat")" | tail -1
python -I "$ROOT/server/test_relay.py" | tail -1
"$ROOT/build.sh" release open
APK="$ROOT/build/ProjectArena-release-open.apk"
python -I "$ROOT/tools/check_store_apk.py" "$APK" --release
OUT="$ROOT/build/release-$VER"
rm -rf "$OUT"; mkdir -p "$OUT"
cp "$APK" "$OUT/ProjectArena-$VER-store.apk"   # for the Meta upload (your key); not for GitHub
# GitHub: the same build signed with the anonymous key (no personal details in the certificate; 2026-10-17).
# Key ~/projectarena-github.jks, alias projectarena-github, password in ~/.projectarena-github-key.pass.
GHKS="${PROJECTARENA_GITHUB_KEYSTORE:-$HOME/projectarena-github.jks}"
BT="${ANDROID_HOME:-/c/Android/sdk}/build-tools/34.0.0"
"$BT/apksigner.bat" sign --ks "$(cygpath -w "$GHKS")" --ks-pass "file:$(cygpath -w "$HOME/.projectarena-github-key.pass")"   --ks-key-alias projectarena-github --out "$OUT/ProjectArena-$VER.apk" "$ROOT/build/release-open/aligned.apk"
"$BT/apksigner.bat" verify --print-certs "$OUT/ProjectArena-$VER.apk" | grep -i "certificate DN"
cp "$ROOT/docs/INSTALL.md" "$OUT/INSTALL.md"
[ -f "$ROOT/docs/RELEASE_NOTES_v$VER.md" ] && cp "$ROOT/docs/RELEASE_NOTES_v$VER.md" "$OUT/RELEASE_NOTES.md"
cp "$ROOT/docs/PC_README.txt" "$OUT/README-PC.txt"
sed "s/<version>/$VER/g" "$ROOT/docs/QUEST_README.txt" > "$OUT/README-Quest.txt"
# PC VR build (Windows): ProjectArena-<ver>-PC.zip
cmd.exe //c "$(cygpath -w "$ROOT/tools/build_pc.bat")" | tail -1
(cd "$ROOT/build/pc" && rm -f "$OUT/ProjectArena-$VER-PC.zip" && powershell.exe -NoProfile -Command   "Compress-Archive -Path ProjectArena -DestinationPath '$(cygpath -w "$OUT")\ProjectArena-$VER-PC.zip'")
(cd "$OUT" && sha256sum ProjectArena-$VER.apk ProjectArena-$VER-PC.zip README-PC.txt README-Quest.txt > SHA256SUMS.txt)
echo "release folder: $OUT  (GitHub: ProjectArena-$VER.apk; Meta upload: ProjectArena-$VER-store.apk)"
