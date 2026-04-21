#pragma once

#include "freikino/media/ffmpeg_source.h"
#include "freikino/media/thumbnail_source.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <thread>

#include <d3d11.h>
#include <windows.h>

namespace freikino::render { class Presenter; class WallClock; }
namespace freikino::audio  { class WasapiRenderer; }

namespace freikino::app {

class MainWindow;
class PlaybackController;

// Completion artifact posted from the background open worker back to
// the UI thread via WM_APP_OPEN_COMPLETE. Defined here rather than in
// the cpp because MainWindow's message handler instantiates
// `unique_ptr<PendingOpen>` which needs the complete type.
struct PendingOpen {
    std::uint64_t generation = 0;
    std::wstring  path;
    std::unique_ptr<media::FFmpegSource>    source;
    std::unique_ptr<media::ThumbnailSource> thumbnail;
    bool          success = false;
    std::string   error_message;
};

// Owner of the currently-playing file's decoder + thumbnail. On `open`,
// cleanly tears down the previous session (if any) and starts a fresh
// one — wiring the new source into the presenter, audio renderer,
// playback controller, window, and debug overlay in one place so that
// callers (drag-drop, playlist, initial open) don't repeat the 30-line
// setup sequence.
//
// Opening is asynchronous. `open()` returns immediately after spawning
// a worker thread that calls the blocking FFmpeg probe + stream-info
// lookups. The UI thread would otherwise freeze on OneDrive / network
// paths while waiting on ~seconds of I/O. When the worker finishes it
// posts a WM_APP_OPEN_COMPLETE message to the window; the UI thread's
// handler calls `complete_open()` to splice the new source into the
// live playback stack.
class MediaSession {
public:
    MediaSession(
        render::Presenter*      presenter,
        render::WallClock*      wall_clock,
        audio::WasapiRenderer*  audio,
        bool                    audio_ready,
        PlaybackController*     playback,
        MainWindow*             window);
    ~MediaSession();

    MediaSession(const MediaSession&)            = delete;
    MediaSession& operator=(const MediaSession&) = delete;
    MediaSession(MediaSession&&)                 = delete;
    MediaSession& operator=(MediaSession&&)      = delete;

    // Window that receives the async completion message. Must be set
    // before the first `open()`.
    void set_notify_window(HWND hwnd) noexcept { notify_hwnd_ = hwnd; }

    // Kick off an async open. Returns true if the request was accepted
    // (always true unless already-closed conditions prevent startup).
    // Actual success/failure is signalled later via WM_APP_OPEN_COMPLETE
    // → `complete_open()`.
    bool open(const std::wstring& path);

    // Called by MainWindow's WM_APP_OPEN_COMPLETE handler. Takes
    // ownership of the worker-built artifact, wires it into the
    // presenter/audio/playback/window, and returns true on success. A
    // stale completion (superseded by a later open()) is silently
    // dropped here.
    bool complete_open(std::unique_ptr<PendingOpen> p) noexcept;

    // Tear down the current source without opening a new one. Safe to
    // call when no source is open. Also waits for any in-flight worker
    // to finish (results are discarded).
    void close();

    [[nodiscard]] bool is_opening() const noexcept
    {
        return opening_.load(std::memory_order_acquire);
    }

    [[nodiscard]] bool has_source() const noexcept
    {
        return static_cast<bool>(source_);
    }

    [[nodiscard]] bool end_of_stream() const noexcept;

    // True when everything decoded has actually been PLAYED, not
    // just when the decoder finished. Different from end_of_stream
    // in that it also waits for the audio + video queues to drain
    // and the playback clock to catch up to the file's duration —
    // otherwise auto-advance cuts the last few seconds of audio
    // (queue + WASAPI device buffer still pending).
    [[nodiscard]] bool playback_finished() const noexcept;

    [[nodiscard]] media::FFmpegSource* source() const noexcept
    {
        return source_.get();
    }

    [[nodiscard]] const std::wstring& current_path() const noexcept
    {
        return current_path_;
    }

private:
    render::Presenter*     presenter_    = nullptr;
    render::WallClock*     wall_clock_   = nullptr;
    audio::WasapiRenderer* audio_        = nullptr;
    bool                   audio_ready_  = false;
    PlaybackController*    playback_     = nullptr;
    MainWindow*            window_       = nullptr;

    std::unique_ptr<media::FFmpegSource>    source_;
    std::unique_ptr<media::ThumbnailSource> thumbnail_;
    std::wstring                            current_path_;

    // Async open plumbing. `generation_` advances on every open(); the
    // worker stamps its PendingOpen with the current generation so a
    // superseded open can be detected and discarded. `worker_` is
    // detached/joined in close() — we always finish the in-flight
    // worker, we just may ignore its result.
    HWND                    notify_hwnd_ = nullptr;
    std::atomic<bool>       opening_{false};
    std::atomic<std::uint64_t> generation_{0};
    std::thread             worker_;
};

} // namespace freikino::app
