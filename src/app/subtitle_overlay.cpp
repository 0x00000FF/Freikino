#include "subtitle_overlay.h"

#include "freikino/common/log.h"
#include "freikino/common/strings.h"
#include "freikino/media/ffmpeg_source.h"
#include "freikino/render/overlay_renderer.h"
#include "playback.h"

#include <algorithm>
#include <cstdio>
#include <utility>

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
    const std::uint32_t event_alpha = 255u - t;

    std::vector<std::uint8_t> buf;
    buf.resize(static_cast<std::size_t>(img.width) * img.height * 4);

    for (int y = 0; y < img.height; ++y) {
        const std::uint8_t* src_row =
            img.mask.data() + static_cast<std::size_t>(y) * img.stride;
        std::uint8_t* dst_row =
            buf.data() + static_cast<std::size_t>(y) * img.width * 4;
        for (int x = 0; x < img.width; ++x) {
            const std::uint32_t glyph = src_row[x];
            const std::uint32_t alpha =
                (glyph * event_alpha + 127u) / 255u;
            dst_row[x * 4 + 0] = static_cast<std::uint8_t>((b * alpha + 127u) / 255u);
            dst_row[x * 4 + 1] = static_cast<std::uint8_t>((g * alpha + 127u) / 255u);
            dst_row[x * 4 + 2] = static_cast<std::uint8_t>((r * alpha + 127u) / 255u);
            dst_row[x * 4 + 3] = static_cast<std::uint8_t>(alpha);
        }
    }
    return buf;
}

std::wstring format_embedded_label(
    const media::FFmpegSource::SubtitleTrack& t, std::size_t ordinal)
{
    wchar_t buf[160] = {};
    const std::wstring codec = utf8_to_wide(t.codec_name);
    const std::wstring lang  = t.language.empty()
        ? std::wstring{L"und"} : utf8_to_wide(t.language);
    const std::wstring title = utf8_to_wide(t.title);
    std::swprintf(
        buf, 160,
        L"#%zu  %-4s  %s%s%s",
        ordinal + 1,
        lang.c_str(),
        codec.empty() ? L"?" : codec.c_str(),
        title.empty() ? L"" : L"  \x2014  ",
        title.c_str());
    return std::wstring{buf};
}

} // namespace

void SubtitleOverlay::create(render::OverlayRenderer& renderer)
{
    (void)renderer;
    // No one-shot D2D resources needed — bitmaps are made on demand
    // from each ASS_Image tile and cached per track in `cache`.
}

void SubtitleOverlay::set_source(media::FFmpegSource* src) noexcept
{
    if (source_ == src) {
        return;
    }
    // Drop all embedded tracks bound to the previous source. External
    // slot (if any) is preserved so dropping a subtitle on one file
    // and switching to another via the playlist doesn't lose the
    // external caption the user just loaded.
    tracks_.erase(
        std::remove_if(
            tracks_.begin(), tracks_.end(),
            [](const std::unique_ptr<Track>& t) {
                return t && !t->external;
            }),
        tracks_.end());
    source_ = src;
    sync_embedded_tracks();
}

void SubtitleOverlay::sync_embedded_tracks() noexcept
{
    if (source_ == nullptr) {
        return;
    }
    const auto streams = source_->subtitle_tracks();
    std::size_t ordinal = 0;
    for (const auto& st : streams) {
        ++ordinal;
        // Already present?
        bool exists = false;
        for (const auto& t : tracks_) {
            if (t && !t->external && t->stream_index == st.stream_index) {
                exists = true;
                break;
            }
        }
        if (exists) {
            continue;
        }
        auto t = std::make_unique<Track>();
        t->external     = false;
        t->stream_index = st.stream_index;
        t->available    = st.is_text;
        t->label        = format_embedded_label(st, ordinal - 1);
        tracks_.push_back(std::move(t));
    }
}

bool SubtitleOverlay::load(const std::wstring& path)
{
    // Drop every existing external track before loading the new file.
    // SAMI files can expand into several tracks at once (one per
    // language class) so a single-slot "reuse" doesn't fit here; a
    // uniform wipe-and-rebuild is simpler and correct for both the
    // single-track and multi-track paths.
    tracks_.erase(
        std::remove_if(
            tracks_.begin(), tracks_.end(),
            [](const std::unique_ptr<Track>& t) {
                return t && t->external;
            }),
        tracks_.end());

    const auto slash    = path.find_last_of(L"\\/");
    const std::wstring basename =
        (slash == std::wstring::npos) ? path : path.substr(slash + 1);

    // ---- SAMI multi-language fast path ----
    // Only fires when the file actually declares >= 2 language
    // classes in its <STYLE> block. For single-language SAMI (or
    // non-SAMI) the helper returns an empty vector and we fall
    // through to the regular load.
    const auto sami = subtitle::parse_sami_language_tracks(
        path, forced_encoding_);
    if (!sami.empty()) {
        std::size_t inserted = 0;
        for (std::size_t idx = 0; idx < sami.size(); ++idx) {
            const auto& entry = sami[idx];
            auto t = std::make_unique<Track>();
            t->external  = true;
            t->available = true;
            t->source    = std::make_unique<subtitle::SubtitleSource>();
            t->renderer  = std::make_unique<subtitle::SubtitleRenderer>();
            const std::wstring display =
                basename + L" (" + entry.display_name + L")";
            if (!t->source->open_from_memory(
                    std::string(entry.ass_content), display)) {
                continue;
            }
            t->renderer->set_source(t->source.get());
            t->ever_loaded = true;
            t->label       = display;
            // First class is activated by default — users can toggle
            // the rest from the S panel. Activating all of them by
            // default would stack captions on top of each other for
            // people who only want one language.
            t->active      = (inserted == 0);
            t->cache_dirty = true;
            apply_settings(*t);
            tracks_.insert(tracks_.begin() + inserted, std::move(t));
            ++inserted;
        }
        if (inserted == 0) {
            external_path_.clear();
            active_display_name_.clear();
            return false;
        }
        external_path_ = path;
        for (const auto& t : tracks_) {
            if (t && t->external && t->active) {
                active_display_name_ = t->label;
                break;
            }
        }
        return true;
    }

    // ---- Regular single-track load ----
    auto t = std::make_unique<Track>();
    t->external  = true;
    t->available = true;
    t->source    = std::make_unique<subtitle::SubtitleSource>();
    t->renderer  = std::make_unique<subtitle::SubtitleRenderer>();
    if (!t->source->open(path, forced_encoding_)) {
        external_path_.clear();
        active_display_name_.clear();
        return false;
    }
    t->renderer->set_source(t->source.get());
    t->ever_loaded = true;
    t->active      = true;
    t->cache_dirty = true;
    t->label       = basename;
    apply_settings(*t);
    tracks_.insert(tracks_.begin(), std::move(t));

    external_path_       = path;
    active_display_name_ = basename;
    return true;
}

void SubtitleOverlay::clear() noexcept
{
    tracks_.clear();
    external_path_.clear();
    active_display_name_.clear();
}

bool SubtitleOverlay::loaded() const noexcept
{
    for (const auto& t : tracks_) {
        if (t && t->active && t->ever_loaded && t->source
            && t->source->loaded()) {
            return true;
        }
    }
    return false;
}

const std::wstring& SubtitleOverlay::current_name() const noexcept
{
    // Prefer the active external track for the setup overlay's
    // "source:" line — that's the label the user is most likely to
    // identify. Fall back to the first active embedded track, then
    // to empty.
    const Track* ext = external_track();
    if (ext != nullptr && ext->active && ext->ever_loaded) {
        return ext->label;
    }
    for (const auto& t : tracks_) {
        if (t && t->active && t->ever_loaded) {
            return t->label;
        }
    }
    static const std::wstring kEmpty;
    return kEmpty;
}

void SubtitleOverlay::set_delay_ns(int64_t ns) noexcept
{
    delay_ns_ = ns;
    for (auto& t : tracks_) {
        if (t && t->source) {
            t->source->set_delay_ns(ns);
        }
    }
}

void SubtitleOverlay::set_font_scale(float s) noexcept
{
    font_scale_ = s;
    for (auto& t : tracks_) {
        if (t && t->renderer) {
            t->renderer->set_font_scale(s);
            t->cache_dirty = true;
        }
    }
}

void SubtitleOverlay::set_font_override(std::string family) noexcept
{
    font_override_ = std::move(family);
    for (auto& t : tracks_) {
        if (t && t->renderer) {
            t->renderer->set_font_override(font_override_);
            t->cache_dirty = true;
        }
    }
}

void SubtitleOverlay::set_forced_encoding(std::string enc)
{
    if (enc == forced_encoding_) {
        return;
    }
    forced_encoding_ = std::move(enc);
    // Only the external slot is affected by the encoding cycle —
    // embedded tracks ship in UTF-8-over-ASS format from FFmpeg's
    // converter and aren't ambiguous.
    if (!external_path_.empty()) {
        const std::wstring path = external_path_;
        (void)load(path);
    }
}

std::size_t SubtitleOverlay::track_count() const noexcept
{
    return tracks_.size();
}

std::vector<SubtitleOverlay::TrackInfo> SubtitleOverlay::list_tracks() const
{
    std::vector<TrackInfo> out;
    out.reserve(tracks_.size());
    for (const auto& t : tracks_) {
        if (!t) continue;
        TrackInfo info;
        info.active    = t->active && t->ever_loaded;
        info.available = t->available;
        if (t->external) {
            info.label = L"external: ";
            if (t->label.empty()) {
                info.label += L"(not loaded)";
            } else {
                info.label += t->label;
            }
        } else {
            info.label = L"embedded: " + t->label;
            if (!t->available) {
                info.label += L"  (image-based)";
            }
        }
        out.push_back(std::move(info));
    }
    return out;
}

void SubtitleOverlay::toggle_track(std::size_t index) noexcept
{
    if (index >= tracks_.size()) {
        return;
    }
    Track* t = tracks_[index].get();
    if (t == nullptr || !t->available) {
        return;
    }
    if (t->external) {
        // External slot — requires a prior load(). Toggling only flips
        // the active flag; we don't re-extract anything.
        if (!t->ever_loaded) {
            return;
        }
        t->active      = !t->active;
        t->cache_dirty = true;
    } else {
        if (!t->ever_loaded) {
            if (!ensure_embedded_loaded(*t)) {
                return;
            }
        }
        t->active      = !t->active;
        t->cache_dirty = true;
    }

    // Refresh the display name with whatever's currently active.
    active_display_name_.clear();
    for (const auto& tr : tracks_) {
        if (tr && tr->active && tr->ever_loaded) {
            active_display_name_ = tr->label;
            break;
        }
    }
}

bool SubtitleOverlay::ensure_embedded_loaded(Track& t) noexcept
{
    if (t.ever_loaded) {
        return true;
    }
    if (t.extraction_attempted) {
        // A previous extraction failed — don't spin on it. The user
        // can still toggle, but the track will stay inert.
        return false;
    }
    t.extraction_attempted = true;
    if (source_ == nullptr || t.stream_index < 0) {
        return false;
    }
    std::string ass = source_->extract_subtitle_ass(t.stream_index);
    if (ass.empty()) {
        log::warn("subtitle: embedded extraction returned empty for stream {}",
                  t.stream_index);
        return false;
    }
    if (t.source == nullptr) {
        t.source = std::make_unique<subtitle::SubtitleSource>();
    }
    if (t.renderer == nullptr) {
        t.renderer = std::make_unique<subtitle::SubtitleRenderer>();
    }
    if (!t.source->open_from_memory(std::move(ass), t.label)) {
        return false;
    }
    t.renderer->set_source(t.source.get());
    t.ever_loaded = true;
    apply_settings(t);
    return true;
}

void SubtitleOverlay::apply_settings(Track& t) noexcept
{
    if (t.source) {
        t.source->set_delay_ns(delay_ns_);
    }
    if (t.renderer) {
        t.renderer->set_font_scale(font_scale_);
        t.renderer->set_font_override(font_override_);
    }
}

SubtitleOverlay::Track* SubtitleOverlay::external_track() noexcept
{
    for (auto& t : tracks_) {
        if (t && t->external) {
            return t.get();
        }
    }
    return nullptr;
}

const SubtitleOverlay::Track* SubtitleOverlay::external_track() const noexcept
{
    for (const auto& t : tracks_) {
        if (t && t->external) {
            return t.get();
        }
    }
    return nullptr;
}

void SubtitleOverlay::draw(
    ID2D1DeviceContext* ctx, UINT w, UINT h) noexcept
{
    if (ctx == nullptr || playback_ == nullptr) {
        return;
    }
    if (w == 0 || h == 0) {
        return;
    }

    // Share the libass canvas size across every track — we reduce
    // the reported height by the transport bar's footprint so
    // captions land above it regardless of which track produced
    // them. Always reserve the space so subtitles don't jump up
    // and down as the bar auto-hides.
    constexpr int kTransportBarPx = 96;
    const int canvas_h =
        (static_cast<int>(h) > kTransportBarPx + 40)
            ? static_cast<int>(h) - kTransportBarPx
            : static_cast<int>(h);

    const bool size_changed =
        (static_cast<int>(w) != last_w_ || canvas_h != last_h_);
    if (size_changed) {
        last_w_ = static_cast<int>(w);
        last_h_ = canvas_h;
    }

    const int64_t now = playback_->current_time_ns();

    D2D1_BITMAP_PROPERTIES props{};
    props.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_PREMULTIPLIED;
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;

    for (auto& tptr : tracks_) {
        if (!tptr) continue;
        Track& t = *tptr;
        if (!t.active || !t.ever_loaded || !t.renderer) continue;

        if (size_changed) {
            t.renderer->set_frame_size(last_w_, last_h_);
            t.cache_dirty = true;
        }
        const bool changed = t.renderer->render_at(now, t.images);
        if (changed) {
            t.cache_dirty = true;
        }

        if (t.cache_dirty) {
            t.cache.clear();
            t.cache.reserve(t.images.size());
            for (const auto& img : t.images) {
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
                    continue;
                }
                t.cache.push_back(std::move(c));
            }
            t.cache_dirty = false;
        }

        for (const auto& c : t.cache) {
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
}

} // namespace freikino::app
