#include "debug_overlay.h"

#include "freikino/common/error.h"
#include "freikino/render/overlay_renderer.h"
#include "freikino/render/presenter.h"
#include "playback.h"

#if FREIKINO_WITH_MEDIA
#include "freikino/media/ffmpeg_source.h"
#endif

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <utility>

namespace freikino::app {

namespace {

constexpr float kPanelLeft   = 16.0f;
constexpr float kPanelTop    = 16.0f;
constexpr float kPanelWidth  = 400.0f;
constexpr float kPanelHeight = 300.0f;
constexpr float kPadding     = 12.0f;
constexpr float kLineHeight  = 16.0f;

// Frame-time threshold (ms) above which the plot turns red — draws the
// user's attention to real stalls. 20 ms ≈ below 50 fps.
constexpr float kWarnThresholdMs = 20.0f;

std::uint64_t filetime_to_100ns(const FILETIME& ft) noexcept
{
    return (static_cast<std::uint64_t>(ft.dwHighDateTime) << 32)
         | ft.dwLowDateTime;
}

} // namespace

void DebugOverlay::create(render::OverlayRenderer& renderer,
                          IDXGIAdapter* adapter)
{
    ID2D1DeviceContext* ctx = renderer.context();
    if (ctx == nullptr) {
        throw_hresult(E_UNEXPECTED);
    }

    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.62f), &brush_bg_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.00f), &brush_text_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.35f, 0.85f, 0.55f, 1.0f), &brush_plot_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.00f, 0.40f, 0.40f, 1.0f), &brush_plot_warn_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.55f, 0.55f, 0.55f, 0.70f), &brush_target_));

    IDWriteFactory* dw = renderer.dwrite();
    if (dw == nullptr) {
        throw_hresult(E_UNEXPECTED);
    }
    check_hr(dw->CreateTextFormat(
        L"Consolas", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        12.0f, L"en-us", &text_format_));
    text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    check_hr(dw->CreateTextFormat(
        L"Consolas", nullptr,
        DWRITE_FONT_WEIGHT_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        12.0f, L"en-us", &text_format_bold_));
    text_format_bold_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    text_format_bold_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    if (adapter != nullptr) {
        // Optional — if the adapter doesn't implement IDXGIAdapter3
        // (pre-Win10 drivers), VRAM stats stay at zero.
        (void)adapter->QueryInterface(IID_PPV_ARGS(&dxgi_adapter3_));
    }

    ::QueryPerformanceFrequency(&qpc_freq_);

    SYSTEM_INFO si{};
    ::GetSystemInfo(&si);
    cpu_cores_ = static_cast<int>(si.dwNumberOfProcessors);
    if (cpu_cores_ < 1) cpu_cores_ = 1;

    FILETIME create{}, exit_ft{}, kernel{}, user{};
    if (::GetProcessTimes(::GetCurrentProcess(),
                          &create, &exit_ft, &kernel, &user)) {
        cpu_last_proc_100ns_ =
            filetime_to_100ns(kernel) + filetime_to_100ns(user);
    }
    FILETIME sys_idle{}, sys_kernel{}, sys_user{};
    if (::GetSystemTimes(&sys_idle, &sys_kernel, &sys_user)) {
        cpu_last_sys_idle_100ns_ = filetime_to_100ns(sys_idle);
        cpu_last_sys_total_100ns_ =
            filetime_to_100ns(sys_kernel) + filetime_to_100ns(sys_user);
    }

    cpu_last_sample_ms_  = ::GetTickCount64();
    vram_last_sample_ms_ = cpu_last_sample_ms_;
}

void DebugOverlay::record_frame_time(int64_t ns) noexcept
{
    frame_ns_[ring_head_] = ns;
    ring_head_ = (ring_head_ + 1) % kRingSize;
    if (ring_count_ < kRingSize) {
        ++ring_count_;
    }
}

void DebugOverlay::set_media_info(std::wstring video_line,
                                  std::wstring audio_line) noexcept
{
    media_video_line_ = std::move(video_line);
    media_audio_line_ = std::move(audio_line);
}

void DebugOverlay::set_runtime(render::Presenter*  presenter,
                               PlaybackController* playback) noexcept
{
    presenter_ = presenter;
    playback_  = playback;
    displayed_last_count_     = 0;
    displayed_last_sample_ms_ = 0;
}

void DebugOverlay::set_media_source(media::FFmpegSource* source) noexcept
{
    source_ = source;
    decoded_v_last_count_   = 0;
    decoded_a_last_count_   = 0;
    decoded_last_sample_ms_ = 0;
    decoded_v_fps_ = 0.0f;
    decoded_a_fps_ = 0.0f;
}

void DebugOverlay::sample_cpu_if_due() noexcept
{
    const ULONGLONG now = ::GetTickCount64();
    if (now - cpu_last_sample_ms_ < 500) {
        return;
    }
    const std::uint64_t wall_delta_100ns =
        static_cast<std::uint64_t>(now - cpu_last_sample_ms_) * 10000ULL;

    FILETIME create{}, exit_ft{}, kernel{}, user{};
    if (::GetProcessTimes(::GetCurrentProcess(),
                          &create, &exit_ft, &kernel, &user)) {
        const std::uint64_t proc =
            filetime_to_100ns(kernel) + filetime_to_100ns(user);
        if (proc >= cpu_last_proc_100ns_ && wall_delta_100ns > 0) {
            const std::uint64_t delta = proc - cpu_last_proc_100ns_;
            cpu_proc_pct_ = 100.0f
                * static_cast<float>(delta)
                / (static_cast<float>(wall_delta_100ns)
                   * static_cast<float>(cpu_cores_));
        }
        cpu_last_proc_100ns_ = proc;
    }

    FILETIME sys_idle{}, sys_kernel{}, sys_user{};
    if (::GetSystemTimes(&sys_idle, &sys_kernel, &sys_user)) {
        const std::uint64_t sys_total =
            filetime_to_100ns(sys_kernel) + filetime_to_100ns(sys_user);
        const std::uint64_t sys_idle_v = filetime_to_100ns(sys_idle);
        if (sys_total >= cpu_last_sys_total_100ns_
            && sys_idle_v >= cpu_last_sys_idle_100ns_) {
            const std::uint64_t total_delta =
                sys_total - cpu_last_sys_total_100ns_;
            const std::uint64_t idle_delta =
                sys_idle_v - cpu_last_sys_idle_100ns_;
            if (total_delta > 0) {
                const std::uint64_t busy =
                    total_delta > idle_delta ? total_delta - idle_delta : 0;
                cpu_sys_pct_ = 100.0f
                    * static_cast<float>(busy)
                    / static_cast<float>(total_delta);
            }
        }
        cpu_last_sys_total_100ns_ = sys_total;
        cpu_last_sys_idle_100ns_  = sys_idle_v;
    }

    cpu_last_sample_ms_ = now;
}

void DebugOverlay::sample_vram_if_due() noexcept
{
    const ULONGLONG now = ::GetTickCount64();
    if (now - vram_last_sample_ms_ < 1000) {
        return;
    }
    vram_last_sample_ms_ = now;

    if (!dxgi_adapter3_) {
        return;
    }
    DXGI_QUERY_VIDEO_MEMORY_INFO info{};
    if (SUCCEEDED(dxgi_adapter3_->QueryVideoMemoryInfo(
            0, DXGI_MEMORY_SEGMENT_GROUP_LOCAL, &info))) {
        vram_used_bytes_   = info.CurrentUsage;
        vram_budget_bytes_ = info.Budget;
    }
}

void DebugOverlay::draw(ID2D1DeviceContext* ctx, UINT w, UINT h) noexcept
{
    (void)w;
    (void)h;

    // Record frame time.
    LARGE_INTEGER now_qpc{};
    ::QueryPerformanceCounter(&now_qpc);
    if (qpc_last_set_ && qpc_freq_.QuadPart > 0) {
        const int64_t ticks = now_qpc.QuadPart - qpc_last_.QuadPart;
        if (ticks > 0) {
            const int64_t ns =
                (ticks * 1'000'000'000LL) / qpc_freq_.QuadPart;
            record_frame_time(ns);
        }
    }
    qpc_last_     = now_qpc;
    qpc_last_set_ = true;

    sample_cpu_if_due();
    sample_vram_if_due();

    // Sample true video FPS from the presenter's displayed-frame
    // counter. Recomputed every 500 ms for stable digits.
    if (presenter_ != nullptr) {
        const std::uint64_t now_count = presenter_->displayed_frames();
        const ULONGLONG now_ms        = ::GetTickCount64();
        if (displayed_last_sample_ms_ == 0) {
            displayed_last_sample_ms_ = now_ms;
            displayed_last_count_     = now_count;
        } else if (now_ms - displayed_last_sample_ms_ >= 500) {
            const double dt_sec =
                static_cast<double>(now_ms - displayed_last_sample_ms_)
                / 1000.0;
            const std::uint64_t delta =
                now_count >= displayed_last_count_
                    ? now_count - displayed_last_count_ : 0;
            if (dt_sec > 0.0) {
                video_fps_ = static_cast<float>(
                    static_cast<double>(delta) / dt_sec);
            }
            displayed_last_sample_ms_ = now_ms;
            displayed_last_count_     = now_count;
        }
    }

    // Decoder rate — tells us whether the decoder is keeping up with
    // realtime (should roughly match source fps for video, ~43 /s for
    // AAC audio when matching the source rate).
#if FREIKINO_WITH_MEDIA
    if (source_ != nullptr) {
        const std::uint64_t v_count = source_->decoded_video_frames();
        const std::uint64_t a_count = source_->decoded_audio_frames();
        const ULONGLONG now_ms = ::GetTickCount64();
        if (decoded_last_sample_ms_ == 0) {
            decoded_last_sample_ms_ = now_ms;
            decoded_v_last_count_   = v_count;
            decoded_a_last_count_   = a_count;
        } else if (now_ms - decoded_last_sample_ms_ >= 500) {
            const double dt_sec =
                static_cast<double>(now_ms - decoded_last_sample_ms_) / 1000.0;
            if (dt_sec > 0.0) {
                const std::uint64_t v_delta =
                    v_count >= decoded_v_last_count_
                        ? v_count - decoded_v_last_count_ : 0;
                const std::uint64_t a_delta =
                    a_count >= decoded_a_last_count_
                        ? a_count - decoded_a_last_count_ : 0;
                decoded_v_fps_ = static_cast<float>(
                    static_cast<double>(v_delta) / dt_sec);
                decoded_a_fps_ = static_cast<float>(
                    static_cast<double>(a_delta) / dt_sec);
            }
            decoded_last_sample_ms_ = now_ms;
            decoded_v_last_count_   = v_count;
            decoded_a_last_count_   = a_count;
        }
    }
#endif

    if (!visible_ || ring_count_ < 2 || ctx == nullptr) {
        return;
    }

    // Snapshot of the ring in chronological order (oldest → newest).
    std::array<int64_t, kRingSize> chrono{};
    const std::size_t n = ring_count_;
    for (std::size_t i = 0; i < n; ++i) {
        chrono[i] = frame_ns_[
            (kRingSize + ring_head_ - n + i) % kRingSize];
    }

    // Aggregate stats.
    int64_t sum = 0;
    int64_t peak = 0;
    for (std::size_t i = 0; i < n; ++i) {
        sum += chrono[i];
        if (chrono[i] > peak) peak = chrono[i];
    }
    const double avg_ms = (static_cast<double>(sum) / static_cast<double>(n))
                        / 1'000'000.0;
    const double fps = avg_ms > 0.0 ? 1000.0 / avg_ms : 0.0;

    std::array<int64_t, kRingSize> sorted = chrono;
    std::sort(sorted.begin(), sorted.begin() + static_cast<std::ptrdiff_t>(n));
    const std::size_t low_n =
        std::max<std::size_t>(1, n / 100); // worst 1 %
    int64_t low_sum = 0;
    for (std::size_t i = 0; i < low_n; ++i) {
        low_sum += sorted[n - 1 - i];
    }
    const double low_avg_ms =
        (static_cast<double>(low_sum) / static_cast<double>(low_n)) / 1'000'000.0;
    const double low_fps = low_avg_ms > 0.0 ? 1000.0 / low_avg_ms : 0.0;

    // ---- Panel background ----
    const D2D1_RECT_F panel = {
        kPanelLeft, kPanelTop,
        kPanelLeft + kPanelWidth, kPanelTop + kPanelHeight };
    ctx->FillRectangle(panel, brush_bg_.Get());

    // ---- Text lines ----
    wchar_t buf[192];
    float ty = kPanelTop + kPadding;

    auto draw_text = [&](const wchar_t* text, UINT32 len,
                         IDWriteTextFormat* fmt) noexcept {
        if (len == 0) {
            ty += kLineHeight;
            return;
        }
        const D2D1_RECT_F r = {
            kPanelLeft + kPadding, ty,
            kPanelLeft + kPanelWidth - kPadding, ty + kLineHeight };
        ctx->DrawTextW(
            text, len, fmt, r, brush_text_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            DWRITE_MEASURING_MODE_NATURAL);
        ty += kLineHeight;
    };

    auto line = [&](const wchar_t* fmt, auto... args) noexcept {
        const int len = std::swprintf(buf, 192, fmt, args...);
        draw_text(buf, len > 0 ? static_cast<UINT32>(len) : 0,
                  text_format_.Get());
    };
    auto line_bold = [&](const wchar_t* text) noexcept {
        draw_text(text,
                  static_cast<UINT32>(std::wcslen(text)),
                  text_format_bold_.Get());
    };

    // --- Top: media info ---
    if (!media_video_line_.empty()) {
        draw_text(
            media_video_line_.c_str(),
            static_cast<UINT32>(media_video_line_.size()),
            text_format_bold_.Get());
    } else {
        line_bold(L"Video: n/a");
    }
    if (!media_audio_line_.empty()) {
        draw_text(
            media_audio_line_.c_str(),
            static_cast<UINT32>(media_audio_line_.size()),
            text_format_bold_.Get());
    } else {
        line_bold(L"Audio: n/a");
    }

    // --- Live pts + volume ---
    if (playback_ != nullptr) {
        const int64_t cur_ns = playback_->current_time_ns();
        const int64_t dur_ns = playback_->duration_ns();
        const double cur_s = static_cast<double>(cur_ns) / 1'000'000'000.0;
        const double dur_s = static_cast<double>(dur_ns) / 1'000'000'000.0;
        if (playback_->muted()) {
            line(L"t: %.2f / %.2f s   vol: muted", cur_s, dur_s);
        } else {
            const int vol_pct = static_cast<int>(
                playback_->volume() * 100.0f + 0.5f);
            line(L"t: %.2f / %.2f s   vol: %d%%",
                 cur_s, dur_s, vol_pct);
        }
    } else {
        line(L"t: n/a");
    }

    ty += 4.0f;

    // --- Perf ---
    line(L"FPS:      %.1f video", video_fps_);
    line(L"Present:  %.1f Hz (avg %.2f ms)", fps, avg_ms);
    line(L"1%% low:  %.1f fps (%.2f ms)", low_fps, low_avg_ms);
    line(L"CPU:      proc %.1f%%  sys %.1f%%", cpu_proc_pct_, cpu_sys_pct_);
    if (vram_budget_bytes_ > 0) {
        const double used_mb   = static_cast<double>(vram_used_bytes_)
                              / (1024.0 * 1024.0);
        const double budget_mb = static_cast<double>(vram_budget_bytes_)
                              / (1024.0 * 1024.0);
        const double pct = vram_budget_bytes_ > 0
            ? 100.0 * static_cast<double>(vram_used_bytes_)
                   / static_cast<double>(vram_budget_bytes_)
            : 0.0;
        line(L"VRAM:     %.0f / %.0f MB (%.1f%%)",
             used_mb, budget_mb, pct);
    } else {
        line(L"VRAM:     n/a");
    }
    line(L"GPU:      n/a (PDH counters not wired)");

    // --- Source telemetry ---
#if FREIKINO_WITH_MEDIA
    if (source_ != nullptr) {
        const std::size_t v_d = source_->video_queue_depth();
        const std::size_t v_c = source_->video_queue_capacity();
        const std::size_t a_d = source_->audio_queue_depth();
        const std::size_t a_c = source_->audio_queue_capacity();
        line(L"Decode:   %.1f v/s  %.1f a/s", decoded_v_fps_, decoded_a_fps_);
        line(L"Queue:    v %zu/%zu   a %zu/%zu",
             v_d, v_c, a_d, a_c);
    }
#endif

    ty += 4.0f;

    // ---- Plot ----
    const float plot_x = kPanelLeft + kPadding;
    const float plot_y = ty;
    const float plot_w = kPanelWidth  - 2 * kPadding;
    const float plot_h = kPanelTop + kPanelHeight - plot_y - kPadding;
    if (plot_h < 8.0f || plot_w < 8.0f) {
        return;
    }

    // Auto-scale the Y axis to the 99th percentile of recent intervals
    // rather than the absolute max. A single seek-induced spike
    // (~30-80 ms of main-thread block) would otherwise stretch the
    // whole plot vertical and collapse normal Present intervals into
    // a flat line at the bottom. With p99 scaling, spikes clip at the
    // top edge and the steady-state shape stays readable. Floor at
    // 33 ms keeps the 60 Hz reference line meaningful.
    const std::size_t p99_idx =
        static_cast<std::size_t>((static_cast<double>(n) * 0.99)) ;
    const int64_t p99 = sorted[std::min(p99_idx, n - 1)];
    double plot_max_ms = static_cast<double>(p99) / 1'000'000.0;
    if (plot_max_ms < 33.0) plot_max_ms = 33.0;
    (void)peak;

    // Plot background (slightly darker than the panel).
    const D2D1_RECT_F plot_bg = {
        plot_x, plot_y, plot_x + plot_w, plot_y + plot_h };
    ctx->FillRectangle(plot_bg, brush_bg_.Get());

    // 60 Hz target (16.67 ms) reference line.
    const float target_y = plot_y + plot_h
        - plot_h * (16.67f / static_cast<float>(plot_max_ms));
    ctx->DrawLine(
        D2D1::Point2F(plot_x,          target_y),
        D2D1::Point2F(plot_x + plot_w, target_y),
        brush_target_.Get(), 1.0f);

    // Frame-time series.
    if (n >= 2) {
        D2D1_POINT_2F prev{};
        bool have_prev = false;
        const float denom = static_cast<float>(n - 1);
        for (std::size_t i = 0; i < n; ++i) {
            const float ft_ms = static_cast<float>(chrono[i]) / 1'000'000.0f;
            const float x = plot_x + plot_w * (static_cast<float>(i) / denom);
            float y = plot_y + plot_h
                - plot_h * (ft_ms / static_cast<float>(plot_max_ms));
            if (y < plot_y)         y = plot_y;
            if (y > plot_y + plot_h) y = plot_y + plot_h;

            if (have_prev) {
                auto& brush = (ft_ms > kWarnThresholdMs)
                    ? brush_plot_warn_ : brush_plot_;
                ctx->DrawLine(prev, D2D1::Point2F(x, y), brush.Get(), 1.0f);
            }
            prev      = D2D1::Point2F(x, y);
            have_prev = true;
        }
    }
}

} // namespace freikino::app
