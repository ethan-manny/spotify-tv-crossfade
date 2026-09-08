#pragma once
#include <cstdint>
#include "backend.h"

namespace xfade {

// What sits between the eSDK's buffer queue calls and the Backend: RingPipeline for 16-bit PCM,
// PassthroughPipeline for anything else.
class Pipeline {
public:
    virtual ~Pipeline() = default;
    virtual bool start(const PcmFormat& fmt, Backend* backend) = 0;
    virtual uint32_t enqueue(const void* data, uint32_t bytes) = 0;   // returns SLresult
    virtual uint32_t clear() = 0;                                     // returns SLresult
    virtual void getState(uint32_t* count, uint32_t* index) = 0;
    virtual void setPlayState(uint32_t slPlayState) = 0;
    virtual uint32_t playState() = 0;
    virtual void setBufferDoneCallback(void (*fn)(void*), void* ctx) = 0;
    virtual void stop() = 0;
    virtual const char* stateName() { return "passthrough"; }   // what getStatus() reports for this pipeline
};

}  // namespace xfade
