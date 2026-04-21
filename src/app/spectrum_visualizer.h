#pragma once

#include "freikino/common/com.h"

#include <array>
#include <cstdint>

#include <d2d1.h>
#include <d2d1_1.h>
#include <windows.h>

namespace freikino::audio  { class WasapiRenderer; }
namespace freikino::render { class OverlayRenderer; }

namespace freikino::app {

// Draws a frequency-domain bar graph of the currently-playing audio.
// Runs a 512-point radix-2 FFT per draw on the most recent mono-mixed
// samples that WasapiRenderer captures via its tap ring. Visible only
// when `set_active(true)` — enabled for audio-only files so it
// doesn't obscure the video.
class SpectrumVisualizer {
public:
    SpectrumVisualizer() noexcept = default;
    ~SpectrumVisualizer() = default;

    SpectrumVisualizer(const SpectrumVisualizer&)            = delete;
    SpectrumVisualizer& operator=(const SpectrumVisualizer&) = delete;
    SpectrumVisualizer(SpectrumVisualizer&&)                 = delete;
    SpectrumVisualizer& operator=(SpectrumVisualizer&&)      = delete;

    void create(render::OverlayRenderer& renderer);

    void set_source(audio::WasapiRenderer* src) noexcept { source_ = src; }
    void set_active(bool a) noexcept { active_ = a; }

    [[nodiscard]] bool active() const noexcept { return active_; }

    void draw(ID2D1DeviceContext* ctx, UINT width, UINT height) noexcept;

private:
    // FFT-related sizes. kFftSize must be a power of two; kBands is
    // the number of visible bars (grouped into log-spaced bins).
    static constexpr std::size_t kFftSize = 512;
    static constexpr std::size_t kBands   = 48;

    audio::WasapiRenderer* source_ = nullptr;
    bool                   active_ = false;

    // Smoothed bar heights for the envelope-follower effect —
    // without smoothing the spectrum flickers on consecutive fills.
    std::array<float, kBands> bar_height_{};

    ComPtr<ID2D1SolidColorBrush> brush_bar_;
    ComPtr<ID2D1SolidColorBrush> brush_bar_dim_;
};

} // namespace freikino::app
