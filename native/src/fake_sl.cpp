// The OpenSL ES surface the eSDK sees. Exports slCreateEngine and four interface IDs; every other
// call arrives through the vtables below.
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <cstddef>
#include <cstring>
#include <new>
#include "fake_sl.h"
#include "log.h"
#include "passthrough_pipeline.h"
#include "status.h"

// ptr's type (SLObjectItf, SLPlayItf, ...) is "const struct X_ * const *": reinterpret_cast alone
// cannot drop that constness (that's what const_cast is for), so strip it explicitly before the
// pointer arithmetic that recovers the (writable) enclosing object.
#define CONTAINER_OF(ptr, type, member) \
    reinterpret_cast<type*>(const_cast<char*>(reinterpret_cast<const char*>(ptr)) - offsetof(type, member))

namespace {
using namespace xfade;

BackendFactory g_backendFactory = nullptr;
PipelineFactory g_pipelineFactory = nullptr;

// Interface IDs handed to the eSDK. Arbitrary but unique; compared by pointer or by value.
const SLInterfaceID_ kIidEngine      = {0x58465345, 0x0001, 0x1000, 0x8000, {0x58, 0x46, 0x41, 0x44, 0x45, 0x01}};
const SLInterfaceID_ kIidPlay        = {0x58465345, 0x0002, 0x1000, 0x8000, {0x58, 0x46, 0x41, 0x44, 0x45, 0x02}};
const SLInterfaceID_ kIidBufferQueue = {0x58465345, 0x0003, 0x1000, 0x8000, {0x58, 0x46, 0x41, 0x44, 0x45, 0x03}};
const SLInterfaceID_ kIidVolume      = {0x58465345, 0x0004, 0x1000, 0x8000, {0x58, 0x46, 0x41, 0x44, 0x45, 0x04}};

bool sameIid(SLInterfaceID a, const SLInterfaceID_* b) {
    return a == b || (a != nullptr && std::memcmp(a, b, sizeof(SLInterfaceID_)) == 0);
}

const char* iidName(SLInterfaceID id) {
    if (sameIid(id, &kIidEngine)) return "ENGINE";
    if (sameIid(id, &kIidPlay)) return "PLAY";
    if (sameIid(id, &kIidBufferQueue)) return "BUFFERQUEUE";
    if (sameIid(id, &kIidVolume)) return "VOLUME";
    return "unknown";
}

// ---- object layouts (standard-layout; first members are vtable pointers) ----
struct Engine {
    const SLObjectItf_* objVt;
    const SLEngineItf_* engVt;
    SLuint32 state;
};
struct OutputMix {
    const SLObjectItf_* objVt;
    SLuint32 state;
};
struct Player {
    const SLObjectItf_* objVt;
    const SLPlayItf_* playVt;
    const SLAndroidSimpleBufferQueueItf_* bqVt;
    const SLVolumeItf_* volVt;
    SLuint32 state;
    PcmFormat fmt;
    Backend* backend;
    Pipeline* pipeline;
    slAndroidSimpleBufferQueueCallback callback;
    void* callbackCtx;
    SLuint32 playState;
};

// ---- shared SLObjectItf stubs ----
SLresult objResume(SLObjectItf, SLboolean) { return SL_RESULT_SUCCESS; }
SLresult objRegisterCallback(SLObjectItf, slObjectCallback, void*) { return SL_RESULT_SUCCESS; }
void objAbortAsyncOperation(SLObjectItf) {}
SLresult objSetPriority(SLObjectItf, SLint32, SLboolean) { return SL_RESULT_SUCCESS; }
SLresult objGetPriority(SLObjectItf, SLint32* p, SLboolean* pre) { if (p) *p = 0; if (pre) *pre = SL_BOOLEAN_FALSE; return SL_RESULT_SUCCESS; }
SLresult objSetLossOfControl(SLObjectItf, SLint16, SLInterfaceID*, SLboolean) { return SL_RESULT_SUCCESS; }

// ---- Engine object ----
SLresult engRealize(SLObjectItf self, SLboolean) {
    CONTAINER_OF(self, Engine, objVt)->state = SL_OBJECT_STATE_REALIZED;
    LOGI("engine realized");
    return SL_RESULT_SUCCESS;
}
SLresult engGetState(SLObjectItf self, SLuint32* s) { if (s) *s = CONTAINER_OF(self, Engine, objVt)->state; return SL_RESULT_SUCCESS; }
SLresult engGetInterface(SLObjectItf self, SLInterfaceID iid, void* out) {
    Engine* e = CONTAINER_OF(self, Engine, objVt);
    if (sameIid(iid, &kIidEngine)) { *static_cast<SLEngineItf*>(out) = &e->engVt; return SL_RESULT_SUCCESS; }
    LOGW("engine GetInterface(%s) unsupported", iidName(iid));
    return SL_RESULT_FEATURE_UNSUPPORTED;
}
void engDestroy(SLObjectItf self) { LOGI("engine destroyed"); delete CONTAINER_OF(self, Engine, objVt); }

const SLObjectItf_ kEngineObjVt = {
    .Realize = engRealize, .Resume = objResume, .GetState = engGetState, .GetInterface = engGetInterface,
    .RegisterCallback = objRegisterCallback, .AbortAsyncOperation = objAbortAsyncOperation, .Destroy = engDestroy,
    .SetPriority = objSetPriority, .GetPriority = objGetPriority, .SetLossOfControlInterfaces = objSetLossOfControl,
};

// ---- OutputMix object ----
SLresult mixRealize(SLObjectItf self, SLboolean) { CONTAINER_OF(self, OutputMix, objVt)->state = SL_OBJECT_STATE_REALIZED; return SL_RESULT_SUCCESS; }
SLresult mixGetState(SLObjectItf self, SLuint32* s) { if (s) *s = CONTAINER_OF(self, OutputMix, objVt)->state; return SL_RESULT_SUCCESS; }
SLresult mixGetInterface(SLObjectItf, SLInterfaceID iid, void*) { LOGW("outputmix GetInterface(%s) unsupported", iidName(iid)); return SL_RESULT_FEATURE_UNSUPPORTED; }
void mixDestroy(SLObjectItf self) { delete CONTAINER_OF(self, OutputMix, objVt); }

const SLObjectItf_ kMixObjVt = {
    .Realize = mixRealize, .Resume = objResume, .GetState = mixGetState, .GetInterface = mixGetInterface,
    .RegisterCallback = objRegisterCallback, .AbortAsyncOperation = objAbortAsyncOperation, .Destroy = mixDestroy,
    .SetPriority = objSetPriority, .GetPriority = objGetPriority, .SetLossOfControlInterfaces = objSetLossOfControl,
};

// ---- Player: object interface ----
void playerBufferDone(void* ctx) {
    Player* p = static_cast<Player*>(ctx);
    if (p->callback) p->callback(&p->bqVt, p->callbackCtx);
}

SLresult playerRealize(SLObjectItf self, SLboolean) {
    Player* p = CONTAINER_OF(self, Player, objVt);
    if (p->pipeline) { LOGW("player Realize called twice"); return SL_RESULT_SUCCESS; }
    if (!g_backendFactory || !g_pipelineFactory) { LOGE("no factories set"); return SL_RESULT_RESOURCE_ERROR; }
    p->backend = g_backendFactory();
    p->pipeline = g_pipelineFactory(p->fmt);
    if (!p->backend || !p->pipeline) {
        LOGE("factory returned null");
        setPlayerInfo("no-player", p->fmt.rateHz, p->fmt.channels);
        delete p->pipeline; delete p->backend; p->pipeline = nullptr; p->backend = nullptr;
        return SL_RESULT_MEMORY_FAILURE;
    }
    p->pipeline->setBufferDoneCallback(playerBufferDone, p);
    if (!p->pipeline->start(p->fmt, p->backend)) {
        LOGE("pipeline start failed (rate=%u ch=%u bits=%u); falling back to passthrough", p->fmt.rateHz, p->fmt.channels, p->fmt.bits);
        delete p->pipeline;
        p->pipeline = new (std::nothrow) PassthroughPipeline();
        if (p->pipeline) p->pipeline->setBufferDoneCallback(playerBufferDone, p);
        if (!p->pipeline || !p->pipeline->start(p->fmt, p->backend)) {
            LOGE("passthrough start failed; no player");
            setPlayerInfo("no-player", p->fmt.rateHz, p->fmt.channels);
            delete p->pipeline; p->pipeline = nullptr;
            delete p->backend; p->backend = nullptr;
            return SL_RESULT_RESOURCE_ERROR;
        }
    }
    p->state = SL_OBJECT_STATE_REALIZED;
    setPlayerInfo(p->pipeline->stateName(), p->fmt.rateHz, p->fmt.channels);
    LOGI("player realized rate=%u ch=%u bits=%u buffers=%u (%s)", p->fmt.rateHz, p->fmt.channels, p->fmt.bits, p->fmt.numBuffers, p->pipeline->stateName());
    return SL_RESULT_SUCCESS;
}
SLresult playerGetState(SLObjectItf self, SLuint32* s) { if (s) *s = CONTAINER_OF(self, Player, objVt)->state; return SL_RESULT_SUCCESS; }
SLresult playerGetInterface(SLObjectItf self, SLInterfaceID iid, void* out) {
    Player* p = CONTAINER_OF(self, Player, objVt);
    if (sameIid(iid, &kIidPlay)) { *static_cast<SLPlayItf*>(out) = &p->playVt; return SL_RESULT_SUCCESS; }
    if (sameIid(iid, &kIidBufferQueue)) { *static_cast<SLAndroidSimpleBufferQueueItf*>(out) = &p->bqVt; return SL_RESULT_SUCCESS; }
    if (sameIid(iid, &kIidVolume)) { *static_cast<SLVolumeItf*>(out) = &p->volVt; return SL_RESULT_SUCCESS; }
    LOGW("player GetInterface(%s) unsupported", iidName(iid));
    return SL_RESULT_FEATURE_UNSUPPORTED;
}
void playerDestroy(SLObjectItf self) {
    Player* p = CONTAINER_OF(self, Player, objVt);
    LOGI("player destroyed");
    if (p->pipeline) { p->pipeline->stop(); delete p->pipeline; }
    delete p->backend;
    setPlayerInfo("no-player", 0, 0);
    delete p;
}

const SLObjectItf_ kPlayerObjVt = {
    .Realize = playerRealize, .Resume = objResume, .GetState = playerGetState, .GetInterface = playerGetInterface,
    .RegisterCallback = objRegisterCallback, .AbortAsyncOperation = objAbortAsyncOperation, .Destroy = playerDestroy,
    .SetPriority = objSetPriority, .GetPriority = objGetPriority, .SetLossOfControlInterfaces = objSetLossOfControl,
};

// ---- Player: SLPlayItf ----
SLresult playSetPlayState(SLPlayItf self, SLuint32 s) {
    Player* p = CONTAINER_OF(self, Player, playVt);
    LOGI("SetPlayState(%u)", s);
    p->playState = s;
    if (p->pipeline) p->pipeline->setPlayState(s);
    return SL_RESULT_SUCCESS;
}
SLresult playGetPlayState(SLPlayItf self, SLuint32* s) { if (s) *s = CONTAINER_OF(self, Player, playVt)->playState; return SL_RESULT_SUCCESS; }
SLresult playGetDuration(SLPlayItf, SLmillisecond* ms) { if (ms) *ms = SL_TIME_UNKNOWN; return SL_RESULT_SUCCESS; }
SLresult playGetPosition(SLPlayItf, SLmillisecond* ms) { if (ms) *ms = 0; return SL_RESULT_SUCCESS; }
SLresult playRegisterCallback(SLPlayItf, slPlayCallback, void*) { LOGW("SLPlayItf.RegisterCallback ignored"); return SL_RESULT_SUCCESS; }
SLresult playSetCallbackEventsMask(SLPlayItf, SLuint32) { return SL_RESULT_SUCCESS; }
SLresult playGetCallbackEventsMask(SLPlayItf, SLuint32* m) { if (m) *m = 0; return SL_RESULT_SUCCESS; }
SLresult playSetMarkerPosition(SLPlayItf, SLmillisecond) { return SL_RESULT_SUCCESS; }
SLresult playClearMarkerPosition(SLPlayItf) { return SL_RESULT_SUCCESS; }
SLresult playGetMarkerPosition(SLPlayItf, SLmillisecond* ms) { if (ms) *ms = 0; return SL_RESULT_SUCCESS; }
SLresult playSetPositionUpdatePeriod(SLPlayItf, SLmillisecond) { return SL_RESULT_SUCCESS; }
SLresult playGetPositionUpdatePeriod(SLPlayItf, SLmillisecond* ms) { if (ms) *ms = 0; return SL_RESULT_SUCCESS; }

const SLPlayItf_ kPlayVt = {
    .SetPlayState = playSetPlayState, .GetPlayState = playGetPlayState, .GetDuration = playGetDuration,
    .GetPosition = playGetPosition, .RegisterCallback = playRegisterCallback,
    .SetCallbackEventsMask = playSetCallbackEventsMask, .GetCallbackEventsMask = playGetCallbackEventsMask,
    .SetMarkerPosition = playSetMarkerPosition, .ClearMarkerPosition = playClearMarkerPosition,
    .GetMarkerPosition = playGetMarkerPosition, .SetPositionUpdatePeriod = playSetPositionUpdatePeriod,
    .GetPositionUpdatePeriod = playGetPositionUpdatePeriod,
};

// ---- Player: buffer queue (SL_IID_BUFFERQUEUE on Android shares this layout) ----
SLresult bqEnqueue(SLAndroidSimpleBufferQueueItf self, const void* data, SLuint32 size) {
    Player* p = CONTAINER_OF(self, Player, bqVt);
    return p->pipeline ? p->pipeline->enqueue(data, size) : SL_RESULT_RESOURCE_ERROR;
}
SLresult bqClear(SLAndroidSimpleBufferQueueItf self) {
    Player* p = CONTAINER_OF(self, Player, bqVt);
    LOGI("BufferQueue.Clear");
    return p->pipeline ? p->pipeline->clear() : SL_RESULT_SUCCESS;
}
SLresult bqGetState(SLAndroidSimpleBufferQueueItf self, SLAndroidSimpleBufferQueueState* st) {
    Player* p = CONTAINER_OF(self, Player, bqVt);
    if (!st) return SL_RESULT_PARAMETER_INVALID;
    st->count = 0; st->index = 0;
    if (p->pipeline) p->pipeline->getState(&st->count, &st->index);
    return SL_RESULT_SUCCESS;
}
SLresult bqRegisterCallback(SLAndroidSimpleBufferQueueItf self, slAndroidSimpleBufferQueueCallback cb, void* ctx) {
    Player* p = CONTAINER_OF(self, Player, bqVt);
    p->callback = cb; p->callbackCtx = ctx;
    LOGI("BufferQueue.RegisterCallback");
    return SL_RESULT_SUCCESS;
}

const SLAndroidSimpleBufferQueueItf_ kBqVt = {
    .Enqueue = bqEnqueue, .Clear = bqClear, .GetState = bqGetState, .RegisterCallback = bqRegisterCallback,
};

// ---- Player: SLVolumeItf (forwarded) ----
Backend* be(SLVolumeItf self) { return CONTAINER_OF(self, Player, volVt)->backend; }
SLresult volSetVolumeLevel(SLVolumeItf s, SLmillibel v) { LOGI("SetVolumeLevel(%d)", v); return be(s) ? be(s)->setVolumeLevel(v) : SL_RESULT_RESOURCE_ERROR; }
SLresult volGetVolumeLevel(SLVolumeItf s, SLmillibel* v) { return be(s) ? be(s)->getVolumeLevel(v) : SL_RESULT_RESOURCE_ERROR; }
SLresult volGetMaxVolumeLevel(SLVolumeItf s, SLmillibel* v) { return be(s) ? be(s)->getMaxVolumeLevel(v) : SL_RESULT_RESOURCE_ERROR; }
SLresult volSetMute(SLVolumeItf s, SLboolean m) { return be(s) ? be(s)->setMute(m == SL_BOOLEAN_TRUE) : SL_RESULT_RESOURCE_ERROR; }
SLresult volGetMute(SLVolumeItf s, SLboolean* m) { bool b = false; SLresult r = be(s) ? be(s)->getMute(&b) : SL_RESULT_RESOURCE_ERROR; if (m) *m = b ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE; return r; }
SLresult volEnableStereoPosition(SLVolumeItf s, SLboolean e) { return be(s) ? be(s)->enableStereoPosition(e == SL_BOOLEAN_TRUE) : SL_RESULT_RESOURCE_ERROR; }
SLresult volIsEnabledStereoPosition(SLVolumeItf s, SLboolean* e) { bool b = false; SLresult r = be(s) ? be(s)->isEnabledStereoPosition(&b) : SL_RESULT_RESOURCE_ERROR; if (e) *e = b ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE; return r; }
SLresult volSetStereoPosition(SLVolumeItf s, SLpermille p) { return be(s) ? be(s)->setStereoPosition(p) : SL_RESULT_RESOURCE_ERROR; }
SLresult volGetStereoPosition(SLVolumeItf s, SLpermille* p) { return be(s) ? be(s)->getStereoPosition(p) : SL_RESULT_RESOURCE_ERROR; }

const SLVolumeItf_ kVolVt = {
    .SetVolumeLevel = volSetVolumeLevel, .GetVolumeLevel = volGetVolumeLevel, .GetMaxVolumeLevel = volGetMaxVolumeLevel,
    .SetMute = volSetMute, .GetMute = volGetMute, .EnableStereoPosition = volEnableStereoPosition,
    .IsEnabledStereoPosition = volIsEnabledStereoPosition, .SetStereoPosition = volSetStereoPosition,
    .GetStereoPosition = volGetStereoPosition,
};

// ---- SLEngineItf ----
SLresult unsupportedCreate(const char* what) { LOGW("engine.%s unsupported", what); return SL_RESULT_FEATURE_UNSUPPORTED; }
SLresult engCreateLEDDevice(SLEngineItf, SLObjectItf*, SLuint32, SLuint32, const SLInterfaceID*, const SLboolean*) { return unsupportedCreate("CreateLEDDevice"); }
SLresult engCreateVibraDevice(SLEngineItf, SLObjectItf*, SLuint32, SLuint32, const SLInterfaceID*, const SLboolean*) { return unsupportedCreate("CreateVibraDevice"); }
SLresult engCreateAudioRecorder(SLEngineItf, SLObjectItf*, SLDataSource*, SLDataSink*, SLuint32, const SLInterfaceID*, const SLboolean*) { return unsupportedCreate("CreateAudioRecorder"); }
SLresult engCreateMidiPlayer(SLEngineItf, SLObjectItf*, SLDataSource*, SLDataSource*, SLDataSink*, SLDataSink*, SLDataSink*, SLuint32, const SLInterfaceID*, const SLboolean*) { return unsupportedCreate("CreateMidiPlayer"); }
SLresult engCreateListener(SLEngineItf, SLObjectItf*, SLuint32, const SLInterfaceID*, const SLboolean*) { return unsupportedCreate("CreateListener"); }
SLresult engCreate3DGroup(SLEngineItf, SLObjectItf*, SLuint32, const SLInterfaceID*, const SLboolean*) { return unsupportedCreate("Create3DGroup"); }
SLresult engCreateMetadataExtractor(SLEngineItf, SLObjectItf*, SLDataSource*, SLuint32, const SLInterfaceID*, const SLboolean*) { return unsupportedCreate("CreateMetadataExtractor"); }
SLresult engCreateExtensionObject(SLEngineItf, SLObjectItf*, void*, SLuint32, SLuint32, const SLInterfaceID*, const SLboolean*) { return unsupportedCreate("CreateExtensionObject"); }
SLresult engQueryNumSupportedInterfaces(SLEngineItf, SLuint32, SLuint32* n) { if (n) *n = 0; return SL_RESULT_SUCCESS; }
SLresult engQuerySupportedInterfaces(SLEngineItf, SLuint32, SLuint32, SLInterfaceID*) { LOGW("engine.QuerySupportedInterfaces unsupported"); return SL_RESULT_FEATURE_UNSUPPORTED; }
SLresult engQueryNumSupportedExtensions(SLEngineItf, SLuint32* n) { if (n) *n = 0; return SL_RESULT_SUCCESS; }
SLresult engQuerySupportedExtension(SLEngineItf, SLuint32, SLchar*, SLint16*) { LOGW("engine.QuerySupportedExtension unsupported"); return SL_RESULT_FEATURE_UNSUPPORTED; }
SLresult engIsExtensionSupported(SLEngineItf, const SLchar*, SLboolean* s) { if (s) *s = SL_BOOLEAN_FALSE; return SL_RESULT_SUCCESS; }

SLresult engCreateOutputMix(SLEngineItf, SLObjectItf* out, SLuint32 n, const SLInterfaceID* ids, const SLboolean*) {
    for (SLuint32 i = 0; i < n; ++i) LOGI("CreateOutputMix wants interface %s", iidName(ids[i]));
    auto* m = new (std::nothrow) OutputMix{&kMixObjVt, SL_OBJECT_STATE_UNREALIZED};
    if (!m) return SL_RESULT_MEMORY_FAILURE;
    *out = &m->objVt;
    LOGI("output mix created");
    return SL_RESULT_SUCCESS;
}

SLresult engCreateAudioPlayer(SLEngineItf, SLObjectItf* out, SLDataSource* src, SLDataSink* snk,
                              SLuint32 n, const SLInterfaceID* ids, const SLboolean* req) {
    PcmFormat fmt;
    SLuint32 locatorType = src && src->pLocator ? *static_cast<SLuint32*>(src->pLocator) : 0;
    if (locatorType == SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE || locatorType == SL_DATALOCATOR_BUFFERQUEUE) {
        fmt.numBuffers = static_cast<SLDataLocator_AndroidSimpleBufferQueue*>(src->pLocator)->numBuffers;
    } else {
        LOGW("CreateAudioPlayer: unexpected source locator 0x%08x", locatorType);
    }
    SLuint32 formatType = src && src->pFormat ? *static_cast<SLuint32*>(src->pFormat) : 0;
    if (formatType == SL_DATAFORMAT_PCM) {
        auto* pcm = static_cast<SLDataFormat_PCM*>(src->pFormat);
        fmt.channels = pcm->numChannels;
        fmt.rateHz = pcm->samplesPerSec / 1000;   // OpenSL rates are in milliHz
        fmt.bits = pcm->bitsPerSample;
        LOGI("CreateAudioPlayer: PCM ch=%u rate=%u bits=%u container=%u mask=0x%x endian=%u buffers=%u",
             pcm->numChannels, fmt.rateHz, pcm->bitsPerSample, pcm->containerSize, pcm->channelMask, pcm->endianness, fmt.numBuffers);
    } else {
        LOGW("CreateAudioPlayer: unexpected source format 0x%08x", formatType);
    }
    SLuint32 sinkType = snk && snk->pLocator ? *static_cast<SLuint32*>(snk->pLocator) : 0;
    LOGI("CreateAudioPlayer: sink locator 0x%08x", sinkType);
    for (SLuint32 i = 0; i < n; ++i) LOGI("CreateAudioPlayer wants interface %s (required=%d)", iidName(ids[i]), req ? req[i] : -1);

    auto* p = new (std::nothrow) Player{};
    if (!p) return SL_RESULT_MEMORY_FAILURE;
    p->objVt = &kPlayerObjVt; p->playVt = &kPlayVt; p->bqVt = &kBqVt; p->volVt = &kVolVt;
    p->state = SL_OBJECT_STATE_UNREALIZED;
    p->fmt = fmt;
    p->playState = SL_PLAYSTATE_STOPPED;
    *out = &p->objVt;
    return SL_RESULT_SUCCESS;
}

const SLEngineItf_ kEngineVt = {
    .CreateLEDDevice = engCreateLEDDevice, .CreateVibraDevice = engCreateVibraDevice,
    .CreateAudioPlayer = engCreateAudioPlayer, .CreateAudioRecorder = engCreateAudioRecorder,
    .CreateMidiPlayer = engCreateMidiPlayer, .CreateListener = engCreateListener, .Create3DGroup = engCreate3DGroup,
    .CreateOutputMix = engCreateOutputMix, .CreateMetadataExtractor = engCreateMetadataExtractor,
    .CreateExtensionObject = engCreateExtensionObject,
    .QueryNumSupportedInterfaces = engQueryNumSupportedInterfaces, .QuerySupportedInterfaces = engQuerySupportedInterfaces,
    .QueryNumSupportedExtensions = engQueryNumSupportedExtensions, .QuerySupportedExtension = engQuerySupportedExtension,
    .IsExtensionSupported = engIsExtensionSupported,
};

}  // namespace

namespace xfade {
void setFactories(BackendFactory backend, PipelineFactory pipeline) {
    g_backendFactory = backend;
    g_pipelineFactory = pipeline;
}
}  // namespace xfade

// ---- exported symbols (names match the eSDK's imports) ----
extern "C" {
__attribute__((visibility("default"))) const SLInterfaceID SL_IID_ENGINE = &kIidEngine;
__attribute__((visibility("default"))) const SLInterfaceID SL_IID_PLAY = &kIidPlay;
__attribute__((visibility("default"))) const SLInterfaceID SL_IID_BUFFERQUEUE = &kIidBufferQueue;
__attribute__((visibility("default"))) const SLInterfaceID SL_IID_VOLUME = &kIidVolume;

__attribute__((visibility("default")))
SLresult slCreateEngine(SLObjectItf* out, SLuint32 numOptions, const SLEngineOption*,
                        SLuint32 numInterfaces, const SLInterfaceID* ids, const SLboolean*) {
    LOGI("slCreateEngine(options=%u, interfaces=%u)", numOptions, numInterfaces);
    for (SLuint32 i = 0; i < numInterfaces; ++i) LOGI("slCreateEngine wants interface %s", iidName(ids[i]));
    auto* e = new (std::nothrow) Engine{&kEngineObjVt, &kEngineVt, SL_OBJECT_STATE_UNREALIZED};
    if (!e) return SL_RESULT_MEMORY_FAILURE;
    *out = &e->objVt;
    return SL_RESULT_SUCCESS;
}
}
