#pragma once
#include <mutex>
#include "pipeline.h"

namespace xfade {

// Forwards every buffer straight to the Backend and relays completion to the eSDK. No audio processing.
// The eSDK's buffer pointer is forwarded without copying (OpenSL's own contract: the memory stays valid
// until the completion callback fires); RingPipeline is the one that copies on enqueue.
class PassthroughPipeline : public Pipeline {
public:
    bool start(const PcmFormat& fmt, Backend* backend) override;
    uint32_t enqueue(const void* data, uint32_t bytes) override;
    uint32_t clear() override;
    void getState(uint32_t* count, uint32_t* index) override;
    void setPlayState(uint32_t slPlayState) override;
    uint32_t playState() override;
    void setBufferDoneCallback(void (*fn)(void*), void* ctx) override;
    void stop() override;

private:
    static void onBackendDone(void* self);
    std::mutex mu_;
    Backend* backend_ = nullptr;
    uint32_t queued_ = 0;
    uint32_t completed_ = 0;
    void (*doneFn_)(void*) = nullptr;
    void* doneCtx_ = nullptr;
};

}  // namespace xfade
