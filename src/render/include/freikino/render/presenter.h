#pragma once

#include "freikino/render/device.h"
#include "freikino/render/presentation_clock.h"
#include "freikino/render/swap_chain.h"
#include "freikino/render/video_frame.h"
#include "freikino/render/video_frame_source.h"
#include "freikino/render/video_pipeline.h"

#include <atomic>
#include <cstdint>
#include <functional>

#include <windows.h>

namespace freikino::render {

// Owns the device + swapchain for a single HWND and drives per-frame
// rendering.
//
// Frame selection is PTS-based when a PresentationClock is attached:
//   * drain the source, holding at most one future frame in `pending_`
//   * promote any frame whose pts is at or before `clock.now_ns() + early_window`
//   * stop promoting once the held frame's pts is in the future
// With no clock attached, the older "show the latest" fallback applies.
class Presenter {
public:
    Presenter() noexcept = default;
    ~Presenter() = default;

    Presenter(const Presenter&)            = delete;
    Presenter& operator=(const Presenter&) = delete;
    Presenter(Presenter&&)                 = delete;
    Presenter& operator=(Presenter&&)      = delete;

    void create(HWND hwnd, UINT width, UINT height, bool enable_d3d_debug);
    void resize(UINT width, UINT height);

    void render();

    ID3D11Device*    d3d()        const noexcept { return device_.d3d(); }
    IDXGIAdapter1*   adapter()    const noexcept { return device_.adapter(); }
    IDXGISwapChain1* swap_chain() const noexcept { return swap_.dxgi_swap_chain(); }

    void set_frame_source(VideoFrameSource* src) noexcept { source_ = src; }
    void set_clock(PresentationClock* clock) noexcept { clock_ = clock; }

    // Drop only the held lookahead frame (`pending_`). The currently
    // displayed frame stays on screen, so a seek replaces the picture
    // with the new frame as soon as it arrives rather than briefly
    // flashing black. The old pending's pts is from the pre-seek stream
    // position and would otherwise block the new stream from being
    // promoted, hence dropping it.
    void drop_lookahead() noexcept;

    // Drop both the held lookahead and the currently-displayed frame.
    // Use on full teardown (source change, shutdown) — not on seek,
    // where `drop_lookahead` alone avoids the black-flash gap.
    void flush_video_state() noexcept;

    // Post-video overlay hook. Invoked every frame after the video draw and
    // before Present. Called on the render thread. Exceptions are caught
    // and logged; they do not abort the frame.
    using OverlayCallback = std::function<void(UINT width, UINT height)>;
    void set_overlay_callback(OverlayCallback cb) noexcept
    {
        overlay_cb_ = std::move(cb);
    }

    HANDLE wait_object() const noexcept { return swap_.waitable(); }
    bool   ready()       const noexcept { return swap_.rtv() != nullptr; }

    // Cumulative number of times a fresh video frame has been promoted
    // onto the pipeline (i.e. real visible frames on screen). Sampled
    // by the debug overlay to compute the playback frame rate — which
    // is what users mean by "FPS", distinct from the vsync-paced
    // Present rate. Only written on the render thread; reads are
    // monotonic so a relaxed load is fine.
    [[nodiscard]] std::uint64_t displayed_frames() const noexcept
    {
        return displayed_frames_.load(std::memory_order_relaxed);
    }

private:
    Device             device_;
    SwapChain          swap_;
    VideoPipeline      video_pipeline_;

    VideoFrameSource*  source_ = nullptr;
    PresentationClock* clock_  = nullptr;
    OverlayCallback    overlay_cb_;

    // One-slot lookahead. Holds a frame we've popped from the source whose
    // presentation time is still in the future.
    VideoFrame         pending_{};

    // Heartbeat counters. render() runs only on the UI thread, so plain
    // integers (no atomics) are safe.
    std::uint64_t      hb_calls_     = 0;
    std::uint64_t      hb_displayed_ = 0;
    std::uint64_t      hb_held_      = 0;
    ULONGLONG          hb_last_tick_ = 0;

    // Cumulative video frames promoted. Written on the render thread
    // only; atomic so external samplers (debug overlay) get a
    // consistent read without locking.
    std::atomic<std::uint64_t> displayed_frames_{0};
};

} // namespace freikino::render
