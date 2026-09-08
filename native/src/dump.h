#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>

namespace xfade {

// Debug PCM dump (spec 5.9): keeps the last 5 s of output; on trigger() writes them plus the next 15 s
// to <dir>/xfade_dump_<n>.pcm. push() and the file writes run on the audio thread; trigger() may be
// called from any thread (an atomic flag). push() checks the flag before folding its own frames into
// history, so a trigger raised between two pushes freezes history at the previous push and the tail
// starts exactly with the next one. Enabled only when the system property debug.xfade.dump is 1 and a
// directory has been set; otherwise every method is a no-op. setup() also refuses to enable when rateHz
// or channels is 0 (a still-default format), logging a warning instead of sizing a zero-length history
// ring. A dumper is configured exactly once, in RingPipeline::start(): changing the directory via
// RingPipeline::setDumpDir() only takes effect for the next player that is created, never on a live one.
class PcmDumper {
public:
    static constexpr uint32_t kHistorySeconds = 5;
    static constexpr uint32_t kTailSeconds = 15;
    ~PcmDumper();
    void configure(const std::string& dir, uint32_t rateHz, uint32_t channels);   // reads the property
    void configureForTests(const std::string& dir, uint32_t rateHz, uint32_t channels, bool enabled);
    bool enabled() const { return enabled_; }
    void trigger() { if (enabled_) triggered_.store(true); }
    void push(const int16_t* frames, uint32_t n);   // audio thread
private:
    void setup(const std::string& dir, uint32_t rateHz, uint32_t channels, bool enabled);
    void startFile();
    bool enabled_ = false;
    std::string dir_;
    uint32_t channels_ = 0;
    uint32_t historyFrames_ = 0, tailFrames_ = 0;
    std::vector<int16_t> history_;   // ring of historyFrames_ frames
    uint64_t pushed_ = 0;            // frames pushed so far (history write position)
    std::atomic<bool> triggered_{false};
    FILE* file_ = nullptr;
    uint64_t remaining_ = 0;         // frames still to write to file_
    int count_ = 0;
};

}  // namespace xfade
