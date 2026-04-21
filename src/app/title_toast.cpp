#include "title_toast.h"

#include "freikino/common/error.h"
#include "freikino/render/overlay_renderer.h"

#include <algorithm>
#include <utility>

namespace freikino::app {

namespace {

// Timing. Hold is ~1 s as requested; short fades on either side make
// the pop-on/off feel less jarring without stretching the total.
constexpr ULONGLONG kHoldMs      = 1000;
constexpr float     kFadeInPerMs  = 1.0f / 120.0f;
constexpr float     kFadeOutPerMs = 1.0f / 300.0f;

constexpr float kTopMargin  = 24.0f;
constexpr float kPanelH     = 40.0f;
constexpr float kPanelPadX  = 18.0f;
constexpr float kPanelSideMargin = 40.0f;

} // namespace

void TitleToast::create(render::OverlayRenderer& renderer)
{
    auto* ctx = renderer.context();
    if (ctx == nullptr) {
        throw_hresult(E_UNEXPECTED);
    }

    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f), &brush_bg_));
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
        16.0f,
        L"en-us",
        &text_format_));
    text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    text_format_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);

    DWRITE_TRIMMING trim{};
    trim.granularity    = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    trim.delimiter      = 0;
    trim.delimiterCount = 0;
    text_format_->SetTrimming(&trim, nullptr);

    last_tick_ms_ = ::GetTickCount64();
}

void TitleToast::show(std::wstring text) noexcept
{
    text_        = std::move(text);
    shown_at_ms_ = ::GetTickCount64();
}

void TitleToast::draw(ID2D1DeviceContext* ctx, UINT w, UINT h) noexcept
{
    (void)h;
    if (ctx == nullptr) {
        return;
    }
    // Tick animation even when invisible so the fade-out plays out on
    // the first frame after the hold window expires.
    const ULONGLONG now_ms = ::GetTickCount64();
    const ULONGLONG dt = (last_tick_ms_ == 0)
        ? 16
        : (now_ms > last_tick_ms_ ? now_ms - last_tick_ms_ : 0);
    last_tick_ms_ = now_ms;

    const bool within_hold = shown_at_ms_ != 0
        && now_ms >= shown_at_ms_
        && (now_ms - shown_at_ms_) < kHoldMs;

    if (within_hold) {
        alpha_ = (std::min)(1.0f, alpha_ + kFadeInPerMs  * static_cast<float>(dt));
    } else {
        alpha_ = (std::max)(0.0f, alpha_ - kFadeOutPerMs * static_cast<float>(dt));
    }

    if (alpha_ <= 0.001f || text_.empty() || text_format_ == nullptr) {
        return;
    }

    const float fw = static_cast<float>(w);

    D2D1_RECT_F panel{};
    panel.left   = kPanelSideMargin;
    panel.right  = fw - kPanelSideMargin;
    panel.top    = kTopMargin;
    panel.bottom = kTopMargin + kPanelH;
    if (panel.right <= panel.left + 40.0f) {
        // Window too narrow — give up; a toast here would look worse
        // than silently skipping it.
        return;
    }

    brush_bg_->SetOpacity(alpha_ * 0.85f);
    ctx->FillRectangle(panel, brush_bg_.Get());

    D2D1_RECT_F text_rect = panel;
    text_rect.left  += kPanelPadX;
    text_rect.right -= kPanelPadX;

    brush_text_->SetOpacity(alpha_);
    ctx->DrawTextW(
        text_.c_str(),
        static_cast<UINT32>(text_.size()),
        text_format_.Get(),
        text_rect,
        brush_text_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP,
        DWRITE_MEASURING_MODE_NATURAL);
}

} // namespace freikino::app
