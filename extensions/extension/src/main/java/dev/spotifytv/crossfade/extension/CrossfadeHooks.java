package dev.spotifytv.crossfade.extension;

import android.app.Activity;
import android.content.Context;
import android.content.Intent;
import android.util.Log;
import android.view.KeyEvent;

/** Entry points invoked from patched Spotify code. Nothing here may throw into the caller. */
public final class CrossfadeHooks {
    private static final String TAG = "xfade";
    private static final long LONG_PRESS_MS = 700;
    private static final LongPressDetector MENU = new LongPressDetector(LongPressDetector.KEYCODE_MENU, LONG_PRESS_MS);

    private CrossfadeHooks() {}

    /** Called at the start of SpotifyTVApplication.onCreate(): loads libxfade.so before the eSDK and pushes the saved length. */
    public static void init(Context context) {
        try {
            int ms = context == null ? CrossfadeSettings.DEFAULT_MS : CrossfadeSettings.getMs(context);
            CrossfadeNative.setCrossfade(ms);
            if (context != null) CrossfadeNative.dumpDir(context.getFilesDir().getAbsolutePath());
            Log.i(TAG, "init: native available=" + CrossfadeNative.isAvailable() + " crossfade_ms=" + ms + " " + CrossfadeNative.status()
                    + " package=" + (context == null ? "null" : context.getPackageName()));
        } catch (Throwable t) {
            Log.e(TAG, "init failed", t);
        }
    }

    /** Called at the start of the eSDK metadata callback in TVBridgeCallbacksRouter with the raw JSON. */
    public static void onMetadataJson(String json) {
        try {
            TrackMeta m = MetadataParser.parse(json);
            if (m != null) CrossfadeNative.metadata(m.playbackId, m.trackUri, m.durationMs, m.positionMs, m.isAd, m.isVideo);
        } catch (Throwable t) {
            Log.e(TAG, "onMetadataJson failed", t);
        }
    }

    /** Called at the start of SpotifyTVActivity.dispatchKeyEvent. Returns true when the event is consumed. */
    public static boolean onKeyEvent(Activity activity, KeyEvent event) {
        try {
            if (event == null) return false;
            LongPressDetector.Result r;
            synchronized (MENU) {
                r = MENU.onKey(event.getKeyCode(), event.getAction(), event.getRepeatCount(), event.getEventTime());
            }
            if (r == LongPressDetector.Result.FIRE && activity != null) {
                Intent intent = new Intent(activity, CrossfadeSettingsActivity.class);
                intent.putExtra(CrossfadeSettingsActivity.EXTRA_SHOW_UI, true);
                activity.startActivity(intent);
                Log.i(TAG, "long press Menu: opening settings");
            }
            return r != LongPressDetector.Result.PASS;
        } catch (Throwable t) {
            Log.e(TAG, "onKeyEvent failed", t);
            return false;
        }
    }
}
