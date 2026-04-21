#pragma once

#include "freikino/common/com.h"

#include <array>
#include <cstdint>

#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi1_4.h>
#include <windows.h>

#include <string>

namespace freikino::render {
    class OverlayRenderer;
    class Presenter;
}
namespace freikino::media { class FFmpegSource; }

namespace freikino::app {

class PlaybackController;

// In-window diagnostic overlay. Top-left panel with frame-time stats
// (FPS, avg, 1%-low), CPU utilisation (process + system), VRAM usage,
// and a live plot of the last ~4 s of frame intervals. Toggle via
// `F3` from the main window.
//
// Drawn from the presenter's overlay callback on the UI thread. Each
// `draw()` call measures the time since the previous call and records
// it — that interval is the inter-Present gap, i.e. the real frame
// time.
class DebugOverlay {
public:
    DebugOverlay() noexcept = default;
    ~DebugOverlay() = default;

    DebugOverlay(const DebugOverlay&)            = delete;
    DebugOverlay& operator=(const DebugOverlay&) = delete;
    DebugOverlay(DebugOverlay&&)                 = delete;
    DebugOverlay& operator=(DebugOverlay&&)      = delete;

    // Allocate D2D resources. `adapter` is optional; when set, VRAM
    // stats are populated from `IDXGIAdapter3::QueryVideoMemoryInfo`.
    void create(render::OverlayRenderer& renderer, IDXGIAdapter* adapter);

    // Static stream descriptors — populated by the app layer once a
    // file is opened. Each line is rendered verbatim; pre-format with
    // the exact text you want shown.
    void set_media_info(std::wstring video_line,
                        std::wstring audio_line) noexcept;

    // Live-data sources for real FPS / pts / volume readouts.
    void set_runtime(render::Presenter*    presenter,
                     PlaybackController*   playback) noexcept;

    // Optional — the media source exposes queue depths and a decoded-
    // frame counter for deeper diagnostics. Null when media is
    // disabled at build time or no file is open.
    void set_media_source(media::FFmpegSource* source) noexcept;

    // Called from the overlay callback once per frame. Records the
    // inter-call interval as a frame time and — if visible — paints
    // the panel.
    void draw(ID2D1DeviceContext* ctx, UINT width, UINT height) noexcept;

    void set_visible(bool v)    noexcept { visible_ = v; }
    void toggle_visible()       noexcept { visible_ = !visible_; }
    [[nodiscard]] bool visible() const noexcept { return visible_; }

private:
    void record_frame_time(int64_t ns) noexcept;
    void sample_cpu_if_due() noexcept;
    void sample_vram_if_due() noexcept;

    bool visible_ = false;

    // Frame-time ring (chronological order).
    static constexpr std::size_t kRingSize = 240;
    std::array<int64_t, kRingSize> frame_ns_{};
    std::size_t ring_head_  = 0;
    std::size_t ring_count_ = 0;

    LARGE_INTEGER qpc_freq_{};
    LARGE_INTEGER qpc_last_{};
    bool          qpc_last_set_ = false;

    // CPU sampling — updated at most every 500 ms so the numbers read
    // cleanly and the sampling cost is negligible.
    ULONGLONG cpu_last_sample_ms_     = 0;
    std::uint64_t cpu_last_proc_100ns_ = 0;
    std::uint64_t cpu_last_sys_total_100ns_ = 0;
    std::uint64_t cpu_last_sys_idle_100ns_  = 0;
    float     cpu_proc_pct_ = 0.0f;
    float     cpu_sys_pct_  = 0.0f;
    int       cpu_cores_    = 1;

    // VRAM sampling — DXGI query is cheap but we still rate-limit.
    ComPtr<IDXGIAdapter3> dxgi_adapter3_;
    ULONGLONG     vram_last_sample_ms_ = 0;
    std::uint64_t vram_used_bytes_   = 0;
    std::uint64_t vram_budget_bytes_ = 0;

    // Media info (pre-formatted lines).
    std::wstring media_video_line_;
    std::wstring media_audio_line_;

    // Runtime sources.
    render::Presenter*  presenter_ = nullptr;
    PlaybackController* playback_  = nullptr;

    // Video FPS tracking (sampled from presenter's displayed counter).
    std::uint64_t displayed_last_count_     = 0;
    ULONGLONG     displayed_last_sample_ms_ = 0;
    float         video_fps_                = 0.0f;

    // Decoder push rate — sampled from the source's cumulative counter.
    media::FFmpegSource* source_ = nullptr;
    std::uint64_t  decoded_v_last_count_     = 0;
    std::uint64_t  decoded_a_last_count_     = 0;
    ULONGLONG      decoded_last_sample_ms_   = 0;
    float          decoded_v_fps_            = 0.0f;
    float          decoded_a_fps_            = 0.0f;

    // D2D resources.
    ComPtr<ID2D1SolidColorBrush> brush_bg_;
    ComPtr<ID2D1SolidColorBrush> brush_text_;
    ComPtr<ID2D1SolidColorBrush> brush_plot_;
    ComPtr<ID2D1SolidColorBrush> brush_plot_warn_;
    ComPtr<ID2D1SolidColorBrush> brush_target_;
    ComPtr<IDWriteTextFormat>    text_format_;
    ComPtr<IDWriteTextFormat>    text_format_bold_;
};

} // namespace freikino::app
