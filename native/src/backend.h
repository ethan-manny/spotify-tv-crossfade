#pragma once
#include <cstdint>

namespace xfade {

struct PcmFormat {
    uint32_t rateHz = 0;
    uint32_t channels = 0;
    uint32_t bits = 0;
    uint32_t numBuffers = 0;
};

struct DoneCallback {
    void (*fn)(void*) = nullptr;
    void* ctx = nullptr;
};

// Output side. In production this is the real system OpenSL ES; tests use FakeBackend.
class Backend {
public:
    virtual ~Backend() = default;
    virtual bool open(const PcmFormat& fmt, DoneCallback done) = 0;
    virtual uint32_t enqueue(const void* data, uint32_t bytes) = 0;   // returns SLresult; data stays valid until 'done' fires
    virtual void clear() = 0;
    virtual bool setPlayState(uint32_t slPlayState) = 0;
    virtual uint32_t playState() = 0;
    virtual uint32_t setVolumeLevel(int16_t mB) = 0;
    virtual uint32_t getVolumeLevel(int16_t* mB) = 0;
    virtual uint32_t getMaxVolumeLevel(int16_t* mB) = 0;
    virtual uint32_t setMute(bool mute) = 0;
    virtual uint32_t getMute(bool* mute) = 0;
    virtual uint32_t enableStereoPosition(bool enable) = 0;
    virtual uint32_t isEnabledStereoPosition(bool* enabled) = 0;
    virtual uint32_t setStereoPosition(int16_t permille) = 0;
    virtual uint32_t getStereoPosition(int16_t* permille) = 0;
    // Must not return while a completion callback can still run; after close() returns, no further
    // DoneCallback invocation may occur. Real backends satisfy this by destroying the OpenSL player
    // synchronously.
    virtual void close() = 0;
};

}  // namespace xfade
