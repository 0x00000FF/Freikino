#pragma once

#include <cstdint>

namespace freikino::render {

// The presentation clock tells the renderer "what stream time is `now`?" in
// nanoseconds on the same timeline as VideoFrame::pts_ns.
//
// Implementations:
//   - `WallClock` — QPC-derived, ticks at wall-clock rate from a chosen zero
//     (used when no audio stream is playing).
//   - `AudioClock` — derived from IAudioClock, ticks at the rate the sound
//     card is actually consuming samples (used when audio is playing; this
//     is the master clock for AV sync).
//
// `now_ns()` must be monotonic non-decreasing and cheap to call — it will
// be invoked once per rendered frame.
class PresentationClock {
public:
    PresentationClock() noexcept = default;
    virtual ~PresentationClock() = default;

    PresentationClock(const PresentationClock&)            = delete;
    PresentationClock& operator=(const PresentationClock&) = delete;
    PresentationClock(PresentationClock&&)                 = delete;
    PresentationClock& operator=(PresentationClock&&)      = delete;

    [[nodiscard]] virtual int64_t now_ns() const noexcept = 0;
};

} // namespace freikino::render
