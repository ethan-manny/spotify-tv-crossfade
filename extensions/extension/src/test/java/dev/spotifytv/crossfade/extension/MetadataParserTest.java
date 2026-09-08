package dev.spotifytv.crossfade.extension;

import static org.junit.Assert.*;
import org.junit.Test;

public class MetadataParserTest {
    // Shape of the eSDK's metadata JSON (keys as read by TVBridgeCallbacksRouter.handleAndroidEvent$lambda$2).
    static final String REAL = "{\"playback_id\":\"a1b2c3\",\"duration_ms\":214000,\"position\":1234,\"is_ad_playing\":false,"
            + "\"is_shuffled\":false,\"is_repeated\":0,\"is_available_to_play\":true,\"can_skip_prev\":true,\"can_skip_next\":true,"
            + "\"track\":\"Song\",\"context_uri\":\"spotify:album:x\",\"track_uri\":\"spotify:track:t1\",\"track_uid\":\"u\","
            + "\"track_index\":3,\"album\":\"Album\",\"artist\":\"Artist\",\"album_cover_url\":\"https://i\",\"video\":false}";

    @Test public void parsesRealShape() {
        TrackMeta m = MetadataParser.parse(REAL);
        assertNotNull(m);
        assertEquals("a1b2c3", m.playbackId);
        assertEquals("spotify:track:t1", m.trackUri);
        assertEquals(214000L, m.durationMs);
        assertEquals(1234L, m.positionMs);
        assertFalse(m.isAd);
        assertFalse(m.isVideo);
    }

    @Test public void flagsAdsAndVideo() {
        TrackMeta m = MetadataParser.parse("{\"playback_id\":\"p\",\"is_ad_playing\":true,\"video\":true}");
        assertTrue(m.isAd);
        assertTrue(m.isVideo);
        assertEquals("", m.trackUri);
        assertEquals(0L, m.durationMs);
    }

    @Test public void missingOrNullPlaybackIdIsIgnored() {
        assertNull(MetadataParser.parse("{\"duration_ms\":1}"));
        assertNull(MetadataParser.parse("{\"playback_id\":null}"));
        assertNull(MetadataParser.parse("{\"playback_id\":\"\"}"));
    }

    @Test public void garbageNeverThrows() {
        assertNull(MetadataParser.parse(null));
        assertNull(MetadataParser.parse(""));
        assertNull(MetadataParser.parse("not json"));
        assertNull(MetadataParser.parse("[1,2]"));
        assertNotNull(MetadataParser.parse("{\"playback_id\":\"p\",\"duration_ms\":\"abc\"}"));   // bad number -> 0
        assertEquals(0L, MetadataParser.parse("{\"playback_id\":\"p\",\"duration_ms\":\"abc\"}").durationMs);
    }
}
