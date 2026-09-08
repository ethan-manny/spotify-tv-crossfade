#include "boundary.h"
#include <cstdlib>
#include "log.h"

namespace xfade {

void BoundaryModel::configure(uint32_t rateHz, uint32_t crossfadeFrames) {
    rate_ = rateHz;
    crossfade_ = crossfadeFrames;
}

void BoundaryModel::onFlush(uint64_t written, int64_t nowMs) {
    flushPending_ = true;   // a burst of Clears collapses into the latest one
    flushFrame_ = written;
    flushTimeMs_ = nowMs;
    lateCutValid_ = false;  // a new flush is a newer position jump, superseding any pending late-cut memory
}

bool BoundaryModel::isEpisode(const std::string& uri) { return uri.rfind("spotify:episode:", 0) == 0; }
// The eSDK's "video" flag flaps within a track (it reflects the client showing video/canvas), so it is not a boundary criterion.
bool BoundaryModel::fadeable(const TrackMeta& m) { return !m.isAd && !isEpisode(m.trackUri); }

Decision BoundaryModel::decide(uint64_t b, const TrackMeta& incoming, uint64_t read) const {
    Decision d;
    d.at = b;
    uint64_t tail = b > read ? b - read : 0;
    uint64_t minTail = static_cast<uint64_t>(rate_) * kMinTailMs / 1000;
    bool fade = crossfade_ > 0 && fadeable(current_) && fadeable(incoming) && tail >= minTail;
    if (fade) {
        d.kind = Decision::Fade;
        d.n = static_cast<uint32_t>(tail < crossfade_ ? tail : crossfade_);
    } else {
        d.kind = Decision::Cut;
    }
    return d;
}

Decision BoundaryModel::onMetadata(const TrackMeta& m, uint64_t written, uint64_t read, int64_t nowMs) {
    Decision d;
    bool newTrack = !hasCurrent_ || m.playbackId != current_.playbackId;
    if (flushPending_) {
        uint64_t b = flushFrame_;
        int64_t age = nowMs - flushTimeMs_;
        flushPending_ = false;
        lateCutValid_ = false;  // this flush is a newer, now-resolved position jump
        if (age > kFlushWindowMs) {
            // The flush aged out before metadata arrived, but metadata still claims it: one cut, not two.
            d.kind = Decision::Cut;
            d.at = b;
            LOGI("boundary: metadata %lld ms after flush -> cut at %llu", (long long)age, (unsigned long long)b);
        } else if (newTrack) {
            // A manual skip is a cut, not a fade: the user asked for the next song now, and the tail behind
            // the flush is audio they have already left (R-T8d).
            d.kind = Decision::Cut;
            d.at = b;
            LOGI("boundary: skip -> cut at %llu", (unsigned long long)b);
        } else {
            d.kind = Decision::Cut;
            d.at = b;
            LOGI("boundary: seek -> cut at %llu", (unsigned long long)b);
        }
    } else if (newTrack && hasCurrent_ && lateCutValid_ && nowMs - lateCutTimeMs_ <= kLateMetadataMs) {
        // The timeout cut already executed this track change; a fade here would mix the new track with itself.
        lateCutValid_ = false;
        startFrame_ = lateCutFrame_;
        startPositionMs_ = m.positionMs;
        current_ = m;
        hasCurrent_ = true;
        LOGI("boundary: late announcement of the cut at %llu, no new boundary", (unsigned long long)lateCutFrame_);
        return d;
    } else if (newTrack && hasCurrent_) {
        lateCutValid_ = false;
        uint64_t b = written;
        if (rate_ > 0 && current_.durationMs > 0 && current_.durationMs >= startPositionMs_) {
            uint64_t p = startFrame_ + static_cast<uint64_t>((current_.durationMs - startPositionMs_) * rate_ / 1000);
            int64_t delta = static_cast<int64_t>(p) - static_cast<int64_t>(written);
            lastDeltaMs_ = static_cast<int32_t>(delta * 1000 / static_cast<int64_t>(rate_));
            if (std::llabs(delta) <= static_cast<int64_t>(rate_) * kSanityWindowMs / 1000) b = p;
        }
        d = decide(b, m, read);
        LOGI("boundary: natural -> %s at %llu (announced %llu, delta %+d ms) n=%u",
             d.kind == Decision::Fade ? "fade" : "cut", (unsigned long long)b, (unsigned long long)written, lastDeltaMs_, d.n);
    }
    // The announced track becomes current; same-track updates refresh the prediction anchor.
    startFrame_ = d.kind == Decision::None ? written : d.at;
    startPositionMs_ = m.positionMs;
    current_ = m;
    hasCurrent_ = true;
    return d;
}

Decision BoundaryModel::tick(uint64_t, uint64_t, int64_t nowMs) {
    Decision d;
    if (flushPending_ && nowMs - flushTimeMs_ > kFlushWindowMs) {
        flushPending_ = false;
        d.kind = Decision::Cut;
        d.at = flushFrame_;
        lateCutValid_ = true;
        lateCutFrame_ = flushFrame_;
        lateCutTimeMs_ = nowMs;
        LOGI("boundary: flush without metadata -> cut at %llu", (unsigned long long)flushFrame_);
    }
    return d;
}

}  // namespace xfade
