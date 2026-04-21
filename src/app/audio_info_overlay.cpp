#include "audio_info_overlay.h"

#include "freikino/common/error.h"
#include "freikino/render/overlay_renderer.h"

#include <algorithm>
#include <utility>

namespace freikino::app {

namespace {

constexpr float kCardMarginX = 48.0f;
constexpr float kCardTop     = 48.0f;
constexpr float kCardHeight  = 200.0f;
constexpr float kArtSize     = 168.0f;   // square
constexpr float kArtGap      = 20.0f;    // between art and text
constexpr float kPadX        = 20.0f;

} // namespace

void AudioInfoOverlay::create(render::OverlayRenderer& renderer)
{
    auto* ctx = renderer.context();
    if (ctx == nullptr) {
        throw_hresult(E_UNEXPECTED);
    }

    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.08f, 0.08f, 0.10f, 0.72f), &brush_card_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.16f, 0.16f, 0.18f, 1.00f), &brush_art_bg_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.00f), &brush_text_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.65f), &brush_text_dim_));

    auto* dw = renderer.dwrite();
    if (dw == nullptr) {
        throw_hresult(E_UNEXPECTED);
    }

    auto make_fmt = [&](float size, DWRITE_FONT_WEIGHT weight,
                        IDWriteTextFormat** out) {
        check_hr(dw->CreateTextFormat(
            L"Segoe UI", nullptr,
            weight,
            DWRITE_FONT_STYLE_NORMAL,
            DWRITE_FONT_STRETCH_NORMAL,
            size, L"en-us", out));
        (*out)->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_LEADING);
        (*out)->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_NEAR);
        (*out)->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
        DWRITE_TRIMMING trim{};
        trim.granularity    = DWRITE_TRIMMING_GRANULARITY_CHARACTER;
        trim.delimiter      = 0;
        trim.delimiterCount = 0;
        (*out)->SetTrimming(&trim, nullptr);
    };

    make_fmt(22.0f, DWRITE_FONT_WEIGHT_SEMI_BOLD, &text_title_);
    make_fmt(15.0f, DWRITE_FONT_WEIGHT_NORMAL,    &text_artist_);
    make_fmt(13.0f, DWRITE_FONT_WEIGHT_NORMAL,    &text_line_);
}

void AudioInfoOverlay::set_track(Track t) noexcept
{
    track_     = std::move(t);
    active_    = true;
    art_dirty_ = true;
    art_bitmap_.Reset();
}

void AudioInfoOverlay::clear() noexcept
{
    active_ = false;
    track_  = Track{};
    art_bitmap_.Reset();
    art_dirty_ = true;
}

void AudioInfoOverlay::draw(
    ID2D1DeviceContext* ctx, UINT w, UINT h) noexcept
{
    if (!active_ || ctx == nullptr) {
        return;
    }
    (void)h;

    const float fw = static_cast<float>(w);

    D2D1_RECT_F card{};
    card.left   = kCardMarginX;
    card.right  = fw - kCardMarginX;
    card.top    = kCardTop;
    card.bottom = card.top + kCardHeight;
    if (card.right < card.left + 240.0f) {
        return;   // window too narrow for a readable card
    }
    ctx->FillRectangle(card, brush_card_.Get());

    // Album art square on the left.
    D2D1_RECT_F art_rect{};
    art_rect.left   = card.left + kPadX;
    art_rect.right  = art_rect.left + kArtSize;
    art_rect.top    = card.top + (kCardHeight - kArtSize) * 0.5f;
    art_rect.bottom = art_rect.top + kArtSize;
    ctx->FillRectangle(art_rect, brush_art_bg_.Get());

    if (art_dirty_ && !track_.art_bgra.empty()
        && track_.art_width > 0 && track_.art_height > 0) {
        D2D1_BITMAP_PROPERTIES props{};
        props.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
        props.dpiX = 96.0f;
        props.dpiY = 96.0f;

        ComPtr<ID2D1Bitmap> bmp;
        const HRESULT hr = ctx->CreateBitmap(
            D2D1::SizeU(static_cast<UINT32>(track_.art_width),
                        static_cast<UINT32>(track_.art_height)),
            track_.art_bgra.data(),
            static_cast<UINT32>(track_.art_width) * 4,
            props,
            &bmp);
        if (SUCCEEDED(hr)) {
            art_bitmap_ = bmp;
        }
        art_dirty_ = false;
    }

    if (art_bitmap_) {
        ctx->DrawBitmap(
            art_bitmap_.Get(),
            art_rect,
            1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    } else {
        // "No art" placeholder: a slightly lighter square with a
        // centered musical-note-ish glyph would be nice. For now,
        // just leave the flat gray.
    }

    // Text column on the right.
    const float text_left = art_rect.right + kArtGap;
    const float text_right = card.right - kPadX;
    if (text_right <= text_left + 40.0f) {
        return;   // out of horizontal room
    }

    // Row 1: track title (falls back to filename if no id3 title).
    const std::wstring& primary =
        !track_.title.empty() ? track_.title : track_.filename;

    float y = card.top + 22.0f;
    if (text_title_) {
        D2D1_RECT_F rect = D2D1::RectF(text_left, y, text_right, y + 32.0f);
        ctx->DrawTextW(
            primary.c_str(),
            static_cast<UINT32>(primary.size()),
            text_title_.Get(),
            rect,
            brush_text_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            DWRITE_MEASURING_MODE_NATURAL);
    }
    y += 36.0f;

    // Row 2: artist — album.
    std::wstring artist_line;
    if (!track_.artist.empty()) {
        artist_line = track_.artist;
        if (!track_.album.empty()) {
            artist_line += L" \x2014 ";   // em-dash
            artist_line += track_.album;
        }
    } else {
        artist_line = track_.album;
    }
    if (!artist_line.empty() && text_artist_) {
        D2D1_RECT_F rect = D2D1::RectF(text_left, y, text_right, y + 24.0f);
        ctx->DrawTextW(
            artist_line.c_str(),
            static_cast<UINT32>(artist_line.size()),
            text_artist_.Get(),
            rect,
            brush_text_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            DWRITE_MEASURING_MODE_NATURAL);
        y += 28.0f;
    } else {
        y += 4.0f;
    }

    // Row 3: codec / bitrate / sample-rate / channels.
    if (!track_.format_line.empty() && text_line_) {
        D2D1_RECT_F rect = D2D1::RectF(text_left, y, text_right, y + 20.0f);
        ctx->DrawTextW(
            track_.format_line.c_str(),
            static_cast<UINT32>(track_.format_line.size()),
            text_line_.Get(),
            rect,
            brush_text_dim_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            DWRITE_MEASURING_MODE_NATURAL);
        y += 22.0f;
    }

    // Row 4: filename (dimmer — orienting info, not primary).
    if (!track_.filename.empty() && text_line_) {
        D2D1_RECT_F rect = D2D1::RectF(text_left, y, text_right, y + 20.0f);
        ctx->DrawTextW(
            track_.filename.c_str(),
            static_cast<UINT32>(track_.filename.size()),
            text_line_.Get(),
            rect,
            brush_text_dim_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            DWRITE_MEASURING_MODE_NATURAL);
    }
}

} // namespace freikino::app
