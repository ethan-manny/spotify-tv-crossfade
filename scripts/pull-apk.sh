#!/usr/bin/env bash
# Pulls the installed Spotify TV APK from the Fire Stick into apk/base.apk.
set -euo pipefail
cd "$(dirname "$0")/.."
source scripts/env.sh
require_device
adb -s "$DEVICE" get-state >/dev/null || { echo "device $DEVICE not authorized/online"; exit 1; }
mkdir -p apk
REMOTE=$(adb -s "$DEVICE" shell pm path com.spotify.tv.android | tr -d '\r' | sed 's/^package://' | head -1)
[ -n "$REMOTE" ] || { echo "com.spotify.tv.android not installed"; exit 1; }
# MSYS would rewrite a leading /data path; disable path conversion for this call.
MSYS_NO_PATHCONV=1 adb -s "$DEVICE" pull "$REMOTE" apk/base.apk
adb -s "$DEVICE" shell dumpsys package com.spotify.tv.android | grep -E "versionName|versionCode" | head -2

# morphe-desktop 1.15.0 fails to sign its output when the input APK uses streamed zip
# entries (data descriptors), which the APK pulled from the device does; normalize them.
python - <<'EOF'
import os, zipfile
src = "apk/base.apk"; tmp = src + ".tmp"
with zipfile.ZipFile(src) as zin, zipfile.ZipFile(tmp, "w") as zout:
    for info in zin.infolist():
        out = zipfile.ZipInfo(info.filename, date_time=info.date_time)
        out.compress_type = info.compress_type
        out.external_attr = info.external_attr
        zout.writestr(out, zin.read(info.filename))
os.replace(tmp, src)
with zipfile.ZipFile(src) as z:
    assert not any(i.flag_bits & 0x8 for i in z.infolist()), "data descriptors still present"
print("normalized zip container:", src)
EOF

ls -la apk/base.apk
