#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

namespace freikino::media {

// Lightweight thumbnail decoder. Independent of the playback decoder —
// owns its own AVFormatContext and AVCodecContext so scrubbing the
// thumbnail never perturbs playback.
//
// Always software decode. Thumbnails are small (≤ 240×135) so a SW
// keyframe decode per request is cheap even for 4K source, and the
// thumbnail thread can't compete with the playback HW decoder for a
// shared D3D11 device.
class ThumbnailSource {
public:
    // A delivered thumbnail. Pixels are tightly packed BGRA8.
    struct Frame {
        std::vector<std::uint8_t> pixels;
        int     width  = 0;
        int     height = 0;
        int64_t pts_ns = -1;
    };

    // Declared out-of-line because `State` is a forward-declared
    // incomplete type at this point and `unique_ptr<State>`'s
    // destructor needs the full definition to compile.
    ThumbnailSource() noexcept;
    ~ThumbnailSource();

    ThumbnailSource(const ThumbnailSource&)            = delete;
    ThumbnailSource& operator=(const ThumbnailSource&) = delete;
    ThumbnailSource(ThumbnailSource&&)                 = delete;
    ThumbnailSource& operator=(ThumbnailSource&&)      = delete;

    // Open the file. Returns false if there's no video stream we can
    // decode — in which case thumbnails are simply unavailable; the
    // caller treats this as "no preview" and carries on. Called on the
    // UI thread.
    bool open(const std::wstring& path);

    // Request a thumbnail near `pts_ns`. Asynchronous. If a decode is
    // already in flight for an older target, this call supersedes it;
    // the in-flight result is discarded in favour of the latest target.
    // Safe to call at mouse-move frequency.
    void request(int64_t pts_ns) noexcept;

    // Copy the most recently delivered thumbnail, if any. Returns false
    // if no thumbnail has been produced yet, or if the file couldn't be
    // opened. Cheap — mutex-locked copy of a small-ish buffer.
    bool peek_latest(Frame& out) const;

    // The pts of the most recent delivered thumbnail (for cache keying
    // on the draw side). Returns -1 if none.
    [[nodiscard]] int64_t latest_pts() const noexcept;

    // Join the worker thread and release AV state. Idempotent.
    void stop() noexcept;

    [[nodiscard]] bool is_open() const noexcept { return opened_; }
    [[nodiscard]] int  thumb_width()  const noexcept { return thumb_w_; }
    [[nodiscard]] int  thumb_height() const noexcept { return thumb_h_; }

private:
    void thread_main() noexcept;
    bool decode_one(int64_t target_ns, Frame& out) noexcept;

    struct State;
    std::unique_ptr<State> s_;

    int  thumb_w_ = 0;
    int  thumb_h_ = 0;
    bool opened_  = false;

    mutable std::mutex              mutex_;
    std::condition_variable         cv_;
    std::atomic<int64_t>            requested_pts_{INT64_MIN};
    std::atomic<bool>               stop_{false};
    Frame                           latest_;   // guarded by mutex_

    std::thread                     thread_;
};

} // namespace freikino::media
