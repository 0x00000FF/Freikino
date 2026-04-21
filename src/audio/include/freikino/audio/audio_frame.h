#pragma once

#include <cstdint>
#include <vector>

namespace freikino::audio {

// A chunk of decoded PCM audio, always interleaved 32-bit float at the
// renderer's mix sample rate and channel count. Lifetime: owned end-to-end
// by the queue slot it sits in. Moved from decoder thread to audio-pump
// thread via an SPSC queue.
struct AudioFrame {
    // Interleaved float32 samples. `.size() == frame_count * channel_count`.
    std::vector<float> samples;

    uint32_t frame_count   = 0; // sample frames (1 frame = 1 sample per channel)
    uint32_t channel_count = 0;
    uint32_t sample_rate   = 0;

    // Presentation timestamp of the first sample in nanoseconds on the same
    // stream timeline as VideoFrame::pts_ns.
    int64_t  pts_ns        = 0;

    bool valid() const noexcept { return frame_count > 0 && !samples.empty(); }
};

} // namespace freikino::audio
