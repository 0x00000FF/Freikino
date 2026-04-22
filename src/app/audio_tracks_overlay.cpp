#include "audio_tracks_overlay.h"

#include "freikino/common/error.h"
#include "freikino/common/strings.h"
#include "freikino/media/ffmpeg_source.h"
#include "freikino/render/overlay_renderer.h"

#include <algorithm>
#include <cstdio>
#include <vector>

namespace freikino::app {

namespace {

constexpr float kPanelW     = 520.0f;
constexpr float kPanelPadX  = 22.0f;
constexpr float kPanelPadY  = 18.0f;
constexpr float kRowH       = 42.0f;
constexpr float kHeaderH    = 30.0f;
constexpr float kFooterH    = 22.0f;
// Cap the visible row count — beyond this we'd need scrolling, which the
// list browser doesn't yet support. Very few files have more than a
// handful of audio streams so the cap is conservative rather than a
// real limit users are likely to hit.
constexpr std::size_t kMaxVisibleRows = 10;

std::wstring format_row(
    const media::FFmpegSource::AudioTrack& t, std::size_t ordinal)
{
    wchar_t buf[256] = {};

    std::wstring codec = utf8_to_wide(t.codec_name);
    if (codec.empty()) {
        codec = L"?";
    }

    std::wstring lang;
    if (!t.language.empty()) {
        lang = utf8_to_wide(t.language);
    }

    std::wstring title;
    if (!t.title.empty()) {
        title = utf8_to_wide(t.title);
    }

    // Build "  #N  LANG  codec  CHch/SRkHz  — Title".
    std::swprintf(
        buf, 256,
        L"#%zu  %-4s  %s  %dch / %d Hz%s%s",
        ordinal + 1,
        lang.empty() ? L"und" : lang.c_str(),
        codec.c_str(),
        t.channels,
        t.sample_rate,
        title.empty() ? L"" : L"  \x2014  ",  // em-dash
        title.c_str());
    return std::wstring{buf};
}

} // namespace

void AudioTracksOverlay::create(render::OverlayRenderer& renderer)
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
        D2D1::ColorF(0.26f, 0.55f, 0.95f, 0.22f), &brush_row_current_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.10f), &brush_row_highlight_));
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
        14.0f, L"en-us", &text_row_));
    text_row_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    text_row_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    text_row_->SetTrimming(nullptr, nullptr);

    check_hr(dw->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        12.0f, L"en-us", &text_row_small_));
    text_row_small_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    text_row_small_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    check_hr(dw->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_NORMAL,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        12.0f, L"en-us", &text_hint_));
    text_hint_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
    text_hint_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
}

void AudioTracksOverlay::set_source(media::FFmpegSource* source) noexcept
{
    source_        = source;
    highlight_row_ = 0;
}

void AudioTracksOverlay::toggle_visible() noexcept
{
    visible_ = !visible_;
    if (visible_ && source_ != nullptr) {
        // Re-home the highlight onto the active track each time the
        // panel opens so Enter-without-navigation applies the current
        // selection (effectively a no-op) instead of jumping to row 0.
        const auto tracks = source_->audio_tracks();
        const int active  = source_->active_audio_stream_index();
        highlight_row_ = 0;
        for (std::size_t i = 0; i < tracks.size(); ++i) {
            if (tracks[i].stream_index == active) {
                highlight_row_ = i;
                break;
            }
        }
    }
}

void AudioTracksOverlay::move_highlight(int delta) noexcept
{
    if (source_ == nullptr) {
        return;
    }
    const auto tracks = source_->audio_tracks();
    if (tracks.size() < 2) {
        return;
    }
    const auto n = static_cast<std::ptrdiff_t>(tracks.size());
    auto idx = static_cast<std::ptrdiff_t>(highlight_row_) + delta;
    idx = ((idx % n) + n) % n;
    highlight_row_ = static_cast<std::size_t>(idx);
}

int AudioTracksOverlay::highlighted_stream_index() const noexcept
{
    if (source_ == nullptr) {
        return -1;
    }
    const auto tracks = source_->audio_tracks();
    if (tracks.empty()) {
        return -1;
    }
    const std::size_t row = (std::min)(highlight_row_, tracks.size() - 1);
    return tracks[row].stream_index;
}

void AudioTracksOverlay::draw(
    ID2D1DeviceContext* ctx, UINT w, UINT h) noexcept
{
    if (!visible_ || ctx == nullptr || text_title_ == nullptr) {
        return;
    }

    std::vector<media::FFmpegSource::AudioTrack> tracks;
    int active_stream = -1;
    if (source_ != nullptr) {
        tracks        = source_->audio_tracks();
        active_stream = source_->active_audio_stream_index();
    }

    // Clamp the row to the list bounds — the track list can shrink
    // across sessions while the panel is open between files.
    if (!tracks.empty()
        && highlight_row_ >= tracks.size()) {
        highlight_row_ = tracks.size() - 1;
    }

    const std::size_t visible_rows =
        tracks.empty() ? 1 : (std::min)(tracks.size(), kMaxVisibleRows);
    const float rows_area_h =
        static_cast<float>(visible_rows) * kRowH;
    const float panel_h =
        kPanelPadY * 2.0f + kHeaderH + rows_area_h + kFooterH;

    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);
    D2D1_RECT_F panel{};
    panel.left   = (fw - kPanelW) * 0.5f;
    panel.top    = (fh - panel_h) * 0.5f;
    panel.right  = panel.left + kPanelW;
    panel.bottom = panel.top  + panel_h;

    D2D1_ROUNDED_RECT rr{ panel, 10.0f, 10.0f };
    ctx->FillRoundedRectangle(rr, brush_bg_.Get());

    // Accent strip on the left edge — same visual pattern as the
    // subtitle-setup panel so the two overlays feel like a family.
    D2D1_RECT_F strip = panel;
    strip.right = strip.left + 4.0f;
    ctx->FillRectangle(strip, brush_accent_.Get());

    // Title row.
    D2D1_RECT_F title_rect{};
    title_rect.left   = panel.left + kPanelPadX;
    title_rect.right  = panel.right - kPanelPadX;
    title_rect.top    = panel.top + kPanelPadY;
    title_rect.bottom = title_rect.top + kHeaderH;
    ctx->DrawTextW(
        L"Audio tracks",
        static_cast<UINT32>(::wcslen(L"Audio tracks")),
        text_title_.Get(), title_rect, brush_text_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);

    // Rows.
    const float rows_top = title_rect.bottom;
    if (tracks.empty()) {
        D2D1_RECT_F empty_rect{};
        empty_rect.left   = panel.left + kPanelPadX;
        empty_rect.right  = panel.right - kPanelPadX;
        empty_rect.top    = rows_top;
        empty_rect.bottom = rows_top + kRowH;
        const wchar_t* msg = L"No audio tracks in this file.";
        ctx->DrawTextW(
            msg, static_cast<UINT32>(::wcslen(msg)),
            text_row_.Get(), empty_rect, brush_muted_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
    } else {
        const std::size_t shown = (std::min)(tracks.size(), kMaxVisibleRows);
        for (std::size_t i = 0; i < shown; ++i) {
            const auto& t = tracks[i];
            D2D1_RECT_F row_rect{};
            row_rect.left   = panel.left + 8.0f;
            row_rect.right  = panel.right - 8.0f;
            row_rect.top    = rows_top + static_cast<float>(i) * kRowH;
            row_rect.bottom = row_rect.top + kRowH;

            // Active-stream background (subtle) + highlight background
            // (brighter). They can stack: the active track being
            // highlighted gets both treatments.
            if (t.stream_index == active_stream) {
                ctx->FillRectangle(row_rect, brush_row_current_.Get());
            }
            if (i == highlight_row_) {
                ctx->FillRectangle(row_rect, brush_row_highlight_.Get());
                // Left edge accent marker on the highlighted row.
                D2D1_RECT_F marker = row_rect;
                marker.right = marker.left + 3.0f;
                ctx->FillRectangle(marker, brush_accent_.Get());
            }

            const std::wstring line = format_row(t, i);

            D2D1_RECT_F label_rect = row_rect;
            label_rect.left  += 18.0f;
            label_rect.right -= 80.0f;
            ctx->DrawTextW(
                line.c_str(),
                static_cast<UINT32>(line.size()),
                text_row_.Get(), label_rect, brush_text_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);

            // Active-track tag on the right edge.
            if (t.stream_index == active_stream) {
                D2D1_RECT_F tag_rect = row_rect;
                tag_rect.left  = row_rect.right - 80.0f;
                tag_rect.right = row_rect.right - 16.0f;
                const wchar_t* tag = L"active";
                ctx->DrawTextW(
                    tag, static_cast<UINT32>(::wcslen(tag)),
                    text_row_small_.Get(), tag_rect,
                    brush_accent_.Get(),
                    D2D1_DRAW_TEXT_OPTIONS_CLIP,
                    DWRITE_MEASURING_MODE_NATURAL);
            }
        }

        if (tracks.size() > kMaxVisibleRows) {
            // Rare: too many tracks to show. Let the user know so they
            // don't think the list is truncated silently.
            D2D1_RECT_F more_rect{};
            more_rect.left   = panel.left + kPanelPadX;
            more_rect.right  = panel.right - kPanelPadX;
            more_rect.bottom = panel.bottom - kPanelPadY - kFooterH;
            more_rect.top    = more_rect.bottom - 16.0f;
            wchar_t more_buf[64] = {};
            std::swprintf(
                more_buf, 64, L"\x2026 %zu more not shown",
                tracks.size() - kMaxVisibleRows);
            ctx->DrawTextW(
                more_buf, static_cast<UINT32>(::wcslen(more_buf)),
                text_hint_.Get(), more_rect, brush_muted_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP,
                DWRITE_MEASURING_MODE_NATURAL);
        }
    }

    // Footer hint.
    D2D1_RECT_F hint_rect{};
    hint_rect.left   = panel.left + kPanelPadX;
    hint_rect.right  = panel.right - kPanelPadX;
    hint_rect.bottom = panel.bottom - kPanelPadY + 4.0f;
    hint_rect.top    = hint_rect.bottom - kFooterH;
    const wchar_t* hint = L"\x2191 / \x2193  select   Enter  apply   A / Esc  close";
    ctx->DrawTextW(
        hint, static_cast<UINT32>(::wcslen(hint)),
        text_hint_.Get(), hint_rect, brush_muted_.Get(),
        D2D1_DRAW_TEXT_OPTIONS_CLIP, DWRITE_MEASURING_MODE_NATURAL);
}

} // namespace freikino::app
