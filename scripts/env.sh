#!/usr/bin/env bash
# Source this file: `source scripts/env.sh`. Sets up JDK, SDK, NDK, device and GitHub Packages creds.
#
# Values come from, in order: the environment, then scripts/env.local.sh (git-ignored, copy it from
# scripts/env.local.sh.example). ANDROID_HOME and ANDROID_NDK_HOME must end up set; JAVA_HOME and
# DEVICE are optional.

# shellcheck disable=SC1091
if [ -f "$(dirname "${BASH_SOURCE[0]}")/env.local.sh" ]; then
  source "$(dirname "${BASH_SOURCE[0]}")/env.local.sh"
fi

if [ -z "${ANDROID_HOME:-}" ] || [ -z "${ANDROID_NDK_HOME:-}" ]; then
  echo "ANDROID_HOME and ANDROID_NDK_HOME must be set (export them, or copy" >&2
  echo "scripts/env.local.sh.example to scripts/env.local.sh and fill it in)." >&2
  return 1 2>/dev/null || exit 1
fi

export ANDROID_HOME
export ANDROID_SDK_ROOT="$ANDROID_HOME"
export ANDROID_NDK_HOME

# POSIX forms of the two Windows paths for the shell scripts; cygpath is present under Git Bash and
# MSYS2. Without it the values are assumed to be POSIX already.
if command -v cygpath >/dev/null 2>&1; then
  export SDK_POSIX="$(cygpath -u "$ANDROID_HOME")"
  export NDK_POSIX="$(cygpath -u "$ANDROID_NDK_HOME")"
else
  export SDK_POSIX="$ANDROID_HOME"
  export NDK_POSIX="$ANDROID_NDK_HOME"
fi

if [ -n "${JAVA_HOME:-}" ]; then
  export JAVA_HOME
  export PATH="$JAVA_HOME/bin:$PATH"
fi
export PATH="$SDK_POSIX/platform-tools:$PATH"

# Read by settings.gradle.kts for the Morphe Gradle plugin on GitHub Packages.
export GITHUB_ACTOR="${GITHUB_ACTOR:-$(gh api user -q .login 2>/dev/null || echo)}"
export GITHUB_TOKEN="${GITHUB_TOKEN:-$(gh auth token 2>/dev/null || echo)}"

# adb target: an adb serial, or "host:port" for a network device. Only the scripts that talk to a
# device need it (see require_device below).
export DEVICE="${DEVICE:-}"
export MORPHE_DESKTOP_JAR="${MORPHE_DESKTOP_JAR:-tools/morphe-desktop-1.15.0-all.jar}"

# Used by the scripts that talk to a device: fails with a clear message when no target is
# configured, and connects first when DEVICE is a network address.
require_device() {
  if [ -z "${DEVICE:-}" ]; then
    echo "DEVICE is not set: export it or put it in scripts/env.local.sh" >&2
    echo "(an adb serial from 'adb devices', or host:port for a network device)." >&2
    exit 1
  fi
  case "$DEVICE" in
    *:*) adb connect "$DEVICE" >/dev/null ;;
  esac
}
