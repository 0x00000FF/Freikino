#pragma once

#include "freikino/render/video_frame.h"

namespace freikino::render {

// Minimal abstract producer of VideoFrames, decoupled from how the frames
// are acquired. The render layer only needs a non-blocking poll — anything
// that can satisfy this interface is a valid source (FFmpeg demuxer, Media
// Foundation session, a live capture, a synthetic test pattern).
class VideoFrameSource {
public:
    VideoFrameSource() noexcept = default;
    virtual ~VideoFrameSource() = default;

    VideoFrameSource(const VideoFrameSource&)            = delete;
    VideoFrameSource& operator=(const VideoFrameSource&) = delete;
    VideoFrameSource(VideoFrameSource&&)                 = delete;
    VideoFrameSource& operator=(VideoFrameSource&&)      = delete;

    [[nodiscard]] virtual bool try_acquire_video_frame(VideoFrame& out) = 0;
};

} // namespace freikino::render
