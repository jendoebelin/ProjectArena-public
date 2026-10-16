#!/usr/bin/env bash
# Builds app/tests/net_probe.cpp for Quest (arm64) -> build/net_probe. Push and run with adb:
#   adb push build/net_probe /data/local/tmp/ && adb shell /data/local/tmp/net_probe host 30 <server:port> NAME
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
NDK="${ANDROID_NDK_HOME:-/c/Android/sdk/ndk/26.3.11579264}"
case "$(uname -s)" in MINGW*|MSYS*|CYGWIN*) HOST=windows-x86_64; EXE=.cmd ;; Darwin) HOST=darwin-x86_64; EXE= ;; *) HOST=linux-x86_64; EXE= ;; esac
CXX="$NDK/toolchains/llvm/prebuilt/$HOST/bin/aarch64-linux-android29-clang++$EXE"
"$CXX" -std=c++17 -O2 -static-libstdc++ -I"$ROOT/app/src" -o "$ROOT/build/net_probe" \
  "$ROOT/app/tests/net_probe.cpp" "$ROOT/app/src/net.cpp" "$ROOT/app/src/game.cpp" "$ROOT/app/src/avatar.cpp" "$ROOT/app/src/log.cpp" -llog
echo "built build/net_probe"
