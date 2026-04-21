#pragma once

#include "freikino/common/com.h"

#include <cstdint>
#include <string>
#include <vector>

#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <windows.h>

namespace freikino::render { class OverlayRenderer; }

namespace freikino::app {

// Card shown while playing audio-only files — album art on the left,
// metadata lines on the right. Caller drives it via `set_track()` on
// file open; cleared by `clear()`. Entirely passive (no input).
class AudioInfoOverlay {
public:
    struct Track {
        std::wstring filename;   // basename
        std::wstring title;
        std::wstring artist;
        std::wstring album;
        std::wstring codec;      // e.g. "aac"
        std::wstring format_line; // e.g. "320 kbps · 48 kHz · stereo"
        // Album art as tightly-packed BGRA. Empty = no art.
        std::vector<std::uint8_t> art_bgra;
        int art_width  = 0;
        int art_height = 0;
    };

    AudioInfoOverlay() noexcept = default;
    ~AudioInfoOverlay() = default;

    AudioInfoOverlay(const AudioInfoOverlay&)            = delete;
    AudioInfoOverlay& operator=(const AudioInfoOverlay&) = delete;
    AudioInfoOverlay(AudioInfoOverlay&&)                 = delete;
    AudioInfoOverlay& operator=(AudioInfoOverlay&&)      = delete;

    void create(render::OverlayRenderer& renderer);

    void set_track(Track t) noexcept;
    void clear() noexcept;

    [[nodiscard]] bool active() const noexcept { return active_; }

    void draw(ID2D1DeviceContext* ctx, UINT width, UINT height) noexcept;

private:
    bool     active_    = false;
    Track    track_;
    bool     art_dirty_ = true;

    ComPtr<ID2D1Bitmap>          art_bitmap_;
    ComPtr<ID2D1SolidColorBrush> brush_card_;
    ComPtr<ID2D1SolidColorBrush> brush_art_bg_;
    ComPtr<ID2D1SolidColorBrush> brush_text_;
    ComPtr<ID2D1SolidColorBrush> brush_text_dim_;
    ComPtr<IDWriteTextFormat>    text_title_;
    ComPtr<IDWriteTextFormat>    text_artist_;
    ComPtr<IDWriteTextFormat>    text_line_;
};

} // namespace freikino::app
