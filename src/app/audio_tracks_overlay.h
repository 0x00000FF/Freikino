#pragma once

#include "freikino/common/com.h"

#include <cstddef>
#include <cstdint>

#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <windows.h>

namespace freikino::render { class OverlayRenderer; }
namespace freikino::media { class FFmpegSource; }

namespace freikino::app {

// Centered panel that lists the container's audio tracks and lets the
// user switch between them. Toggled with the `A` keyboard shortcut.
//
// The overlay is a passive view: MainWindow reads selection/apply
// requests via `highlighted_stream_index()` on Enter and performs the
// actual codec swap through `PlaybackController::change_audio_track`.
class AudioTracksOverlay {
public:
    AudioTracksOverlay() noexcept = default;
    ~AudioTracksOverlay() = default;

    AudioTracksOverlay(const AudioTracksOverlay&)            = delete;
    AudioTracksOverlay& operator=(const AudioTracksOverlay&) = delete;
    AudioTracksOverlay(AudioTracksOverlay&&)                 = delete;
    AudioTracksOverlay& operator=(AudioTracksOverlay&&)      = delete;

    void create(render::OverlayRenderer& renderer);

    // Bind the currently-loaded source. Pass nullptr to detach (e.g.
    // between files). The overlay caches the track list lazily on the
    // first `draw()` after the source changes, so no refresh hook is
    // needed from the session layer.
    void set_source(media::FFmpegSource* source) noexcept;

    void toggle_visible() noexcept;
    void hide() noexcept { visible_ = false; }
    [[nodiscard]] bool visible() const noexcept { return visible_; }

    // Move the highlight up / down through the track list. Wraps at
    // the ends. No-op when the list has fewer than two entries.
    void move_highlight(int delta) noexcept;

    // Container-level stream index of the currently-highlighted row, or
    // -1 if the list is empty.
    [[nodiscard]] int highlighted_stream_index() const noexcept;

    void draw(ID2D1DeviceContext* ctx, UINT width, UINT height) noexcept;

private:
    media::FFmpegSource* source_ = nullptr;
    bool visible_ = false;

    // Index into the FFmpegSource::audio_tracks() vector, not a
    // stream index. Reset to match the active track each time the
    // overlay is shown.
    std::size_t highlight_row_ = 0;

    ComPtr<ID2D1SolidColorBrush> brush_bg_;
    ComPtr<ID2D1SolidColorBrush> brush_accent_;
    ComPtr<ID2D1SolidColorBrush> brush_row_current_;
    ComPtr<ID2D1SolidColorBrush> brush_row_highlight_;
    ComPtr<ID2D1SolidColorBrush> brush_text_;
    ComPtr<ID2D1SolidColorBrush> brush_muted_;
    ComPtr<IDWriteTextFormat>    text_title_;
    ComPtr<IDWriteTextFormat>    text_row_;
    ComPtr<IDWriteTextFormat>    text_row_small_;
    ComPtr<IDWriteTextFormat>    text_hint_;
};

} // namespace freikino::app
