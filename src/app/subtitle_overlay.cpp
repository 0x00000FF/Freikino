#include "subtitle_overlay.h"

#include "freikino/common/log.h"
#include "freikino/render/overlay_renderer.h"
#include "playback.h"

#include <algorithm>
#include <utility>
#include <vector>

namespace freikino::app {

namespace {

// Expand libass's 8-bit alpha mask + RGBA color into a full BGRA
// buffer that D2D can consume. `color` is libass's packed RRGGBBAA
// where AA is *transparency* (0 = fully opaque, 255 = transparent).
std::vector<std::uint8_t> expand_mask_to_bgra(
    const subtitle::RenderedImage& img) noexcept
{
    const auto r = static_cast<std::uint8_t>((img.color_rgba >> 24) & 0xFF);
    const auto g = static_cast<std::uint8_t>((img.color_rgba >> 16) & 0xFF);
    const auto b = static_cast<std::uint8_t>((img.color_rgba >>  8) & 0xFF);
    const auto t = static_cast<std::uint8_t>( img.color_rgba        & 0xFF);
    // Event alpha (opacity) = 255 - transparency.
    const std::uint32_t event_alpha = 255u - t;

    std::vector<std::uint8_t> buf;
    buf.resize(static_cast<std::size_t>(img.width) * img.height * 4);

    for (int y = 0; y < img.height; ++y) {
        const std::uint8_t* src_row =
            img.mask.data() + static_cast<std::size_t>(y) * img.stride;
        std::uint8_t* dst_row =
            buf.data() + static_cast<std::size_t>(y) * img.width * 4;
        for (int x = 0; x < img.width; ++x) {
            const std::uint32_t glyph = src_row[x];                 // 0..255
            const std::uint32_t alpha =
                (glyph * event_alpha + 127u) / 255u;                // 0..255
            // D2D premultiplied BGRA.
            dst_row[x * 4 + 0] = static_cast<std::uint8_t>((b * alpha + 127u) / 255u);
            dst_row[x * 4 + 1] = static_cast<std::uint8_t>((g * alpha + 127u) / 255u);
            dst_row[x * 4 + 2] = static_cast<std::uint8_t>((r * alpha + 127u) / 255u);
            dst_row[x * 4 + 3] = static_cast<std::uint8_t>(alpha);
        }
    }
    return buf;
}

} // namespace

void SubtitleOverlay::create(render::OverlayRenderer& renderer)
{
    (void)renderer;
    // No one-shot D2D resources needed — bitmaps are made on demand
    // from each ASS_Image tile and cached in `cache_`.
}

bool SubtitleOverlay::load(const std::wstring& path)
{
    cache_.clear();
    cache_dirty_ = true;
    images_.clear();

    if (!source_.open(path, forced_encoding_)) {
        renderer_.set_source(nullptr);
        current_name_.clear();
        current_path_.clear();
        return false;
    }
    renderer_.set_source(&source_);

    // Keep just the basename for display on the setup overlay.
    const auto slash = path.find_last_of(L"\\/");
    current_name_ = (slash == std::wstring::npos)
        ? path : path.substr(slash + 1);
    current_path_ = path;
    return true;
}

void SubtitleOverlay::clear() noexcept
{
    renderer_.set_source(nullptr);
    cache_.clear();
    cache_dirty_ = true;
    images_.clear();
    current_name_.clear();
    current_path_.clear();
}

void SubtitleOverlay::set_forced_encoding(std::string enc)
{
    if (enc == forced_encoding_) {
        return;
    }
    forced_encoding_ = std::move(enc);
    // Reload the same file under the new encoding so the change is
    // visible immediately. load() resets name/path on failure, so if
    // the new encoding breaks the parse the user at least gets a
    // clear "subtitle gone" rather than stale glyphs.
    if (!current_path_.empty()) {
        const std::wstring path = current_path_;
        (void)load(path);
    }
}

void SubtitleOverlay::draw(
    ID2D1DeviceContext* ctx, UINT w, UINT h) noexcept
{
    if (ctx == nullptr || !source_.loaded() || playback_ == nullptr) {
        return;
    }
    if (w == 0 || h == 0) {
        return;
    }

    // Keep libass in sync with the current window size. We reduce
    // the reported canvas height by the transport bar's footprint so
    // libass lays subtitles out above the bar — the rendered tiles'
    // dst_y values then land in the upper portion of the screen and
    // never sit behind the bar's scrub/buttons.
    //
    // We always reserve the space (even when the transport is
    // faded out) so subtitles don't jump up and down every time the
    // user moves the mouse. A small dead zone at the bottom is the
    // better tradeoff — most subtitles are bottom-anchored and the
    // user expects a small gap above the edge anyway.
    constexpr int kTransportBarPx = 96;
    const int canvas_h =
        (static_cast<int>(h) > kTransportBarPx + 40)
            ? static_cast<int>(h) - kTransportBarPx
            : static_cast<int>(h);
    if (static_cast<int>(w) != last_w_ || canvas_h != last_h_) {
        last_w_ = static_cast<int>(w);
        last_h_ = canvas_h;
        renderer_.set_frame_size(last_w_, last_h_);
        cache_dirty_ = true;
    }

    const int64_t now = playback_->current_time_ns();
    const bool changed = renderer_.render_at(now, images_);
    if (changed) {
        cache_dirty_ = true;
    }

    if (cache_dirty_) {
        cache_.clear();
        cache_.reserve(images_.size());

        D2D1_BITMAP_PROPERTIES props{};
        props.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
        props.dpiX = 96.0f;
        props.dpiY = 96.0f;

        for (const auto& img : images_) {
            if (img.width <= 0 || img.height <= 0) continue;
            const std::vector<std::uint8_t> bgra = expand_mask_to_bgra(img);

            CachedBitmap c;
            c.dst_x = img.dst_x;
            c.dst_y = img.dst_y;
            c.w     = img.width;
            c.h     = img.height;

            const HRESULT hr = ctx->CreateBitmap(
                D2D1::SizeU(static_cast<UINT32>(img.width),
                            static_cast<UINT32>(img.height)),
                bgra.data(),
                static_cast<UINT32>(img.width) * 4,
                props,
                &c.bmp);
            if (FAILED(hr)) {
                // Skip this tile; others may still upload.
                continue;
            }
            cache_.push_back(std::move(c));
        }
        cache_dirty_ = false;
    }

    for (const auto& c : cache_) {
        if (!c.bmp) continue;
        const D2D1_RECT_F dst = D2D1::RectF(
            static_cast<float>(c.dst_x),
            static_cast<float>(c.dst_y),
            static_cast<float>(c.dst_x + c.w),
            static_cast<float>(c.dst_y + c.h));
        ctx->DrawBitmap(
            c.bmp.Get(),
            dst,
            1.0f,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);
    }
}

} // namespace freikino::app
