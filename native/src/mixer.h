#pragma once
#include <cstdint>

namespace xfade {

// Equal-power crossfade (spec 5.4). Writes 'frames' output frames for fade positions k0 .. k0+frames-1
// of a fade of length n: out = clamp16(a * cos(theta) + b * sin(theta)), theta = (pi/2) * (k + 0.5) / n.
// a, b, out are interleaved 16-bit with 'channels' samples per frame; b == nullptr means silence.
void mixEqualPower(const int16_t* a, const int16_t* b, int16_t* out, uint32_t frames, uint32_t channels,
                   uint32_t k0, uint32_t n);

// Same as mixEqualPower but with independent fade positions for the two inputs, so an incoming track
// whose head arrived late can fade in from its own first frame while the outgoing tail is already deep
// into its fade: out = clamp16(a * cos(theta(kA0 + i)) + b * sin(theta(kB0 + i))), theta(k) as above.
// a or b may be null (silence).
void mixEqualPowerSplit(const int16_t* a, const int16_t* b, int16_t* out, uint32_t frames, uint32_t channels,
                        uint32_t kA0, uint32_t kB0, uint32_t n);

// As above, but the two sides also have independent fade lengths: the outgoing follows theta over nA, the
// incoming theta over nB. A re-anchored fade uses this to keep the outgoing on the ramp it is already on
// while the incoming fades in over the configured crossfade length. An index past the end of its ramp
// holds that ramp's end gain, so the side that finishes first stays there while the other one runs on.
void mixEqualPowerSplit(const int16_t* a, const int16_t* b, int16_t* out, uint32_t frames, uint32_t channels,
                        uint32_t kA0, uint32_t nA, uint32_t kB0, uint32_t nB);

}  // namespace xfade
