#include "subtitle_setup_overlay.h"

#include "freikino/common/error.h"
#include "freikino/common/strings.h"
#include "freikino/render/overlay_renderer.h"
#include "subtitle_overlay.h"

#include <algorithm>
#include <cstdio>

namespace freikino::app {

namespace {

constexpr float kPanelW   = 460.0f;
constexpr float kPanelH   = 300.0f;
constexpr float kPadX     = 22.0f;
constexpr float kPadY     = 18.0f;
constexpr float kRowH     = 34.0f;

} // namespace

void SubtitleSetupOverlay::create(render::OverlayRenderer& renderer)
{
    auto* ctx = renderer.context();
    if (ctx == nullptr) {
        throw_hresult(E_UNEXPECTED);
    }
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.06f, 0.06f, 0.08f, 0.88f), &brush_bg_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.26f, 0.55f, 0.95f, 1.0f), &brush_accent_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &brush_text_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.55f), &brush_muted_));

    auto* dw = renderer.dwrite();
    if (dw == nullptr) {
        throw_hresult(E_UNEXPECTED);
    }
    check_hr(dw->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        16.0f, L"en-us", &text_title_));
    text_title_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    text_title_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);

    check_hr(dw->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        14.0f, L"en-us", &text_label_));
    text_label_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    text_label_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    check_hr(dw->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        14.0f, L"en-us", &text_value_));
    text_value_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    text_value_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    check_hr(dw->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        12.0f, L"en-us", &text_hint_));
    text_hint_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    text_hint_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}

void SubtitleSetupOverlay::draw(
    ID2D1DeviceContext* ctx, UINT w, UINT h) noexcept
{
    if (!visible_ || ctx == nullptr || text_title_ == nullptr) {
        return;
    }

    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);

    D2D1_RECT_F panel{};
    panel.left   = (fw - kPanelW) * 0.5f;
    panel.top    = (fh - kPanelH) * 0.5f;
    panel.right  = panel.left + kPanelW;
    panel.bottom = panel.top  + kPanelH;

    D2D1_ROUNDED_RECT rr{ panel, 10.0f, 10.0f };
    ctx->FillRoundedRectangle(rr, brush_bg_.Get());

    // Accent strip on the left edge — visually ties the panel to the
    // "settings panel" pattern.
    D2D1_RECT_F strip = panel;
    strip.right = strip.left + 4.0f;
    ctx->FillRectangle(strip, brush_accent_.Get());

    // Title row.
    D2D1_RECT_F title_rect{};
    title_rect.left   = panel.left + kPadX;
    title_rect.right  = panel.right - kPadX;
    title_rect.top    = panel.top + kPadY;
    title_rect.bottom = title_rect.top + 22.0f;
    ctx->DrawTextW(
        L"Subtitle setup",
        static_cast<UINT32>(::wcslen(L"Subtitle setup")),
        text_title_.Get(), title_rect, brush_text_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);

    // Source line (basename or "none").
    const wchar_t* name = L"(no subtitle loaded)";
    std::wstring name_buf;
    if (subtitles_ != nullptr && !subtitles_->current_name().empty()) {
        name_buf = subtitles_->current_name();
        name = name_buf.c_str();
    }
    D2D1_RECT_F source_rect = title_rect;
    source_rect.top    = title_rect.bottom + 4.0f;
    source_rect.bottom = source_rect.top + 18.0f;
    ctx->DrawTextW(
        name, static_cast<UINT32>(::wcslen(name)),
        text_hint_.Get(), source_rect, brush_muted_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);

    // Rows (label left, value right).
    auto draw_row = [&](float y, const wchar_t* label,
                        const wchar_t* value) {
        D2D1_RECT_F rl{};
        rl.left   = panel.left  + kPadX;
        rl.right  = panel.left  + kPanelW * 0.55f;
        rl.top    = y;
        rl.bottom = y + kRowH;
        ctx->DrawTextW(
            label, static_cast<UINT32>(::wcslen(label)),
            text_label_.Get(), rl, brush_muted_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);

        D2D1_RECT_F rv{};
        rv.left   = rl.right;
        rv.right  = panel.right - kPadX;
        rv.top    = y;
        rv.bottom = y + kRowH;
        ctx->DrawTextW(
            value, static_cast<UINT32>(::wcslen(value)),
            text_value_.Get(), rv, brush_text_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    };

    const float rows_top = source_rect.bottom + 12.0f;

    wchar_t delay_buf[32] = L"—";
    wchar_t size_buf[16]  = L"—";
    std::wstring font_name = L"(track default)";
    std::wstring enc_name  = L"auto";
    if (subtitles_ != nullptr) {
        const double delay_s =
            static_cast<double>(subtitles_->delay_ns()) / 1e9;
        std::swprintf(delay_buf, 32, L"%+.2f s", delay_s);
        std::swprintf(size_buf, 16, L"%.2fx",
                      static_cast<double>(subtitles_->font_scale()));
        if (!subtitles_->font_override().empty()) {
            try {
                font_name = utf8_to_wide(subtitles_->font_override());
            } catch (...) {
                // Strict UTF-8 conversion failed — shouldn't happen
                // because we round-trip through ChooseFontW, but fall
                // back to the default label rather than crashing.
                font_name = L"(track default)";
            }
        }
        if (!subtitles_->forced_encoding().empty()) {
            try {
                enc_name = utf8_to_wide(subtitles_->forced_encoding());
            } catch (...) {
                enc_name = L"auto";
            }
        }
    }

    draw_row(rows_top + 0 * kRowH, L"Delay",     delay_buf);
    draw_row(rows_top + 1 * kRowH, L"Font size", size_buf);
    draw_row(rows_top + 2 * kRowH, L"Font",      font_name.c_str());
    draw_row(rows_top + 3 * kRowH, L"Encoding",  enc_name.c_str());

    // Hint footer.
    D2D1_RECT_F hint_rect{};
    hint_rect.left   = panel.left + kPadX;
    hint_rect.right  = panel.right - kPadX;
    hint_rect.bottom = panel.bottom - kPadY + 4.0f;
    hint_rect.top    = hint_rect.bottom - 16.0f;
    const wchar_t* hint =
        L", / .  delay   -  / =  size   0  reset   F  font\x2026   E  encoding";
    ctx->DrawTextW(
        hint, static_cast<UINT32>(::wcslen(hint)),
        text_hint_.Get(), hint_rect, brush_muted_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

} // namespace freikino::app
