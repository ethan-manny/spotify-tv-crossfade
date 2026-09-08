package dev.spotifytv.crossfade.extension;

/** One metadata update from the eSDK, reduced to what the crossfade needs. */
public final class TrackMeta {
    public final String playbackId;
    public final String trackUri;
    public final long durationMs;
    public final long positionMs;
    public final boolean isAd;
    public final boolean isVideo;

    public TrackMeta(String playbackId, String trackUri, long durationMs, long positionMs, boolean isAd, boolean isVideo) {
        this.playbackId = playbackId;
        this.trackUri = trackUri;
        this.durationMs = durationMs;
        this.positionMs = positionMs;
        this.isAd = isAd;
        this.isVideo = isVideo;
    }
}
