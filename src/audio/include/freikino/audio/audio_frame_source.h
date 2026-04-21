#pragma once

#include "freikino/audio/audio_frame.h"

namespace freikino::audio {

// Minimal producer abstraction over decoded PCM audio. The WASAPI renderer
// only needs a non-blocking poll.
class AudioFrameSource {
public:
    AudioFrameSource() noexcept = default;
    virtual ~AudioFrameSource() = default;

    AudioFrameSource(const AudioFrameSource&)            = delete;
    AudioFrameSource& operator=(const AudioFrameSource&) = delete;
    AudioFrameSource(AudioFrameSource&&)                 = delete;
    AudioFrameSource& operator=(AudioFrameSource&&)      = delete;

    [[nodiscard]] virtual bool try_acquire_audio_frame(AudioFrame& out) = 0;
};

} // namespace freikino::audio
