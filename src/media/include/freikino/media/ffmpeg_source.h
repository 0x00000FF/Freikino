#pragma once

#include "freikino/audio/audio_frame.h"
#include "freikino/audio/audio_frame_source.h"
#include "freikino/common/com.h"
#include "freikino/media/frame_queue.h"
#include "freikino/media/source.h"
#include "freikino/render/video_frame.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include <d3d11.h>
#include <mmreg.h>

namespace freikino::media {

// FFmpeg-backed Source. Hardware-decodes video through D3D11VA when the
// codec supports it, sharing the render module's ID3D11Device. Decodes and
// resamples audio to a target PCM format (typically the WASAPI mix format)
// on the same demux thread.
//
// Both video frames and audio frames leave this object through SPSC queues.
// The renderer thread pops video; the WASAPI pump thread pops audio.
class FFmpegSource final
    : public Source
    , public audio::AudioFrameSource
{
public:
    // `share_device` must outlive the source.
    // `target_audio_format` may be null when audio output is not available;
    // audio decoding is then skipped entirely. When non-null, audio is
    // resampled to that format (float32 interleaved assumed).
    explicit FFmpegSource(
        ID3D11Device* share_device,
        const WAVEFORMATEX* target_audio_format = nullptr);
    ~FFmpegSource() override;

    void open(const std::wstring& path) override;
    void start() override;
    void stop() noexcept override;

    // Perform an absolute seek. Precondition: `stop()` has been called and
    // no other threads are popping from the internal queues. The seek jumps
    // to the keyframe at-or-before `target_ns` and flushes the decoders.
    // After returning, `start()` resumes decoding from the new position.
    void seek_while_stopped(int64_t target_ns) noexcept;

    // Drain both internal queues. Precondition: same as `seek_while_stopped`.
    void clear_queues_while_stopped() noexcept;

    [[nodiscard]] bool try_acquire_video_frame(render::VideoFrame& out) override;
    [[nodiscard]] bool try_acquire_audio_frame(audio::AudioFrame& out) override;

    [[nodiscard]] int64_t duration_ns() const noexcept override;
    [[nodiscard]] bool    is_running() const noexcept override
    {
        return running_.load(std::memory_order_acquire);
    }
    [[nodiscard]] bool    end_of_stream() const noexcept override
    {
        return eos_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool has_audio() const noexcept;
    [[nodiscard]] bool has_video() const noexcept;

    // Static descriptors populated at `open()`. Safe to read from any
    // thread after `open()` returns (they're never mutated after that).
    struct VideoInfo {
        int    width         = 0;
        int    height        = 0;
        std::string codec_name;
        std::string hwaccel;       // "d3d11va" or "software"
        double src_fps       = 0.0;
    };
    struct AudioInfo {
        std::string codec_name;
        int    src_sample_rate = 0;
        int    src_channels    = 0;
        // Bitrate in bits/sec. Prefer the audio stream's own bitrate,
        // fall back to the container's. 0 if neither is available.
        int64_t bit_rate_bps   = 0;
    };

    // ID3 / Vorbis comment style metadata. Keys are normalised to
    // lowercase; canonical keys include "title", "artist", "album",
    // "album_artist", "date" / "year", "track", "genre". Extra keys
    // from the container are passed through unchanged.
    struct Metadata {
        std::string title;
        std::string artist;
        std::string album;
        std::string album_artist;
        std::string date;
        std::string track;
        std::string genre;
    };

    // Album art, decoded to tightly-packed BGRA8. `bgra` is empty when
    // the file has no attached picture. Width/height reflect the
    // source dimensions.
    struct AlbumArt {
        std::vector<std::uint8_t> bgra;
        int width  = 0;
        int height = 0;
    };

    [[nodiscard]] VideoInfo video_info() const noexcept;
    [[nodiscard]] AudioInfo audio_info() const noexcept;
    [[nodiscard]] Metadata  metadata()   const noexcept;
    [[nodiscard]] const AlbumArt& album_art() const noexcept;

    // Live metrics for the debug overlay.
    [[nodiscard]] std::size_t   video_queue_depth() const noexcept
    {
        return video_queue_.size();
    }
    [[nodiscard]] std::size_t   audio_queue_depth() const noexcept
    {
        return audio_queue_.size();
    }
    [[nodiscard]] std::size_t   video_queue_capacity() const noexcept
    {
        return video_queue_.capacity();
    }
    [[nodiscard]] std::size_t   audio_queue_capacity() const noexcept
    {
        return audio_queue_.capacity();
    }
    [[nodiscard]] std::uint64_t decoded_video_frames() const noexcept
    {
        return decoded_video_frames_.load(std::memory_order_relaxed);
    }
    [[nodiscard]] std::uint64_t decoded_audio_frames() const noexcept
    {
        return decoded_audio_frames_.load(std::memory_order_relaxed);
    }

private:
    void decode_loop() noexcept;

    struct State;
    std::unique_ptr<State> s_;

    std::atomic<bool> running_{false};
    std::atomic<bool> stop_requested_{false};
    std::atomic<bool> eos_{false};

    std::thread decode_thread_;

    // Cumulative decoded counters. Incremented on each successful push
    // from the decode thread; read-only from other threads via the
    // accessors above.
    std::atomic<std::uint64_t> decoded_video_frames_{0};
    std::atomic<std::uint64_t> decoded_audio_frames_{0};

    // Each stream paces itself via its own queue.
    //   Video: 16 slots (~500 ms at 30 fps). Kept small because each
    //     slot is backed by a VRAM texture — for 4K NV12 that's ~12 MB
    //     per slot. Growing the video queue would require a larger
    //     texture pool and multiply VRAM residency.
    //   Audio: 128 slots (~3 s at ~23 ms/frame). Cheap — each slot is
    //     an AudioFrame with a float vector (~9 KB). The deeper buffer
    //     lets the pump run without underruns across decoder catch-ups
    //     (post-seek keyframe windows on heavy content) and lets the
    //     pump pre-buffer before starting the WASAPI client so the
    //     first post-seek samples aren't preceded by silence.
    SpscQueue<render::VideoFrame, 16>  video_queue_;
    SpscQueue<audio::AudioFrame,  128> audio_queue_;
};

} // namespace freikino::media
