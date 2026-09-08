#include <SLES/OpenSLES.h>
#include <dirent.h>
#include <time.h>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>
#include "test.h"
#include "fake_backend.h"
#include "../src/mixer.h"
#include "../src/ring_pipeline.h"
#include "../src/status.h"

namespace {

int64_t g_now = 0;                 // fake monotonic clock (ms) for the pipeline under test
int64_t fakeNow() { return g_now; }
void sleepMs(int ms) { timespec ts{ms / 1000, (ms % 1000) * 1000000L}; nanosleep(&ts, nullptr); }

// The debug dumper writes one <dir>/xfade_dump_<n>.pcm per trigger, so counting the files counts triggers.
const char* kDumpDir = "/data/local/tmp";
std::vector<std::string> dumpFiles() {
    std::vector<std::string> names;
    DIR* d = opendir(kDumpDir);
    if (!d) return names;
    while (dirent* e = readdir(d)) {
        std::string name = e->d_name;
        if (name.rfind("xfade_dump_", 0) == 0 && name.size() > 4 && name.compare(name.size() - 4, 4, ".pcm") == 0)
            names.push_back(name);
    }
    closedir(d);
    return names;
}
int countDumps() { return static_cast<int>(dumpFiles().size()); }
void removeDumps() {
    for (const std::string& n : dumpFiles()) std::remove((std::string(kDumpDir) + "/" + n).c_str());
}

// The test is the eSDK: it enqueues, gets acknowledged on the pipeline's ack thread, and drives the
// back end's completion callback by hand (completeOne = the audio thread finished one 10 ms buffer).
struct Harness {
    FakeBackend be;
    xfade::RingPipeline p;
    std::atomic<int> acks{0};
    uint32_t ch;

    explicit Harness(int crossfadeMs, uint32_t rate = 1000, uint32_t channels = 2) : ch(channels) {
        g_now = 0;
        xfade::setCrossfadeMs(crossfadeMs);
        p.setClockForTests(fakeNow);
        p.setBufferDoneCallback([](void* c) { ++static_cast<Harness*>(c)->acks; }, this);
        EXPECT_TRUE(p.start(xfade::PcmFormat{rate, channels, 16, 2}, &be));
    }
    ~Harness() { p.stop(); }

    // Copies semantics: the source buffer dies when this returns.
    void enqueue(int16_t value, uint32_t frames) {
        std::vector<int16_t> buf(static_cast<size_t>(frames) * ch, value);
        EXPECT_EQ(p.enqueue(buf.data(), static_cast<uint32_t>(buf.size() * 2)), (uint32_t)SL_RESULT_SUCCESS);
    }
    bool waitAcks(int n, int timeoutMs = 1000) {
        for (int i = 0; i < timeoutMs && acks.load() < n; ++i) sleepMs(1);
        return acks.load() >= n;
    }
    int acksAfterSettling() { sleepMs(80); return acks.load(); }
    int16_t* lastOut() { return static_cast<int16_t*>(const_cast<void*>(be.enqueued.back().data)); }
    // Completes output buffers until 'read' reaches the given frame; returns the last filled buffer.
    int16_t* drainTo(uint64_t readFrame) {
        while (p.snapshot().read < readFrame) be.completeOne();
        return lastOut();
    }
    bool waitRead(uint64_t frame, int timeoutMs = 1000) {
        for (int i = 0; i < timeoutMs && p.snapshot().read < frame; ++i) sleepMs(1);
        return p.snapshot().read >= frame;
    }
};

}  // namespace

TEST(ring_pipeline_primes_output_and_copies_on_enqueue) {
    Harness h(2000);                                   // rate 1000: X = 2000 frames, T = 3000
    EXPECT_EQ(h.be.enqueued.size(), (size_t)4);        // 4 x 10 ms silence primed
    EXPECT_EQ(h.be.opened.rateHz, 1000u);
    // Our own queue depth, not the 2 buffers the eSDK asked us for.
    EXPECT_EQ(h.be.opened.numBuffers, (uint32_t)xfade::RingPipeline::kOutBuffers);
    h.enqueue(7, 1000);
    EXPECT_TRUE(h.waitAcks(1));
    h.be.completeOne();                                // frames 0..9 of the ring go out
    EXPECT_EQ(h.be.enqueued.size(), (size_t)4);
    EXPECT_EQ(h.lastOut()[0], (int16_t)7);
    EXPECT_EQ(h.lastOut()[19], (int16_t)7);
    EXPECT_EQ(h.p.snapshot().read, 10u);
    EXPECT_EQ(h.p.snapshot().written, 1000u);
}

TEST(ring_pipeline_acks_only_up_to_the_lead_target) {
    Harness h(2000);                                   // T = 3000 frames
    for (int i = 1; i <= 4; ++i) h.enqueue((int16_t)i, 1000);
    EXPECT_TRUE(h.waitAcks(3));                        // ends 1000, 2000, 3000 <= read(0) + 3000
    EXPECT_EQ(h.acksAfterSettling(), 3);               // end 4000 is withheld
    uint32_t count = 0, index = 0;
    h.p.getState(&count, &index);
    EXPECT_EQ(count, 1u);
    EXPECT_EQ(index, 3u);
    h.drainTo(1000);                                   // read = 1000: 4000 <= 1000 + 3000
    EXPECT_TRUE(h.waitAcks(4));
    h.p.getState(&count, &index);
    EXPECT_EQ(count, 0u);
    EXPECT_EQ(index, 4u);
    EXPECT_EQ(h.lastOut()[0], (int16_t)1);             // frames 990..999 are still buffer 1
    h.drainTo(1010);
    EXPECT_EQ(h.lastOut()[0], (int16_t)2);
}

TEST(ring_pipeline_crossfade_off_keeps_a_minimum_lead) {
    Harness h(0);                                      // X = 0: T = kMinLeadMs = 500 frames at 1000 Hz
    EXPECT_TRUE(std::strcmp(h.p.stateName(), "passthrough") == 0);
    h.enqueue(5, 300);
    EXPECT_TRUE(h.waitAcks(1));                        // 300 <= 0 + 500
    h.enqueue(6, 300);
    EXPECT_EQ(h.acksAfterSettling(), 1);               // 600 > 500: waits for consumption
    h.drainTo(90);
    EXPECT_EQ(h.acksAfterSettling(), 1);
    h.drainTo(100);
    EXPECT_TRUE(h.waitAcks(2));                        // 600 <= 100 + 500
}

TEST(ring_pipeline_underrun_is_silence_and_counted) {
    Harness h(0);
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    EXPECT_EQ(h.be.state, (uint32_t)SL_PLAYSTATE_PLAYING);
    h.enqueue(9, 15);
    h.be.completeOne();                                // 10 frames of audio
    EXPECT_EQ(h.lastOut()[0], (int16_t)9);
    h.be.completeOne();                                // 5 frames of audio + 5 of silence
    EXPECT_EQ(h.lastOut()[8], (int16_t)9);
    EXPECT_EQ(h.lastOut()[10], (int16_t)0);
    EXPECT_EQ(h.p.snapshot().underruns, 1u);
    h.be.completeOne();                                // all silence
    EXPECT_EQ(h.lastOut()[0], (int16_t)0);
    EXPECT_EQ(h.p.snapshot().underruns, 2u);
    EXPECT_EQ(h.p.snapshot().read, 15u);               // read never passes written
}

TEST(ring_pipeline_clear_drops_pending_acks_but_keeps_audio) {
    Harness h(2000);
    for (int i = 1; i <= 4; ++i) h.enqueue((int16_t)i, 1000);
    EXPECT_TRUE(h.waitAcks(3));
    EXPECT_EQ(h.p.clear(), (uint32_t)SL_RESULT_SUCCESS);
    uint32_t count = 9, index = 0;
    h.p.getState(&count, &index);
    EXPECT_EQ(count, 0u);
    h.drainTo(1500);
    EXPECT_EQ(h.acksAfterSettling(), 3);               // the cleared buffer is never acknowledged
    EXPECT_EQ(h.lastOut()[0], (int16_t)2);             // audio after the flush point still plays (Task 4 decides the cut)
    EXPECT_EQ(h.be.clears, 0);                         // the real queue is never cleared: it holds our own buffers
}

TEST(ring_pipeline_pause_forwards_state_and_keeps_acking) {
    Harness h(2000);
    h.p.setPlayState(SL_PLAYSTATE_PAUSED);
    EXPECT_EQ(h.be.state, (uint32_t)SL_PLAYSTATE_PAUSED);
    EXPECT_EQ(h.p.playState(), (uint32_t)SL_PLAYSTATE_PAUSED);
    h.enqueue(1, 500);
    EXPECT_TRUE(h.waitAcks(1));                        // lead below target: acknowledged while paused
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], (int16_t)1);
}

TEST(ring_pipeline_crossfade_change_applies_immediately) {
    Harness h(2000);
    for (int i = 1; i <= 4; ++i) h.enqueue((int16_t)i, 1000);
    EXPECT_TRUE(h.waitAcks(3));
    h.p.setCrossfadeMs(0);                             // T drops to 0: nothing more until consumed
    EXPECT_TRUE(std::strcmp(h.p.stateName(), "passthrough") == 0);
    h.drainTo(3400);
    EXPECT_EQ(h.acksAfterSettling(), 3);
    h.drainTo(3500);
    EXPECT_TRUE(h.waitAcks(4));
    h.drainTo(4000);                                   // empty the ring again: the status line below reads the lead
    h.p.setCrossfadeMs(12000);
    EXPECT_TRUE(std::strcmp(h.p.stateName(), "active") == 0);
    EXPECT_TRUE(h.p.stats().find("crossfade_ms=12000") != std::string::npos);
    EXPECT_TRUE(xfade::statusLine().find("lead_ms=0") != std::string::npos);
}

TEST(ring_pipeline_stats_and_cut) {
    Harness h(1000);
    h.enqueue(1, 2000);
    EXPECT_TRUE(h.waitAcks(1));
    EXPECT_TRUE(h.p.stats() == "lead_ms=2000 crossfade_ms=1000 fades=0 last_delta_ms=+0 underruns=0");
    xfade::Decision cut;
    cut.kind = xfade::Decision::Cut;
    cut.at = 1500;
    h.p.applyForTests(cut);
    EXPECT_EQ(h.p.snapshot().read, 1500u);
    h.p.applyForTests(cut);                            // never moves backwards
    EXPECT_EQ(h.p.snapshot().read, 1500u);
    cut.at = 9000;                                     // beyond written: clamps
    h.p.applyForTests(cut);
    EXPECT_EQ(h.p.snapshot().read, 2000u);
}

TEST(ring_pipeline_rejects_non_16_bit_and_overflow) {
    FakeBackend be;
    xfade::RingPipeline p;
    EXPECT_TRUE(!p.start(xfade::PcmFormat{44100, 2, 8, 2}, &be));
    EXPECT_EQ(p.enqueue(nullptr, 0), (uint32_t)SL_RESULT_RESOURCE_ERROR);   // not started
    Harness h(0, 100, 1);                              // capacity 3000 frames
    h.enqueue(1, 2000);
    h.enqueue(2, 2000);                                // 4000 > 3000: oldest unplayed frames are dropped, no crash
    EXPECT_EQ(h.p.snapshot().written, 4000u);
    EXPECT_EQ(h.p.snapshot().read, 1000u);
    std::vector<int16_t> huge(3100, 0);
    EXPECT_EQ(h.p.enqueue(huge.data(), 6200), (uint32_t)SL_RESULT_PARAMETER_INVALID);
}

TEST(ring_pipeline_start_fails_when_priming_fails) {
    FakeBackend be;
    be.enqueueResult = SL_RESULT_BUFFER_INSUFFICIENT;   // a player whose queue is shallower than ours
    xfade::RingPipeline p;
    EXPECT_TRUE(!p.start(xfade::PcmFormat{1000, 2, 16, 2}, &be));
    EXPECT_TRUE(FakeBackend::s_closed);                 // the half-open player is closed again
    be.enqueueResult = SL_RESULT_SUCCESS;
}

TEST(ring_pipeline_stop_from_the_ack_thread_is_safe) {
    Harness h(2000);
    // The eSDK destroys the player from inside its own buffer callback, so stop() runs on the ack thread.
    h.p.setBufferDoneCallback([](void* c) {
        auto* self = static_cast<Harness*>(c);
        if (self->acks.fetch_add(1) == 0) self->p.stop();
    }, &h);
    h.enqueue(3, 100);                                  // acknowledged at once: lead is far below the target
    for (int i = 0; i < 1000 && !FakeBackend::s_closed; ++i) sleepMs(1);
    sleepMs(80);   // s_closed is set mid-stop(); let the ack thread unwind out of stop() and the loop
    EXPECT_TRUE(FakeBackend::s_closed);
    EXPECT_EQ(h.p.enqueue(&h, 4), (uint32_t)SL_RESULT_RESOURCE_ERROR);
    // ~Harness calls stop() a second time; it must be a no-op.
}

TEST(ring_pipeline_stop_is_idempotent_and_closes_backend) {
    FakeBackend be;
    {
        xfade::RingPipeline p;
        xfade::setCrossfadeMs(1000);
        EXPECT_TRUE(p.start(xfade::PcmFormat{1000, 2, 16, 2}, &be));
        p.stop();
        EXPECT_TRUE(FakeBackend::s_closed);
        EXPECT_EQ(p.enqueue(&be, 4), (uint32_t)SL_RESULT_RESOURCE_ERROR);
        p.stop();                                      // second stop is a no-op; destructor calls it again
    }
    EXPECT_TRUE(xfade::statusLine().find("lead_ms") == std::string::npos);   // stats source unregistered
}

namespace {
xfade::TrackMeta track(const char* id, int64_t durationMs, int64_t positionMs = 0, const char* uri = "spotify:track:x",
                       bool ad = false, bool video = false) {
    xfade::TrackMeta m;
    m.playbackId = id; m.trackUri = uri; m.durationMs = durationMs; m.positionMs = positionMs; m.isAd = ad; m.isVideo = video;
    return m;
}
// Expected mixed sample for constant tracks a and b at fade position k of n.
int16_t mixed(int a, int b, uint32_t k, uint32_t n) {
    int16_t va[2] = {(int16_t)a, (int16_t)a}, vb[2] = {(int16_t)b, (int16_t)b}, out[2];
    xfade::mixEqualPower(va, vb, out, 1, 2, k, n);
    return out[0];
}
}  // namespace

// The pipeline is current() from its constructor, so JNI can reach it before start(). Until start()
// publishes backend_ nothing may act on it, and start() must then leave it armed with no remembered track.
TEST(ring_pipeline_metadata_before_start_is_ignored) {
    FakeBackend be;
    xfade::RingPipeline p;
    g_now = 0;
    xfade::setCrossfadeMs(0);
    p.setClockForTests(fakeNow);
    p.onMetadata(track("A", 1000));                    // no format, no ring, no backend yet: ignored
    p.setCrossfadeMs(3000);                            // ... and start() takes the crossfade from the global
    EXPECT_TRUE(!p.snapshot().armed);
    EXPECT_TRUE(p.start(xfade::PcmFormat{1000, 2, 16, 2}, &be));   // X = 0, T = 500 frames
    EXPECT_TRUE(p.snapshot().armed);                   // the player start is itself a boundary
    std::vector<int16_t> buf(2 * 600, 7);
    EXPECT_EQ(p.enqueue(buf.data(), (uint32_t)(buf.size() * 2)), (uint32_t)SL_RESULT_SUCCESS);
    EXPECT_TRUE(!p.snapshot().armed);                  // 600 frames delivered >= T: the arm is done
    p.onMetadata(track("A", 1000));                    // lastPlaybackId_ was never set: still a new track
    EXPECT_TRUE(p.snapshot().armed);
    p.stop();
}

TEST(fade_natural_transition_mixes_then_jumps_to_b_plus_n) {
    Harness h(1000);                                   // X = 1000 frames, T = 2000
    h.p.onMetadata(track("A", 5000));                  // announced at written 0
    for (int i = 0; i < 5; ++i) h.enqueue(10000, 1000);  // A: 5000 frames of 10000
    g_now = 100;
    h.p.onMetadata(track("B", 9000));                  // at written 5000: predicted 0 + 5000 = 5000 -> fade [4000, 5000)
    xfade::RingPipeline::Snapshot s = h.p.snapshot();
    EXPECT_TRUE(s.fadeScheduled);
    EXPECT_EQ(s.fadeAt, 5000u);
    EXPECT_EQ(s.fadeN, 1000u);
    for (int i = 0; i < 3; ++i) h.enqueue(-10000, 1000);   // B: 3000 frames of -10000
    h.drainTo(4000);
    EXPECT_EQ(h.lastOut()[0], (int16_t)10000);         // frames 3990..3999: still A
    h.be.completeOne();                                // frames 4000..4009: fade positions 0..9
    EXPECT_EQ(h.lastOut()[0], mixed(10000, -10000, 0, 1000));
    EXPECT_EQ(h.lastOut()[19], mixed(10000, -10000, 9, 1000));
    h.drainTo(5000);                                   // last fade buffer: positions 990..999
    EXPECT_EQ(h.lastOut()[18], mixed(10000, -10000, 999, 1000));
    EXPECT_EQ(h.p.snapshot().read, 6000u);             // b + n: the mixed-in head of B is consumed
    EXPECT_EQ(h.p.snapshot().fades, 1u);
    EXPECT_TRUE(!h.p.snapshot().fadeScheduled);
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], (int16_t)-10000);        // B continues from frame 6000
    EXPECT_EQ(h.p.snapshot().underruns, 0u);
    EXPECT_TRUE(h.p.stats().find("fades=1") != std::string::npos);
    EXPECT_TRUE(h.p.stats().find("last_delta_ms=+0") != std::string::npos);
}

TEST(fade_dump_triggers_when_the_fade_starts) {
    std::remove("/data/local/tmp/xfade_dump_1.pcm");   // in case a stale file survived an earlier run
    Harness h(1000);                                   // X = 1000 frames, T = 2000
    h.p.dumperForTests().configureForTests("/data/local/tmp", 1000, 2, true);
    h.p.onMetadata(track("A", 5000));                  // announced at written 0
    for (int i = 0; i < 5; ++i) h.enqueue(10000, 1000);  // A: 5000 frames of 10000
    g_now = 100;
    h.p.onMetadata(track("B", 9000));                  // at written 5000: fade [4000, 5000)
    for (int i = 0; i < 3; ++i) h.enqueue(-10000, 1000);   // B: 3000 frames of -10000
    h.drainTo(3990);                                   // right before the fade starts: not triggered yet
    FILE* before = std::fopen("/data/local/tmp/xfade_dump_1.pcm", "rb");
    EXPECT_TRUE(before == nullptr);
    if (before) std::fclose(before);
    h.drainTo(4010);                                   // the fade has now started (read_ passed fadeStart_=4000)
    FILE* after = std::fopen("/data/local/tmp/xfade_dump_1.pcm", "rb");
    EXPECT_TRUE(after != nullptr);
    if (after) { std::fclose(after); std::remove("/data/local/tmp/xfade_dump_1.pcm"); }
}

// A fade whose outgoing tail has not been delivered sits at k0 == 0 for as long as the ring is dry; the
// dump must be triggered by the first such buffer only, not by every one of them.
TEST(fade_dump_triggers_once_when_the_fade_start_stalls) {
    removeDumps();
    Harness h(1000);                                   // X = 1000 frames, T = 2000
    h.p.dumperForTests().configureForTests(kDumpDir, 1000, 2, true);
    h.p.onMetadata(track("A", 5000));                  // anchored at written 0
    for (int i = 0; i < 4; ++i) h.enqueue(10000, 1000);   // only 4000 of A's 5000 frames ever arrive
    g_now = 100;
    h.p.onMetadata(track("B", 9000));                  // predicted boundary 5000 -> fade [4000, 5000)
    EXPECT_EQ(h.p.snapshot().fadeAt, 5000u);
    EXPECT_EQ(countDumps(), 0);
    h.drainTo(4000);                                   // read reaches the fade start with nothing left to mix
    h.be.completeOne();                                // k0 == 0, haveA == 0: the fade cannot start yet
    h.be.completeOne();                                // ... and again, from the same position
    EXPECT_EQ(h.p.snapshot().read, 4000u);
    EXPECT_EQ(countDumps(), 1);
    removeDumps();
}

TEST(fade_frontier_keeps_acking_during_fade) {
    Harness h(1000);                                   // T = 2000
    h.p.onMetadata(track("A", 5000));
    for (int i = 0; i < 5; ++i) h.enqueue(1, 1000);
    h.drainTo(3000);
    EXPECT_TRUE(h.waitAcks(5));                        // all of A: 5000 <= 3000 + 2000
    g_now = 100;
    h.p.onMetadata(track("B", 9000));                  // fade [4000, 5000), n = 1000
    h.enqueue(2, 1000);                                // B end 6000 <= frontier (3000 + 1000) + 2000
    EXPECT_TRUE(h.waitAcks(6));
    h.enqueue(2, 1000);                                // end 7000 > 6000: withheld until read advances
    EXPECT_EQ(h.acksAfterSettling(), 6);
    h.drainTo(4000);
    EXPECT_TRUE(h.waitAcks(7));
}

TEST(fade_skip_cuts_at_flush_frame) {
    Harness h(1000);
    h.p.onMetadata(track("A", 100000));
    for (int i = 0; i < 5; ++i) h.enqueue(10000, 1000);
    h.drainTo(2000);
    g_now = 1000;
    h.p.clear();                                       // skip: flush at written 5000
    for (int i = 0; i < 3; ++i) h.enqueue(-10000, 1000);
    g_now = 1050;
    h.p.onMetadata(track("B", 9000));                  // within the flush window -> a manual skip cuts
    EXPECT_TRUE(!h.p.snapshot().fadeScheduled);
    EXPECT_EQ(h.p.snapshot().read, 5000u);
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], (int16_t)-10000);        // B from its first frame
    EXPECT_EQ(h.p.snapshot().fades, 0u);
}

TEST(fade_seek_cuts_without_fade) {
    Harness h(1000);
    h.p.onMetadata(track("A", 100000));
    for (int i = 0; i < 5; ++i) h.enqueue(10000, 1000);
    h.drainTo(1000);
    g_now = 1000;
    h.p.clear();                                       // seek: flush at 5000
    h.enqueue(20000, 2000);                            // audio from the new position
    g_now = 1050;
    h.p.onMetadata(track("A", 100000, 60000));         // same playback id
    EXPECT_EQ(h.p.snapshot().read, 5000u);             // cut: old tail dropped
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], (int16_t)20000);
    EXPECT_EQ(h.p.snapshot().fades, 0u);
}

TEST(fade_flush_without_metadata_cuts_after_timeout) {
    Harness h(1000);
    h.p.onMetadata(track("A", 100000));
    for (int i = 0; i < 5; ++i) h.enqueue(10000, 1000);
    h.drainTo(1000);
    g_now = 1000;
    h.p.clear();
    h.enqueue(20000, 1000);
    g_now = 1400;
    sleepMs(120);
    EXPECT_EQ(h.p.snapshot().read, 1000u);             // still inside the window
    g_now = 1600;
    EXPECT_TRUE(h.waitRead(5000));                     // the ack thread's tick applied the cut
    EXPECT_EQ(h.p.snapshot().read, 5000u);
}

TEST(fade_short_tail_cuts_instead) {
    Harness h(1000);
    h.p.onMetadata(track("A", 5000));
    for (int i = 0; i < 5; ++i) h.enqueue(10000, 1000);
    h.drainTo(4900);                                   // only 100 frames of tail left (< 200 ms)
    g_now = 100;
    h.p.onMetadata(track("B", 9000));
    EXPECT_TRUE(!h.p.snapshot().fadeScheduled);
    EXPECT_EQ(h.p.snapshot().read, 5000u);
    h.enqueue(-10000, 1000);
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], (int16_t)-10000);
}

TEST(fade_supply_stall_uses_silence_for_missing_incoming_frames) {
    Harness h(1000);
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    h.p.onMetadata(track("A", 5000));
    for (int i = 0; i < 5; ++i) h.enqueue(10000, 1000);
    g_now = 100;
    h.p.onMetadata(track("B", 9000));                  // fade [4000, 5000)
    h.enqueue(-10000, 500);                            // only half of the incoming head arrives
    h.drainTo(4500);
    EXPECT_EQ(h.lastOut()[18], mixed(10000, -10000, 499, 1000));
    h.be.completeOne();                                // positions 500..509: B missing -> A * cos only
    EXPECT_EQ(h.lastOut()[0], mixed(10000, 0, 500, 1000));
    EXPECT_TRUE(h.p.snapshot().underruns >= 1u);
    h.drainTo(5000);                                   // outgoing exhausted; incoming still owed 500 frames
    EXPECT_TRUE(h.p.snapshot().fadeScheduled);
    EXPECT_EQ(h.p.snapshot().fadeKB, 500u);
    EXPECT_EQ(h.p.snapshot().read, 5000u);
    h.be.completeOne();                                // nothing delivered: silence, read holds
    EXPECT_EQ(h.lastOut()[0], (int16_t)0);
    EXPECT_EQ(h.p.snapshot().read, 5000u);
    h.enqueue(-10000, 1000);                           // the rest of B arrives late
    h.be.completeOne();                                // incoming resumes at its own gain index 500
    EXPECT_EQ(h.lastOut()[0], mixed(0, -10000, 500, 1000));
    EXPECT_EQ(h.lastOut()[18], mixed(0, -10000, 509, 1000));
    for (int i = 0; i < 49; ++i) h.be.completeOne();
    EXPECT_TRUE(!h.p.snapshot().fadeScheduled);
    EXPECT_EQ(h.p.snapshot().read, 6000u);             // fadeB_: 5000 + 1000 consumed, nothing skipped
    EXPECT_EQ(h.p.snapshot().fades, 1u);
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], (int16_t)-10000);
}

TEST(fade_late_incoming_head_is_not_skipped) {
    Harness h(1000);
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    h.p.onMetadata(track("A", 5000));
    for (int i = 0; i < 5; ++i) h.enqueue(10000, 1000);
    g_now = 100;
    h.p.onMetadata(track("B", 9000));                  // fade [4000, 5000); nothing of B arrives yet
    h.drainTo(5000);                                   // outgoing fades over silence
    EXPECT_EQ(h.lastOut()[18], mixed(10000, 0, 999, 1000));
    EXPECT_TRUE(h.p.snapshot().underruns >= 1u);
    EXPECT_EQ(h.p.snapshot().read, 5000u);
    EXPECT_EQ(h.p.snapshot().fadeKB, 0u);
    EXPECT_TRUE(h.p.snapshot().fadeScheduled);
    h.be.completeOne();
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], (int16_t)0);
    EXPECT_EQ(h.p.snapshot().read, 5000u);
    for (int i = 0; i < 3; ++i) h.enqueue(-10000, 1000);   // B arrives late
    h.be.completeOne();                                // fades in from its true first frame
    EXPECT_EQ(h.lastOut()[0], mixed(0, -10000, 0, 1000));
    EXPECT_EQ(h.lastOut()[18], mixed(0, -10000, 9, 1000));
    int n = 0;
    while (h.p.snapshot().fadeScheduled && n < 120) { h.be.completeOne(); ++n; }
    EXPECT_TRUE(!h.p.snapshot().fadeScheduled);
    EXPECT_EQ(h.p.snapshot().read, 6000u);             // 5000 + 1000 consumed; no incoming frame skipped
    EXPECT_EQ(h.p.snapshot().fades, 1u);
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], (int16_t)-10000);
}

TEST(fade_pause_mid_fade_resumes_where_it_stopped) {
    Harness h(1000);
    h.p.onMetadata(track("A", 5000));
    for (int i = 0; i < 5; ++i) h.enqueue(10000, 1000);
    g_now = 100;
    h.p.onMetadata(track("B", 9000));
    for (int i = 0; i < 3; ++i) h.enqueue(-10000, 1000);
    h.drainTo(4500);
    h.p.setPlayState(SL_PLAYSTATE_PAUSED);             // the audio thread stops completing buffers
    sleepMs(60);
    EXPECT_EQ(h.p.snapshot().read, 4500u);
    EXPECT_TRUE(h.p.snapshot().fadeScheduled);
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], mixed(10000, -10000, 500, 1000));
}

TEST(fade_cut_cancels_scheduled_fade) {
    Harness h(1000);
    h.p.onMetadata(track("A", 5000));
    for (int i = 0; i < 5; ++i) h.enqueue(10000, 1000);
    g_now = 100;
    h.p.onMetadata(track("B", 9000));                  // fade at 5000
    h.enqueue(-10000, 1000);
    g_now = 200;
    h.p.clear();                                       // user seeks inside B right away: flush at 6000
    h.enqueue(-20000, 1000);
    g_now = 250;
    h.p.onMetadata(track("B", 9000, 30000));           // same id -> cut at 6000
    EXPECT_TRUE(!h.p.snapshot().fadeScheduled);
    EXPECT_EQ(h.p.snapshot().read, 6000u);
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], (int16_t)-20000);
    EXPECT_EQ(h.p.snapshot().fades, 0u);
}

TEST(ring_pipeline_drops_every_zero_buffer_while_armed_and_probes) {
    Harness h(2000);                                   // T = 3000; the player start arms
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);            // the probe only runs while playing
    h.enqueue(0, 500);                                 // padding: not written
    EXPECT_EQ(h.p.snapshot().written, 0u);
    EXPECT_EQ(h.p.snapshot().armDropped, 500u);
    EXPECT_TRUE(h.p.snapshot().armSawZero);
    uint32_t count = 0, index = 0;
    h.p.getState(&count, &index);
    EXPECT_EQ(count, 1u);                              // the eSDK still owns the buffer
    g_now = 50;
    EXPECT_EQ(h.acksAfterSettling(), 0);               // one probe per 100 ms, whatever the buffer holds
    g_now = 110;
    EXPECT_TRUE(h.waitAcks(1));
    h.enqueue(7, 1000);                                // the first real audio: written at frame 0
    EXPECT_EQ(h.p.snapshot().written, 1000u);
    EXPECT_TRUE(h.waitAcks(2));                        // 1000 <= 0 + 3000: the normal lead rule
    h.enqueue(0, 300);                                 // the eSDK pads again: still armed, so still dropped
    EXPECT_EQ(h.p.snapshot().written, 1000u);
    EXPECT_EQ(h.p.snapshot().armDropped, 800u);
    g_now = 260;
    EXPECT_TRUE(h.waitAcks(3));                        // next probe: max(110, 110) + 100 = 210
    h.enqueue(8, 200);                                 // the audio continues, contiguous with the 7s
    EXPECT_EQ(h.p.snapshot().written, 1200u);
    h.drainTo(1000);
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], (int16_t)8);             // no silence between the two pieces of audio
}

TEST(ring_pipeline_flush_and_new_track_re_arm) {
    Harness h(2000);                                   // T = 3000
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    for (int i = 0; i < 5; ++i) h.enqueue(7, 1000);    // the lead target is met: the player-start arm ends
    EXPECT_TRUE(!h.p.snapshot().armed);
    h.enqueue(0, 200);                                 // not armed: a quiet passage is ordinary audio
    EXPECT_EQ(h.p.snapshot().written, 5200u);
    h.p.clear();                                       // a flush is a boundary: re-armed
    EXPECT_TRUE(h.p.snapshot().armed);
    EXPECT_EQ(h.p.snapshot().armDropped, 0u);
    h.enqueue(0, 200);                                 // dropped again
    EXPECT_EQ(h.p.snapshot().written, 5200u);
    EXPECT_EQ(h.p.snapshot().armDropped, 200u);
    EXPECT_TRUE(h.p.snapshot().armSawZero);
    h.enqueue(6, 100);                                 // written, and the arm holds: only 100 frames of the
    EXPECT_EQ(h.p.snapshot().written, 5300u);          // new track have arrived, whatever the old tail holds
    EXPECT_TRUE(h.p.snapshot().armed);
    g_now = 100;
    h.p.onMetadata(track("A", 100000));                // a new playback id is a boundary too
    EXPECT_TRUE(h.p.snapshot().armed);
    EXPECT_EQ(h.p.snapshot().armDropped, 0u);          // the budget starts again
    EXPECT_TRUE(!h.p.snapshot().armSawZero);
}

TEST(ring_pipeline_arm_survives_the_flush_until_new_audio_fills_the_lead) {
    Harness h(2000);                                   // T = 3000
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    for (int i = 0; i < 3; ++i) h.enqueue(5, 1000);    // lead 3000: the player-start arm ends
    EXPECT_TRUE(!h.p.snapshot().armed);
    h.p.clear();                                       // a skip at written 3000, with all of it still unplayed
    EXPECT_TRUE(h.p.snapshot().armed);
    h.enqueue(0, 300);                                 // the eSDK pads while its decoder opens the new track
    EXPECT_EQ(h.p.snapshot().written, 3000u);
    h.enqueue(7, 1000);                                // 1000 frames of the new track: not the lead, only the tail
    EXPECT_EQ(h.p.snapshot().written, 4000u);
    EXPECT_TRUE(h.p.snapshot().armed);                 // so the arm holds and its next zeros are still padding
    h.enqueue(0, 300);
    EXPECT_EQ(h.p.snapshot().written, 4000u);
    EXPECT_EQ(h.p.snapshot().armDropped, 600u);
    h.enqueue(7, 2000);                                // 3000 frames since the flush: the eSDK has caught up
    EXPECT_EQ(h.p.snapshot().written, 6000u);
    EXPECT_TRUE(!h.p.snapshot().armed);
    h.enqueue(0, 100);                                 // and its silence is the track's own again
    EXPECT_EQ(h.p.snapshot().written, 6100u);
}

TEST(ring_pipeline_zero_cap_keeps_zeros) {
    Harness h(2000);
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    for (int i = 0; i < 12; ++i) {                     // 12 s of zeros: all dropped, one probe ack each
        h.enqueue(0, 1000);
        g_now += 130;
        EXPECT_TRUE(h.waitAcks(i + 1));
    }
    EXPECT_EQ(h.p.snapshot().written, 0u);
    EXPECT_TRUE(!h.p.snapshot().armed);                // cap reached: we stop guessing and keep the zeros
    h.enqueue(0, 1000);
    EXPECT_EQ(h.p.snapshot().written, 1000u);
}

TEST(fade_cut_during_incoming_only_phase_does_not_replay) {
    Harness h(1000);
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    h.p.onMetadata(track("A", 5000));
    for (int i = 0; i < 5; ++i) h.enqueue(10000, 1000);
    g_now = 100;
    h.p.onMetadata(track("B", 9000));                  // fade [4000, 5000)
    h.enqueue(-10000, 500);                            // half the head
    h.drainTo(5000);                                   // outgoing exhausted, 500 incoming consumed, read parked at 5000
    EXPECT_EQ(h.p.snapshot().fadeKB, 500u);
    xfade::Decision cut;                               // a cut arriving now (e.g. seek) must not replay 5000..5499
    cut.kind = xfade::Decision::Cut;
    cut.at = 5200;
    h.p.applyForTests(cut);
    EXPECT_TRUE(!h.p.snapshot().fadeScheduled);
    EXPECT_EQ(h.p.snapshot().read, 5500u);             // clamped to fadeB_ first, then max(read, at)
}

TEST(ring_pipeline_probe_waits_for_play_and_arm_resets_on_flush) {
    Harness h(2000);                                   // not playing yet; armed by the player start
    h.enqueue(0, 500);                                 // padding, first probe due at 100 ms
    g_now = 800;
    EXPECT_EQ(h.acksAfterSettling(), 0);               // not playing: no probe
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    EXPECT_TRUE(h.waitAcks(1));                        // playing and past due
    for (int i = 0; i < 6; ++i) h.enqueue(0, 1000);    // 6 s more padding (6.5 s dropped in this arm)
    EXPECT_EQ(h.p.snapshot().armDropped, 6500u);
    h.p.clear();                                       // play-press flush: the budget starts again
    EXPECT_EQ(h.p.snapshot().armDropped, 0u);
    EXPECT_TRUE(h.p.snapshot().armed);
    for (int i = 0; i < 11; ++i) h.enqueue(0, 1000);   // 11 s: still under the cap after the reset
    EXPECT_TRUE(h.p.snapshot().armed);
    EXPECT_EQ(h.p.snapshot().written, 0u);
    h.enqueue(0, 1000);                                // 12 s: cap
    EXPECT_TRUE(!h.p.snapshot().armed);
}

TEST(fade_prediction_ignores_dropped_padding) {
    Harness h(1000);                                   // X = 1000
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    h.p.onMetadata(track("A", 10000));                 // a new track: armed
    h.enqueue(0, 500);                                 // 500 ms of padding at A's head: dropped
    h.enqueue(7, 3000);                                // A's audio: ring frames 0..2999
    h.p.onMetadata(track("A", 10000, 3500));           // eSDK says 3500 ms (it counts the padding); real is 3000
    h.enqueue(7, 7000);                                // A continues to ring frame 10000
    g_now = 100;
    h.p.onMetadata(track("B", 5000));                  // announced at written 10000
    EXPECT_TRUE(h.p.snapshot().fadeScheduled);
    EXPECT_EQ(h.p.snapshot().fadeAt, 10000u);          // predicted 3000 + (10000 - 3000) = 10000, not 9500
}

TEST(fade_reanchors_to_the_incoming_audio_after_interleaved_zeros) {
    Harness h(1000);                                   // X = 1000, T = 2000
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    h.p.onMetadata(track("A", 5000));                  // A: a 5000 ms file
    for (int i = 0; i < 4; ++i) h.enqueue(10000, 1000);
    h.drainTo(2000);                                   // the eSDK is never further ahead than the lead target
    g_now = 100;
    h.p.onMetadata(track("B", 9000));                  // announced early at written 4000: fade [4000, 5000)
    EXPECT_EQ(h.p.snapshot().fadeAt, 5000u);
    EXPECT_TRUE(h.p.snapshot().armed);
    EXPECT_TRUE(!h.p.snapshot().armSawZero);
    h.enqueue(10000, 500);                             // A's remaining audio: written 4500, nothing dropped
    EXPECT_EQ(h.p.snapshot().written, 4500u);
    h.enqueue(0, 300);                                 // the eSDK starts padding
    h.enqueue(-10000, 50);                             // B's first real chunk: the fade re-anchors here
    EXPECT_EQ(h.p.snapshot().fadeAt, 4500u);
    EXPECT_EQ(h.p.snapshot().fadeN, 1000u);
    EXPECT_TRUE(h.p.snapshot().armReanchored);
    h.enqueue(0, 800);                                 // its decoder starves again: dropped, no second re-anchor
    EXPECT_EQ(h.p.snapshot().fadeAt, 4500u);
    EXPECT_EQ(h.p.snapshot().written, 4550u);
    h.enqueue(-10000, 2000);                           // the rest of B, contiguous with its first chunk
    EXPECT_EQ(h.p.snapshot().written, 6550u);
    h.drainTo(3500);
    EXPECT_EQ(h.lastOut()[0], (int16_t)10000);         // still A before the re-anchored window
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], mixed(10000, -10000, 0, 1000));   // A and B overlap from the first fade frame
    h.drainTo(4500);
    EXPECT_EQ(h.p.snapshot().read, 5500u);             // b + n with b = 4500
    EXPECT_EQ(h.p.snapshot().fades, 1u);
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], (int16_t)-10000);        // no silence anywhere
    EXPECT_EQ(h.p.snapshot().underruns, 0u);
}

TEST(fade_reanchor_with_tiny_tail_keeps_fading) {
    Harness h(1000);
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    h.p.onMetadata(track("A", 5000));
    for (int i = 0; i < 5; ++i) h.enqueue(10000, 1000);
    g_now = 100;
    h.p.onMetadata(track("B", 9000));                  // fade [4000, 5000)
    h.drainTo(4950);                                   // the fade is 950 frames in when B's audio finally arrives
    h.enqueue(0, 2000);                                // zero run (window opens at written 5000)
    h.enqueue(-10000, 2000);                           // only 50 frames of tail left, but A has already been faded down
    xfade::RingPipeline::Snapshot s = h.p.snapshot();
    EXPECT_TRUE(s.fadeScheduled);                      // cutting here would step the outgoing gain back up
    EXPECT_EQ(s.fadeAt, 5000u);
    int n = 0;
    while (h.p.snapshot().fadeScheduled && n < 300) { h.be.completeOne(); ++n; }
    EXPECT_EQ(h.p.snapshot().read, 6000u);             // 5000 + the 1000 incoming frames of the fade
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], (int16_t)-10000);
}

TEST(ring_pipeline_arm_expires_when_the_lead_is_met) {
    Harness h(2000);                                   // T = 3000
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    for (int i = 0; i < 2; ++i) h.enqueue(5, 1000);    // lead 2000 < T: the eSDK is still behind
    EXPECT_TRUE(h.p.snapshot().armed);
    h.enqueue(0, 500);                                 // so its zeros are still padding
    EXPECT_EQ(h.p.snapshot().written, 2000u);
    h.enqueue(5, 1000);                                // lead 3000 = T: it has caught up, the arm ends
    EXPECT_TRUE(!h.p.snapshot().armed);
    h.enqueue(0, 500);                                 // a quiet passage inside the song is kept
    EXPECT_EQ(h.p.snapshot().written, 3500u);
}

TEST(ring_pipeline_arm_hard_cap) {
    Harness h(12000);                                  // T = 13000 frames: a lead the test never grants
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    for (int i = 0; i < 46; ++i) {                     // the speaker keeps up, so the lead stays at 500
        h.enqueue(5, 1000);
        h.drainTo(h.p.snapshot().written - 500);
    }
    EXPECT_TRUE(!h.p.snapshot().armed);                // 45 s of audio: we stop guessing
    h.enqueue(0, 500);
    EXPECT_EQ(h.p.snapshot().written, 46500u);
}

TEST(fade_reanchor_keeps_the_outgoing_gain_continuous) {
    Harness h(1000);                                   // X = 1000, T = 2000
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    h.p.onMetadata(track("A", 5000));
    for (int i = 0; i < 4; ++i) h.enqueue(10000, 1000);
    h.drainTo(2000);                                   // the eSDK is never further ahead than the lead target
    g_now = 100;
    h.p.onMetadata(track("B", 9000));                  // predicted 5000 -> fade [4000, 5000) scheduled
    h.enqueue(10000, 500);                             // A's audio to 4500 (the lead is still under the target)
    h.drainTo(4250);                                   // the old fade has started: k0 = 250 of 1000
    EXPECT_EQ(h.lastOut()[18], mixed(10000, 0, 249, 1000));   // outgoing fading over silence (B not there yet)
    h.enqueue(0, 2000);                                // the run: window opens at written 4500
    h.enqueue(-10000, 2000);                           // B arrives: re-anchor to 4500 with the outgoing at its current gain
    xfade::RingPipeline::Snapshot s = h.p.snapshot();
    EXPECT_TRUE(s.fadeScheduled);
    EXPECT_EQ(s.fadeAt, 4500u);
    EXPECT_EQ(s.fadeN, 1000u);                         // incoming still fades in over X
    h.be.completeOne();                                // first mixed buffer after the re-anchor
    int16_t before = mixed(10000, 0, 250, 1000);       // the gain the outgoing had reached
    int16_t got = h.lastOut()[0];                      // A at ~the same gain + B at its first sin step
    int16_t bPart = mixed(0, -10000, 0, 1000);
    EXPECT_TRUE(got - bPart >= before - 400 && got - bPart <= before + 400);   // continuous within ~3 %
    h.drainTo(4500);                                   // outgoing reaches its end at ~zero gain
    EXPECT_TRUE(std::abs(h.lastOut()[18] - mixed(0, -10000, 249, 1000)) <= 400);
    EXPECT_EQ(h.p.snapshot().read, 4500u);             // incoming-only phase continues B's fade-in
    int n = 0;
    while (h.p.snapshot().fadeScheduled && n < 200) { h.be.completeOne(); ++n; }
    EXPECT_EQ(h.p.snapshot().read, 5500u);             // 4500 + 1000 consumed
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], (int16_t)-10000);
}

TEST(fade_flush_arm_ends_in_a_cut_not_a_fade) {
    Harness h(1000);
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    h.p.onMetadata(track("A", 5000));
    for (int i = 0; i < 4; ++i) h.enqueue(10000, 1000);
    g_now = 100;
    h.p.onMetadata(track("B", 9000));                  // fade at 5000 scheduled, armed
    g_now = 150;
    h.p.clear();                                       // manual skip before the boundary: re-armed, cut pending
    h.enqueue(0, 300);                                 // padding dropped
    h.enqueue(-20000, 1000);                           // the new track's audio (the doomed fade may re-anchor here)
    g_now = 200;
    h.p.onMetadata(track("C", 9000));                  // the skip's metadata: cut at the flush frame 4000
    EXPECT_TRUE(!h.p.snapshot().fadeScheduled);        // the cut cancels the fade either way
    EXPECT_EQ(h.p.snapshot().read, 4000u);
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], (int16_t)-20000);        // instant switch
}

TEST(fade_fallback_announced_boundary_is_reanchored) {
    Harness h(1000);
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    h.p.onMetadata(track("A", 60000));                 // prediction 60000 is far from the announcement below
    for (int i = 0; i < 5; ++i) h.enqueue(10000, 1000);
    h.drainTo(3000);                                   // the eSDK is never further ahead than the lead target
    g_now = 100;
    h.p.onMetadata(track("B", 9000));                  // announced at 5000: fallback b = 5000, fade [4000, 5000)
    EXPECT_EQ(h.p.snapshot().fadeAt, 5000u);
    h.enqueue(10000, 700);                             // A's remaining audio (announce-early)
    h.enqueue(0, 1000);                                // run
    h.enqueue(-10000, 2000);                           // B: re-anchor to 5700
    EXPECT_EQ(h.p.snapshot().fadeAt, 5700u);
    h.drainTo(4700);
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], mixed(10000, -10000, 0, 1000));
}

TEST(fade_reanchor_cancels_only_when_not_started) {
    {
        Harness h(1000);                               // X = 1000: fade [4000, 5000)
        h.p.setPlayState(SL_PLAYSTATE_PLAYING);
        h.p.onMetadata(track("A", 5000));
        for (int i = 0; i < 5; ++i) h.enqueue(10000, 1000);
        g_now = 100;
        h.p.onMetadata(track("B", 9000));
        h.drainTo(3000);                               // the fade has not started yet
        h.enqueue(0, 1500);                            // run
        h.enqueue(-10000, 2000);                       // tail 2000 >= 200 ms: a normal re-anchor
        xfade::RingPipeline::Snapshot s = h.p.snapshot();
        EXPECT_TRUE(s.fadeScheduled);
        EXPECT_EQ(s.fadeAt, 5000u);
        EXPECT_EQ(s.fadeNA, 1000u);
        EXPECT_EQ(s.fadeN, 1000u);
    }
    Harness h(100);                                    // X = 100: fade [4900, 5000)
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    h.p.onMetadata(track("A", 5000));
    for (int i = 0; i < 5; ++i) h.enqueue(10000, 1000);
    g_now = 100;
    h.p.onMetadata(track("B", 9000));
    EXPECT_EQ(h.p.snapshot().fadeN, 100u);
    h.drainTo(4850);                                   // still before the fade starts
    h.enqueue(0, 1500);                                // run
    h.enqueue(-10000, 1000);                           // tail 150 frames < 200 ms and nothing was faded: cut
    EXPECT_TRUE(!h.p.snapshot().fadeScheduled);
    EXPECT_EQ(h.p.snapshot().read, 4850u);
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], (int16_t)10000);         // A at full gain to its last frame
    h.drainTo(5000);
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], (int16_t)-10000);        // then B from its first frame
}

TEST(fade_reanchor_skips_frames_already_mixed_as_incoming) {
    Harness h(1000);                                   // X = 1000
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    h.p.onMetadata(track("A", 60000));                 // duration far from the announcement: announced-frame fallback
    for (int i = 0; i < 5; ++i) h.enqueue(10000, 1000);
    h.drainTo(3000);                                   // the eSDK is never further ahead than the lead target
    g_now = 100;
    h.p.onMetadata(track("B", 9000));                  // fade [4000, 5000)
    h.enqueue(10000, 700);                             // A's announce-early tail: written 5700
    h.drainTo(4300);                                   // the fade mixed A's own tail in as the incoming
    xfade::RingPipeline::Snapshot before = h.p.snapshot();
    EXPECT_EQ(before.fadeKB, 300u);
    EXPECT_EQ(before.fadeB, 5300u);
    h.enqueue(0, 1000);                                // run: window opens at 5700
    h.enqueue(-10000, 3000);                           // B arrives: re-anchor
    xfade::RingPipeline::Snapshot s = h.p.snapshot();
    EXPECT_TRUE(s.fadeScheduled);
    EXPECT_EQ(s.fadeAt, 5700u);
    EXPECT_EQ(s.read, 5300u);                          // skipped past what was already emitted, not rewound
    EXPECT_EQ(s.fadeKB, 0u);
    h.be.completeOne();
    EXPECT_EQ(h.p.snapshot().read, 5310u);             // moves on from 5300; 5000..5300 is never played twice
    int n = 0;
    while (h.p.snapshot().fadeScheduled && n < 300) { h.be.completeOne(); ++n; }
    EXPECT_EQ(h.p.snapshot().read, 6700u);             // 5700 + the 1000 incoming frames of the fade
    EXPECT_EQ(h.p.snapshot().fades, 1u);
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], (int16_t)-10000);
}

TEST(fade_outgoing_finishes_after_the_incoming_ramp) {
    Harness h(1000);                                   // X = 1000
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    h.p.onMetadata(track("A", 60000));
    for (int i = 0; i < 5; ++i) h.enqueue(10000, 1000);
    h.drainTo(3800);                                   // the eSDK is never further ahead than the lead target
    g_now = 100;
    h.p.onMetadata(track("B", 9000));                  // fade [4000, 5000)
    h.enqueue(10000, 1700);                            // a long announce-early tail: written 6700
    h.drainTo(4300);
    h.enqueue(0, 1000);                                // run
    h.enqueue(-10000, 3000);                           // B: re-anchor with r = 6700 - 5300 = 1400 > fadeN
    xfade::RingPipeline::Snapshot s = h.p.snapshot();
    EXPECT_EQ(s.fadeAt, 6700u);
    EXPECT_EQ(s.read, 5300u);
    EXPECT_TRUE(s.fadeNA > s.fadeN);                   // the outgoing ramp is longer than the incoming one
    int n = 0;
    while (h.p.snapshot().fadeKB < 1000u && n < 300) { h.be.completeOne(); ++n; }
    s = h.p.snapshot();
    EXPECT_TRUE(s.fadeScheduled);                      // the incoming ramp is done but the outgoing is not
    EXPECT_TRUE(s.read < 6700u);
    uint32_t k0 = s.fadeNA - (uint32_t)(s.fadeAt - s.read);   // the outgoing's position on its own ramp
    h.be.completeOne();
    int expected = mixed(10000, 0, k0, s.fadeNA) + mixed(0, -10000, 999, 1000);
    EXPECT_TRUE(std::abs(h.lastOut()[0] - expected) <= 4);    // A still fading, B held at its end gain
    EXPECT_TRUE(h.lastOut()[0] > mixed(0, -10000, 999, 1000));   // the outgoing was not cut off
    EXPECT_TRUE(h.p.snapshot().fadeKB > 1000u);        // the incoming keeps being consumed at full gain
    n = 0;
    while (h.p.snapshot().fadeScheduled && n < 300) { h.be.completeOne(); ++n; }
    EXPECT_EQ(h.p.snapshot().read, 8100u);             // 6700 + the 1400 incoming frames consumed
    EXPECT_EQ(h.p.snapshot().fades, 1u);
    h.be.completeOne();
    EXPECT_EQ(h.lastOut()[0], (int16_t)-10000);
}

TEST(fade_reanchor_candidate_outside_the_window_is_ignored) {
    Harness h(2000);                                   // X = 2000, T = 3000
    h.p.setPlayState(SL_PLAYSTATE_PLAYING);
    h.p.onMetadata(track("A", 5000));
    for (int i = 0; i < 4; ++i) h.enqueue(10000, 1000);
    h.drainTo(2900);                                   // the eSDK is never further ahead than the lead target
    g_now = 100;
    h.p.onMetadata(track("B", 9000));                  // predicted 5000: fade [3000, 5000)
    EXPECT_EQ(h.p.snapshot().fadeAt, 5000u);
    h.enqueue(10000, 3800);                            // A really runs 2.8 s past the prediction
    h.enqueue(0, 100);                                 // padding
    h.enqueue(-10000, 1000);                           // c = 7800 is more than 2.5 s past the boundary
    EXPECT_EQ(h.p.snapshot().fadeAt, 5000u);           // left alone: this is not the boundary we scheduled
    EXPECT_TRUE(h.p.snapshot().armReanchored);         // but the one chance to re-anchor is spent
}
