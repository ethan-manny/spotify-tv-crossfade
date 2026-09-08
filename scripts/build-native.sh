#!/usr/bin/env bash
# Builds libxfade.so and xfade_tests with ndk-build, then stages the .so into the patch resources.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/env.sh
"$NDK_POSIX/ndk-build.cmd" NDK_PROJECT_PATH=native NDK_APPLICATION_MK=native/jni/Application.mk APP_BUILD_SCRIPT=native/jni/Android.mk -j8
mkdir -p patches/src/main/resources/xfade/armeabi-v7a
cp native/libs/armeabi-v7a/libxfade.so patches/src/main/resources/xfade/armeabi-v7a/libxfade.so
ls -la native/libs/armeabi-v7a/
