#!/usr/bin/env bash
# Builds the release APKs and a release folder for GitHub: APK, SHA-256, release notes, install guide, PC zip.
# Usage: ./tools/make_release.sh [--github-only | --store-only]
#   (no option)    everything: the store APK with your release key (asks for its password) and the GitHub files
#   --github-only  the GitHub files only, signed with the anonymous GitHub key (no password asked)
#   --store-only   only ProjectArena-<ver>-store.apk for the Meta upload (your key; asks for the password), added to
#                  the release folder without touching the GitHub files there (their SHA256SUMS stay valid)
# Your key: ~/projectarena-release.jks, alias projectarena (docs/META_STORE.md); never put its password on the command
# line. Override with PROJECTARENA_KEYSTORE / PROJECTARENA_KEY_ALIAS. The GitHub key (no personal details in its
# certificate; 2026-10-17): ~/projectarena-github.jks, alias projectarena-github, password in ~/.projectarena-github-key.pass.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
WHAT="${1:-all}"
case "$WHAT" in all|--github-only|--store-only) ;; *) echo "unknown option $WHAT" >&2; exit 1 ;; esac
VER=$(sed -n 's/.*android:versionName="\([^"]*\)".*/\1/p' "$ROOT/app/AndroidManifest.xml")
OUT="$ROOT/build/release-$VER"
GHKS="${PROJECTARENA_GITHUB_KEYSTORE:-$HOME/projectarena-github.jks}"
BT="${ANDROID_HOME:-/c/Android/sdk}/build-tools/34.0.0"
APK="$ROOT/build/ProjectArena-release-open.apk"

if [ "$WHAT" != --github-only ]; then   # your key
  export PROJECTARENA_KEYSTORE="${PROJECTARENA_KEYSTORE:-$HOME/projectarena-release.jks}"
  export PROJECTARENA_KEY_ALIAS="${PROJECTARENA_KEY_ALIAS:-projectarena}"
  if [ ! -f "$PROJECTARENA_KEYSTORE" ]; then
    echo "release key not found at $PROJECTARENA_KEYSTORE; refusing to publish a debug-signed build" >&2; exit 1
  fi
  if [ -z "${PROJECTARENA_KEY_PASS:-}" ]; then
    read -rsp "Password for $PROJECTARENA_KEYSTORE: " PROJECTARENA_KEY_PASS; echo
    export PROJECTARENA_KEY_PASS
  fi
else                                     # the anonymous GitHub key signs the build directly
  [ -f "$GHKS" ] && [ -f "$HOME/.projectarena-github-key.pass" ] || { echo "GitHub key or its password file missing" >&2; exit 1; }
  export PROJECTARENA_KEYSTORE="$GHKS" PROJECTARENA_KEY_ALIAS=projectarena-github
  PROJECTARENA_KEY_PASS="$(cat "$HOME/.projectarena-github-key.pass")"
  export PROJECTARENA_KEY_PASS
fi

cmd.exe //c "$(cygpath -w "$ROOT/tools/run_tests.bat")" | tail -1
python -I "$ROOT/server/test_relay.py" | tail -1
"$ROOT/build.sh" release open
python -I "$ROOT/tools/check_store_apk.py" "$APK" --release

if [ "$WHAT" = --store-only ]; then
  mkdir -p "$OUT"
  cp "$APK" "$OUT/ProjectArena-$VER-store.apk"
  "$BT/apksigner.bat" verify --print-certs "$OUT/ProjectArena-$VER-store.apk" | grep -i "certificate DN"
  echo "store APK: $OUT/ProjectArena-$VER-store.apk (upload it in the Meta dashboard; not for GitHub)"
  exit 0
fi

rm -rf "$OUT"; mkdir -p "$OUT"
if [ "$WHAT" = --github-only ]; then
  cp "$APK" "$OUT/ProjectArena-$VER.apk"
else
  cp "$APK" "$OUT/ProjectArena-$VER-store.apk"   # for the Meta upload (your key); not for GitHub
  "$BT/apksigner.bat" sign --ks "$(cygpath -w "$GHKS")" --ks-pass "file:$(cygpath -w "$HOME/.projectarena-github-key.pass")" \
    --ks-key-alias projectarena-github --out "$OUT/ProjectArena-$VER.apk" "$ROOT/build/release-open/aligned.apk"
fi
"$BT/apksigner.bat" verify --print-certs "$OUT/ProjectArena-$VER.apk" | grep -i "certificate DN"
cp "$ROOT/docs/INSTALL.md" "$OUT/INSTALL.md"
[ -f "$ROOT/docs/RELEASE_NOTES_v$VER.md" ] && cp "$ROOT/docs/RELEASE_NOTES_v$VER.md" "$OUT/RELEASE_NOTES.md"
cp "$ROOT/docs/PC_README.txt" "$OUT/README-PC.txt"
sed "s/<version>/$VER/g" "$ROOT/docs/QUEST_README.txt" > "$OUT/README-Quest.txt"
# PC VR build (Windows): ProjectArena-<ver>-PC.zip
cmd.exe //c "$(cygpath -w "$ROOT/tools/build_pc.bat")" | tail -1
python -I "$ROOT/tools/check_pc_icon.py" "$(cygpath -w "$ROOT/build/pc/ProjectArena/ProjectArena.exe")" || exit 1   # every release has its icon
(cd "$ROOT/build/pc" && rm -f "$OUT/ProjectArena-$VER-PC.zip" && powershell.exe -NoProfile -Command \
  "Compress-Archive -Path ProjectArena -DestinationPath '$(cygpath -w "$OUT")\ProjectArena-$VER-PC.zip'")
(cd "$OUT" && sha256sum ProjectArena-$VER.apk ProjectArena-$VER-PC.zip README-PC.txt README-Quest.txt > SHA256SUMS.txt)
echo "release folder: $OUT  (GitHub: ProjectArena-$VER.apk$([ "$WHAT" = all ] && echo "; Meta upload: ProjectArena-$VER-store.apk"))"
