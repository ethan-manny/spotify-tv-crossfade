#pragma once
#include <cstdint>
#include <cstring>
#include <new>

namespace xfade {

// Fixed-capacity ring of 16-bit interleaved frames addressed by absolute (monotonic) frame number.
// The owner keeps its [read, written) window within 'capacity' frames; the ring only maps absolute
// frame numbers onto storage and handles wrap-around. Not thread-safe: the owner locks.
class Ring {
public:
    Ring() = default;
    ~Ring() { delete[] data_; }
    Ring(const Ring&) = delete;
    Ring& operator=(const Ring&) = delete;

    bool init(uint32_t capacityFrames, uint32_t channels) {
        delete[] data_;
        data_ = nullptr;
        capacity_ = 0;
        channels_ = channels;
        if (capacityFrames == 0 || channels == 0) return false;
        uint64_t samples = static_cast<uint64_t>(capacityFrames) * channels;
        if (samples > (1ull << 28)) return false;           // > 512 MB of int16 is never legitimate here
        data_ = new (std::nothrow) int16_t[static_cast<size_t>(samples)];
        if (!data_) return false;
        capacity_ = capacityFrames;
        return true;
    }
    uint32_t capacity() const { return capacity_; }
    uint32_t channels() const { return channels_; }

    // Copies 'frames' frames from src into absolute positions [frame, frame + frames).
    void write(uint64_t frame, const int16_t* src, uint32_t frames) {
        while (frames > 0) {
            uint32_t pos = static_cast<uint32_t>(frame % capacity_);
            uint32_t chunk = capacity_ - pos < frames ? capacity_ - pos : frames;
            std::memcpy(slot(pos), src, bytes(chunk));
            src += static_cast<size_t>(chunk) * channels_;
            frame += chunk;
            frames -= chunk;
        }
    }
    // Copies absolute positions [frame, frame + frames) into dst.
    void read(uint64_t frame, int16_t* dst, uint32_t frames) const {
        while (frames > 0) {
            uint32_t pos = static_cast<uint32_t>(frame % capacity_);
            uint32_t chunk = capacity_ - pos < frames ? capacity_ - pos : frames;
            std::memcpy(dst, slot(pos), bytes(chunk));
            dst += static_cast<size_t>(chunk) * channels_;
            frame += chunk;
            frames -= chunk;
        }
    }

private:
    int16_t* slot(uint32_t pos) const { return data_ + static_cast<size_t>(pos) * channels_; }
    size_t bytes(uint32_t frames) const { return static_cast<size_t>(frames) * channels_ * sizeof(int16_t); }
    int16_t* data_ = nullptr;
    uint32_t capacity_ = 0;
    uint32_t channels_ = 0;
};

}  // namespace xfade
