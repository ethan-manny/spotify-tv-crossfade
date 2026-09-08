#include "passthrough_pipeline.h"
#include <SLES/OpenSLES.h>
#include "log.h"

namespace xfade {

bool PassthroughPipeline::start(const PcmFormat& fmt, Backend* backend) {
    backend_ = backend;
    return backend_->open(fmt, DoneCallback{&PassthroughPipeline::onBackendDone, this});
}

uint32_t PassthroughPipeline::enqueue(const void* data, uint32_t bytes) {
    if (!backend_) return SL_RESULT_RESOURCE_ERROR;
    {
        std::lock_guard<std::mutex> lock(mu_);
        ++queued_;
    }
    uint32_t r = backend_->enqueue(data, bytes);
    if (r != SL_RESULT_SUCCESS) {
        std::lock_guard<std::mutex> lock(mu_);
        --queued_;
    }
    return r;
}

uint32_t PassthroughPipeline::clear() {
    if (backend_) backend_->clear();
    std::lock_guard<std::mutex> lock(mu_);
    queued_ = 0;
    return SL_RESULT_SUCCESS;
}

void PassthroughPipeline::getState(uint32_t* count, uint32_t* index) {
    std::lock_guard<std::mutex> lock(mu_);
    if (count) *count = queued_;
    if (index) *index = completed_;
}

void PassthroughPipeline::setPlayState(uint32_t s) { if (backend_) backend_->setPlayState(s); }
uint32_t PassthroughPipeline::playState() { return backend_ ? backend_->playState() : SL_PLAYSTATE_STOPPED; }

void PassthroughPipeline::setBufferDoneCallback(void (*fn)(void*), void* ctx) {
    std::lock_guard<std::mutex> lock(mu_);
    doneFn_ = fn;
    doneCtx_ = ctx;
}

void PassthroughPipeline::stop() {
    if (backend_) backend_->close();
    backend_ = nullptr;
}

void PassthroughPipeline::onBackendDone(void* self) {
    auto* p = static_cast<PassthroughPipeline*>(self);
    void (*fn)(void*) = nullptr;
    void* ctx = nullptr;
    {
        std::lock_guard<std::mutex> lock(p->mu_);
        if (p->queued_ > 0) { --p->queued_; ++p->completed_; }
        fn = p->doneFn_;
        ctx = p->doneCtx_;
    }
    if (fn) fn(ctx);   // never call the eSDK with our lock held
}

}  // namespace xfade
