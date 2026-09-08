package dev.spotifytv.crossfade.extension;

import static dev.spotifytv.crossfade.extension.LongPressDetector.*;
import static org.junit.Assert.*;
import org.junit.Test;

public class LongPressDetectorTest {
    private final LongPressDetector d = new LongPressDetector(KEYCODE_MENU, 700);

    @Test public void shortPressPasses() {
        assertEquals(Result.PASS, d.onKey(KEYCODE_MENU, ACTION_DOWN, 0, 0));
        assertEquals(Result.PASS, d.onKey(KEYCODE_MENU, ACTION_DOWN, 1, 300));
        assertEquals(Result.PASS, d.onKey(KEYCODE_MENU, ACTION_UP, 0, 400));
    }

    @Test public void longPressFiresOnceThenConsumesUntilUp() {
        assertEquals(Result.PASS, d.onKey(KEYCODE_MENU, ACTION_DOWN, 0, 1000));
        assertEquals(Result.PASS, d.onKey(KEYCODE_MENU, ACTION_DOWN, 1, 1400));
        assertEquals(Result.FIRE, d.onKey(KEYCODE_MENU, ACTION_DOWN, 2, 1700));
        assertEquals(Result.CONSUME, d.onKey(KEYCODE_MENU, ACTION_DOWN, 3, 1750));
        assertEquals(Result.CONSUME, d.onKey(KEYCODE_MENU, ACTION_DOWN, 4, 2500));
        assertEquals(Result.CONSUME, d.onKey(KEYCODE_MENU, ACTION_UP, 0, 2600));
        assertEquals(Result.PASS, d.onKey(KEYCODE_MENU, ACTION_DOWN, 0, 3000));   // fresh press
        assertEquals(Result.PASS, d.onKey(KEYCODE_MENU, ACTION_UP, 0, 3100));
    }

    @Test public void otherKeysAlwaysPass() {
        assertEquals(Result.PASS, d.onKey(KEYCODE_MENU, ACTION_DOWN, 0, 0));
        assertEquals(Result.PASS, d.onKey(23, ACTION_DOWN, 5, 5000));     // DPAD_CENTER held for 5 s
        assertEquals(Result.PASS, d.onKey(23, ACTION_UP, 0, 5100));
        assertEquals(Result.FIRE, d.onKey(KEYCODE_MENU, ACTION_DOWN, 1, 800));
    }

    @Test public void repeatWithoutInitialDownStartsAPress() {
        assertEquals(Result.PASS, d.onKey(KEYCODE_MENU, ACTION_DOWN, 3, 0));
        assertEquals(Result.FIRE, d.onKey(KEYCODE_MENU, ACTION_DOWN, 4, 700));
    }
}
