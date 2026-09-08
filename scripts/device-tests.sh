#!/usr/bin/env bash
# Builds the native test executable, pushes it to the Fire Stick and runs it. Extra args are passed through
# (e.g. a name filter, or --real to include tests that play audio).
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/env.sh
scripts/build-native.sh >/dev/null
require_device
MSYS_NO_PATHCONV=1 adb -s "$DEVICE" push native/libs/armeabi-v7a/xfade_tests /data/local/tmp/xfade_tests >/dev/null
REMOTE_ARGS=""
for a in "$@"; do
  case "$a" in *"'"*) echo "argument must not contain a single quote: $a" >&2; exit 1 ;; esac
  REMOTE_ARGS+=" '$a'"
done
MSYS_NO_PATHCONV=1 adb -s "$DEVICE" shell "chmod 755 /data/local/tmp/xfade_tests && /data/local/tmp/xfade_tests$REMOTE_ARGS; echo EXIT=\$?" | tr -d '\r' | tee /tmp/xfade_tests.out
grep -qx "EXIT=0" /tmp/xfade_tests.out
