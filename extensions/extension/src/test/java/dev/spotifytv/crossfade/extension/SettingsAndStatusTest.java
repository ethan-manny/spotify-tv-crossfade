package dev.spotifytv.crossfade.extension;

import static org.junit.Assert.*;
import org.junit.Test;

public class SettingsAndStatusTest {
    @Test public void clampRoundsToWholeSecondsWithinRange() {
        assertEquals(0, CrossfadeSettings.clamp(-5));
        assertEquals(12000, CrossfadeSettings.clamp(12500));
        assertEquals(5000, CrossfadeSettings.clamp(5400));
        assertEquals(6000, CrossfadeSettings.clamp(5600));
        assertEquals(5000, CrossfadeSettings.clamp(CrossfadeSettings.DEFAULT_MS));
    }

    @Test public void describesStatusLines() {
        assertEquals("Active, lead 6.1 s, 3 fades",
                StatusFormatter.describe("state=active rate=44100 ch=2 lead_ms=6120 crossfade_ms=5000 fades=3 last_delta_ms=+40 underruns=0"));
        assertEquals("Active, lead 0.0 s, 0 fades", StatusFormatter.describe("state=active rate=44100 ch=2 lead_ms=0 crossfade_ms=5000 fades=0 last_delta_ms=+0 underruns=0"));
        assertEquals("Passthrough", StatusFormatter.describe("state=passthrough rate=44100 ch=2 crossfade_ms=0"));
        assertEquals("Player unavailable", StatusFormatter.describe("state=no-player rate=0 ch=0 crossfade_ms=5000"));
        assertEquals("Library missing", StatusFormatter.describe("state=no-library"));
        assertEquals("Player unavailable", StatusFormatter.describe(null));
        assertEquals("Player unavailable", StatusFormatter.describe("garbage"));
    }
}
