#include <jni.h>
#include <mutex>
#include <string>
#include "log.h"
#include "ring_pipeline.h"
#include "status.h"
#include "track_meta.h"

namespace {
std::string utf(JNIEnv* env, jstring s) {
    if (!s) return "";
    const char* c = env->GetStringUTFChars(s, nullptr);
    if (!c) return "";
    std::string out(c);
    env->ReleaseStringUTFChars(s, c);
    return out;
}
}  // namespace

extern "C" {

JNIEXPORT jstring JNICALL
Java_dev_spotifytv_crossfade_extension_CrossfadeNative_getStatus(JNIEnv* env, jclass) {
    return env->NewStringUTF(xfade::statusLine().c_str());
}

JNIEXPORT void JNICALL
Java_dev_spotifytv_crossfade_extension_CrossfadeNative_setCrossfadeMs(JNIEnv*, jclass, jint ms) {
    xfade::setCrossfadeMs(ms);                       // the value a future player starts from
    std::lock_guard<std::mutex> lock(xfade::RingPipeline::registryMutex());
    if (auto* p = xfade::RingPipeline::current()) p->setCrossfadeMs(xfade::crossfadeMs());
    LOGI("setCrossfadeMs(%d)", xfade::crossfadeMs());
}

JNIEXPORT void JNICALL
Java_dev_spotifytv_crossfade_extension_CrossfadeNative_onMetadata(JNIEnv* env, jclass, jstring playbackId, jstring trackUri,
                                                                 jlong durationMs, jlong positionMs, jboolean isAd, jboolean isVideo) {
    xfade::TrackMeta m;
    m.playbackId = utf(env, playbackId);
    m.trackUri = utf(env, trackUri);
    m.durationMs = durationMs;
    m.positionMs = positionMs;
    m.isAd = isAd == JNI_TRUE;
    m.isVideo = isVideo == JNI_TRUE;
    LOGI("metadata playback_id=%s uri=%s duration=%lld position=%lld ad=%d video=%d", m.playbackId.c_str(), m.trackUri.c_str(),
         (long long)durationMs, (long long)positionMs, m.isAd, m.isVideo);
    if (m.playbackId.empty()) return;
    std::lock_guard<std::mutex> lock(xfade::RingPipeline::registryMutex());
    if (auto* p = xfade::RingPipeline::current()) p->onMetadata(m);
}

JNIEXPORT void JNICALL
Java_dev_spotifytv_crossfade_extension_CrossfadeNative_setDumpDir(JNIEnv* env, jclass, jstring dir) {
    xfade::RingPipeline::setDumpDir(utf(env, dir));
}

}  // extern "C"
