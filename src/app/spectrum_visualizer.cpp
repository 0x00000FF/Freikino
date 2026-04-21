#include "spectrum_visualizer.h"

#include "freikino/audio/wasapi_renderer.h"
#include "freikino/common/error.h"
#include "freikino/render/overlay_renderer.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <complex>

namespace freikino::app {

namespace {

// In-place iterative radix-2 Cooley-Tukey FFT. `n` must be a power of
// two. Small + self-contained so we don't pull in a dependency just
// for the visualizer.
void fft_radix2(std::complex<float>* x, std::size_t n) noexcept
{
    // Bit-reversal permutation.
    for (std::size_t i = 1, j = 0; i < n; ++i) {
        std::size_t bit = n >> 1;
        for (; j & bit; bit >>= 1) {
            j ^= bit;
        }
        j ^= bit;
        if (i < j) {
            std::swap(x[i], x[j]);
        }
    }
    // Butterflies.
    for (std::size_t len = 2; len <= n; len <<= 1) {
        const float ang = -2.0f * 3.14159265358979f / static_cast<float>(len);
        const std::complex<float> wn(std::cos(ang), std::sin(ang));
        for (std::size_t i = 0; i < n; i += len) {
            std::complex<float> w(1.0f, 0.0f);
            for (std::size_t k = 0; k < len / 2; ++k) {
                const std::complex<float> u = x[i + k];
                const std::complex<float> v = x[i + k + len / 2] * w;
                x[i + k]             = u + v;
                x[i + k + len / 2]   = u - v;
                w *= wn;
            }
        }
    }
}

// Hann window — modest main-lobe widening but a big side-lobe
// suppression; good default for broadband music content.
float hann(std::size_t n, std::size_t N) noexcept
{
    const float t =
        static_cast<float>(n) / static_cast<float>(N - 1);
    const float v = std::sin(3.14159265358979f * t);
    return v * v;
}

constexpr float kFadeDown = 0.82f;   // per-frame decay
constexpr float kFadeUp   = 1.00f;   // (immediate rise)

} // namespace

void SpectrumVisualizer::create(render::OverlayRenderer& renderer)
{
    auto* ctx = renderer.context();
    if (ctx == nullptr) {
        throw_hresult(E_UNEXPECTED);
    }
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.40f, 0.70f, 1.00f, 0.95f), &brush_bar_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.40f, 0.70f, 1.00f, 0.35f), &brush_bar_dim_));
}

void SpectrumVisualizer::draw(
    ID2D1DeviceContext* ctx, UINT w, UINT h) noexcept
{
    if (!active_ || ctx == nullptr || brush_bar_ == nullptr) {
        return;
    }

    // 1. Pull the most recent samples into a local buffer. Torn reads
    //    across the pump/UI boundary just cause a visual blip and are
    //    not a correctness issue (the ring is fixed-size).
    std::array<float, kFftSize> win{};
    std::size_t got = 0;
    if (source_ != nullptr) {
        got = source_->read_tap_snapshot(win.data(), win.size());
    }
    if (got == 0) {
        // No audio yet — decay the bars toward zero so the display
        // eases off rather than freezing.
        for (auto& b : bar_height_) {
            b *= kFadeDown;
        }
    } else {
        // 2. Window + FFT in place.
        std::array<std::complex<float>, kFftSize> buf;
        for (std::size_t i = 0; i < kFftSize; ++i) {
            buf[i] = std::complex<float>(
                win[i] * hann(i, kFftSize), 0.0f);
        }
        fft_radix2(buf.data(), kFftSize);

        // 3. Magnitudes for the first Nyquist-half bins, then bin
        //    them into log-spaced bands. We skip bin 0 (DC).
        std::array<float, kBands> fresh{};
        const std::size_t usable = kFftSize / 2;   // Nyquist
        // Log-spaced band edges from bin 1 to `usable-1`.
        auto edge = [&](std::size_t b) {
            const float t = static_cast<float>(b) / static_cast<float>(kBands);
            const float lo = std::log(1.0f);
            const float hi = std::log(static_cast<float>(usable - 1));
            return std::exp(lo + (hi - lo) * t);
        };
        for (std::size_t b = 0; b < kBands; ++b) {
            const std::size_t lo =
                static_cast<std::size_t>(std::floor(edge(b)));
            const std::size_t hi =
                static_cast<std::size_t>(std::ceil(edge(b + 1)));
            const std::size_t a =
                (std::max)(static_cast<std::size_t>(1), lo);
            const std::size_t z =
                (std::min)(usable - 1, (std::max)(a + 1, hi));
            float peak = 0.0f;
            for (std::size_t i = a; i < z; ++i) {
                const float m = std::abs(buf[i]);
                if (m > peak) peak = m;
            }
            // Normalize: FFT amplitudes scale with kFftSize * window
            // energy. Rough empirical normalization that looks OK
            // across typical music content.
            const float norm = peak / (static_cast<float>(kFftSize) * 0.5f);
            // Convert to dB then map [-60, 0] dB → [0, 1].
            const float db =
                20.0f * std::log10((std::max)(norm, 1e-6f));
            float v = (db + 60.0f) / 60.0f;
            if (v < 0.0f) v = 0.0f;
            if (v > 1.0f) v = 1.0f;
            fresh[b] = v;
        }

        // 4. Fade-up immediate, fade-down slow. Classic envelope
        //    follower feel.
        for (std::size_t b = 0; b < kBands; ++b) {
            if (fresh[b] > bar_height_[b]) {
                bar_height_[b] = bar_height_[b]
                    + (fresh[b] - bar_height_[b]) * kFadeUp;
            } else {
                bar_height_[b] =
                    bar_height_[b] * kFadeDown + fresh[b] * (1.0f - kFadeDown);
            }
        }
    }

    // 5. Draw the bars filling the whole viewport, leaving room at
    //    the top (for audio info card) and bottom (transport bar).
    constexpr float kTopReserve    = 280.0f;  // space for info card
    constexpr float kBottomReserve = 112.0f;  // space for transport
    constexpr float kSidePad       = 32.0f;
    constexpr float kGap           = 3.0f;

    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);

    float area_top    = kTopReserve;
    float area_bottom = fh - kBottomReserve;
    if (area_bottom <= area_top + 40.0f) {
        area_top    = fh * 0.2f;
        area_bottom = fh * 0.8f;
    }
    const float area_h = area_bottom - area_top;
    const float area_l = kSidePad;
    const float area_r = fw - kSidePad;
    if (area_r <= area_l + 40.0f) {
        return;
    }

    const float total_w = area_r - area_l;
    const float bar_w = (total_w - kGap * (kBands - 1)) / kBands;
    if (bar_w < 1.0f) {
        return;
    }

    for (std::size_t b = 0; b < kBands; ++b) {
        const float v = bar_height_[b];
        const float x0 = area_l + static_cast<float>(b) * (bar_w + kGap);
        const float x1 = x0 + bar_w;

        // Dim full-range track for reference (very faint).
        const D2D1_RECT_F dim = D2D1::RectF(
            x0, area_bottom - area_h, x1, area_bottom);
        ctx->FillRectangle(dim, brush_bar_dim_.Get());

        const float bar_h = area_h * v;
        if (bar_h < 1.0f) continue;
        const D2D1_RECT_F bar = D2D1::RectF(
            x0, area_bottom - bar_h, x1, area_bottom);
        ctx->FillRectangle(bar, brush_bar_.Get());
    }
}

} // namespace freikino::app
