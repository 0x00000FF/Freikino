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
    ASS_Library*    lib      = nullptr;
    ASS_Renderer*   renderer = nullptr;
    SubtitleSource* source   = nullptr;
    int             frame_w  = 0;
    int             frame_h  = 0;

    ~State()
    {
        if (renderer != nullptr) {
            ass_renderer_done(renderer);
        }
        if (lib != nullptr) {
            ass_library_done(lib);
        }
    }
};

SubtitleRenderer::SubtitleRenderer()
    : s_(std::make_unique<State>())
{
    s_->lib = ass_library_init();
    if (s_->lib == nullptr) {
        log::error("libass: ass_library_init failed");
        return;
    }
    ass_set_message_cb(s_->lib, &log_ass_msg, nullptr);
    // Tell libass to use the system font discovery (fontconfig on
    // Linux; DirectWrite shim on Windows builds of libass). Without
    // ass_set_fonts, libass will still render but fall back to a
    // default-family search, which sometimes fails on Windows CI.
    s_->renderer = ass_renderer_init(s_->lib);
    if (s_->renderer == nullptr) {
        log::error("libass: ass_renderer_init failed");
        return;
    }
    ass_set_fonts(
        s_->renderer,
        nullptr /* default font */,
        "sans-serif",
        ASS_FONTPROVIDER_AUTODETECT,
        nullptr,
        1);
}

SubtitleRenderer::~SubtitleRenderer() = default;

void SubtitleRenderer::set_frame_size(int width, int height) noexcept
{
    if (s_ == nullptr || s_->renderer == nullptr) return;
    if (width <= 0 || height <= 0) return;
    if (width == s_->frame_w && height == s_->frame_h) return;

    s_->frame_w = width;
    s_->frame_h = height;
    ass_set_frame_size(s_->renderer, width, height);
    ass_set_storage_size(s_->renderer, width, height);
}

void SubtitleRenderer::set_source(SubtitleSource* src) noexcept
{
    if (s_ == nullptr) return;
    s_->source = src;
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
