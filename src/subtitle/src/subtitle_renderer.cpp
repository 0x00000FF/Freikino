#include "freikino/subtitle/subtitle_renderer.h"
#include "freikino/subtitle/subtitle_source.h"

#include "freikino/common/log.h"

#include <cstring>

#include <ass/ass.h>

namespace freikino::subtitle {

namespace {

void log_ass_msg(int level, const char* fmt, va_list args, void*)
{
    // libass is chatty at INFO (level 5+) — clamp to warnings and
    // above. MSGL_WARN = 3, MSGL_ERR = 1, MSGL_FATAL = 0 in libass's
    // internal scale.
    if (level > 3) {
        return;
    }
    char buf[512];
    std::vsnprintf(buf, sizeof(buf), fmt, args);
    if (level <= 1) {
        log::error("libass: {}", buf);
    } else {
        log::warn("libass: {}", buf);
    }
}

} // namespace

struct SubtitleRenderer::State {
    // The library is *not* owned — it comes from the bound
    // SubtitleSource. libass requires that a renderer and the tracks
    // it draws share the same ASS_Library, so we can't create our own
    // up front; we have to wait until set_source() hands us one.
    ASS_Library*    lib            = nullptr;
    ASS_Renderer*   renderer       = nullptr;
    SubtitleSource* source         = nullptr;
    int             frame_w        = 0;
    int             frame_h        = 0;
    float           font_scale     = 1.0f;
    // libass does a shallow copy of the ASS_Style passed to
    // ass_set_selective_style_override(), so the FontName buffer must
    // outlive the renderer. Keep the string member here and hand out
    // its .c_str() to libass.
    std::string     font_override;

    ~State()
    {
        if (renderer != nullptr) {
            ass_renderer_done(renderer);
        }
    }
};

SubtitleRenderer::SubtitleRenderer()
    : s_(std::make_unique<State>())
{
    // No library / renderer yet — they're created in set_source()
    // once we know which library the track was parsed against.
}

SubtitleRenderer::~SubtitleRenderer() = default;

void SubtitleRenderer::set_frame_size(int width, int height) noexcept
{
    if (s_ == nullptr) return;
    if (width <= 0 || height <= 0) return;
    if (width == s_->frame_w && height == s_->frame_h) return;

    s_->frame_w = width;
    s_->frame_h = height;
    if (s_->renderer != nullptr) {
        ass_set_frame_size(s_->renderer, width, height);
        ass_set_storage_size(s_->renderer, width, height);
    }
}

void SubtitleRenderer::set_source(SubtitleSource* src) noexcept
{
    if (s_ == nullptr) return;
    s_->source = src;

    ASS_Library* needed = (src != nullptr) ? src->library() : nullptr;
    if (needed == s_->lib) {
        return;
    }

    // Rebuild the renderer against the source's library. Without this
    // ass_render_frame would be called with a track+renderer from two
    // different ASS_Libraries and produce nothing — the symptom the
    // user hits as "subtitles loaded but invisible".
    if (s_->renderer != nullptr) {
        ass_renderer_done(s_->renderer);
        s_->renderer = nullptr;
    }
    s_->lib = needed;
    if (s_->lib == nullptr) {
        return;
    }

    ass_set_message_cb(s_->lib, &log_ass_msg, nullptr);
    s_->renderer = ass_renderer_init(s_->lib);
    if (s_->renderer == nullptr) {
        log::error("libass: ass_renderer_init failed");
        return;
    }
    // DirectWrite font discovery on Windows builds of libass.
    ass_set_fonts(
        s_->renderer,
        nullptr /* default font file */,
        "sans-serif",
        ASS_FONTPROVIDER_AUTODETECT,
        nullptr,
        1);
    if (s_->frame_w > 0 && s_->frame_h > 0) {
        ass_set_frame_size(s_->renderer, s_->frame_w, s_->frame_h);
        ass_set_storage_size(s_->renderer, s_->frame_w, s_->frame_h);
    }
    ass_set_font_scale(s_->renderer, static_cast<double>(s_->font_scale));

    // Reapply any active font override to the fresh renderer.
    if (!s_->font_override.empty()) {
        ASS_Style style{};
        style.FontName = const_cast<char*>(s_->font_override.c_str());
        ass_set_selective_style_override(s_->renderer, &style);
        ass_set_selective_style_override_enabled(
            s_->renderer, ASS_OVERRIDE_BIT_FONT_NAME);
    }
}

void SubtitleRenderer::set_font_scale(float scale) noexcept
{
    if (s_ == nullptr) return;
    if (scale < 0.2f) scale = 0.2f;
    if (scale > 4.0f) scale = 4.0f;
    s_->font_scale = scale;
    if (s_->renderer != nullptr) {
        ass_set_font_scale(s_->renderer, static_cast<double>(scale));
    }
}

float SubtitleRenderer::font_scale() const noexcept
{
    return s_ != nullptr ? s_->font_scale : 1.0f;
}

void SubtitleRenderer::set_font_override(std::string family) noexcept
{
    if (s_ == nullptr) return;
    s_->font_override = std::move(family);
    if (s_->renderer == nullptr) {
        return;
    }
    if (s_->font_override.empty()) {
        ass_set_selective_style_override_enabled(
            s_->renderer, ASS_OVERRIDE_DEFAULT);
        return;
    }
    ASS_Style style{};
    style.FontName = const_cast<char*>(s_->font_override.c_str());
    ass_set_selective_style_override(s_->renderer, &style);
    ass_set_selective_style_override_enabled(
        s_->renderer, ASS_OVERRIDE_BIT_FONT_NAME);
}

const std::string& SubtitleRenderer::font_override() const noexcept
{
    static const std::string kEmpty;
    return s_ != nullptr ? s_->font_override : kEmpty;
}

bool SubtitleRenderer::render_at(
    int64_t pts_ns, std::vector<RenderedImage>& out) noexcept
{
    out.clear();
    if (s_ == nullptr || s_->renderer == nullptr || s_->source == nullptr) {
        return false;
    }
    ASS_Track* tr = s_->source->track();
    if (tr == nullptr) {
        return false;
    }
    if (s_->frame_w <= 0 || s_->frame_h <= 0) {
        return false;
    }

    const int64_t adjusted = pts_ns + s_->source->delay_ns();
    const long long now_ms = adjusted / 1'000'000LL;

    int changed = 0;
    ASS_Image* img = ass_render_frame(s_->renderer, tr, now_ms, &changed);

    for (ASS_Image* p = img; p != nullptr; p = p->next) {
        if (p->w <= 0 || p->h <= 0 || p->bitmap == nullptr) {
            continue;
        }
        RenderedImage r;
        r.dst_x      = p->dst_x;
        r.dst_y      = p->dst_y;
        r.width      = p->w;
        r.height     = p->h;
        r.stride     = p->stride;
        r.color_rgba = p->color;
        const std::size_t bytes =
            static_cast<std::size_t>(p->h) * p->stride;
        r.mask.assign(p->bitmap, p->bitmap + bytes);
        out.push_back(std::move(r));
    }

    return changed != 0;
}

} // namespace freikino::subtitle
