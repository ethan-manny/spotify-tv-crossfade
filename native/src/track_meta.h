#pragma once
#include <cstdint>
#include <string>

namespace xfade {

// One metadata update from Java (spec 5.8 onMetadata).
struct TrackMeta {
    std::string playbackId;
    std::string trackUri;
    int64_t durationMs = 0;
    int64_t positionMs = 0;
    bool isAd = false;
    bool isVideo = false;
};

}  // namespace xfade
