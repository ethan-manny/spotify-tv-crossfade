package dev.spotifytv.crossfade.extension;

import org.json.JSONObject;

/** Parses the eSDK's metadata JSON. Returns null when there is nothing usable (no playback_id, malformed, null). */
public final class MetadataParser {
    private MetadataParser() {}

    public static TrackMeta parse(String json) {
        if (json == null || json.isEmpty()) return null;
        try {
            JSONObject o = new JSONObject(json);
            if (o.isNull("playback_id")) return null;
            String id = o.optString("playback_id", "");
            if (id.isEmpty()) return null;
            return new TrackMeta(id, o.isNull("track_uri") ? "" : o.optString("track_uri", ""),
                    o.optLong("duration_ms", 0L), o.optLong("position", 0L),
                    o.optBoolean("is_ad_playing", false), o.optBoolean("video", false));
        } catch (Throwable t) {
            return null;
        }
    }
}
