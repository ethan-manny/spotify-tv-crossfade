package dev.spotifytv.crossfade.extension;

import android.content.Context;
import android.content.SharedPreferences;

/** The persisted crossfade length. */
public final class CrossfadeSettings {
    public static final String PREFS = "crossfade";
    public static final String KEY_MS = "crossfade_ms";
    public static final int DEFAULT_MS = 5000;
    public static final int MAX_MS = 12000;
    public static final int STEP_MS = 1000;

    private CrossfadeSettings() {}

    /** Clamps to 0..MAX_MS and rounds to whole seconds. */
    public static int clamp(int ms) {
        if (ms < 0) ms = 0;
        if (ms > MAX_MS) ms = MAX_MS;
        return (ms + STEP_MS / 2) / STEP_MS * STEP_MS;
    }

    public static int getMs(Context context) {
        return clamp(prefs(context).getInt(KEY_MS, DEFAULT_MS));
    }

    public static void setMs(Context context, int ms) {
        prefs(context).edit().putInt(KEY_MS, clamp(ms)).apply();
    }

    private static SharedPreferences prefs(Context context) {
        return context.getSharedPreferences(PREFS, Context.MODE_PRIVATE);
    }
}
