#!/usr/bin/env bash
# Applies the built patch bundle to apk/base.apk, verifies the result, optionally installs it.
# Flags: --install  install to $DEVICE after verifying
#        --debug    also enable the "Debuggable build" patch
#        --no-lib   skip the native-library checks (before Task 10 exists)
#        --no-hook  skip the extension-hook checks (before Task 11 exists)
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/env.sh
INSTALL=0; VERIFY=(--expect-lib --expect-hook); EXTRA=()
for a in "$@"; do
  case "$a" in
    --install) INSTALL=1 ;;
    --debug)   EXTRA+=(-e "Debuggable build"); VERIFY+=(--debug) ;;
    --no-lib)  VERIFY=("${VERIFY[@]/--expect-lib}") ;;
    --no-hook) VERIFY=("${VERIFY[@]/--expect-hook}") ;;
  esac
done
shopt -s nullglob
MPPS=(patches/build/libs/patches-*.mpp)
shopt -u nullglob
if [ ${#MPPS[@]} -eq 0 ]; then
  echo "no patch bundle in patches/build/libs; run ./gradlew buildAndroid"
  exit 1
elif [ ${#MPPS[@]} -gt 1 ]; then
  echo "ambiguous patch bundle in patches/build/libs: ${MPPS[*]}"
  exit 1
fi
MPP="${MPPS[0]}"
[ -f apk/base.apk ] || { echo "apk/base.apk missing; run scripts/pull-apk.sh"; exit 1; }
mkdir -p out tools
java -jar "$MORPHE_DESKTOP_JAR" patch \
  -p "$MPP" "${EXTRA[@]}" \
  --keystore tools/morphe.keystore \
  -o out/spotify-tv-crossfade.apk \
  apk/base.apk
python scripts/verify-apk.py out/spotify-tv-crossfade.apk ${VERIFY[@]}
if [ "$INSTALL" = 1 ]; then
  require_device
  adb -s "$DEVICE" install -r out/spotify-tv-crossfade.apk
fi
