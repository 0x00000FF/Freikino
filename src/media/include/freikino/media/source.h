#pragma once

#include "freikino/render/video_frame.h"
#include "freikino/render/video_frame_source.h"

#include <string>

namespace freikino::media {

// Abstract contract for a media container that produces decoded video
// frames. Concrete implementations: `FFmpegSource` (covers arbitrary
// container/codec combinations) and, later, a Media Foundation source for
// DRM-protected content.
//
// Inherits render::VideoFrameSource so the render layer can consume a
// Source without knowing that media exists — it only sees the minimal
// VideoFrameSource interface.
class Source : public render::VideoFrameSource {
public:
    Source() noexcept = default;
    ~Source() override = default;

    Source(const Source&)            = delete;
    Source& operator=(const Source&) = delete;
    Source(Source&&)                 = delete;
    Source& operator=(Source&&)      = delete;

    // Open a local file or URL. Throws on failure. Must be called before
    // `start`.
    virtual void open(const std::wstring& path) = 0;

    // Begin demuxing and decoding on an internal worker thread. Idempotent
    // once running.
    virtual void start() = 0;

    // Request a stop and join the worker thread. Safe to call from any
    // thread; safe to call multiple times.
    virtual void stop() noexcept = 0;

    [[nodiscard]] virtual int64_t duration_ns() const noexcept = 0;
    [[nodiscard]] virtual bool    is_running()   const noexcept = 0;
    [[nodiscard]] virtual bool    end_of_stream() const noexcept = 0;
};

} // namespace freikino::media
