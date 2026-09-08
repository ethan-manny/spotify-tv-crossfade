#pragma once
#include <cstdint>
#include "track_meta.h"

namespace xfade {

struct Decision {
    enum Kind { None, Cut, Fade };
    Kind kind = None;
    uint64_t at = 0;   // boundary frame b
    uint32_t n = 0;    // fade length in frames (Fade only)
};

// Turns flushes, metadata updates and time into cut/fade decisions (spec 5.3). Pure: no locks, no
// threads, no clock; the pipeline passes frame counters and a millisecond timestamp in.
class BoundaryModel {
public:
    static constexpr int64_t kFlushWindowMs = 500;     // metadata must arrive this soon after a Clear
    static constexpr uint32_t kMinTailMs = 200;        // shorter outgoing tails are cut, not faded
    static constexpr uint32_t kSanityWindowMs = 2500;  // |predicted - announced| beyond this: use announced
                                                       // (the eSDK announces the next track 0.4-1.6 s early)
    static constexpr int64_t kLateMetadataMs = 2000;   // a new track within this long after a timeout cut is its late announcement

    void configure(uint32_t rateHz, uint32_t crossfadeFrames);
    void setCrossfadeFrames(uint32_t frames) { crossfade_ = frames; }
    void onFlush(uint64_t written, int64_t nowMs);
    Decision onMetadata(const TrackMeta& m, uint64_t written, uint64_t read, int64_t nowMs);
    Decision tick(uint64_t written, uint64_t read, int64_t nowMs);
    int32_t lastDeltaMs() const { return lastDeltaMs_; }
    bool hasCurrent() const { return hasCurrent_; }
    const TrackMeta& current() const { return current_; }

private:
    // Only the natural branch decides between a fade and a cut; a flush is always a cut.
    Decision decide(uint64_t b, const TrackMeta& incoming, uint64_t read) const;
    static bool isEpisode(const std::string& uri);
    static bool fadeable(const TrackMeta& m);

    uint32_t rate_ = 0;
    uint32_t crossfade_ = 0;
    bool hasCurrent_ = false;
    TrackMeta current_;
    uint64_t startFrame_ = 0;
    int64_t startPositionMs_ = 0;
    bool flushPending_ = false;
    uint64_t flushFrame_ = 0;
    int64_t flushTimeMs_ = 0;
    int32_t lastDeltaMs_ = 0;
    bool lateCutValid_ = false;
    uint64_t lateCutFrame_ = 0;
    int64_t lateCutTimeMs_ = 0;
};

}  // namespace xfade
