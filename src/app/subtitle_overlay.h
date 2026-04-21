#pragma once

#include "freikino/common/com.h"
#include "freikino/subtitle/subtitle_renderer.h"
#include "freikino/subtitle/subtitle_source.h"

#include <memory>
#include <string>
#include <vector>

#include <d2d1.h>
#include <d2d1_1.h>
#include <windows.h>

namespace freikino::render { class OverlayRenderer; }

namespace freikino::app {

class PlaybackController;

// Draws external subtitle files (SRT / SMI / ASS / SSA) on top of the
// video. Pulls frames from the current playback clock so timing
// follows seeks and pause/resume transparently.
class SubtitleOverlay {
public:
    SubtitleOverlay() = default;
    ~SubtitleOverlay() = default;

    SubtitleOverlay(const SubtitleOverlay&)            = delete;
    SubtitleOverlay& operator=(const SubtitleOverlay&) = delete;
    SubtitleOverlay(SubtitleOverlay&&)                 = delete;
    SubtitleOverlay& operator=(SubtitleOverlay&&)      = delete;

    void create(render::OverlayRenderer& renderer);

    void set_playback(PlaybackController* pc) noexcept { playback_ = pc; }

    // Load a subtitle file. Returns true on success. Any previously
    // loaded track is replaced.
    bool load(const std::wstring& path);

    // Drop the current subtitle track.
    void clear() noexcept;

    [[nodiscard]] bool loaded() const noexcept { return source_.loaded(); }

    void draw(ID2D1DeviceContext* ctx, UINT width, UINT height) noexcept;

private:
    PlaybackController*           playback_ = nullptr;
    subtitle::SubtitleSource      source_;
    subtitle::SubtitleRenderer    renderer_;
    std::vector<subtitle::RenderedImage> images_;

    // D2D bitmap cache — one bitmap per subtitle image, regenerated
    // whenever the image set changes. Cached across calls so the
    // (common) case of unchanged subtitles doesn't re-upload every
    // frame.
    struct CachedBitmap {
        ComPtr<ID2D1Bitmap> bmp;
        int dst_x = 0;
        int dst_y = 0;
        int w     = 0;
        int h     = 0;
    };
    std::vector<CachedBitmap> cache_;
    bool cache_dirty_ = true;

    int last_w_ = 0;
    int last_h_ = 0;
};

} // namespace freikino::app
