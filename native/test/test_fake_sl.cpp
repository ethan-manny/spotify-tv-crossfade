#include <SLES/OpenSLES.h>
#include <SLES/OpenSLES_Android.h>
#include "test.h"
#include "fake_backend.h"
#include "../src/fake_sl.h"
#include "../src/passthrough_pipeline.h"

static FakeBackend* g_lastFake = nullptr;
static int g_callbacks = 0;
static int g_backendsCreated = 0;

static void useFakes() {
    xfade::setFactories([]() -> xfade::Backend* { ++g_backendsCreated; g_lastFake = new FakeBackend(); return g_lastFake; },
                        [](const xfade::PcmFormat&) -> xfade::Pipeline* { return new xfade::PassthroughPipeline(); });
}

// A realized engine plus an unrealized stereo 44.1 kHz player, the way the eSDK creates them.
struct Rig {
    SLObjectItf engineObj = nullptr;
    SLEngineItf engine = nullptr;
    SLObjectItf playerObj = nullptr;
};

static Rig makeRig() {
    Rig r;
    useFakes();
    slCreateEngine(&r.engineObj, 0, nullptr, 0, nullptr, nullptr);
    (*r.engineObj)->Realize(r.engineObj, SL_BOOLEAN_FALSE);
    (*r.engineObj)->GetInterface(r.engineObj, SL_IID_ENGINE, &r.engine);
    SLDataLocator_AndroidSimpleBufferQueue loc = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 2};
    SLDataFormat_PCM pcm = {SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_44_1, 16, 16, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT, SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource src = {&loc, &pcm};
    SLDataSink snk = {nullptr, nullptr};
    (*r.engine)->CreateAudioPlayer(r.engine, &r.playerObj, &src, &snk, 0, nullptr, nullptr);
    return r;
}

static void destroyRig(Rig& r) {
    (*r.playerObj)->Destroy(r.playerObj);
    (*r.engineObj)->Destroy(r.engineObj);
}

// Drives the front end exactly the way the eSDK's OpenSLAudioDriver is expected to.
TEST(front_end_creates_player_and_passes_buffers_through) {
    useFakes();
    g_callbacks = 0;

    SLObjectItf engineObj = nullptr;
    EXPECT_EQ(slCreateEngine(&engineObj, 0, nullptr, 0, nullptr, nullptr), (SLresult)SL_RESULT_SUCCESS);
    EXPECT_EQ((*engineObj)->Realize(engineObj, SL_BOOLEAN_FALSE), (SLresult)SL_RESULT_SUCCESS);
    SLEngineItf engine = nullptr;
    EXPECT_EQ((*engineObj)->GetInterface(engineObj, SL_IID_ENGINE, &engine), (SLresult)SL_RESULT_SUCCESS);

    SLObjectItf mixObj = nullptr;
    EXPECT_EQ((*engine)->CreateOutputMix(engine, &mixObj, 0, nullptr, nullptr), (SLresult)SL_RESULT_SUCCESS);
    EXPECT_EQ((*mixObj)->Realize(mixObj, SL_BOOLEAN_FALSE), (SLresult)SL_RESULT_SUCCESS);

    SLDataLocator_AndroidSimpleBufferQueue loc = {SL_DATALOCATOR_ANDROIDSIMPLEBUFFERQUEUE, 3};
    SLDataFormat_PCM pcm = {SL_DATAFORMAT_PCM, 2, SL_SAMPLINGRATE_44_1, SL_PCMSAMPLEFORMAT_FIXED_16,
                            SL_PCMSAMPLEFORMAT_FIXED_16, SL_SPEAKER_FRONT_LEFT | SL_SPEAKER_FRONT_RIGHT,
                            SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource src = {&loc, &pcm};
    SLDataLocator_OutputMix outLoc = {SL_DATALOCATOR_OUTPUTMIX, mixObj};
    SLDataSink snk = {&outLoc, nullptr};
    const SLInterfaceID ids[] = {SL_IID_BUFFERQUEUE, SL_IID_VOLUME};
    const SLboolean req[] = {SL_BOOLEAN_TRUE, SL_BOOLEAN_FALSE};

    SLObjectItf playerObj = nullptr;
    EXPECT_EQ((*engine)->CreateAudioPlayer(engine, &playerObj, &src, &snk, 2, ids, req), (SLresult)SL_RESULT_SUCCESS);
    EXPECT_EQ((*playerObj)->Realize(playerObj, SL_BOOLEAN_FALSE), (SLresult)SL_RESULT_SUCCESS);
    EXPECT_TRUE(g_lastFake != nullptr);
    EXPECT_EQ(g_lastFake->opened.rateHz, 44100u);
    EXPECT_EQ(g_lastFake->opened.channels, 2u);
    EXPECT_EQ(g_lastFake->opened.bits, 16u);
    EXPECT_EQ(g_lastFake->opened.numBuffers, 3u);

    SLAndroidSimpleBufferQueueItf bq = nullptr;
    EXPECT_EQ((*playerObj)->GetInterface(playerObj, SL_IID_BUFFERQUEUE, &bq), (SLresult)SL_RESULT_SUCCESS);
    EXPECT_EQ((*bq)->RegisterCallback(bq, [](SLAndroidSimpleBufferQueueItf, void*) { ++g_callbacks; }, nullptr),
              (SLresult)SL_RESULT_SUCCESS);

    int16_t buf[64] = {};
    EXPECT_EQ((*bq)->Enqueue(bq, buf, sizeof buf), (SLresult)SL_RESULT_SUCCESS);
    EXPECT_EQ(g_lastFake->enqueued.size(), (size_t)1);
    EXPECT_TRUE(g_lastFake->enqueued[0].data == buf);
    SLAndroidSimpleBufferQueueState st{};
    (*bq)->GetState(bq, &st);
    EXPECT_EQ(st.count, 1u);
    EXPECT_EQ(st.index, 0u);

    g_lastFake->completeOne();
    EXPECT_EQ(g_callbacks, 1);
    (*bq)->GetState(bq, &st);
    EXPECT_EQ(st.count, 0u);
    EXPECT_EQ(st.index, 1u);

    SLPlayItf play = nullptr;
    EXPECT_EQ((*playerObj)->GetInterface(playerObj, SL_IID_PLAY, &play), (SLresult)SL_RESULT_SUCCESS);
    EXPECT_EQ((*play)->SetPlayState(play, SL_PLAYSTATE_PLAYING), (SLresult)SL_RESULT_SUCCESS);
    EXPECT_EQ(g_lastFake->state, (uint32_t)SL_PLAYSTATE_PLAYING);
    SLuint32 ps = 0;
    (*play)->GetPlayState(play, &ps);
    EXPECT_EQ(ps, (SLuint32)SL_PLAYSTATE_PLAYING);

    SLVolumeItf vol = nullptr;
    EXPECT_EQ((*playerObj)->GetInterface(playerObj, SL_IID_VOLUME, &vol), (SLresult)SL_RESULT_SUCCESS);
    EXPECT_EQ((*vol)->SetVolumeLevel(vol, -600), (SLresult)SL_RESULT_SUCCESS);
    EXPECT_EQ(g_lastFake->volumeMb, (int16_t)-600);

    EXPECT_EQ((*bq)->Clear(bq), (SLresult)SL_RESULT_SUCCESS);
    EXPECT_EQ(g_lastFake->clears, 1);

    void* dummy = nullptr;
    EXPECT_EQ((*playerObj)->GetInterface(playerObj, SL_IID_ENGINE, &dummy), (SLresult)SL_RESULT_FEATURE_UNSUPPORTED);

    (*playerObj)->Destroy(playerObj);
    EXPECT_TRUE(FakeBackend::s_closed);
    (*mixObj)->Destroy(mixObj);
    (*engineObj)->Destroy(engineObj);
}

TEST(front_end_accepts_generic_bufferqueue_locator) {
    useFakes();
    SLObjectItf engineObj = nullptr;
    slCreateEngine(&engineObj, 0, nullptr, 0, nullptr, nullptr);
    (*engineObj)->Realize(engineObj, SL_BOOLEAN_FALSE);
    SLEngineItf engine = nullptr;
    (*engineObj)->GetInterface(engineObj, SL_IID_ENGINE, &engine);
    SLDataLocator_BufferQueue loc = {SL_DATALOCATOR_BUFFERQUEUE, 4};
    SLDataFormat_PCM pcm = {SL_DATAFORMAT_PCM, 1, SL_SAMPLINGRATE_48, 16, 16, SL_SPEAKER_FRONT_CENTER, SL_BYTEORDER_LITTLEENDIAN};
    SLDataSource src = {&loc, &pcm};
    SLDataSink snk = {nullptr, nullptr};
    SLObjectItf playerObj = nullptr;
    EXPECT_EQ((*engine)->CreateAudioPlayer(engine, &playerObj, &src, &snk, 0, nullptr, nullptr), (SLresult)SL_RESULT_SUCCESS);
    EXPECT_EQ((*playerObj)->Realize(playerObj, SL_BOOLEAN_FALSE), (SLresult)SL_RESULT_SUCCESS);
    EXPECT_EQ(g_lastFake->opened.numBuffers, 4u);
    EXPECT_EQ(g_lastFake->opened.rateHz, 48000u);
    EXPECT_EQ(g_lastFake->opened.channels, 1u);
    (*playerObj)->Destroy(playerObj);
    (*engineObj)->Destroy(engineObj);
}

TEST(front_end_relays_backend_enqueue_result) {
    Rig r = makeRig();
    EXPECT_EQ((*r.playerObj)->Realize(r.playerObj, SL_BOOLEAN_FALSE), (SLresult)SL_RESULT_SUCCESS);
    SLAndroidSimpleBufferQueueItf bq = nullptr;
    (*r.playerObj)->GetInterface(r.playerObj, SL_IID_BUFFERQUEUE, &bq);
    g_lastFake->enqueueResult = SL_RESULT_BUFFER_INSUFFICIENT;
    int16_t buf[64] = {};
    EXPECT_EQ((*bq)->Enqueue(bq, buf, sizeof buf), (SLresult)SL_RESULT_BUFFER_INSUFFICIENT);
    SLAndroidSimpleBufferQueueState st{};
    (*bq)->GetState(bq, &st);
    EXPECT_EQ(st.count, 0u);
    EXPECT_EQ(g_lastFake->enqueued.size(), (size_t)0);
    destroyRig(r);
}

TEST(front_end_ignores_second_realize) {
    Rig r = makeRig();
    g_backendsCreated = 0;
    EXPECT_EQ((*r.playerObj)->Realize(r.playerObj, SL_BOOLEAN_FALSE), (SLresult)SL_RESULT_SUCCESS);
    EXPECT_EQ((*r.playerObj)->Realize(r.playerObj, SL_BOOLEAN_FALSE), (SLresult)SL_RESULT_SUCCESS);
    EXPECT_EQ(g_backendsCreated, 1);
    destroyRig(r);
    EXPECT_TRUE(FakeBackend::s_closed);
}

TEST(front_end_destroys_unrealized_player) {
    Rig r = makeRig();
    FakeBackend::s_closed = false;
    destroyRig(r);
    EXPECT_TRUE(!FakeBackend::s_closed);
}

TEST(front_end_stray_completion_after_clear_does_not_underflow) {
    Rig r = makeRig();
    (*r.playerObj)->Realize(r.playerObj, SL_BOOLEAN_FALSE);
    SLAndroidSimpleBufferQueueItf bq = nullptr;
    (*r.playerObj)->GetInterface(r.playerObj, SL_IID_BUFFERQUEUE, &bq);
    int16_t buf[64] = {};
    (*bq)->Enqueue(bq, buf, sizeof buf);
    EXPECT_EQ((*bq)->Clear(bq), (SLresult)SL_RESULT_SUCCESS);
    SLAndroidSimpleBufferQueueState before{};
    (*bq)->GetState(bq, &before);
    g_lastFake->completeOne();   // the audio thread finishing a buffer that Clear already discarded
    SLAndroidSimpleBufferQueueState after{};
    (*bq)->GetState(bq, &after);
    EXPECT_EQ(after.count, 0u);
    EXPECT_TRUE(after.index - before.index <= 1);
    destroyRig(r);
}

TEST(front_end_engine_rejects_unknown_interface) {
    Rig r = makeRig();
    void* sentinel = &r;
    void* out = sentinel;
    EXPECT_EQ((*r.engineObj)->GetInterface(r.engineObj, SL_IID_VOLUME, &out), (SLresult)SL_RESULT_FEATURE_UNSUPPORTED);
    EXPECT_TRUE(out == sentinel);
    destroyRig(r);
}
