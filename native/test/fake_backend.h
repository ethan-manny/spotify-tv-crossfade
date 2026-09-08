#pragma once
#include <SLES/OpenSLES.h>
#include <cstdint>
#include <vector>
#include "../src/backend.h"

// Records everything the pipeline does; completeOne() simulates the audio thread finishing a buffer.
struct FakeBackend : xfade::Backend {
    struct Buf { const void* data; uint32_t bytes; };
    xfade::PcmFormat opened;
    xfade::DoneCallback done;
    std::vector<Buf> enqueued;
    uint32_t state = SL_PLAYSTATE_STOPPED;
    int16_t volumeMb = 0;
    bool muted = false;
    int clears = 0;
    static inline bool s_closed = false;
    uint32_t enqueueResult = SL_RESULT_SUCCESS;   // what enqueue() returns; nothing is recorded unless it is success

    bool open(const xfade::PcmFormat& f, xfade::DoneCallback d) override { opened = f; done = d; s_closed = false; return true; }
    uint32_t enqueue(const void* data, uint32_t bytes) override {
        if (enqueueResult != SL_RESULT_SUCCESS) return enqueueResult;
        enqueued.push_back({data, bytes});
        return SL_RESULT_SUCCESS;
    }
    void clear() override { enqueued.clear(); ++clears; }
    bool setPlayState(uint32_t s) override { state = s; return true; }
    uint32_t playState() override { return state; }
    uint32_t setVolumeLevel(int16_t mB) override { volumeMb = mB; return SL_RESULT_SUCCESS; }
    uint32_t getVolumeLevel(int16_t* mB) override { *mB = volumeMb; return SL_RESULT_SUCCESS; }
    uint32_t getMaxVolumeLevel(int16_t* mB) override { *mB = 0; return SL_RESULT_SUCCESS; }
    uint32_t setMute(bool m) override { muted = m; return SL_RESULT_SUCCESS; }
    uint32_t getMute(bool* m) override { *m = muted; return SL_RESULT_SUCCESS; }
    uint32_t enableStereoPosition(bool) override { return SL_RESULT_SUCCESS; }
    uint32_t isEnabledStereoPosition(bool* e) override { *e = false; return SL_RESULT_SUCCESS; }
    uint32_t setStereoPosition(int16_t) override { return SL_RESULT_SUCCESS; }
    uint32_t getStereoPosition(int16_t* p) override { *p = 0; return SL_RESULT_SUCCESS; }
    void close() override { s_closed = true; }

    void completeOne() { if (!enqueued.empty()) enqueued.erase(enqueued.begin()); if (done.fn) done.fn(done.ctx); }
};
