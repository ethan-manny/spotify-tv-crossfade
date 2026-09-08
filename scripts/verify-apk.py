#!/usr/bin/env python3
"""Structural checks on a patched Spotify TV APK.

Usage: verify-apk.py <apk> [--expect-lib] [--expect-hook]
Exit code 0 = all checks passed. Prints the first failing check otherwise.
"""
import glob, hashlib, os, re, subprocess, sys, tempfile, zipfile

SDK = os.environ.get("ANDROID_HOME")
if not SDK:
    print("FAIL: ANDROID_HOME is not set (source scripts/env.sh first)")
    sys.exit(1)
_BUILD_TOOLS = sorted(glob.glob(os.path.join(SDK, "build-tools", "*")))
if not _BUILD_TOOLS:
    print(f"FAIL: no build-tools under {SDK}")
    sys.exit(1)
BUILD_TOOLS = _BUILD_TOOLS[-1]


def tool(name):
    return os.path.join(BUILD_TOOLS, name)


def fail(msg):
    print("FAIL:", msg)
    sys.exit(1)


def run(args):
    return subprocess.run(args, capture_output=True, text=True, errors="replace").stdout


def method_body(dump, cls, name, sig):
    """Return the dexdump text of one method (from its header to the next method), or None."""
    pat = re.compile(
        r"\(in " + re.escape(cls) + r"\)\s*\n\s*name\s*:\s*'" + re.escape(name)
        + r"'\s*\n\s*type\s*:\s*'" + re.escape(sig) + r"'(.*?)(?=\n\s*#\d+\s*:|\Z)",
        re.S,
    )
    m = pat.search(dump)
    return m.group(1) if m else None


def method_body_any_sig(dump, cls, name):
    """Like method_body but ignores the signature (for methods whose return type is obfuscated)."""
    pat = re.compile(r"\(in " + re.escape(cls) + r"\)\s*\n\s*name\s*:\s*'" + re.escape(name) + r"'\s*\n\s*type\s*:\s*'[^']*'(.*?)(?=\n\s*#\d+\s*:|\Z)", re.S)
    m = pat.search(dump)
    return m.group(1) if m else None


def first_instruction(body):
    m = re.search(r"\|0000: (.*)", body)
    return m.group(1).strip() if m else ""


def main():
    apk = sys.argv[1]
    flags = set(sys.argv[2:])
    z = zipfile.ZipFile(apk)
    names = set(z.namelist())

    # 1. Manifest: extractNativeLibs must be true.
    xml = run([tool("aapt2.exe"), "dump", "xmltree", apk, "--file", "AndroidManifest.xml"])
    if not re.search(r"extractNativeLibs\([^)]*\)=true", xml):
        fail("android:extractNativeLibs=true not found in manifest")
    if "--debug" in flags and not re.search(r"debuggable\([^)]*\)=true", xml):
        fail("android:debuggable=true not found in manifest")

    # 2. Native libraries.
    if "--expect-lib" in flags:
        if "lib/armeabi-v7a/libxfade.so" not in names:
            fail("lib/armeabi-v7a/libxfade.so missing from APK")
        if "lib/armeabi-v7a/libspotify_tv_jni.so" not in names:
            fail("lib/armeabi-v7a/libspotify_tv_jni.so missing from APK")
        esdk = z.read("lib/armeabi-v7a/libspotify_tv_jni.so")
        if b"libOpenSLES.so" in esdk:
            fail("eSDK still references libOpenSLES.so")
        if esdk.count(b"libxfade.so\0") != 1:
            fail("eSDK dependency rename not present exactly once")
        bundled = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "patches", "src", "main",
                               "resources", "xfade", "armeabi-v7a", "libxfade.so")
        with open(bundled, "rb") as f:
            want = hashlib.sha256(f.read()).hexdigest()
        got = hashlib.sha256(z.read("lib/armeabi-v7a/libxfade.so")).hexdigest()
        if got != want:
            fail("libxfade.so in APK differs from patches/src/main/resources/... "
                 "(stale bundle? run scripts/build-native.sh && ./gradlew buildAndroid)")

    # 3. Dex: DRM no-ops and (optionally) the extension hook.
    tmp = tempfile.mkdtemp()
    dump = ""
    for n in sorted(x for x in names if re.fullmatch(r"classes\d*\.dex", x)):
        p = os.path.join(tmp, n)
        with open(p, "wb") as f:
            f.write(z.read(n))
        dump += run([tool("dexdump.exe"), "-d", p])

    kiwi = "Lcom/amazon/android/Kiwi;"
    checks = [
        ("onCreate", "(Landroid/app/Activity;Z)V", "return-void"),
        ("onCreate", "(Landroid/app/Service;Z)V", "return-void"),
        ("onStart", "(Landroid/app/Activity;)V", "return-void"),
        ("onResume", "(Landroid/app/Activity;)V", "return-void"),
        ("onPause", "(Landroid/app/Activity;)V", "return-void"),
        ("onStop", "(Landroid/app/Activity;)V", "return-void"),
        ("onDestroy", "(Landroid/app/Activity;)V", "return-void"),
        ("onDestroy", "(Landroid/app/Service;)V", "return-void"),
        ("onWindowFocusChanged", "(Landroid/app/Activity;Z)V", "return-void"),
        ("onActivityResult", "(Landroid/app/Activity;IILandroid/content/Intent;)Z", "const/4 v0, #int 0"),
        ("onCreateDialog", "(Landroid/app/Activity;I)Landroid/app/Dialog;", "const/4 v0, #int 0"),
    ]
    for name, sig, expect in checks:
        body = method_body(dump, kiwi, name, sig)
        if body is None:
            fail(f"Kiwi.{name}{sig} not found in dex")
        got = first_instruction(body)
        if not got.startswith(expect):
            fail(f"Kiwi.{name}{sig}: first instruction is '{got}', expected '{expect}'")

    if "--expect-hook" in flags:
        for cls in ("CrossfadeHooks", "CrossfadeSettingsActivity", "MetadataParser", "LongPressDetector"):
            if f"Ldev/spotifytv/crossfade/extension/{cls};" not in dump:
                fail(f"extension class {cls} missing from dex")
        body = method_body(dump, "Lcom/spotify/tv/android/SpotifyTVApplication;", "onCreate", "()V")
        if body is None:
            fail("SpotifyTVApplication.onCreate not found")
        if "CrossfadeHooks;.init:(Landroid/content/Context;)V" not in body:
            fail("Application.onCreate does not call CrossfadeHooks.init")
        body = method_body_any_sig(dump, "Lcom/spotify/tv/android/bindings/tvbridge/TVBridgeCallbacksRouter;", "handleAndroidEvent$lambda$2")
        if body is None:
            fail("TVBridgeCallbacksRouter.handleAndroidEvent$lambda$2 not found")
        if "CrossfadeHooks;.onMetadataJson:(Ljava/lang/String;)V" not in body:
            fail("metadata lambda does not call CrossfadeHooks.onMetadataJson")
        body = method_body(dump, "Lcom/spotify/tv/android/SpotifyTVActivity;", "dispatchKeyEvent", "(Landroid/view/KeyEvent;)Z")
        if body is None:
            fail("SpotifyTVActivity.dispatchKeyEvent not found")
        if "CrossfadeHooks;.onKeyEvent:(Landroid/app/Activity;Landroid/view/KeyEvent;)Z" not in body:
            fail("dispatchKeyEvent does not call CrossfadeHooks.onKeyEvent")
        if "CrossfadeSettingsActivity" not in xml:
            fail("CrossfadeSettingsActivity missing from the manifest")

    print("OK:", apk)


if __name__ == "__main__":
    main()
