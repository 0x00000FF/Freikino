#include "subtitle_setup_overlay.h"

#include "freikino/common/error.h"
#include "freikino/common/strings.h"
#include "freikino/render/overlay_renderer.h"
#include "subtitle_overlay.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>

namespace freikino::app {

namespace {

constexpr float kPanelW      = 520.0f;
constexpr float kPadX        = 22.0f;
constexpr float kPadY        = 18.0f;
constexpr float kTitleH      = 22.0f;
constexpr float kSourceH     = 18.0f;
constexpr float kSettingsRowH = 30.0f;
constexpr int   kSettingsRows = 4;
constexpr float kDividerGap  = 10.0f;
constexpr float kDividerH    = 1.0f;
constexpr float kTracksHeaderH = 18.0f;
constexpr float kTrackRowH   = 30.0f;
constexpr float kFooterH     = 16.0f;
constexpr std::size_t kMaxVisibleTracks = 8;

float panel_height_for(std::size_t track_rows)
{
    const std::size_t visible = (std::min)(
        track_rows == 0 ? std::size_t{1} : track_rows, kMaxVisibleTracks);
    return kPadY
        + kTitleH + 4.0f
        + kSourceH + 12.0f
        + kSettingsRows * kSettingsRowH
        + kDividerGap + kDividerH + kDividerGap
        + kTracksHeaderH + 4.0f
        + visible * kTrackRowH
        + 8.0f + kFooterH
        + kPadY;
}

// 8-dot rotating spinner centred on `cx,cy`. Dots fade around the
// ring and the whole ring rotates by `phase_seconds` at 1.1 Hz, which
// reads as "working" without pulling attention away from the label.
// `brush` is borrowed; its opacity is saved and restored so callers
// don't see lingering side effects.
void draw_row_spinner(
    ID2D1DeviceContext* ctx,
    ID2D1SolidColorBrush* brush,
    float cx, float cy,
    double phase_seconds) noexcept
{
    constexpr int   kDots   = 8;
    constexpr float kRadius = 6.5f;
    constexpr float kDotR   = 1.6f;
    constexpr double kRevPerSec = 1.1;
    constexpr double kTau   = 6.283185307179586;

    const float saved = brush->GetOpacity();
    const double rot  = phase_seconds * kRevPerSec * kTau;
    for (int i = 0; i < kDots; ++i) {
        const double a = rot + (kTau * i) / kDots;
        const float dx = static_cast<float>(std::cos(a)) * kRadius;
        const float dy = static_cast<float>(std::sin(a)) * kRadius;
        const float alpha = 0.15f
            + 0.85f * (static_cast<float>(i) / (kDots - 1));
        brush->SetOpacity(alpha);
        D2D1_ELLIPSE e{ { cx + dx, cy + dy }, kDotR, kDotR };
        ctx->FillEllipse(e, brush);
    }
    brush->SetOpacity(saved);
}

} // namespace

void SubtitleSetupOverlay::create(render::OverlayRenderer& renderer)
{
    auto* ctx = renderer.context();
    if (ctx == nullptr) {
        throw_hresult(E_UNEXPECTED);
    }
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.06f, 0.06f, 0.08f, 0.92f), &brush_bg_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.26f, 0.55f, 0.95f, 1.0f), &brush_accent_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.0f), &brush_text_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.55f), &brush_muted_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.26f, 0.55f, 0.95f, 0.22f), &brush_row_active_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f), &brush_row_highlight_));

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

    check_hr(dw->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        13.0f, L"en-us", &text_row_));
    text_row_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    text_row_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
}

void SubtitleSetupOverlay::toggle_visible() noexcept
{
    visible_ = !visible_;
    if (visible_ && subtitles_ != nullptr) {
        // Re-home the highlight onto the first active track (or row
        // 0 if none) each time the panel opens. Stops the cursor
        // from drifting onto a row the user wasn't looking at.
        highlight_row_ = 0;
        const auto list = subtitles_->list_tracks();
        for (std::size_t i = 0; i < list.size(); ++i) {
            if (list[i].active) {
                highlight_row_ = i;
                break;
            }
        }
    }
}

void SubtitleSetupOverlay::move_highlight(int delta) noexcept
{
    if (subtitles_ == nullptr) {
        return;
    }
    const std::size_t n = subtitles_->track_count();
    if (n < 2) {
        return;
    }
    const auto nn = static_cast<std::ptrdiff_t>(n);
    auto idx = static_cast<std::ptrdiff_t>(highlight_row_) + delta;
    idx = ((idx % nn) + nn) % nn;
    highlight_row_ = static_cast<std::size_t>(idx);
}

void SubtitleSetupOverlay::toggle_highlighted() noexcept
{
    if (subtitles_ == nullptr) {
        return;
    }
    subtitles_->toggle_track(highlight_row_);
}

void SubtitleSetupOverlay::draw(
    ID2D1DeviceContext* ctx, UINT w, UINT h) noexcept
{
    if (!visible_ || ctx == nullptr || text_title_ == nullptr) {
        return;
    }

    std::vector<SubtitleOverlay::TrackInfo> tracks;
    if (subtitles_ != nullptr) {
        tracks = subtitles_->list_tracks();
    }
    if (!tracks.empty() && highlight_row_ >= tracks.size()) {
        highlight_row_ = tracks.size() - 1;
    }

    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    const float panel_h = panel_height_for(tracks.size());

    D2D1_RECT_F panel{};
    panel.left   = (fw - kPanelW) * 0.5f;
    panel.top    = (fh - panel_h) * 0.5f;
    panel.right  = panel.left + kPanelW;
    panel.bottom = panel.top  + panel_h;

    D2D1_ROUNDED_RECT rr{ panel, 10.0f, 10.0f };
    ctx->FillRoundedRectangle(rr, brush_bg_.Get());

    // Left-edge accent strip — matches the audio-tracks panel style.
    D2D1_RECT_F strip = panel;
    strip.right = strip.left + 4.0f;
    ctx->FillRectangle(strip, brush_accent_.Get());

    // Title.
    D2D1_RECT_F title_rect{};
    title_rect.left   = panel.left + kPadX;
    title_rect.right  = panel.right - kPadX;
    title_rect.top    = panel.top + kPadY;
    title_rect.bottom = title_rect.top + kTitleH;
    ctx->DrawTextW(
        L"Subtitle setup",
        static_cast<UINT32>(::wcslen(L"Subtitle setup")),
        text_title_.Get(), title_rect, brush_text_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);

    // Source line.
    const wchar_t* name = L"(no subtitle active)";
    std::wstring name_buf;
    if (subtitles_ != nullptr && !subtitles_->current_name().empty()) {
        name_buf = subtitles_->current_name();
        name = name_buf.c_str();
    }
    D2D1_RECT_F source_rect{};
    source_rect.left   = title_rect.left;
    source_rect.right  = title_rect.right;
    source_rect.top    = title_rect.bottom + 4.0f;
    source_rect.bottom = source_rect.top + kSourceH;
    ctx->DrawTextW(
        name, static_cast<UINT32>(::wcslen(name)),
        text_hint_.Get(), source_rect, brush_muted_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);

    // Settings rows (label left, value right).
    auto draw_row = [&](float y, const wchar_t* label,
                        const wchar_t* value) {
        D2D1_RECT_F rl{};
        rl.left   = panel.left  + kPadX;
        rl.right  = panel.left  + kPanelW * 0.55f;
        rl.top    = y;
        rl.bottom = y + kSettingsRowH;
        ctx->DrawTextW(
            label, static_cast<UINT32>(::wcslen(label)),
            text_label_.Get(), rl, brush_muted_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);

        D2D1_RECT_F rv{};
        rv.left   = rl.right;
        rv.right  = panel.right - kPadX;
        rv.top    = y;
        rv.bottom = y + kSettingsRowH;
        ctx->DrawTextW(
            value, static_cast<UINT32>(::wcslen(value)),
            text_value_.Get(), rv, brush_text_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    };

    const float settings_top = source_rect.bottom + 12.0f;

    wchar_t delay_buf[32] = L"\x2014";
    wchar_t size_buf[16]  = L"\x2014";
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

    draw_row(settings_top + 0 * kSettingsRowH, L"Delay",     delay_buf);
    draw_row(settings_top + 1 * kSettingsRowH, L"Font size", size_buf);
    draw_row(settings_top + 2 * kSettingsRowH, L"Font",      font_name.c_str());
    draw_row(settings_top + 3 * kSettingsRowH, L"Encoding",  enc_name.c_str());

    // Divider.
    const float divider_y =
        settings_top + kSettingsRows * kSettingsRowH + kDividerGap;
    D2D1_RECT_F divider{};
    divider.left   = panel.left  + kPadX;
    divider.right  = panel.right - kPadX;
    divider.top    = divider_y;
    divider.bottom = divider_y + kDividerH;
    ctx->FillRectangle(divider, brush_muted_.Get());

    // Tracks header.
    D2D1_RECT_F tracks_header{};
    tracks_header.left   = panel.left + kPadX;
    tracks_header.right  = panel.right - kPadX;
    tracks_header.top    = divider.bottom + kDividerGap;
    tracks_header.bottom = tracks_header.top + kTracksHeaderH;
    ctx->DrawTextW(
        L"Tracks", static_cast<UINT32>(::wcslen(L"Tracks")),
        text_label_.Get(), tracks_header, brush_muted_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);

    // Tracks rows.
    const float rows_top = tracks_header.bottom + 4.0f;
    if (tracks.empty()) {
        D2D1_RECT_F empty_rect{};
        empty_rect.left   = panel.left + kPadX;
        empty_rect.right  = panel.right - kPadX;
        empty_rect.top    = rows_top;
        empty_rect.bottom = rows_top + kTrackRowH;
        const wchar_t* msg =
            L"No subtitle tracks. Drop a .srt/.ass/.smi file or open "
            L"a video with embedded subs.";
        ctx->DrawTextW(
            msg, static_cast<UINT32>(::wcslen(msg)),
            text_row_.Get(), empty_rect, brush_muted_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    } else {
        const std::size_t shown = (std::min)(tracks.size(), kMaxVisibleTracks);
        for (std::size_t i = 0; i < shown; ++i) {
            const auto& info = tracks[i];
            D2D1_RECT_F row_rect{};
            row_rect.left   = panel.left + 8.0f;
            row_rect.right  = panel.right - 8.0f;
            row_rect.top    = rows_top + static_cast<float>(i) * kTrackRowH;
            row_rect.bottom = row_rect.top + kTrackRowH;

            if (info.active) {
                ctx->FillRectangle(row_rect, brush_row_active_.Get());
            }
            if (i == highlight_row_) {
                ctx->FillRectangle(row_rect, brush_row_highlight_.Get());
                D2D1_RECT_F marker = row_rect;
                marker.right = marker.left + 3.0f;
                ctx->FillRectangle(marker, brush_accent_.Get());
            }

            // Indicator on the left. Three states:
            //   loading (async extract in flight, or just-clicked) → spinner
            //   unavailable                                         → muted box
            //   active / inactive                                   → filled / hollow box
            const float box_cx = row_rect.left + 14.0f + 7.0f;
            const float box_cy = (row_rect.top + row_rect.bottom) * 0.5f;
            if (info.loading) {
                using steady = std::chrono::steady_clock;
                static const steady::time_point kOrigin = steady::now();
                const double phase_s =
                    std::chrono::duration<double>(
                        steady::now() - kOrigin).count();
                draw_row_spinner(ctx, brush_accent_.Get(),
                                 box_cx, box_cy, phase_s);
            } else {
                D2D1_RECT_F box{};
                box.left   = box_cx - 7.0f;
                box.right  = box_cx + 7.0f;
                box.top    = box_cy - 7.0f;
                box.bottom = box_cy + 7.0f;
                if (!info.available) {
                    ctx->DrawRectangle(box, brush_muted_.Get(), 1.0f);
                } else if (info.active) {
                    ctx->FillRectangle(box, brush_accent_.Get());
                } else {
                    ctx->DrawRectangle(box, brush_text_.Get(), 1.0f);
                }
            }

            D2D1_RECT_F label_rect = row_rect;
            label_rect.left  = box_cx + 7.0f + 10.0f;
            label_rect.right = row_rect.right - 12.0f;
            ID2D1SolidColorBrush* br = info.available
                ? brush_text_.Get() : brush_muted_.Get();
            ctx->DrawTextW(
                info.label.c_str(),
                static_cast<UINT32>(info.label.size()),
                text_row_.Get(), label_rect, br,
                D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
        }

        if (tracks.size() > kMaxVisibleTracks) {
            D2D1_RECT_F more_rect{};
            more_rect.left   = panel.left + kPadX;
            more_rect.right  = panel.right - kPadX;
            more_rect.top    = rows_top
                             + static_cast<float>(shown) * kTrackRowH;
            more_rect.bottom = more_rect.top + 16.0f;
            wchar_t buf[64] = {};
            std::swprintf(
                buf, 64, L"\x2026 %zu more not shown",
                tracks.size() - kMaxVisibleTracks);
            ctx->DrawTextW(
                buf, static_cast<UINT32>(::wcslen(buf)),
                text_hint_.Get(), more_rect, brush_muted_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP,
                DWRITE_MEASURING_MODE_NATURAL);
        }
    }

    // Hint footer.
    D2D1_RECT_F hint_rect{};
    hint_rect.left   = panel.left + kPadX;
    hint_rect.right  = panel.right - kPadX;
    hint_rect.bottom = panel.bottom - kPadY + 4.0f;
    hint_rect.top    = hint_rect.bottom - kFooterH;
    const wchar_t* hint =
        L"\x2191/\x2193 select  Space toggle  ,/. delay  -/= size  "
        L"0 reset  F font  E encoding";
    ctx->DrawTextW(
        hint, static_cast<UINT32>(::wcslen(hint)),
        text_hint_.Get(), hint_rect, brush_muted_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

} // namespace freikino::app
