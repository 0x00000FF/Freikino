#include "volume_osd.h"

#include "freikino/common/error.h"
#include "freikino/render/overlay_renderer.h"

#include <algorithm>
#include <cstdio>

namespace freikino::app {

namespace {

constexpr ULONGLONG kHoldMs       = 800;
constexpr float     kFadeInPerMs  = 1.0f / 90.0f;
constexpr float     kFadeOutPerMs = 1.0f / 260.0f;

constexpr float kPanelW   = 220.0f;
constexpr float kPanelH   = 56.0f;
constexpr float kEdgeGap  = 24.0f;   // distance from window edges
constexpr float kPadX     = 14.0f;
constexpr float kBarH     = 6.0f;
constexpr float kLabelW   = 72.0f;   // reserved for "100%" / "Muted"

} // namespace

void VolumeOsd::create(render::OverlayRenderer& renderer)
{
    auto* ctx = renderer.context();
    if (ctx == nullptr) {
        throw_hresult(E_UNEXPECTED);
    }
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.62f), &brush_bg_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.22f), &brush_track_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.00f), &brush_bar_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.00f), &brush_text_));

    auto* dw = renderer.dwrite();
    if (dw == nullptr) {
        throw_hresult(E_UNEXPECTED);
    }
    check_hr(dw->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        14.0f,
        L"en-us",
        &text_format_));
    text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    last_tick_ms_ = ::GetTickCount64();
}

void VolumeOsd::show(float volume, bool muted) noexcept
{
    volume_      = volume;
    muted_       = muted;
    shown_at_ms_ = ::GetTickCount64();
}

void VolumeOsd::draw(ID2D1DeviceContext* ctx, UINT w, UINT h) noexcept
{
    if (ctx == nullptr || text_format_ == nullptr) {
        return;
    }

    // Always tick so the fade-out still animates after the hold window
    // expires even if nothing's calling show().
    const ULONGLONG now_ms = ::GetTickCount64();
    const ULONGLONG dt = (last_tick_ms_ == 0)
        ? 16
        : (now_ms > last_tick_ms_ ? now_ms - last_tick_ms_ : 0);
    last_tick_ms_ = now_ms;

    const bool holding = shown_at_ms_ != 0
        && now_ms >= shown_at_ms_
        && (now_ms - shown_at_ms_) < kHoldMs;

    if (holding) {
        alpha_ = (std::min)(1.0f, alpha_ + kFadeInPerMs  * static_cast<float>(dt));
    } else {
        alpha_ = (std::max)(0.0f, alpha_ - kFadeOutPerMs * static_cast<float>(dt));
    }
    if (alpha_ <= 0.001f) {
        return;
    }

    const float fw = static_cast<float>(w);
    (void)h;

    D2D1_RECT_F panel{};
    panel.right  = fw - kEdgeGap;
    panel.left   = panel.right - kPanelW;
    panel.top    = kEdgeGap;
    panel.bottom = panel.top + kPanelH;
    if (panel.left < 10.0f) {
        panel.left = 10.0f;
    }

    brush_bg_->SetOpacity(alpha_ * 0.85f);
    ctx->FillRectangle(panel, brush_bg_.Get());

    // Horizontal bar on the left portion, percentage text on the right.
    D2D1_RECT_F bar_area{};
    bar_area.left   = panel.left + kPadX;
    bar_area.right  = panel.right - kLabelW - 6.0f;
    bar_area.top    = panel.top + (kPanelH - kBarH) * 0.5f;
    bar_area.bottom = bar_area.top + kBarH;

    const float bar_w = bar_area.right - bar_area.left;
    if (bar_w > 8.0f) {
        brush_track_->SetOpacity(alpha_);
        ctx->FillRectangle(bar_area, brush_track_.Get());

        // Track fills up to 100% along the whole bar; any volume > 1.0
        // is shown as "over full", clipped at the bar's right edge but
        // reflected in the label.
        float v = muted_ ? 0.0f : volume_;
        if (v < 0.0f) v = 0.0f;
        if (v > 1.0f) v = 1.0f;
        D2D1_RECT_F fill = bar_area;
        fill.right = bar_area.left + bar_w * v;
        brush_bar_->SetOpacity(alpha_);
        ctx->FillRectangle(fill, brush_bar_.Get());
    }

    // Label.
    wchar_t buf[32] = {};
    int n = 0;
    if (muted_) {
        n = std::swprintf(buf, 32, L"Muted");
    } else {
        const int pct = static_cast<int>(volume_ * 100.0f + 0.5f);
        n = std::swprintf(buf, 32, L"%d%%", pct);
    }
    if (n > 0) {
        D2D1_RECT_F label_rect{};
        label_rect.left   = panel.right - kLabelW - kPadX;
        label_rect.right  = panel.right - kPadX;
        label_rect.top    = panel.top;
        label_rect.bottom = panel.bottom;
        brush_text_->SetOpacity(alpha_);
        ctx->DrawTextW(
            buf, static_cast<UINT32>(n),
            text_format_.Get(),
            label_rect,
            brush_text_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            DWRITE_MEASURING_MODE_NATURAL);
    }
}

} // namespace freikino::app
