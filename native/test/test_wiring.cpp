#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include <cstring>
#include "test.h"
#include "fake_backend.h"
#include "../src/fake_sl.h"
#include "../src/passthrough_pipeline.h"
#include "../src/ring_pipeline.h"
#include "../src/status.h"

namespace {
FakeBackend* g_be = nullptr;
int g_ringMade = 0, g_passMade = 0;
struct FailingBackend : FakeBackend {
    uint32_t openResult = 0;   // unused; open() fails outright
    bool open(const xfade::PcmFormat&, xfade::DoneCallback) override { return false; }
};

xfade::Pipeline* chooser(const xfade::PcmFormat& f) {
    if (f.bits == 16) { ++g_ringMade; return new xfade::RingPipeline(); }
    ++g_passMade;
    return new xfade::PassthroughPipeline();
}

SLObjectItf makePlayer(SLuint32 bits, SLuint32 rate = SL_SAMPLINGRATE_44_1) {
    SLObjectItf engineObj = nullptr;
    slCreateEngine(&engineObj, 0, nullptr, 0, nullptr, nullptr);
    (*engineObj)->Realize(engineObj, SL_BOOLEAN_FALSE);
    SLEngineItf engine = nullptr;
    (*engineObj)->GetInterface(engineObj, SL_IID_ENGINE, &engine);
    static SLDataLocator_AndroidSimpleBufferQueue loc = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    static SLDataFormat_PCM pcm;
    pcm = {SL_DATAFORMAT_PCM, 2, rate, bits, bits, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT, SL_BYTEORDER_LITTLEENDIAN};
    static SLDataSource src = {&loc, &pcm};
    static SLDataSink snk = {nullptr, nullptr};
    SLObjectItf playerObj = nullptr;
    (*engine)->CreateAudioPlayer(engine, &playerObj, &src, &snk, 0, nullptr, nullptr);
    return playerObj;   // engine object intentionally leaked in tests
}
}  // namespace

TEST(wiring_selects_ring_for_16_bit_and_passthrough_otherwise) {
    g_ringMade = g_passMade = 0;
    xfade::setFactories([]() -> xfade::Backend* { g_be = new FakeBackend(); return g_be; }, chooser);
    xfade::setCrossfadeMs(3000);
    SLObjectItf p16 = makePlayer(SL_PCMSAMPLEFORMAT_FIXED_16);
    EXPECT_EQ((*p16)->Realize(p16, SL_BOOLEAN_FALSE), (SLresult)SL_RESULT_SUCCESS);
    EXPECT_EQ(g_ringMade, 1);
    EXPECT_TRUE(xfade::statusLine().rfind("state=active rate=44100 ch=2 lead_ms=0 crossfade_ms=3000", 0) == 0);
    {
        std::lock_guard<std::mutex> lock(xfade::RingPipeline::registryMutex());
        EXPECT_TRUE(xfade::RingPipeline::current() != nullptr);
    }
    (*p16)->Destroy(p16);
    {
        std::lock_guard<std::mutex> lock(xfade::RingPipeline::registryMutex());
        EXPECT_TRUE(xfade::RingPipeline::current() == nullptr);
    }
    EXPECT_TRUE(xfade::statusLine() == "state=no-player rate=0 ch=0 crossfade_ms=3000");

    SLObjectItf p8 = makePlayer(SL_PCMSAMPLEFORMAT_FIXED_8);
    EXPECT_EQ((*p8)->Realize(p8, SL_BOOLEAN_FALSE), (SLresult)SL_RESULT_SUCCESS);
    EXPECT_EQ(g_passMade, 1);
    EXPECT_TRUE(xfade::statusLine().rfind("state=passthrough", 0) == 0);
    (*p8)->Destroy(p8);
}

TEST(wiring_falls_back_to_passthrough_when_ring_cannot_start) {
    g_ringMade = g_passMade = 0;
    // A ring pipeline refuses a 0 Hz format outright (RingPipeline::start's own format guard); the front
    // end must fall back to passthrough. (An absurdly high rate was tried first to force an allocation
    // failure instead, but SLDataFormat_PCM.samplesPerSec is a milliHz-encoded uint32_t, which caps the
    // deliverable rate at ~4.295 MHz; even at that ceiling a stereo 26 s ring is ~223M samples, always
    // under ring.h's 2^28-sample cutoff, and the ~400 MB it can reach allocated successfully on the test
    // device. rate=0 exercises the same RingPipeline::start() rejection -> passthrough fallback path
    // deterministically, independent of the device's available memory.)
    xfade::setFactories([]() -> xfade::Backend* { g_be = new FakeBackend(); return g_be; }, chooser);
    SLObjectItf p = makePlayer(SL_PCMSAMPLEFORMAT_FIXED_16, 0u);
    EXPECT_EQ((*p)->Realize(p, SL_BOOLEAN_FALSE), (SLresult)SL_RESULT_SUCCESS);
    EXPECT_EQ(g_ringMade, 1);
    // The fallback in playerRealize constructs PassthroughPipeline directly (it never calls back into
    // g_pipelineFactory), so chooser's passthrough branch -- and g_passMade -- is never reached here; the
    // status line is the observable proof that the fallback engaged.
    EXPECT_EQ(g_passMade, 0);
    EXPECT_TRUE(xfade::statusLine().rfind("state=passthrough", 0) == 0);
    (*p)->Destroy(p);
}

TEST(wiring_no_player_when_backend_fails) {
    xfade::setFactories([]() -> xfade::Backend* { return new FailingBackend(); }, chooser);
    SLObjectItf p = makePlayer(SL_PCMSAMPLEFORMAT_FIXED_16);
    EXPECT_EQ((*p)->Realize(p, SL_BOOLEAN_FALSE), (SLresult)SL_RESULT_RESOURCE_ERROR);
    EXPECT_TRUE(xfade::statusLine().rfind("state=no-player", 0) == 0);
    (*p)->Destroy(p);
}

TEST(wiring_dumper_writes_history_and_tail) {
    xfade::PcmDumper d;
    d.configureForTests("/data/local/tmp", 100, 1, /*enabled=*/true);   // 100 Hz mono: 5 s = 500 frames
    EXPECT_TRUE(d.enabled());
    int16_t block[10];
    for (int i = 0; i < 100; ++i) { for (int j = 0; j < 10; ++j) block[j] = (int16_t)(i * 10 + j); d.push(block, 10); }   // 1000 frames
    d.trigger();
    for (int i = 0; i < 200; ++i) { for (int j = 0; j < 10; ++j) block[j] = (int16_t)(-1 - i); d.push(block, 10); }       // 2000 frames > 15 s
    FILE* f = std::fopen("/data/local/tmp/xfade_dump_1.pcm", "rb");
    EXPECT_TRUE(f != nullptr);
    if (f) {
        std::fseek(f, 0, SEEK_END);
        long size = std::ftell(f);
        EXPECT_EQ(size, (long)(500 + 1500) * 2);   // 5 s history + 15 s tail, 16-bit mono
        std::fseek(f, 0, SEEK_SET);
        int16_t first, at500;
        std::fread(&first, 2, 1, f);
        std::fseek(f, 500 * 2, SEEK_SET);
        std::fread(&at500, 2, 1, f);
        EXPECT_EQ(first, (int16_t)500);            // history starts 500 frames before the trigger
        EXPECT_EQ(at500, (int16_t)-1);             // first frame after the trigger
        std::fclose(f);
        std::remove("/data/local/tmp/xfade_dump_1.pcm");
    }
}

TEST(wiring_dumper_refuses_zero_rate) {
    std::remove("/data/local/tmp/xfade_dump_1.pcm");   // in case a stale file survived an earlier run
    xfade::PcmDumper d;
    d.configureForTests("/data/local/tmp", 0, 2, /*enabled=*/true);
    EXPECT_TRUE(!d.enabled());
    d.trigger();
    int16_t block[4] = {1, 2, 3, 4};
    d.push(block, 2);
    FILE* f = std::fopen("/data/local/tmp/xfade_dump_1.pcm", "rb");
    EXPECT_TRUE(f == nullptr);
    if (f) { std::fclose(f); std::remove("/data/local/tmp/xfade_dump_1.pcm"); }
}
