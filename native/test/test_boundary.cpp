#include "test.h"
#include "../src/boundary.h"

using xfade::BoundaryModel;
using xfade::Decision;
using xfade::TrackMeta;

static TrackMeta meta(const char* id, int64_t durationMs, int64_t positionMs = 0, const char* uri = "spotify:track:x",
                      bool ad = false, bool video = false) {
    TrackMeta m;
    m.playbackId = id; m.trackUri = uri; m.durationMs = durationMs; m.positionMs = positionMs; m.isAd = ad; m.isVideo = video;
    return m;
}

static BoundaryModel modelWithTrackA(uint32_t crossfade = 5000) {
    BoundaryModel bm;
    bm.configure(1000, crossfade);
    Decision d = bm.onMetadata(meta("A", 60000), 0, 0, 0);
    EXPECT_EQ(d.kind, Decision::None);
    return bm;
}

TEST(boundary_first_track_sets_anchor_only) {
    BoundaryModel bm = modelWithTrackA();
    EXPECT_TRUE(bm.hasCurrent());
    EXPECT_TRUE(bm.current().playbackId == "A");
}

TEST(boundary_natural_transition_fades_at_predicted_frame) {
    BoundaryModel bm = modelWithTrackA();
    Decision d = bm.onMetadata(meta("B", 200000), 60300, 54000, 1000);
    EXPECT_EQ(d.kind, Decision::Fade);
    EXPECT_EQ(d.at, 60000u);                 // 0 + (60000 - 0) * 1000 / 1000
    EXPECT_EQ(d.n, 5000u);                   // min(X, b - read = 6000)
    EXPECT_EQ(bm.lastDeltaMs(), -300);       // predicted - announced
    EXPECT_TRUE(bm.current().playbackId == "B");
}

TEST(boundary_natural_transition_uses_refreshed_anchor) {
    BoundaryModel bm = modelWithTrackA();
    EXPECT_EQ(bm.onMetadata(meta("A", 60000, 30000), 30500, 25000, 500).kind, Decision::None);   // same track: refresh
    Decision d = bm.onMetadata(meta("B", 100000), 60700, 55000, 1000);
    EXPECT_EQ(d.kind, Decision::Fade);
    EXPECT_EQ(d.at, 60500u);                 // 30500 + (60000 - 30000)
}

TEST(boundary_bad_duration_or_prediction_fades_at_announced_frame) {
    BoundaryModel bm;
    bm.configure(1000, 5000);
    bm.onMetadata(meta("A", 0), 0, 0, 0);                        // unknown duration
    Decision d = bm.onMetadata(meta("B", 100000), 60300, 54000, 1000);
    EXPECT_EQ(d.kind, Decision::Fade);
    EXPECT_EQ(d.at, 60300u);

    BoundaryModel bm2;
    bm2.configure(1000, 5000);
    bm2.onMetadata(meta("A", 30000), 0, 0, 0);                   // prediction 30000, far from 60300
    d = bm2.onMetadata(meta("B", 100000), 60300, 54000, 1000);
    EXPECT_EQ(d.kind, Decision::Fade);
    EXPECT_EQ(d.at, 60300u);
    EXPECT_EQ(bm2.lastDeltaMs(), -30300);

    // The eSDK announces the next track 0.4-1.6 s early: that prediction is still trusted.
    BoundaryModel bm3 = modelWithTrackA();                       // A: 60000 ms from frame 0
    d = bm3.onMetadata(meta("B", 100000), 61800, 55000, 1000);
    EXPECT_EQ(d.kind, Decision::Fade);
    EXPECT_EQ(d.at, 60000u);                                     // delta -1800 ms is inside the sanity window
    EXPECT_EQ(bm3.lastDeltaMs(), -1800);
}

TEST(boundary_flush_then_new_track_cuts_at_flush_frame) {
    BoundaryModel bm = modelWithTrackA();
    bm.onFlush(19000, 100);
    bm.onFlush(20000, 150);                                      // later flush wins (play-press cluster)
    Decision d = bm.onMetadata(meta("B", 100000), 20500, 15000, 400);
    EXPECT_EQ(d.kind, Decision::Cut);                            // a manual skip is a cut, never a fade
    EXPECT_EQ(d.at, 20000u);
    EXPECT_EQ(d.n, 0u);
}

TEST(boundary_flush_then_same_track_cuts_without_fade) {
    BoundaryModel bm = modelWithTrackA();
    bm.onFlush(20000, 100);
    Decision d = bm.onMetadata(meta("A", 60000, 45000), 20500, 15000, 200);
    EXPECT_EQ(d.kind, Decision::Cut);
    EXPECT_EQ(d.at, 20000u);
    Decision later = bm.onMetadata(meta("B", 100000), 35600, 30000, 1000);   // anchor moved to the seek
    EXPECT_EQ(later.at, 35000u);             // 20000 + (60000 - 45000)
}

TEST(boundary_flush_timeout_cuts_once) {
    BoundaryModel bm = modelWithTrackA();
    bm.onFlush(20000, 100);
    EXPECT_EQ(bm.tick(20400, 15000, 500).kind, Decision::None);
    Decision d = bm.tick(20800, 15000, 601);
    EXPECT_EQ(d.kind, Decision::Cut);
    EXPECT_EQ(d.at, 20000u);
    EXPECT_EQ(bm.tick(21000, 15000, 700).kind, Decision::None);
    Decision d2 = bm.onMetadata(meta("B", 100000), 21000, 16000, 800);       // late announcement of the timeout cut
    EXPECT_EQ(d2.kind, Decision::None);
    EXPECT_TRUE(bm.current().playbackId == "B");
}

TEST(boundary_late_metadata_after_window_cuts_once_and_tick_stays_quiet) {
    BoundaryModel bm = modelWithTrackA();
    bm.onFlush(20000, 100);
    Decision d = bm.onMetadata(meta("B", 100000), 20800, 15000, 700);   // 600 ms after the flush, no tick in between
    EXPECT_EQ(d.kind, Decision::Cut);
    EXPECT_EQ(d.at, 20000u);
    EXPECT_EQ(bm.tick(21000, 20000, 750).kind, Decision::None);           // nothing left pending
    EXPECT_TRUE(bm.current().playbackId == "B");
    Decision later = bm.onMetadata(meta("C", 100000), 120000, 115000, 5000);
    EXPECT_EQ(later.at, 120000u);                                          // B anchored at 20000 + 100000
}

TEST(boundary_new_track_soon_after_timeout_cut_is_its_late_announcement) {
    BoundaryModel bm = modelWithTrackA();
    bm.onFlush(20000, 100);
    EXPECT_EQ(bm.tick(20800, 15000, 601).kind, Decision::Cut);            // timeout cut at 20000
    Decision d = bm.onMetadata(meta("B", 100000), 21500, 20000, 1200);   // 600 ms after the cut
    EXPECT_EQ(d.kind, Decision::None);                                    // boundary already executed
    EXPECT_TRUE(bm.current().playbackId == "B");
    Decision later = bm.onMetadata(meta("C", 100000), 120000, 115000, 5000);
    EXPECT_EQ(later.kind, Decision::Fade);
    EXPECT_EQ(later.at, 120000u);                                          // B anchored at the cut frame 20000
}

TEST(boundary_new_track_long_after_timeout_cut_is_a_natural_transition) {
    BoundaryModel bm = modelWithTrackA();                                  // A: 60000 ms from frame 0
    bm.onFlush(20000, 100);
    EXPECT_EQ(bm.tick(20800, 15000, 601).kind, Decision::Cut);
    Decision d = bm.onMetadata(meta("B", 100000), 60200, 55000, 3000);   // 2.4 s later: A really ended
    EXPECT_EQ(d.kind, Decision::Fade);
    EXPECT_EQ(d.at, 60000u);
}

TEST(boundary_newer_flush_supersedes_late_cut_memory) {
    BoundaryModel bm = modelWithTrackA();
    bm.onFlush(20000, 100);
    EXPECT_EQ(bm.tick(20800, 15000, 601).kind, Decision::Cut);            // timeout cut at 20000 arms the memory
    bm.onFlush(21000, 650);                                                // a second skip
    Decision skip = bm.onMetadata(meta("B", 100000), 21200, 16000, 700);   // young flush: normal skip
    EXPECT_EQ(skip.kind, Decision::Cut);
    EXPECT_EQ(skip.at, 21000u);
    Decision next = bm.onMetadata(meta("C", 100000), 90000, 85000, 900);   // unrelated transition 299 ms after the timeout cut
    EXPECT_EQ(next.kind, Decision::Fade);                                  // must NOT be swallowed as a late announcement
    EXPECT_EQ(next.at, 90000u);                                            // prediction 121000 is out of the window
    EXPECT_TRUE(bm.current().playbackId == "C");
}

TEST(boundary_ads_and_episodes_cut_video_does_not) {
    BoundaryModel bm = modelWithTrackA();
    EXPECT_EQ(bm.onMetadata(meta("AD", 30000, 0, "spotify:ad:1", true), 60000, 55000, 1000).kind, Decision::Cut);
    EXPECT_EQ(bm.onMetadata(meta("C", 100000), 90000, 85000, 2000).kind, Decision::Cut);          // outgoing is the ad
    EXPECT_EQ(bm.onMetadata(meta("D", 100000), 190000, 185000, 3000).kind, Decision::Fade);       // ad is gone
    EXPECT_EQ(bm.onMetadata(meta("E", 100000, 0, "spotify:episode:9"), 290000, 285000, 4000).kind, Decision::Cut);
    EXPECT_EQ(bm.onMetadata(meta("F", 100000), 390000, 385000, 5000).kind, Decision::Cut);        // outgoing episode
    EXPECT_EQ(bm.onMetadata(meta("G", 100000, 0, "spotify:track:g", false, true), 490000, 485000, 6000).kind, Decision::Fade);   // video is not a veto
}

TEST(boundary_short_tail_shortens_or_cuts) {
    BoundaryModel bm = modelWithTrackA();
    Decision d = bm.onMetadata(meta("B", 100000), 60000, 57000, 1000);   // tail 3000 < X
    EXPECT_EQ(d.kind, Decision::Fade);
    EXPECT_EQ(d.n, 3000u);
    BoundaryModel bm2 = modelWithTrackA();
    Decision d2 = bm2.onMetadata(meta("B", 100000), 60000, 59900, 1000); // tail 100 < 200 ms
    EXPECT_EQ(d2.kind, Decision::Cut);
    EXPECT_EQ(d2.at, 60000u);
}

TEST(boundary_crossfade_off_cuts) {
    BoundaryModel bm = modelWithTrackA(0);
    EXPECT_EQ(bm.onMetadata(meta("B", 100000), 60000, 54000, 1000).kind, Decision::Cut);
    bm.setCrossfadeFrames(2000);
    EXPECT_EQ(bm.onMetadata(meta("C", 100000), 160000, 154000, 2000).n, 2000u);
}

TEST(boundary_startup_flush_before_any_track_cuts_at_flush) {
    BoundaryModel bm;
    bm.configure(1000, 5000);
    bm.onFlush(0, 0);
    bm.onFlush(300, 200);
    Decision d = bm.onMetadata(meta("A", 60000), 800, 0, 300);
    EXPECT_EQ(d.kind, Decision::Cut);        // tail 300 - 0 >= 200 but there is no outgoing track to fade
    EXPECT_EQ(d.at, 300u);
}
