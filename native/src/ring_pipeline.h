#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>
#include "boundary.h"
#include "dump.h"
#include "pipeline.h"
#include "ring.h"
#include "status.h"
#include "track_meta.h"

namespace xfade {

// Copies every eSDK buffer into a ring, holds a lead of T = max(X + 1 s, 0.5 s) (X = crossfade length) in
// front of the real player, acknowledges the eSDK from its own thread, and renders cuts and fades at track
// boundaries (spec 5.2-5.7). Threads: eSDK (enqueue/clear/getState/setPlayState), the ack thread, the
// back end's audio thread (onBackendDone -> fill), Java via JNI (setCrossfadeMs/onMetadata/stats).
// One mutex (mu_) and one condition variable (cv_); the eSDK is never called with mu_ held.
class RingPipeline : public Pipeline, public StatsSource {
public:
    static constexpr uint32_t kRingSeconds = 30;   // n + T reaches 25 s at X = 12 s; leave the eSDK room
    static constexpr uint32_t kOutBuffers = 4;
    static constexpr uint32_t kOutMs = 10;
    static constexpr int kMaxCrossfadeMs = 12000;
    static constexpr int kMinLeadMs = 500;      // lead held even with crossfade off: the eSDK's refill latency
    static constexpr uint32_t kArmCapSeconds = 45;  // an arm never outlives this much audio, lead or no lead
    static constexpr int kMaxZeroDropMs = 12000;  // give up dropping zeros after this much silence in one arm
    static constexpr int64_t kProbeMs = 100;      // one dropped buffer is acknowledged per this many ms
    static constexpr int64_t kReanchorAheadMs = 2500;    // the incoming audio may start this long after b
    static constexpr int64_t kReanchorBehindMs = 12000;  // ... or this long before it (a long trailing silence)

    RingPipeline();
    ~RingPipeline() override;

    // Pipeline
    bool start(const PcmFormat& fmt, Backend* backend) override;
    uint32_t enqueue(const void* data, uint32_t bytes) override;
    uint32_t clear() override;
    void getState(uint32_t* count, uint32_t* index) override;
    void setPlayState(uint32_t slPlayState) override;
    uint32_t playState() override;
    void setBufferDoneCallback(void (*fn)(void*), void* ctx) override;
    void stop() override;

    // Controls, any thread.
    void setCrossfadeMs(int ms);
    void onMetadata(const TrackMeta& m);
    const char* stateName() override;   // "active" (crossfade > 0) or "passthrough"

    // StatsSource
    std::string stats() override;

    // The live ring pipeline for JNI. Hold registryMutex() for the whole use of the pointer; the
    // destructor blocks on the same mutex, so a Java call never overlaps player destruction.
    static std::mutex& registryMutex();
    static RingPipeline* current();
    // Directory for the debug PCM dumper (spec 5.9), guarded by registryMutex(). Only stores the value:
    // the live pipeline's dumper is configured exactly once, in start(), from this stored directory, so a
    // change here takes effect for the next player created, never for one already playing.
    static void setDumpDir(const std::string& dir);

    // Test hooks.
    struct Snapshot {
        uint64_t written = 0, read = 0;
        uint32_t pending = 0, acked = 0, underruns = 0, fades = 0;
        bool fadeScheduled = false;
        uint64_t fadeAt = 0, fadeB = 0;
        uint32_t fadeN = 0, fadeNA = 0, fadeKB = 0;
        uint32_t armDropped = 0;
        bool armed = false, armSawZero = false, armReanchored = false;
    };
    Snapshot snapshot();
    void setClockForTests(int64_t (*nowMs)()) { nowFn_ = nowMs; }
    void applyForTests(const Decision& d) { std::lock_guard<std::mutex> lock(mu_); apply(d); }
    PcmDumper& dumperForTests() { return dump_; }

private:
    static void onBackendDone(void* self);
    void ackLoop(std::shared_ptr<std::atomic<bool>> alive);
    bool ackReady() const;            // under mu_
    uint64_t frontier() const;        // under mu_
    void fill(int16_t* dst, uint32_t frames);   // under mu_, audio thread
    void apply(const Decision& d);    // under mu_
    void recomputeTargets();          // under mu_
    void arm(const char* why);        // under mu_
    void reanchorFade();              // under mu_
    static bool allZero(const int16_t* samples, size_t count);
    int64_t now() const { return nowFn_(); }
    uint32_t outBytes() const { return outFrames_ * fmt_.channels * 2; }

    std::mutex mu_;
    std::condition_variable cv_;
    std::thread ackThread_;
    // Cleared by stop() and outlives *this: the ack thread reads it to learn that the pipeline was
    // destroyed underneath it (Destroy called from inside the eSDK's own buffer callback).
    std::shared_ptr<std::atomic<bool>> alive_;
    bool stopping_ = false;
    Backend* backend_ = nullptr;      // set in start(), cleared in stop(); read outside mu_ only on the eSDK thread
    PcmFormat fmt_;
    Ring ring_;
    uint64_t written_ = 0;
    uint64_t read_ = 0;
    // One entry per enqueued, not yet acknowledged eSDK buffer, in FIFO order. 'end' is its end frame in
    // the ring; a 'dropped' entry was start-of-track padding that was never written, and is acknowledged
    // on the probe schedule at notBeforeMs instead of against the lead target.
    struct Pending { uint64_t end; bool dropped; int64_t notBeforeMs; };
    std::deque<Pending> pending_;
    uint32_t acked_ = 0;
    void (*doneFn_)(void*) = nullptr;
    void* doneCtx_ = nullptr;
    bool playing_ = false;
    int crossfadeMs_ = 0;
    uint32_t X_ = 0;                  // crossfade in frames
    uint32_t T_ = 0;                  // lead target in frames
    BoundaryModel model_;
    bool fadeScheduled_ = false;
    uint64_t fadeAt_ = 0;             // b
    uint64_t fadeStart_ = 0;          // b - n
    uint32_t fadeN_ = 0;              // incoming ramp length (also the ack frontier's lead)
    uint32_t fadeNA_ = 0;             // outgoing ramp length; equal to fadeN_ until a re-anchor re-fits it
    uint64_t fadeB_ = 0;              // next incoming frame to consume; starts at b and only advances over delivered audio
    uint32_t fadeKB_ = 0;             // incoming frames consumed so far in this fade (its own gain index)
    bool fadeStallLogged_ = false;    // one warning per fade when the incoming head is not there in time
    bool fadeDumped_ = false;         // one PCM dump per fade, even if its first frame stalls repeatedly
    uint32_t fades_ = 0;
    uint32_t underruns_ = 0;
    uint32_t overflows_ = 0;
    // Boundary arm (spec 5.6, ruling R-T8h): between a boundary and the incoming track's real audio the
    // eSDK pads with zeros whenever its decoder runs dry, interleaved with what audio it has, so every
    // all-zero buffer of the arm is dropped rather than only a leading run.
    bool armed_ = false;              // start() arms it: the player start is itself a boundary
    uint64_t armFrame_ = 0;           // written_ at that boundary; the hard cap is measured from here
    uint32_t armDropped_ = 0;         // zero frames dropped in this arm (the cap budget and the position fix)
    bool armSawZero_ = false;         // something was dropped: the next audio is the incoming track's first
    bool armReanchored_ = false;      // the one re-anchor chance of this arm is spent
    int64_t lastProbeMs_ = 0;         // when the last dropped buffer was released to the eSDK
    std::string lastPlaybackId_;      // last id seen by onMetadata; a change is a track start
    uint64_t paddingDroppedTotal_ = 0;
    uint32_t outFrames_ = 0;
    std::vector<int16_t> out_[kOutBuffers];
    uint32_t outHead_ = 0;            // the oldest enqueued output buffer (they complete in order)
    std::vector<int16_t> mixA_, mixB_;
    int64_t (*nowFn_)();
    PcmDumper dump_;
};

}  // namespace xfade
