#include "dump.h"
#include <sys/system_properties.h>
#include <cstring>
#include "log.h"

namespace xfade {

PcmDumper::~PcmDumper() { if (file_) std::fclose(file_); }

void PcmDumper::configure(const std::string& dir, uint32_t rateHz, uint32_t channels) {
    char value[PROP_VALUE_MAX] = {0};
    bool on = __system_property_get("debug.xfade.dump", value) > 0 && std::strcmp(value, "1") == 0;
    setup(dir, rateHz, channels, on && !dir.empty());
}

void PcmDumper::configureForTests(const std::string& dir, uint32_t rateHz, uint32_t channels, bool enabled) {
    setup(dir, rateHz, channels, enabled);
}

void PcmDumper::setup(const std::string& dir, uint32_t rateHz, uint32_t channels, bool enabled) {
    if (enabled && (rateHz == 0 || channels == 0)) {
        LOGW("dump: refusing to enable with rate=%u channels=%u", rateHz, channels);
        enabled = false;
    }
    enabled_ = enabled;
    dir_ = dir;
    channels_ = channels;
    historyFrames_ = rateHz * kHistorySeconds;
    tailFrames_ = rateHz * kTailSeconds;
    pushed_ = 0;
    if (enabled_) {
        history_.assign(static_cast<size_t>(historyFrames_) * channels, 0);
        LOGI("dump: enabled, dir=%s", dir.c_str());
    }
}

void PcmDumper::startFile() {
    std::string path = dir_ + "/xfade_dump_" + std::to_string(++count_) + ".pcm";
    file_ = std::fopen(path.c_str(), "wb");
    if (!file_) { LOGE("dump: cannot open %s", path.c_str()); return; }
    // History: the last historyFrames_ frames in chronological order (zeros if fewer were pushed).
    uint64_t start = pushed_ > historyFrames_ ? pushed_ - historyFrames_ : 0;
    for (uint64_t f = start; f < pushed_; ++f)
        std::fwrite(&history_[static_cast<size_t>(f % historyFrames_) * channels_], 2, channels_, file_);
    remaining_ = tailFrames_;
    LOGI("dump: writing %s", path.c_str());
}

void PcmDumper::push(const int16_t* frames, uint32_t n) {
    if (!enabled_) return;
    // Check the trigger before folding this call's frames into history, so a trigger raised between
    // two pushes freezes history at the previous push and the tail starts exactly with this one.
    if (triggered_.exchange(false)) {
        if (file_) { std::fclose(file_); file_ = nullptr; }
        startFile();
    }
    for (uint32_t i = 0; i < n; ++i) {   // history ring
        std::memcpy(&history_[static_cast<size_t>(pushed_ % historyFrames_) * channels_], frames + static_cast<size_t>(i) * channels_, channels_ * 2);
        ++pushed_;
    }
    if (file_ && remaining_ > 0) {
        uint32_t w = static_cast<uint32_t>(remaining_ < n ? remaining_ : n);
        std::fwrite(frames, 2, static_cast<size_t>(w) * channels_, file_);
        remaining_ -= w;
        if (remaining_ == 0) { std::fclose(file_); file_ = nullptr; LOGI("dump: file complete"); }
    }
}

}  // namespace xfade
