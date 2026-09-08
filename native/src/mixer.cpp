#include "mixer.h"
#include <cmath>

namespace xfade {

void mixEqualPowerSplit(const int16_t* a, const int16_t* b, int16_t* out, uint32_t frames, uint32_t channels,
                        uint32_t kA0, uint32_t nA, uint32_t kB0, uint32_t nB) {
    if (nA == 0) nA = 1;
    if (nB == 0) nB = 1;
    const float scaleA = 1.57079632679f / static_cast<float>(nA);
    const float scaleB = 1.57079632679f / static_cast<float>(nB);
    for (uint32_t i = 0; i < frames; ++i) {
        // An index past the end of its own ramp holds that ramp's end gain instead of running theta past
        // pi/2 (which would invert the sign): with different lengths the side that finishes first stays
        // where it is while the other one runs on.
        const uint32_t kA = kA0 + i < nA ? kA0 + i : nA - 1;
        const uint32_t kB = kB0 + i < nB ? kB0 + i : nB - 1;
        float ga = std::cos((static_cast<float>(kA) + 0.5f) * scaleA);
        float gb = std::sin((static_cast<float>(kB) + 0.5f) * scaleB);
        for (uint32_t c = 0; c < channels; ++c) {
            size_t idx = static_cast<size_t>(i) * channels + c;
            float v = (a ? a[idx] * ga : 0.0f) + (b ? b[idx] * gb : 0.0f);
            int32_t s = static_cast<int32_t>(std::lround(v));
            if (s > 32767) s = 32767;
            if (s < -32768) s = -32768;
            out[idx] = static_cast<int16_t>(s);
        }
    }
}

void mixEqualPowerSplit(const int16_t* a, const int16_t* b, int16_t* out, uint32_t frames, uint32_t channels,
                        uint32_t kA0, uint32_t kB0, uint32_t n) {
    mixEqualPowerSplit(a, b, out, frames, channels, kA0, n, kB0, n);
}

void mixEqualPower(const int16_t* a, const int16_t* b, int16_t* out, uint32_t frames, uint32_t channels,
                   uint32_t k0, uint32_t n) {
    mixEqualPowerSplit(a, b, out, frames, channels, k0, k0, n);
}

}  // namespace xfade
