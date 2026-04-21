#include "opening_overlay.h"

#include "freikino/common/error.h"
#include "freikino/render/overlay_renderer.h"

#include <algorithm>
#include <utility>

namespace freikino::app {

namespace {

constexpr float kCardW       = 460.0f;
constexpr float kCardH       = 120.0f;
constexpr float kCardPadX    = 24.0f;
constexpr float kTitleBottom = 52.0f;   // y of title's bottom edge
constexpr float kSubTop      = 56.0f;   // y of subtitle's top edge
constexpr float kBarH        = 6.0f;
constexpr float kBarPadY     = 18.0f;   // distance from card bottom
constexpr float kBarStripeW  = 90.0f;

} // namespace

void OpeningOverlay::create(render::OverlayRenderer& renderer)
{
    auto* ctx = renderer.context();
    if (ctx == nullptr) {
        throw_hresult(E_UNEXPECTED);
    }

    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.55f), &brush_bg_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.08f, 0.08f, 0.10f, 0.94f), &brush_card_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.00f), &brush_text_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.18f), &brush_track_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.35f, 0.70f, 1.0f, 1.00f), &brush_bar_));

    auto* dw = renderer.dwrite();
    if (dw == nullptr) {
        throw_hresult(E_UNEXPECTED);
    }

    check_hr(dw->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        18.0f,
        L"en-us",
        &text_title_));
    text_title_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    text_title_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    check_hr(dw->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        13.0f,
        L"en-us",
        &text_sub_));
    text_sub_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    text_sub_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    text_sub_->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    DWRITE_TRIMMING trim{};
    trim.granularity    = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
    trim.delimiter      = 0;
    trim.delimiterCount = 0;
    text_sub_->SetTrimming(&trim, nullptr);
}

void OpeningOverlay::show(std::wstring name) noexcept
{
    name_    = std::move(name);
    visible_ = true;
}

void OpeningOverlay::hide() noexcept
{
    visible_ = false;
    name_.clear();
}

void OpeningOverlay::draw(ID2D1DeviceContext* ctx, UINT w, UINT h) noexcept
{
    if (!visible_ || ctx == nullptr || text_title_ == nullptr) {
        return;
    }

    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);

    // Full-screen dimmer so the user can't accidentally interact with
    // obscured controls while a load is in flight.
    const D2D1_RECT_F full{ 0.0f, 0.0f, fw, fh };
    ctx->FillRectangle(full, brush_bg_.Get());

    // Centered card.
    D2D1_RECT_F card{};
    card.left   = (fw - kCardW) * 0.5f;
    card.right  = card.left + kCardW;
    card.top    = (fh - kCardH) * 0.5f;
    card.bottom = card.top + kCardH;
    if (card.right > fw - 10.0f) {
        card.left  = 10.0f;
        card.right = fw - 10.0f;
    }
    ctx->FillRectangle(card, brush_card_.Get());

    // "Opening…" title.
    {
        D2D1_RECT_F title_rect = card;
        title_rect.bottom = card.top + kTitleBottom;
        const wchar_t* title = L"Opening\x2026";
        ctx->DrawTextW(
            title,
            static_cast<UINT32>(std::wcslen(title)),
            text_title_.Get(),
            title_rect,
            brush_text_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            DWRITE_MEASURING_MODE_NATURAL);
    }

    // Filename subtitle — trimmed if it overflows the card.
    if (!name_.empty()) {
        D2D1_RECT_F sub_rect = card;
        sub_rect.top    = card.top + kSubTop;
        sub_rect.bottom = card.bottom - kBarPadY - kBarH - 8.0f;
        sub_rect.left  += kCardPadX;
        sub_rect.right -= kCardPadX;
        ctx->DrawTextW(
            name_.c_str(),
            static_cast<UINT32>(name_.size()),
            text_sub_.Get(),
            sub_rect,
            brush_text_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            DWRITE_MEASURING_MODE_NATURAL);
    }

    // Indeterminate progress bar. Track across the card's inner width,
    // with a stripe that sweeps left→right, wraps, and repeats.
    D2D1_RECT_F track{};
    track.left   = card.left  + kCardPadX;
    track.right  = card.right - kCardPadX;
    track.bottom = card.bottom - kBarPadY;
    track.top    = track.bottom - kBarH;
    ctx->FillRectangle(track, brush_track_.Get());

    const float track_w = track.right - track.left;
    if (track_w > 0.0f) {
        // 1.4s sweep cycle. No easing — keeps the animation predictable
        // across DWM compositor pacing.
        const float t =
            static_cast<float>(::GetTickCount64() % 1400ULL) / 1400.0f;
        float stripe_w = kBarStripeW;
        if (stripe_w > track_w * 0.6f) {
            stripe_w = track_w * 0.6f;
        }
        const float travel  = track_w + stripe_w;
        const float start_x = track.left - stripe_w + travel * t;

        D2D1_RECT_F stripe{};
        stripe.left   = (std::max)(track.left,  start_x);
        stripe.right  = (std::min)(track.right, start_x + stripe_w);
        stripe.top    = track.top;
        stripe.bottom = track.bottom;
        if (stripe.right > stripe.left) {
            ctx->FillRectangle(stripe, brush_bar_.Get());
        }
    }
}

} // namespace freikino::app
