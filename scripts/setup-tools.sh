#!/usr/bin/env bash
# Downloads the Android NDK r27c and morphe-desktop if they are not present. Safe to re-run.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/env.sh
mkdir -p tools
if [ ! -f "$NDK_POSIX/ndk-build.cmd" ]; then
  echo "Downloading NDK r27c (781 MB)..."
  mkdir -p "$SDK_POSIX/ndk"
  curl -L -o "$SDK_POSIX/ndk/android-ndk-r27c-windows.zip" https://dl.google.com/android/repository/android-ndk-r27c-windows.zip
  (cd "$SDK_POSIX/ndk" && unzip -q android-ndk-r27c-windows.zip && rm android-ndk-r27c-windows.zip)
fi
[ -f "$NDK_POSIX/ndk-build.cmd" ] && echo "NDK ok: $NDK_POSIX"
if [ ! -f "$MORPHE_DESKTOP_JAR" ]; then
  echo "Downloading morphe-desktop 1.15.0..."
  curl -L -o "$MORPHE_DESKTOP_JAR" https://github.com/MorpheApp/morphe-desktop/releases/download/v1.15.0/morphe-desktop-1.15.0-all.jar
fi
java -jar "$MORPHE_DESKTOP_JAR" --version
