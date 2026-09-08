package dev.spotifytv.crossfade.extension;

import android.util.Log;

/** JNI wrapper for libxfade.so. Every public method is safe to call when the library failed to load. */
public final class CrossfadeNative {
    private static final String TAG = "xfade";
    private static final boolean AVAILABLE;

    static {
        boolean ok = false;
        try {
            System.loadLibrary("xfade");
            ok = true;
        } catch (Throwable t) {
            Log.e(TAG, "System.loadLibrary(xfade) failed", t);
        }
        AVAILABLE = ok;
    }

    private CrossfadeNative() {}

    public static boolean isAvailable() { return AVAILABLE; }

    public static String status() {
        if (!AVAILABLE) return "state=no-library";
        try { return getStatus(); } catch (Throwable t) { Log.e(TAG, "getStatus failed", t); return "state=error"; }
    }

    public static void setCrossfade(int ms) {
        if (!AVAILABLE) return;
        try { setCrossfadeMs(ms); } catch (Throwable t) { Log.e(TAG, "setCrossfadeMs failed", t); }
    }

    public static void metadata(String playbackId, String trackUri, long durationMs, long positionMs, boolean isAd, boolean isVideo) {
        if (!AVAILABLE) return;
        try { onMetadata(playbackId, trackUri, durationMs, positionMs, isAd, isVideo); } catch (Throwable t) { Log.e(TAG, "onMetadata failed", t); }
    }

    public static void dumpDir(String dir) {
        if (!AVAILABLE) return;
        try { setDumpDir(dir); } catch (Throwable t) { Log.e(TAG, "setDumpDir failed", t); }
    }

    private static native String getStatus();
    private static native void setCrossfadeMs(int ms);
    private static native void onMetadata(String playbackId, String trackUri, long durationMs, long positionMs, boolean isAd, boolean isVideo);
    private static native void setDumpDir(String dir);
}
