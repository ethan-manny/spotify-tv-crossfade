#include <SLES/OpenSLES.h>
#include <cmath>
#include <time.h>
#include <atomic>
#include "test.h"
#include "../src/real_backend.h"

// Plays 0.5 s of a 440 Hz tone through the device. Only runs with --real.
TEST(real_backend_plays_a_tone) {
    if (!xfade_test::g_real) { std::printf("  (skipped: pass --real)\n"); return; }
    xfade::RealBackend be;
    static std::atomic<int> done{0};
    xfade::PcmFormat fmt{44100, 2, 16, 4};
    EXPECT_TRUE(be.open(fmt, {[](void*) { ++done; }, nullptr}));

    const int frames = 441;  // 10 ms
    static int16_t bufs[4][frames * 2];
    for (int b = 0; b < 4; ++b)
        for (int i = 0; i < frames; ++i) {
            int16_t v = (int16_t)(8000 * std::sin(2 * M_PI * 440 * (b * frames + i) / 44100.0));
            bufs[b][2 * i] = v; bufs[b][2 * i + 1] = v;
        }
    for (int b = 0; b < 4; ++b) EXPECT_EQ(be.enqueue(bufs[b], sizeof bufs[b]), (uint32_t)SL_RESULT_SUCCESS);
    EXPECT_TRUE(be.setPlayState(SL_PLAYSTATE_PLAYING));
    int fed = 4;
    timespec ts{0, 5 * 1000 * 1000};
    for (int i = 0; i < 200 && fed < 50; ++i) {          // re-feed as buffers complete, up to 0.5 s of audio
        while (done.load() > fed - 4 && fed < 50) { be.enqueue(bufs[fed % 4], sizeof bufs[0]); ++fed; }
        nanosleep(&ts, nullptr);
    }
    EXPECT_TRUE(done.load() >= 40);
    EXPECT_EQ(be.setVolumeLevel(-300), (uint32_t)SL_RESULT_SUCCESS);
    be.setPlayState(SL_PLAYSTATE_STOPPED);
    be.close();
}
