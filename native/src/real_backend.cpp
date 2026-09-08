#include "real_backend.h"
#include <dlfcn.h>
#include <mutex>
#include "log.h"

namespace xfade {
namespace {

using CreateEngineFn = SLresult (*)(SLObjectItf*, SLuint32, const SLEngineOption*, SLuint32, const SLInterfaceID*, const SLboolean*);

std::once_flag g_once;
bool g_loaded = false;
SLObjectItf g_engineObj = nullptr;
SLEngineItf g_engine = nullptr;
SLObjectItf g_mixObj = nullptr;
SLInterfaceID g_iidEngine, g_iidPlay, g_iidSimpleBq, g_iidVolume;

template <class T> bool sym(void* lib, const char* name, T* out) {
    void* p = dlsym(lib, name);
    if (!p) { LOGE("dlsym(%s) failed: %s", name, dlerror()); return false; }
    *out = reinterpret_cast<T>(p);
    return true;
}

}  // namespace

bool RealBackend::loadLibrary() {
    std::call_once(g_once, [] {
        void* lib = dlopen("libOpenSLES.so", RTLD_NOW);
        if (!lib) { LOGE("dlopen(libOpenSLES.so) failed: %s", dlerror()); return; }
        CreateEngineFn createEngine = nullptr;
        SLInterfaceID* pEngine; SLInterfaceID* pPlay; SLInterfaceID* pBq; SLInterfaceID* pVol;
        if (!sym(lib, "slCreateEngine", &createEngine) || !sym(lib, "SL_IID_ENGINE", &pEngine) ||
            !sym(lib, "SL_IID_PLAY", &pPlay) || !sym(lib, "SL_IID_ANDROIDSIMPLEBUFFERQUEUE", &pBq) ||
            !sym(lib, "SL_IID_VOLUME", &pVol)) return;
        g_iidEngine = *pEngine; g_iidPlay = *pPlay; g_iidSimpleBq = *pBq; g_iidVolume = *pVol;
        if (createEngine(&g_engineObj, 0, nullptr, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) { LOGE("real slCreateEngine failed"); return; }
        if ((*g_engineObj)->Realize(g_engineObj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) { LOGE("real engine Realize failed"); return; }
        if ((*g_engineObj)->GetInterface(g_engineObj, g_iidEngine, &g_engine) != SL_RESULT_SUCCESS) { LOGE("real engine GetInterface failed"); return; }
        if ((*g_engine)->CreateOutputMix(g_engine, &g_mixObj, 0, nullptr, nullptr) != SL_RESULT_SUCCESS) { LOGE("real CreateOutputMix failed"); return; }
        if ((*g_mixObj)->Realize(g_mixObj, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) { LOGE("real output mix Realize failed"); return; }
        g_loaded = true;
        LOGI("real OpenSL ES engine ready");
    });
    return g_loaded;
}

void RealBackend::bqCallback(SLAndroidSimpleBufferQueueItf, void* ctx) {
    auto* self = static_cast<RealBackend*>(ctx);
    if (self->done_.fn) self->done_.fn(self->done_.ctx);
}

bool RealBackend::open(const PcmFormat& fmt, DoneCallback done) {
    if (playerObj_) { LOGW("RealBackend::open called while open; closing previous player"); close(); }
    if (!loadLibrary()) return false;
    done_ = done;
    SLDataLocator_AndroidSimpleBufferQueue loc = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, fmt.numBuffers ? fmt.numBuffers : 4};
    SLuint32 mask = fmt.channels == 2 ? (SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT) : SL_SPEAKER_FRONT_CENTER;
    SLDataFormat_PCM pcm = {SL_DATAFORMAT_PCM, fmt.channels, fmt.rateHz * 1000, fmt.bits, fmt.bits, mask, SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource src = {&loc, &pcm};
    SLDataLocator_OutputMix outLoc = {SL_DATALOCATOR_OUTPUTMIX, g_mixObj};
    SLDataSink snk = {&outLoc, nullptr};
    const SLInterfaceID ids[] = {g_iidSimpleBq, g_iidVolume};
    const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE};
    SLresult r = (*g_engine)->CreateAudioPlayer(g_engine, &playerObj_, &src, &snk, 2, ids, req);
    if (r != SL_RESULT_SUCCESS) { LOGE("real CreateAudioPlayer failed: %u", r); return false; }
    if ((*playerObj_)->Realize(playerObj_, SL_BOOLEAN_FALSE) != SL_RESULT_SUCCESS) { LOGE("real player Realize failed"); close(); return false; }
    if ((*playerObj_)->GetInterface(playerObj_, g_iidPlay, &play_) != SL_RESULT_SUCCESS) { LOGE("real player: no PLAY"); close(); return false; }
    if ((*playerObj_)->GetInterface(playerObj_, g_iidSimpleBq, &bq_) != SL_RESULT_SUCCESS) { LOGE("real player: no BUFFERQUEUE"); close(); return false; }
    if ((*playerObj_)->GetInterface(playerObj_, g_iidVolume, &vol_) != SL_RESULT_SUCCESS) { LOGW("real player: no VOLUME"); vol_ = nullptr; }
    (*bq_)->RegisterCallback(bq_, &RealBackend::bqCallback, this);
    LOGI("real player open rate=%u ch=%u bits=%u buffers=%u", fmt.rateHz, fmt.channels, fmt.bits, loc.numBuffers);
    return true;
}

uint32_t RealBackend::enqueue(const void* data, uint32_t bytes) {
    if (!bq_) return SL_RESULT_RESOURCE_ERROR;
    SLresult r = (*bq_)->Enqueue(bq_, data, bytes);
    if (r != SL_RESULT_SUCCESS) LOGW("real Enqueue failed: %u", r);
    return r;
}

void RealBackend::clear() { if (bq_) (*bq_)->Clear(bq_); }
bool RealBackend::setPlayState(uint32_t s) { return play_ && (*play_)->SetPlayState(play_, s) == SL_RESULT_SUCCESS; }
uint32_t RealBackend::playState() { SLuint32 s = SL_PLAYSTATE_STOPPED; if (play_) (*play_)->GetPlayState(play_, &s); return s; }

#define XF_VOL(call) (vol_ ? (*vol_)->call : (uint32_t)SL_RESULT_FEATURE_UNSUPPORTED)
uint32_t RealBackend::setVolumeLevel(int16_t mB) { return XF_VOL(SetVolumeLevel(vol_, mB)); }
uint32_t RealBackend::getVolumeLevel(int16_t* mB) { return XF_VOL(GetVolumeLevel(vol_, mB)); }
uint32_t RealBackend::getMaxVolumeLevel(int16_t* mB) { return XF_VOL(GetMaxVolumeLevel(vol_, mB)); }
uint32_t RealBackend::setMute(bool m) { return XF_VOL(SetMute(vol_, m ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE)); }
uint32_t RealBackend::getMute(bool* m) { SLboolean b = SL_BOOLEAN_FALSE; uint32_t r = XF_VOL(GetMute(vol_, &b)); *m = b == SL_BOOLEAN_TRUE; return r; }
uint32_t RealBackend::enableStereoPosition(bool e) { return XF_VOL(EnableStereoPosition(vol_, e ? SL_BOOLEAN_TRUE : SL_BOOLEAN_FALSE)); }
uint32_t RealBackend::isEnabledStereoPosition(bool* e) { SLboolean b = SL_BOOLEAN_FALSE; uint32_t r = XF_VOL(IsEnabledStereoPosition(vol_, &b)); *e = b == SL_BOOLEAN_TRUE; return r; }
uint32_t RealBackend::setStereoPosition(int16_t p) { return XF_VOL(SetStereoPosition(vol_, p)); }
uint32_t RealBackend::getStereoPosition(int16_t* p) { return XF_VOL(GetStereoPosition(vol_, p)); }
#undef XF_VOL

void RealBackend::close() {
    // Destroy on the player object is synchronous: it stops the buffer-queue callback (and joins any
    // in-flight invocation) before returning, satisfying Backend::close()'s no-callbacks-after contract.
    if (playerObj_) { (*playerObj_)->Destroy(playerObj_); playerObj_ = nullptr; }
    play_ = nullptr; bq_ = nullptr; vol_ = nullptr;
}

}  // namespace xfade
