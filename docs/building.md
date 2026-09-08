# Building

Everything here runs from the repository root in Git Bash on Windows. The scripts are POSIX shell
and call the Windows NDK and SDK tools directly.

## Prerequisites

- **JDK 21** (the JBR that ships with Android Studio works).
- **Android SDK** with build-tools installed. `verify-apk.py` uses `aapt2` and `dexdump` from the
  newest build-tools directory it finds.
- **Android NDK r27c**. `scripts/setup-tools.sh` downloads it into the SDK if it is missing.
- **morphe-desktop 1.15.0**, the CLI that applies a patch bundle to an APK.
  `scripts/setup-tools.sh` downloads it into `tools/`.
- **adb** (from the SDK's platform-tools) with the device reachable, for the device scripts only.
- **Git Bash** (or MSYS2): the scripts use `cygpath`, `curl`, `unzip` and `python`.
- **A GitHub token with `read:packages`**. The Morphe Gradle plugin is published to GitHub
  Packages, which requires authentication even for public packages. `scripts/env.sh` fills
  `GITHUB_ACTOR` and `GITHUB_TOKEN` from `gh api user` and `gh auth token`, so `gh auth login` with
  that scope is enough; `settings.gradle.kts` also accepts the `gpr.user` and `gpr.key` Gradle
  properties.

## Environment

`scripts/env.sh` is sourced by every other script and can be sourced directly in a shell. It takes
`JAVA_HOME`, `ANDROID_HOME`, `ANDROID_NDK_HOME` and `DEVICE` from the environment, then sources
`scripts/env.local.sh` if it exists, derives the POSIX forms of the SDK and NDK paths with
`cygpath`, and fails with a message if the SDK or NDK path is still unset.

```bash
cp scripts/env.local.sh.example scripts/env.local.sh   # then edit it
source scripts/env.sh
```

`scripts/env.local.sh` is git-ignored: it is where machine-specific paths belong. `DEVICE` is an
adb serial, or `host:port` for a device attached over the network; only the scripts that talk to a
device need it.

## Scripts

| Script | What it does |
|---|---|
| `scripts/env.sh` | sets `JAVA_HOME`, `ANDROID_HOME`, `ANDROID_SDK_ROOT`, `ANDROID_NDK_HOME`, `SDK_POSIX`, `NDK_POSIX`, `PATH`, `GITHUB_ACTOR`, `GITHUB_TOKEN`, `DEVICE`, `MORPHE_DESKTOP_JAR`, and defines `require_device` |
| `scripts/setup-tools.sh` | downloads the NDK r27c into `$ANDROID_HOME/ndk` and morphe-desktop into `tools/` if either is missing, then prints the morphe-desktop version. Safe to re-run |
| `scripts/pull-apk.sh` | pulls the installed Spotify TV APK into `apk/base.apk`, prints its version, and rewrites the zip container without data descriptors (morphe-desktop 1.15.0 cannot sign a streamed-entry APK) |
| `scripts/build-native.sh` | runs `ndk-build` for both `libxfade.so` and the test executable, then copies the library into `patches/src/main/resources/xfade/armeabi-v7a/` |
| `scripts/device-tests.sh` | builds, pushes `xfade_tests` to the device and runs it. Extra arguments are passed through: a substring filter for test names, or `--real` to include the tests that play audio |
| `scripts/patch-and-install.sh` | applies the built `.mpp` to `apk/base.apk`, verifies the result and optionally installs it. Flags: `--install`, `--debug` (also enable the Debuggable build patch), `--no-lib`, `--no-hook` |
| `scripts/verify-apk.py` | structural checks on a patched APK (see below) |

## Build, test, patch, install

```bash
source scripts/env.sh
bash scripts/setup-tools.sh                   # first time only
bash scripts/pull-apk.sh                      # first time, or after an app update

bash scripts/build-native.sh                  # libxfade.so -> patches/src/main/resources/xfade/
./gradlew buildAndroid --no-daemon            # -> patches/build/libs/patches-*.mpp
bash scripts/patch-and-install.sh --install   # patch + verify + adb install -r
```

`buildAndroid` builds the extension and packs it with the Kotlin patches and the staged native
library into the `.mpp` bundle. Always run `scripts/build-native.sh` before it after touching
anything under `native/`: the bundle carries a copy of the library, and the verifier compares the
copy inside the patched APK against the staged one.

## Tests

```bash
bash scripts/device-tests.sh                              # native tests on the device
bash scripts/device-tests.sh fade_reanchor                # only tests whose name contains this
bash scripts/device-tests.sh --real                       # also the test that plays a tone
./gradlew :extensions:extension:testDebugUnitTest         # JVM tests for the Java extension
```

See [testing.md](testing.md) for what they cover and for the manual device checklist.

## What the verifier checks

`python scripts/verify-apk.py out/spotify-tv-crossfade.apk --expect-lib --expect-hook` exits 0 and
prints `OK` when all of these hold:

- `android:extractNativeLibs="true"` in the manifest, and `android:debuggable="true"` too when
  `--debug` is passed;
- with `--expect-lib`: both `lib/armeabi-v7a/libxfade.so` and `lib/armeabi-v7a/libspotify_tv_jni.so`
  are present, the eSDK no longer mentions `libOpenSLES.so`, its `libxfade.so` dependency string
  appears exactly once, and the library in the APK is byte-identical to the staged one;
- the eleven Amazon `Kiwi` DRM lifecycle methods start with the expected no-op instruction;
- with `--expect-hook`: the four extension classes are in the dex, `SpotifyTVApplication.onCreate`
  calls `CrossfadeHooks.init`, the metadata lambda calls `onMetadataJson`,
  `SpotifyTVActivity.dispatchKeyEvent` calls `onKeyEvent`, and `CrossfadeSettingsActivity` is
  declared in the manifest.

## Releases

`main` and `dev` are released by semantic-release from `.github/workflows/release.yml`, driven by
conventional commit messages (`fix`, `feat`, `bump`, `perf`). It builds the bundle, uploads
`patches-<version>.mpp` to a GitHub release and commits the files it regenerates.

Do not edit these by hand: `CHANGELOG.md`, the `version` in `gradle.properties`,
`patches-bundle.json`, `patches-list.json`, and the block between the `PATCHES_START` and
`PATCHES_END` markers in `README.md`.

Before the first public release, set `source` and `website` in the `about` block of
`patches/build.gradle.kts` to the repository URL. They currently hold the placeholders `local` and
`na`, and the Morphe app shows them to users.

## Installing the bundle in the Morphe app

1. Open Morphe, go to the patch bundle sources and add this bundle: **Local** with the `.mpp` file
   from `patches/build/libs/`, or **Remote** with the release URL once the repository is published.
2. Enable **Expert mode** so a bundle that is not from the default source can be selected.
3. Select the Spotify TV APK, enable the **Crossfade** and **Disable Amazon Appstore DRM** patches,
   and patch.
4. Export the patched APK and sideload it onto the device, for example with
   `adb install -r <apk>`.

`scripts/patch-and-install.sh` does the same thing headlessly with morphe-desktop, which is usually
faster during development.
