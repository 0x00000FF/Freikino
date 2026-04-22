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

    // Basename of the most recently loaded subtitle (empty if none).
    // Shown on the subtitle setup overlay so the user knows which
    // track the adjustments apply to.
    [[nodiscard]] const std::wstring& current_name() const noexcept
    {
        return current_name_;
    }

    // Subtitle sync offset (positive = show later). Persists across
    // seek / pause / rebinds.
    void set_delay_ns(int64_t ns) noexcept { source_.set_delay_ns(ns); }
    [[nodiscard]] int64_t delay_ns() const noexcept
    {
        return source_.delay_ns();
    }

    // Font size multiplier (1.0 = ASS default). Clamped inside the
    // renderer.
    void set_font_scale(float s) noexcept { renderer_.set_font_scale(s); }
    [[nodiscard]] float font_scale() const noexcept
    {
        return renderer_.font_scale();
    }

    // Override the font face for every dialogue line (UTF-8 face
    // name). Empty = use the track's own styles.
    void set_font_override(std::string family) noexcept
    {
        renderer_.set_font_override(std::move(family));
    }
    [[nodiscard]] const std::string& font_override() const noexcept
    {
        return renderer_.font_override();
    }

    // Force a specific text encoding ("utf-8", "utf-16le", "cp949",
    // "cp932", …). Empty = auto-detect. Changing the encoding while
    // a subtitle is loaded triggers an in-place reload of the same
    // file with the new setting so the user sees the effect
    // immediately.
    void set_forced_encoding(std::string enc);
    [[nodiscard]] const std::string& forced_encoding() const noexcept
    {
        return forced_encoding_;
    }

    void draw(ID2D1DeviceContext* ctx, UINT width, UINT height) noexcept;

private:
    PlaybackController*           playback_ = nullptr;
    subtitle::SubtitleSource      source_;
    subtitle::SubtitleRenderer    renderer_;
    std::vector<subtitle::RenderedImage> images_;
    std::wstring                  current_name_;
    // Full path of the most-recent successful load — kept so the
    // user can reload the same file under a different encoding from
    // the setup overlay.
    std::wstring                  current_path_;
    std::string                   forced_encoding_;

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
