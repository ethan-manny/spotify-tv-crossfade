package dev.spotifytv.crossfade.extension;

import android.app.Activity;
import android.graphics.Color;
import android.os.Bundle;
import android.os.Handler;
import android.os.Looper;
import android.util.Log;
import android.util.TypedValue;
import android.view.Gravity;
import android.view.KeyEvent;
import android.widget.LinearLayout;
import android.widget.TextView;

/** The on-screen crossfade setting (spec 6). Left/Right adjust by one second, Back closes. */
public final class CrossfadeSettingsActivity extends Activity {
    public static final String EXTRA_SECONDS = "crossfade_seconds";
    public static final String EXTRA_SHOW_UI = "show_ui";
    private static final String TAG = "xfade";

    private final Handler handler = new Handler(Looper.getMainLooper());
    private TextView value;
    private TextView status;
    private int ms;
    private final Runnable refresh = new Runnable() {
        @Override public void run() {
            status.setText(StatusFormatter.describe(CrossfadeNative.status()));
            handler.postDelayed(this, 1000);
        }
    };

    @Override protected void onCreate(Bundle saved) {
        super.onCreate(saved);
        try {
            ms = CrossfadeSettings.getMs(this);
            if (getIntent() != null && getIntent().hasExtra(EXTRA_SECONDS)) {
                apply(getIntent().getIntExtra(EXTRA_SECONDS, CrossfadeSettings.DEFAULT_MS / 1000) * 1000);
                if (!getIntent().getBooleanExtra(EXTRA_SHOW_UI, false)) { finish(); return; }
            }
            buildUi();
        } catch (Throwable t) {
            Log.e(TAG, "settings onCreate failed", t);
            finish();
        }
    }

    private void apply(int newMs) {
        ms = CrossfadeSettings.clamp(newMs);
        CrossfadeSettings.setMs(this, ms);
        CrossfadeNative.setCrossfade(ms);
        Log.i(TAG, "crossfade set to " + ms + " ms");
        if (value != null) value.setText(label(ms));
    }

    static String label(int ms) {
        if (ms == 0) return "Off";
        int s = ms / 1000;
        return s + (s == 1 ? " second" : " seconds");
    }

    private void buildUi() {
        LinearLayout root = new LinearLayout(this);
        root.setOrientation(LinearLayout.VERTICAL);
        root.setGravity(Gravity.CENTER);
        root.setBackgroundColor(Color.rgb(18, 18, 18));
        int pad = (int) TypedValue.applyDimension(TypedValue.COMPLEX_UNIT_DIP, 48, getResources().getDisplayMetrics());
        root.setPadding(pad, pad, pad, pad);
        root.addView(text("Crossfade", 30));
        value = text(label(ms), 72);
        root.addView(value);
        status = text(StatusFormatter.describe(CrossfadeNative.status()), 22);
        root.addView(status);
        root.addView(text("Left / Right adjust, Back closes", 20));
        setContentView(root);
    }

    private TextView text(String s, int sp) {
        TextView t = new TextView(this);
        t.setText(s);
        t.setTextColor(Color.WHITE);
        t.setTextSize(TypedValue.COMPLEX_UNIT_SP, sp);
        t.setGravity(Gravity.CENTER);
        t.setPadding(0, 16, 0, 16);
        return t;
    }

    @Override protected void onResume() { super.onResume(); if (status != null) handler.post(refresh); }
    @Override protected void onPause() { super.onPause(); handler.removeCallbacks(refresh); }

    @Override public boolean onKeyDown(int keyCode, KeyEvent event) {
        try {
            switch (keyCode) {
                case KeyEvent.KEYCODE_DPAD_LEFT: apply(ms - CrossfadeSettings.STEP_MS); return true;
                case KeyEvent.KEYCODE_DPAD_RIGHT: apply(ms + CrossfadeSettings.STEP_MS); return true;
                case KeyEvent.KEYCODE_BACK: finish(); return true;
                default: return super.onKeyDown(keyCode, event);
            }
        } catch (Throwable t) {
            Log.e(TAG, "onKeyDown failed", t);
            return false;
        }
    }
}
