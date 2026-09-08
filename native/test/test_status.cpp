#include "test.h"
#include "../src/status.h"

TEST(status_clamps_crossfade_and_reports_state) {
    xfade::setCrossfadeMs(99999);
    EXPECT_EQ(xfade::crossfadeMs(), 12000);
    xfade::setCrossfadeMs(-5);
    EXPECT_EQ(xfade::crossfadeMs(), 0);
    xfade::setCrossfadeMs(5000);
    xfade::setPlayerInfo("passthrough", 44100, 2);
    EXPECT_TRUE(xfade::statusLine() == "state=passthrough rate=44100 ch=2 crossfade_ms=5000");
}

struct FakeStats : xfade::StatsSource {
    std::string stats() override { return "lead_ms=6120 crossfade_ms=5000 fades=3 last_delta_ms=+40 underruns=0"; }
};

TEST(status_appends_registered_stats) {
    FakeStats s;
    xfade::setCrossfadeMs(5000);
    xfade::setPlayerInfo("active", 44100, 2);
    xfade::setStatsSource(&s);
    EXPECT_TRUE(xfade::statusLine() == "state=active rate=44100 ch=2 lead_ms=6120 crossfade_ms=5000 fades=3 last_delta_ms=+40 underruns=0");
    xfade::clearStatsSource(&s);
    EXPECT_TRUE(xfade::statusLine() == "state=active rate=44100 ch=2 crossfade_ms=5000");
}

// Two players can be live at once; the older one stopping must not take the newer one's stats away.
TEST(status_clear_from_a_foreign_source_is_ignored) {
    FakeStats a, b;
    xfade::setCrossfadeMs(5000);
    xfade::setPlayerInfo("active", 44100, 2);
    xfade::setStatsSource(&a);
    xfade::clearStatsSource(&b);
    EXPECT_TRUE(xfade::statusLine() == "state=active rate=44100 ch=2 lead_ms=6120 crossfade_ms=5000 fades=3 last_delta_ms=+40 underruns=0");
    xfade::clearStatsSource(&a);
    EXPECT_TRUE(xfade::statusLine() == "state=active rate=44100 ch=2 crossfade_ms=5000");
}
