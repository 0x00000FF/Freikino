#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace freikino::subtitle {

// One rendered subtitle glyph-region. Pixels are 8-bit alpha in
// `mask` colored by `color_rgba`; the overlay blitter expands them
// to BGRA on upload.
struct RenderedImage {
    int                       dst_x      = 0;
    int                       dst_y      = 0;
    int                       width      = 0;
    int                       height     = 0;
    int                       stride     = 0;      // bytes per row in `mask`
    // Packed 0xRRGGBBAA (libass convention — AA is *transparency*,
    // 0=opaque. The blitter inverts it on upload).
    std::uint32_t             color_rgba = 0x000000FFu;
    std::vector<std::uint8_t> mask;
};

class SubtitleSource;

// Thin libass wrapper. Owns an ASS_Library + ASS_Renderer and
// produces a list of alpha-mask tiles for a given stream-pts. The
// caller blits them on top of the video frame.
class SubtitleRenderer {
public:
    SubtitleRenderer();
    ~SubtitleRenderer();

    SubtitleRenderer(const SubtitleRenderer&)            = delete;
    SubtitleRenderer& operator=(const SubtitleRenderer&) = delete;
    SubtitleRenderer(SubtitleRenderer&&)                 = delete;
    SubtitleRenderer& operator=(SubtitleRenderer&&)      = delete;

    // Configure the compositor surface size (the video viewport, in
    // pixels — NOT the storage size of the SRT/ASS track). Safe to
    // call whenever the window resizes.
    void set_frame_size(int width, int height) noexcept;

    // Bind a loaded subtitle source. Nullptr clears. Non-owning.
    void set_source(SubtitleSource* src) noexcept;

    // Render every visible line at the given playback pts. `out` is
    // cleared + repopulated. Returns true if the rendered set changed
    // versus the previous call.
    bool render_at(int64_t pts_ns, std::vector<RenderedImage>& out) noexcept;

private:
    struct State;
    std::unique_ptr<State> s_;
};

} // namespace freikino::subtitle
