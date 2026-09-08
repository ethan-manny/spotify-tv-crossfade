package dev.spotifytv.crossfade.extension;

import java.util.HashMap;
import java.util.Locale;
import java.util.Map;

/** Turns CrossfadeNative.status() ("key=value ...") into the settings screen's status line. */
public final class StatusFormatter {
    private StatusFormatter() {}

    public static String describe(String status) {
        if (status == null) return "Player unavailable";
        Map<String, String> kv = new HashMap<>();
        for (String part : status.trim().split("\\s+")) {
            int eq = part.indexOf('=');
            if (eq > 0) kv.put(part.substring(0, eq), part.substring(eq + 1));
        }
        String state = kv.get("state");
        if ("active".equals(state)) {
            return String.format(Locale.US, "Active, lead %.1f s, %d fades", parseLong(kv.get("lead_ms")) / 1000.0, parseLong(kv.get("fades")));
        }
        if ("passthrough".equals(state)) return "Passthrough";
        if ("no-library".equals(state)) return "Library missing";
        return "Player unavailable";
    }

    private static long parseLong(String s) {
        try { return s == null ? 0L : Long.parseLong(s.replace("+", "")); } catch (NumberFormatException e) { return 0L; }
    }
}
