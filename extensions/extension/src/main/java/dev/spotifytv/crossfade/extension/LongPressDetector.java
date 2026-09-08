package dev.spotifytv.crossfade.extension;

/**
 * Detects a long press of one key from the stream of key events an Activity sees: the first DOWN and
 * repeats before the threshold pass through, the repeat that crosses the threshold fires once, and
 * everything else of that press up to and including the UP is consumed. Pure Java; not thread-safe.
 */
public final class LongPressDetector {
    public enum Result { PASS, FIRE, CONSUME }

    // android.view.KeyEvent values, kept here so this class and its tests need no android.jar.
    public static final int ACTION_DOWN = 0;
    public static final int ACTION_UP = 1;
    public static final int KEYCODE_MENU = 82;

    private final int keyCode;
    private final long thresholdMs;
    private boolean pressed;
    private boolean fired;
    private long downTimeMs;

    public LongPressDetector(int keyCode, long thresholdMs) {
        this.keyCode = keyCode;
        this.thresholdMs = thresholdMs;
    }

    public Result onKey(int code, int action, int repeatCount, long eventTimeMs) {
        if (code != keyCode) return Result.PASS;
        if (action == ACTION_DOWN) {
            if (repeatCount == 0 || !pressed) {
                pressed = true;
                fired = false;
                downTimeMs = eventTimeMs;
                return Result.PASS;
            }
            if (fired) return Result.CONSUME;
            if (eventTimeMs - downTimeMs >= thresholdMs) {
                fired = true;
                return Result.FIRE;
            }
            return Result.PASS;
        }
        if (action == ACTION_UP) {
            boolean wasFired = fired;
            pressed = false;
            fired = false;
            return wasFired ? Result.CONSUME : Result.PASS;
        }
        return Result.PASS;
    }
}
