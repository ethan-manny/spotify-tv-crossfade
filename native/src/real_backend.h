#pragma once
#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include "backend.h"

namespace xfade {

// Plays through the system OpenSL ES library (dlopen'd). One engine/output mix per process, one player per open().
class RealBackend : public Backend {
public:
    bool open(const PcmFormat& fmt, DoneCallback done) override;
    uint32_t enqueue(const void* data, uint32_t bytes) override;
    void clear() override;
    bool setPlayState(uint32_t slPlayState) override;
    uint32_t playState() override;
    uint32_t setVolumeLevel(int16_t mB) override;
    uint32_t getVolumeLevel(int16_t* mB) override;
    uint32_t getMaxVolumeLevel(int16_t* mB) override;
    uint32_t setMute(bool mute) override;
    uint32_t getMute(bool* mute) override;
    uint32_t enableStereoPosition(bool enable) override;
    uint32_t isEnabledStereoPosition(bool* enabled) override;
    uint32_t setStereoPosition(int16_t permille) override;
    uint32_t getStereoPosition(int16_t* permille) override;
    void close() override;

private:
    static bool loadLibrary();   // once per process; false if the system library is unusable
    static void bqCallback(SLAndroidSimpleBufferQueueItf, void* ctx);
    DoneCallback done_;
    SLObjectItf playerObj_ = nullptr;
    SLPlayItf play_ = nullptr;
    SLAndroidSimpleBufferQueueItf bq_ = nullptr;
    SLVolumeItf vol_ = nullptr;
};

}  // namespace xfade
