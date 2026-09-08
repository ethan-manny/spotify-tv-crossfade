#include <cmath>
#include <cstdlib>
#include "test.h"
#include "../src/ring.h"
#include "../src/mixer.h"

TEST(ring_copies_by_absolute_frame_and_wraps) {
    xfade::Ring r;
    EXPECT_TRUE(r.init(8, 2));
    EXPECT_EQ(r.capacity(), 8u);
    int16_t src[6 * 2];
    for (int i = 0; i < 12; ++i) src[i] = (int16_t)(100 + i);
    r.write(0, src, 6);                       // frames 0..5
    r.write(6, src, 6);                       // frames 6..11: wraps after frame 7
    int16_t dst[6 * 2] = {};
    r.read(6, dst, 6);
    for (int i = 0; i < 12; ++i) EXPECT_EQ(dst[i], (int16_t)(100 + i));
    r.read(4, dst, 2);                        // frames 4,5 still hold the first write
    EXPECT_EQ(dst[0], (int16_t)108);
    EXPECT_EQ(dst[3], (int16_t)111);
    r.read(1ull << 40, dst, 1);               // large absolute frames map without overflow
}

TEST(ring_init_rejects_impossible_size) {
    xfade::Ring r;
    EXPECT_TRUE(!r.init(0xFFFFFFFFu, 2));     // allocation fails cleanly, no abort
}

TEST(mixer_equal_power_gains_and_clamp) {
    const uint32_t n = 1000;
    int16_t a[2] = {10000, 10000}, b[2] = {20000, 20000}, out[2];
    xfade::mixEqualPower(a, b, out, 1, 2, 0, n);            // first frame: nearly all A
    EXPECT_TRUE(out[0] > 9990 && out[0] < 10040);
    xfade::mixEqualPower(a, b, out, 1, 2, n - 1, n);        // last frame: nearly all B
    EXPECT_TRUE(out[0] > 19960 && out[0] < 20010);
    xfade::mixEqualPower(a, b, out, 1, 2, n / 2, n);        // middle: (a + b) * cos(pi/4)
    int expectedMid = (int)std::lround((10000 + 20000) * std::cos(M_PI / 4));
    EXPECT_TRUE(std::abs(out[0] - expectedMid) <= 10);
    int16_t big[2] = {30000, -30000}, big2[2] = {30000, -30000};
    xfade::mixEqualPower(big, big2, out, 1, 2, n / 2, n);   // 30000 * 1.41 clamps both ways
    EXPECT_EQ(out[0], (int16_t)32767);
    EXPECT_EQ(out[1], (int16_t)-32768);
    xfade::mixEqualPower(a, nullptr, out, 1, 2, n - 1, n);  // null B is silence
    EXPECT_TRUE(out[0] >= 0 && out[0] < 40);
}

TEST(mixer_split_uses_independent_positions) {
    const uint32_t n = 1000;
    int16_t a[2] = {10000, 10000}, b[2] = {20000, 20000}, out[2], out2[2];
    xfade::mixEqualPowerSplit(a, b, out, 1, 2, 999, 0, n);       // both inputs at their quiet end
    EXPECT_TRUE(out[0] >= 0 && out[0] < 40);
    xfade::mixEqualPowerSplit(a, b, out, 1, 2, 0, 999, n);       // both at their loud end
    EXPECT_TRUE(out[0] > 29960 && out[0] < 30020);
    int16_t zeros[2] = {0, 0};                                   // a == nullptr is silence, like an all-zero a
    xfade::mixEqualPowerSplit(nullptr, b, out, 1, 2, 0, 500, n);
    xfade::mixEqualPower(zeros, b, out2, 1, 2, 500, n);
    EXPECT_EQ(out[0], out2[0]);
    EXPECT_EQ(out[1], out2[1]);
}

TEST(mixer_split_lengths_are_independent) {
    int16_t a[2] = {10000, 10000}, b[2] = {20000, 20000}, out[2], out2[2];
    // A at the end of a 500-frame ramp, B at the start of a 1000-frame one: both nearly silent.
    xfade::mixEqualPowerSplit(a, b, out, 1, 2, 499, 500, 0, 1000);
    EXPECT_TRUE(out[0] >= 0 && out[0] < 50);
    // Equal lengths reduce to the 8-argument form.
    xfade::mixEqualPowerSplit(a, b, out, 1, 2, 250, 250, 1000);
    xfade::mixEqualPowerSplit(a, b, out2, 1, 2, 250, 1000, 250, 1000);
    EXPECT_EQ(out[0], out2[0]);
    EXPECT_EQ(out[1], out2[1]);
}

TEST(mixer_indices_clamp_at_the_ramp_end) {
    int16_t a[2] = {10000, 10000}, b[2] = {20000, 20000}, out[2], out2[2];
    // Past the end of both ramps: each side holds its end gain instead of running theta past pi/2.
    xfade::mixEqualPowerSplit(a, b, out, 1, 2, 1500, 1000, 1500, 1000);
    xfade::mixEqualPowerSplit(a, b, out2, 1, 2, 999, 1000, 999, 1000);
    EXPECT_EQ(out[0], out2[0]);
    EXPECT_EQ(out[1], out2[1]);
}

TEST(mixer_multi_frame_call_matches_per_frame_calls) {
    int16_t a[4 * 2], b[4 * 2], whole[4 * 2], one[2];
    for (int i = 0; i < 8; ++i) { a[i] = (int16_t)(1000 * (i + 1)); b[i] = (int16_t)(-500 * (i + 1)); }
    xfade::mixEqualPower(a, b, whole, 4, 2, 10, 40);
    for (uint32_t k = 0; k < 4; ++k) {
        xfade::mixEqualPower(a + 2 * k, b + 2 * k, one, 1, 2, 10 + k, 40);
        EXPECT_EQ(whole[2 * k], one[0]);
        EXPECT_EQ(whole[2 * k + 1], one[1]);
    }
}
