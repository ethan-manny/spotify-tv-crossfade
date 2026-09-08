// Runs when libxfade.so is loaded (as the eSDK's dependency or via System.loadLibrary).
#include <new>
#include "fake_sl.h"
#include "log.h"
#include "passthrough_pipeline.h"
#include "real_backend.h"
#include "ring_pipeline.h"

namespace {
xfade::Backend* makeRealBackend() { return new (std::nothrow) xfade::RealBackend(); }
// 16-bit PCM goes through the ring (crossfade, or a 0.5 s minimum lead when crossfade is off); anything
// else is forwarded untouched (spec 5.1, 5.10).
xfade::Pipeline* makePipeline(const xfade::PcmFormat& fmt) {
    if (fmt.bits == 16) return new (std::nothrow) xfade::RingPipeline();
    LOGW("pipeline: %u-bit PCM is not processed; passthrough", fmt.bits);
    return new (std::nothrow) xfade::PassthroughPipeline();
}
}  // namespace

__attribute__((constructor)) static void xfadeInit() {
    xfade::setFactories(makeRealBackend, makePipeline);
    LOGI("libxfade loaded (phase 2 crossfade)");
}
