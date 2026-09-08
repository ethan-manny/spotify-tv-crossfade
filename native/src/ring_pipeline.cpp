#include "ring_pipeline.h"
#include <SLES/OpenSLES.h>
#include <time.h>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include "log.h"
#include "mixer.h"

namespace xfade {
namespace {
int64_t monotonicMs() {
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<int64_t>(ts.tv_sec) * 1000 + ts.tv_nsec / 1000000;
}
RingPipeline* g_current = nullptr;
std::mutex g_registryMu;
std::string g_dumpDir;   // guarded by g_registryMu
}  // namespace

std::mutex& RingPipeline::registryMutex() { return g_registryMu; }
RingPipeline* RingPipeline::current() { return g_current; }

// Only stores the directory: reconfiguring dump_ on a live pipeline from here would race push() on the
// audio thread (push() runs outside mu_ by design, so taking mu_ here would exclude nothing that
// matters) and, before start() has set fmt_, would configure a zero-rate dumper. start() reads this
// value exactly once, before any concurrent access to the new pipeline's dump_ begins.
void RingPipeline::setDumpDir(const std::string& dir) {
    std::lock_guard<std::mutex> lock(g_registryMu);
    g_dumpDir = dir;
}

namespace {
// Reads the dump directory under the registry lock; called from start(), which does not hold mu_.
std::string dumpDir() {
    std::lock_guard<std::mutex> lock(g_registryMu);
    return g_dumpDir;
}
}  // namespace

RingPipeline::RingPipeline() : nowFn_(monotonicMs) {
    std::lock_guard<std::mutex> lock(g_registryMu);
    g_current = this;                       // the eSDK creates one player at a time; the newest wins
}
RingPipeline::~RingPipeline() {
    stop();
    std::lock_guard<std::mutex> lock(g_registryMu);
    if (g_current == this) g_current = nullptr;
}

void RingPipeline::recomputeTargets() {
    X_ = static_cast<uint32_t>(static_cast<uint64_t>(crossfadeMs_) * fmt_.rateHz / 1000);
    // Even with crossfade off we hold kMinLeadMs: the eSDK's refill latency exceeds our 40 ms output queue,
    // so a zero lead made the ring run dry between buffers.
    uint32_t minLead = static_cast<uint32_t>(static_cast<uint64_t>(fmt_.rateHz) * kMinLeadMs / 1000);
    T_ = X_ > 0 ? std::max(X_ + fmt_.rateHz, minLead) : minLead;
    model_.setCrossfadeFrames(X_);
}

bool RingPipeline::start(const PcmFormat& fmt, Backend* backend) {
    if (fmt.bits != 16 || fmt.channels == 0 || fmt.rateHz == 0 || !backend) {
        LOGE("ring: unsupported format rate=%u ch=%u bits=%u", fmt.rateHz, fmt.channels, fmt.bits);
        return false;
    }
    if (!ring_.init(fmt.rateHz * kRingSeconds, fmt.channels)) {
        LOGE("ring: cannot allocate %u s at %u Hz x %u ch", kRingSeconds, fmt.rateHz, fmt.channels);
        return false;
    }
    dump_.configure(dumpDir(), fmt.rateHz, fmt.channels);
    fmt_ = fmt;
    outFrames_ = std::max<uint32_t>(1, fmt.rateHz * kOutMs / 1000);
    for (auto& b : out_) b.assign(static_cast<size_t>(outFrames_) * fmt.channels, 0);
    mixA_.assign(out_[0].size(), 0);
    mixB_.assign(out_[0].size(), 0);
    crossfadeMs_ = xfade::crossfadeMs();
    model_.configure(fmt.rateHz, 0);
    recomputeTargets();
    // The eSDK's numBuffers describes the queue it wants from us, not the one we need from the real
    // player: we always drive the output with our own kOutBuffers buffers.
    PcmFormat outFmt = fmt;
    outFmt.numBuffers = kOutBuffers;
    if (!backend->open(outFmt, DoneCallback{&RingPipeline::onBackendDone, this})) {
        LOGE("ring: backend open failed");
        return false;
    }
    // backend_ != nullptr is what every other thread reads as "this pipeline is started" (enqueue, the
    // JNI paths, stop), so it is published last, under mu_, once everything it implies is in place. The
    // priming loop uses the local pointer; on a failure path the local is closed and backend_ stays null.
    outHead_ = 0;
    for (uint32_t i = 0; i < kOutBuffers; ++i) {   // silence primes the queue
        uint32_t r = backend->enqueue(out_[i].data(), outBytes());
        if (r != SL_RESULT_SUCCESS) {   // a player that cannot hold our queue is no player at all
            LOGE("ring: priming buffer %u failed: %u", i, r);
            backend->close();
            return false;
        }
    }
    arm("player start");                    // the head of the first track is padding too
    lastPlaybackId_.clear();
    stopping_ = false;
    try {
        alive_ = std::make_shared<std::atomic<bool>>(true);
        // The ack loop only touches mu_-guarded state, never backend_, so it may run before publication.
        ackThread_ = std::thread(&RingPipeline::ackLoop, this, alive_);
    } catch (...) {
        LOGE("ring: cannot start ack thread");
        backend->close();
        return false;
    }
    {
        std::lock_guard<std::mutex> lock(mu_);
        backend_ = backend;
    }
    setStatsSource(this);
    LOGI("ring: started rate=%u ch=%u out=%u frames x %u, X=%u T=%u min_lead=%u (%s)", fmt.rateHz, fmt.channels,
         outFrames_, kOutBuffers, X_, T_, (unsigned)(static_cast<uint64_t>(fmt.rateHz) * kMinLeadMs / 1000), stateName());
    return true;
}

// A boundary (player start, flush, a new playback id) starts a new track: from here until the incoming
// track's first real audio, every all-zero buffer the eSDK sends is padding it produced because its
// decoder ran dry against our demand, and none of it belongs in the ring.
void RingPipeline::arm(const char* why) {
    armed_ = true;
    armFrame_ = written_;
    armDropped_ = 0;
    armSawZero_ = false;
    armReanchored_ = false;
    lastProbeMs_ = now();
    LOGD("ring: armed (%s)", why);
}

// Called with the incoming track's first real audio about to be written at written_. The model's
// boundary was predicted from the eSDK's duration and lands somewhere in the padding; the audio itself is
// the truth, so move the fade onto it. The incoming always fades in over the configured length (fadeN_)
// from its own first frame; the outgoing keeps the gain it has already reached (R-T8f) and its ramp is
// re-fitted to reach zero exactly at the new boundary.
void RingPipeline::reanchorFade() {
    const uint64_t was = fadeAt_;
    // Read the outgoing's position on its current ramp before anything moves: the re-fit below has to
    // reproduce this gain, and read_, fadeStart_ and fadeNA_ are all about to change.
    const bool started = read_ >= fadeStart_;
    const uint32_t k0 = started ? static_cast<uint32_t>(read_ - fadeStart_) : 0;
    const uint32_t nA0 = fadeNA_ ? fadeNA_ : 1;
    if (fadeKB_ > 0 && fadeB_ > read_) {
        // Frames from fadeAt_ onwards were mixed in as the incoming, but they were really the outgoing
        // track's own announce-early tail. Moving read_ to fadeB_ jumps over two different things:
        // [fadeAt_, fadeB_) was played already, quietly, on the incoming ramp; replaying it would repeat
        // audio the listener has just heard; [read_, fadeAt_) was never played at all and is dropped by
        // design, because the real incoming audio starts at written_ and playing that stretch first would
        // only delay the transition by exactly the announce-early lead (R-T8g).
        LOGI("ring: re-anchor skipped %llu frames already mixed as incoming",
             (unsigned long long)(fadeB_ - read_));
        read_ = fadeB_;
    }
    fadeAt_ = written_;
    fadeB_ = fadeAt_;
    fadeKB_ = 0;
    fadeStallLogged_ = false;
    fadeDumped_ = false;              // a re-anchored fade starts somewhere else: dump it once more
    const uint64_t r = fadeAt_ > read_ ? fadeAt_ - read_ : 0;   // outgoing frames left before the incoming starts
    if (!started) {
        const uint64_t minTail = static_cast<uint64_t>(fmt_.rateHz) * BoundaryModel::kMinTailMs / 1000;
        if (r < minTail) {
            // Nothing had been faded yet, so ending here steps no gain: the outgoing remainder plays out
            // and the incoming follows it. read_ stays put, so nothing is skipped and nothing is replayed.
            fadeScheduled_ = false;
            LOGI("ring: fade cancelled, tail %llu ms", (unsigned long long)(fmt_.rateHz ? r * 1000 / fmt_.rateHz : 0));
            return;
        }
        fadeNA_ = static_cast<uint32_t>(std::min<uint64_t>(X_, r));
        fadeStart_ = fadeAt_ - fadeNA_;
    } else if (r == 0) {
        fadeStart_ = fadeAt_;                  // the outgoing is spent; only the incoming is left to fade in
        fadeNA_ = 1;
    } else {
        // Keep theta where it is and make the ramp end at fadeAt_: theta/(pi/2) = (k0 + 0.5) / nA must be
        // unchanged while the r frames left are the rest of the new ramp, so nA' = r / (1 - (k0 + 0.5)/nA)
        // and the new gain index is nA' - r. cos(theta) is then continuous to within one sample step.
        const double pos = (static_cast<double>(k0) + 0.5) / static_cast<double>(nA0);
        const double frac = 1.0 - (pos < 1.0 ? pos : 1.0);
        // The cap keeps fadeStart_ >= 0 and fadeNA_ inside uint32; it only bites when the outgoing is
        // already at ~zero gain, where holding that gain for the last frames is what we want anyway.
        const double cap = std::min<double>(static_cast<double>(fadeAt_), 1.0e9);
        double nA = frac > 1e-9 ? static_cast<double>(r) / frac : cap;
        if (!(nA >= 1.0) || nA > cap) nA = cap;
        uint64_t nAi = static_cast<uint64_t>(std::llround(nA));
        if (nAi < r) nAi = r;                  // the ramp is never shorter than what is left of it
        fadeNA_ = static_cast<uint32_t>(nAi);
        fadeStart_ = fadeAt_ - nAi;            // == read_ - (nA' - r): fadeStart_ + fadeNA_ == fadeAt_
    }
    LOGI("ring: fade re-anchored to %llu (was %llu) outgoing n=%u incoming n=%u k0=%u", (unsigned long long)fadeAt_,
         (unsigned long long)was, fadeNA_, fadeN_, k0);
}

bool RingPipeline::allZero(const int16_t* samples, size_t count) {
    for (size_t i = 0; i < count; ++i) {
        if (samples[i] != 0) return false;
    }
    return true;
}

uint32_t RingPipeline::enqueue(const void* data, uint32_t bytes) {
    if (!backend_ || !data) return SL_RESULT_RESOURCE_ERROR;
    uint32_t frameBytes = fmt_.channels * 2;
    uint32_t frames = bytes / frameBytes;
    if (frames == 0 || frames > ring_.capacity()) return SL_RESULT_PARAMETER_INVALID;
    std::lock_guard<std::mutex> lock(mu_);
    const int16_t* samples = static_cast<const int16_t*>(data);
    // The scan runs on the eSDK's thread for every buffer, so only pay for it while armed. One pass, no
    // allocation.
    if (armed_ && allZero(samples, static_cast<size_t>(frames) * fmt_.channels)) {
        // Padding: never written. The eSDK produces it whenever its decoder is behind our demand, so it
        // comes interleaved with the little real audio it has, not only as a leading run.
        armDropped_ += frames;
        armSawZero_ = true;
        paddingDroppedTotal_ += frames;
        // Probe schedule: one dropped buffer released per kProbeMs. That keeps the eSDK's sink loop alive
        // while its decoder starves, at a pace where its clock advances by a few percent of real time and
        // it is never asked for a burst it can only answer with more padding.
        lastProbeMs_ = std::max(lastProbeMs_, now()) + kProbeMs;
        pending_.push_back(Pending{written_, true, lastProbeMs_});
        if (armDropped_ >= static_cast<uint32_t>(static_cast<uint64_t>(fmt_.rateHz) * kMaxZeroDropMs / 1000)) {
            armed_ = false;
            LOGW("ring: zero run cap reached, keeping zeros");
        }
        cv_.notify_all();
        return SL_RESULT_SUCCESS;
    }
    if (armed_ && armSawZero_ && !armReanchored_) {
        // The first real audio after padding: this frame is where the incoming track actually starts. One
        // chance per arm, taken whether or not the fade turns out to be re-anchorable.
        armReanchored_ = true;
        LOGI("ring: dropped %u ms of zeros since the boundary", (unsigned)(armDropped_ * 1000ull / fmt_.rateHz));
        const uint64_t c = written_;
        const uint64_t ahead = static_cast<uint64_t>(fmt_.rateHz) * kReanchorAheadMs / 1000;
        const uint64_t behind = static_cast<uint64_t>(fmt_.rateHz) * kReanchorBehindMs / 1000;
        // Only the fade this boundary scheduled, and only when the audio really did land near it: further
        // away than that and this is some other silence, not the transition we were aiming at.
        if (fadeScheduled_ && fadeAt_ >= armFrame_ && c <= fadeAt_ + ahead && c + behind >= fadeAt_) {
            reanchorFade();
        }
    }
    // read_ never runs past written_ any more (a fade ends at the last incoming frame it consumed), but a
    // future path that moves it could; never subtract the two, compare sums instead.
    if (written_ + frames > read_ + ring_.capacity()) {
        // Only reachable if acknowledgements are ignored; drop the oldest unplayed audio rather than corrupt it.
        uint64_t newRead = written_ + frames - ring_.capacity();
        if (overflows_++ == 0) LOGW("ring: overflow, dropping %llu unplayed frames", (unsigned long long)(newRead - read_));
        read_ = newRead;
    }
    ring_.write(written_, samples, frames);
    written_ += frames;
    pending_.push_back(Pending{written_, false, 0});
    if (armed_) {
        // The arm ends when the eSDK has caught up, not after a fixed time: while it is starving it can
        // only answer our demand with padding, and the demand is the lead target (T plus the fade's own
        // +n bump), which is exactly what the ack gate allows it to deliver. A fixed 15 s expired mid
        // starvation at X = 12 s, where T + n is 25 s, and its padding was written from then on.
        // Count only what has arrived since the boundary: at a flush the ring still holds the whole
        // pre-flush tail, so written_ - read_ is already the target and the arm would end on the first
        // buffer of the new track, while the eSDK is still interleaving padding.
        const uint64_t base = std::max(read_, armFrame_);
        const uint64_t lead = written_ > base ? written_ - base : 0;
        const uint64_t target = static_cast<uint64_t>(T_) + (fadeScheduled_ ? fadeN_ : 0);
        const char* why = nullptr;
        if (lead >= target) why = "lead met";
        else if (written_ - armFrame_ >= static_cast<uint64_t>(fmt_.rateHz) * kArmCapSeconds) why = "cap";
        if (why) {
            armed_ = false;
            LOGI("ring: dropped %u ms of zeros since the boundary (%s)",
                 (unsigned)(armDropped_ * 1000ull / fmt_.rateHz), why);
        }
    }
    cv_.notify_all();
    return SL_RESULT_SUCCESS;
}

uint32_t RingPipeline::clear() {
    std::lock_guard<std::mutex> lock(mu_);
    pending_.clear();                       // cleared buffers get no callback (OpenSL semantics); audio stays
    model_.onFlush(written_, now());
    arm("flush");
    cv_.notify_all();
    return SL_RESULT_SUCCESS;
}

void RingPipeline::getState(uint32_t* count, uint32_t* index) {
    std::lock_guard<std::mutex> lock(mu_);
    if (count) *count = static_cast<uint32_t>(pending_.size());
    if (index) *index = acked_;
}

void RingPipeline::setPlayState(uint32_t s) {
    {
        std::lock_guard<std::mutex> lock(mu_);
        playing_ = s == SL_PLAYSTATE_PLAYING;
    }
    if (backend_) backend_->setPlayState(s);
}

uint32_t RingPipeline::playState() { return backend_ ? backend_->playState() : SL_PLAYSTATE_STOPPED; }

void RingPipeline::setBufferDoneCallback(void (*fn)(void*), void* ctx) {
    std::lock_guard<std::mutex> lock(mu_);
    doneFn_ = fn;
    doneCtx_ = ctx;
}

void RingPipeline::stop() {
    {
        std::lock_guard<std::mutex> lock(mu_);
        if (!backend_) return;
        stopping_ = true;
        cv_.notify_all();
    }
    if (ackThread_.joinable()) {
        if (ackThread_.get_id() == std::this_thread::get_id()) {
            // The eSDK destroyed the player from inside its own buffer callback, i.e. on this thread.
            // We cannot join ourselves: clear the token so the loop exits without touching *this
            // once the callback returns, and let the thread finish on its own.
            LOGW("ring: stop() called from the ack thread (Destroy inside the callback)");
            alive_->store(false);
            ackThread_.detach();
        } else {
            ackThread_.join();                                            // no eSDK callback can run after this
        }
    }
    if (alive_) alive_->store(false);
    clearStatsSource(this);            // only if we are still the registered source: a newer pipeline's stats survive
    backend_->close();                                                    // no completion callback can run after this
    std::lock_guard<std::mutex> lock(mu_);
    backend_ = nullptr;
    LOGI("ring: stopped written=%llu read=%llu fades=%u underruns=%u", (unsigned long long)written_, (unsigned long long)read_, fades_, underruns_);
}

void RingPipeline::setCrossfadeMs(int ms) {
    if (ms < 0) ms = 0;
    if (ms > kMaxCrossfadeMs) ms = kMaxCrossfadeMs;
    bool started;
    {
        std::lock_guard<std::mutex> lock(mu_);
        crossfadeMs_ = ms;
        started = backend_ != nullptr;
        if (started) recomputeTargets();
        cv_.notify_all();
    }
    if (started) {
        setPlayerInfo(stateName(), fmt_.rateHz, fmt_.channels);
        LOGI("ring: crossfade_ms=%d X=%u T=%u", ms, X_, T_);
    }
}

const char* RingPipeline::stateName() { return crossfadeMs_ > 0 ? "active" : "passthrough"; }

void RingPipeline::onMetadata(const TrackMeta& m) {
    std::lock_guard<std::mutex> lock(mu_);
    if (!backend_) return;
    // The eSDK position counts the padding it believes it played; our frame counter does not. Without this
    // correction every natural-transition prediction is biased by the padding dropped since the boundary.
    TrackMeta corrected = m;
    int64_t droppedMs = fmt_.rateHz ? static_cast<int64_t>(armDropped_) * 1000 / fmt_.rateHz : 0;
    if (droppedMs > corrected.positionMs) droppedMs = corrected.positionMs;
    if (droppedMs > 0) {
        corrected.positionMs -= droppedMs;
        LOGD("ring: position %lld -> %lld (%lld ms of dropped padding)", (long long)m.positionMs,
             (long long)corrected.positionMs, (long long)droppedMs);
    }
    Decision d = model_.onMetadata(corrected, written_, read_, now());
    if (d.kind != Decision::None) apply(d);
    // Even a None decision can be a track start (the first track, or a late announcement after a cut).
    if (m.playbackId != lastPlaybackId_) {
        lastPlaybackId_ = m.playbackId;
        arm("new track");
    }
}

std::string RingPipeline::stats() {
    std::lock_guard<std::mutex> lock(mu_);
    uint64_t lead = written_ > read_ ? written_ - read_ : 0;
    uint64_t leadMs = fmt_.rateHz ? lead * 1000 / fmt_.rateHz : 0;
    char buf[160];
    snprintf(buf, sizeof buf, "lead_ms=%llu crossfade_ms=%d fades=%u last_delta_ms=%+d underruns=%u",
             (unsigned long long)leadMs, crossfadeMs_, fades_, model_.lastDeltaMs(), underruns_);
    return buf;
}

RingPipeline::Snapshot RingPipeline::snapshot() {
    std::lock_guard<std::mutex> lock(mu_);
    Snapshot s;
    s.written = written_; s.read = read_; s.pending = static_cast<uint32_t>(pending_.size()); s.acked = acked_;
    s.underruns = underruns_; s.fades = fades_; s.fadeScheduled = fadeScheduled_; s.fadeAt = fadeAt_; s.fadeN = fadeN_;
    s.fadeB = fadeB_; s.fadeKB = fadeKB_; s.fadeNA = fadeNA_;
    s.armDropped = armDropped_; s.armed = armed_; s.armSawZero = armSawZero_; s.armReanchored = armReanchored_;
    return s;
}

uint64_t RingPipeline::frontier() const { return fadeScheduled_ ? read_ + fadeN_ : read_; }
bool RingPipeline::ackReady() const {
    if (pending_.empty()) return false;
    const Pending& p = pending_.front();
    // Dropped padding holds its place in the FIFO (acks are in order) and is released at playback pace;
    // the ack loop's 50 ms wait bounds the granularity.
    // Only while playing: otherwise the padding is acknowledged, the eSDK clock advances and the budget
    // drains before the user has pressed play. Every play press starts with a Clear, which drops the queue.
    if (p.dropped) return playing_ && now() >= p.notBeforeMs;
    return p.end <= frontier() + T_;
}

void RingPipeline::ackLoop(std::shared_ptr<std::atomic<bool>> alive) {
    std::unique_lock<std::mutex> lock(mu_);
    while (!stopping_) {
        Decision d = model_.tick(written_, read_, now());
        if (d.kind != Decision::None) apply(d);
        if (ackReady()) {
            pending_.pop_front();
            ++acked_;
            void (*fn)(void*) = doneFn_;
            void* ctx = doneCtx_;
            lock.unlock();
            if (fn) fn(ctx);            // the eSDK usually re-enters enqueue() from here
            // 'lock' does not own mu_ here, so returning touches nothing that stop() may have destroyed.
            if (!alive->load()) return; // stop() ran on this thread inside the callback: the pipeline may already be deleted
            lock.lock();
            continue;
        }
        // 50 ms bounds the flush timeout, but a probe deadline that falls sooner must not wait for it:
        // with the 5.8 ms buffers this device uses, a fixed 50 ms poll throttles the eSDK to a third of
        // real time. Clamped to 1 ms so a passed-but-unplayable deadline cannot spin.
        int64_t waitMs = 50;
        if (!pending_.empty() && pending_.front().dropped) {
            int64_t due = pending_.front().notBeforeMs - now();
            if (due > 0) waitMs = std::min<int64_t>(waitMs, due);
        }
        cv_.wait_for(lock, std::chrono::milliseconds(std::max<int64_t>(waitMs, 1)));
    }
}

void RingPipeline::apply(const Decision& d) {
    // Incoming frames already mixed into the output must never be played again: during a fade read_ parks
    // at fadeAt_ while fadeB_ runs ahead, so a fade re-scheduled or cut here would replay fadeAt_..fadeB_.
    // With fadeKB_ == 0 nothing of the incoming has been consumed (fadeB_ is still fadeAt_), and skipping
    // read_ forward to it would throw away the outgoing tail the boundary about to be applied still owns.
    if (fadeScheduled_ && fadeKB_ > 0 && read_ < fadeB_) read_ = fadeB_;
    if (d.kind == Decision::Cut) {
        fadeScheduled_ = false;
        uint64_t at = std::min(d.at, written_);
        if (at > read_) {
            LOGI("ring: cut at %llu, skipping %llu frames", (unsigned long long)at, (unsigned long long)(at - read_));
            read_ = at;
        }
        dump_.trigger();
    } else if (d.kind == Decision::Fade) {
        fadeScheduled_ = true;
        fadeAt_ = d.at;
        fadeN_ = d.n;
        fadeNA_ = d.n;                  // the two ramps only differ once a re-anchor re-fits the outgoing
        fadeStart_ = d.at - d.n;
        fadeB_ = d.at;                  // the incoming starts at b and is never skipped over, however late it is
        fadeKB_ = 0;
        fadeStallLogged_ = false;
        fadeDumped_ = false;
        LOGI("ring: fade scheduled at %llu n=%u (read=%llu written=%llu)", (unsigned long long)fadeAt_, fadeN_,
             (unsigned long long)read_, (unsigned long long)written_);
        // Dump window is anchored at the fade actually starting (spec 5.9), not at scheduling: fill()
        // triggers it when read_ first reaches fadeStart_, which can be up to T = X + 1 s later.
    }
    cv_.notify_all();
}

// Produces 'frames' output frames from read_ onwards (spec 5.3 execution and 5.4). Under mu_, audio thread.
void RingPipeline::fill(int16_t* dst, uint32_t frames) {
    const uint32_t ch = fmt_.channels;
    bool underrun = false;
    uint32_t done = 0;
    while (done < frames) {
        int16_t* out = dst + static_cast<size_t>(done) * ch;
        uint32_t want = frames - done;
        if (fadeScheduled_ && read_ > fadeAt_) fadeScheduled_ = false;   // an overflow moved read_ past the boundary
        if (fadeScheduled_ && read_ >= fadeStart_) {
            // Inside the fade. The two sides have independent gain positions: the outgoing follows read_,
            // the incoming follows fadeKB_, which only advances over incoming frames we actually consumed.
            // An incoming head that arrives late is therefore faded in from its own first frame, never
            // skipped (that skip was the "fade out, silence, fade in" the device showed).
            uint32_t k0 = static_cast<uint32_t>(read_ - fadeStart_);
            // The fade is actually starting now (spec 5.9), not when it was scheduled. k0 stays 0 for as
            // long as the outgoing tail has not been delivered, so trigger on the first such buffer only:
            // one dump file per fade, not one per stalled buffer.
            if (k0 == 0 && !fadeDumped_) { fadeDumped_ = true; dump_.trigger(); }
            if (read_ < fadeAt_) {
                // Outgoing phase: mix the outgoing tail with whatever of the incoming has been delivered.
                uint64_t haveA = written_ > read_ ? written_ - read_ : 0;
                if (haveA == 0) { underrun = true; break; }              // outgoing tail not delivered yet
                // fadeStart_ + fadeNA_ == fadeAt_ by construction, so 'left' only guards a future change;
                // it must never be zero or the loop would stop making progress.
                uint32_t left = fadeNA_ > k0 ? fadeNA_ - k0 : 1;
                uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(
                    std::min<uint64_t>(want, fadeAt_ - read_), std::min<uint64_t>(haveA, left)));
                ring_.read(read_, mixA_.data(), chunk);
                uint32_t haveB = written_ > fadeB_
                        ? static_cast<uint32_t>(std::min<uint64_t>(written_ - fadeB_, chunk)) : 0;
                std::memset(mixB_.data(), 0, static_cast<size_t>(chunk) * ch * 2);
                if (haveB) ring_.read(fadeB_, mixB_.data(), haveB);
                if (haveB < chunk) {
                    underrun = true;                                     // incoming head missing: silence for those frames
                    if (!fadeStallLogged_) {
                        LOGW("ring: incoming head not yet delivered: k=%u have=%u/%u written=%llu", k0, haveB, chunk,
                             (unsigned long long)written_);
                        fadeStallLogged_ = true;                         // one line per fade, on the audio thread
                    }
                }
                // The frames past haveB in mixB_ are zero, so their gain index does not matter.
                mixEqualPowerSplit(mixA_.data(), mixB_.data(), out, chunk, ch, k0, fadeNA_, fadeKB_, fadeN_);
                read_ += chunk;
                fadeB_ += haveB;
                fadeKB_ += haveB;
                done += chunk;
            } else if (fadeKB_ < fadeN_) {
                // Incoming-only phase: the outgoing tail is exhausted but the incoming has not finished
                // fading in. read_ holds at fadeAt_, so the ack frontier (read_ + fadeN_) keeps the eSDK
                // delivering; this is the only silence a late track can still produce.
                uint32_t chunk = std::min(want, fadeN_ - fadeKB_);
                uint32_t haveB = written_ > fadeB_
                        ? static_cast<uint32_t>(std::min<uint64_t>(written_ - fadeB_, chunk)) : 0;
                if (haveB == 0) { underrun = true; break; }              // wait for delivery
                ring_.read(fadeB_, mixB_.data(), haveB);
                mixEqualPowerSplit(nullptr, mixB_.data(), out, haveB, ch, 0, fadeKB_, fadeN_);
                fadeB_ += haveB;
                fadeKB_ += haveB;
                done += haveB;
            }
            if (read_ >= fadeAt_ && fadeKB_ >= fadeN_) {
                // Both ramps are done. read_ becomes fadeB_: b + n when nothing stalled, less when the
                // incoming arrived late, more when a re-fitted outgoing ramp outlasted the incoming one.
                read_ = fadeB_;
                fadeScheduled_ = false;
                ++fades_;
                LOGI("ring: fade done, read=%llu written=%llu incoming_consumed=%u/%u underruns=%u padding_dropped_ms=%llu",
                     (unsigned long long)read_, (unsigned long long)written_, fadeKB_, fadeN_, underruns_,
                     (unsigned long long)(fmt_.rateHz ? paddingDroppedTotal_ * 1000 / fmt_.rateHz : 0));
            }
            continue;
        }
        // Normal output: straight copy up to the fade start (if one is scheduled) and up to written_.
        uint64_t limit = fadeScheduled_ ? std::min(fadeStart_, written_) : written_;
        if (read_ >= limit) {
            if (fadeScheduled_ && read_ >= fadeStart_) break;   // unreachable: the fade branch above already handled this case
            underrun = true;                                        // ring empty: supply stall
            break;
        }
        uint32_t chunk = static_cast<uint32_t>(std::min<uint64_t>(limit - read_, want));
        ring_.read(read_, out, chunk);
        read_ += chunk;
        done += chunk;
    }
    if (done < frames) {
        std::memset(dst + static_cast<size_t>(done) * ch, 0, static_cast<size_t>(frames - done) * ch * 2);
    }
    if (underrun && playing_) ++underruns_;
    cv_.notify_all();
}

void RingPipeline::onBackendDone(void* self) {
    auto* p = static_cast<RingPipeline*>(self);
    int16_t* buf;
    {
        std::lock_guard<std::mutex> lock(p->mu_);
        if (!p->backend_) return;
        buf = p->out_[p->outHead_].data();
        p->outHead_ = (p->outHead_ + 1) % kOutBuffers;
        p->fill(buf, p->outFrames_);
    }
    p->dump_.push(buf, p->outFrames_);   // outside mu_: file writes never block the eSDK
    p->backend_->enqueue(buf, p->outBytes());
}

}  // namespace xfade
