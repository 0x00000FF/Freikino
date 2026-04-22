#pragma once

#include "freikino/common/com.h"

#include <cstddef>
#include <cstdint>
#include <string>

#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <windows.h>

namespace freikino::render { class OverlayRenderer; }

namespace freikino::app {

class SubtitleOverlay;

// Small centered panel that exposes the two subtitle tweaks users
// actually reach for: sync delay and font-size scale. Passive — it
// reads current values from the SubtitleOverlay and displays them.
// MainWindow drives adjustments by calling bump_activity() after
// mutating the underlying state so the panel stays crisp and the
// user sees the change immediately.
class SubtitleSetupOverlay {
public:
    SubtitleSetupOverlay() noexcept = default;
    ~SubtitleSetupOverlay() = default;

    SubtitleSetupOverlay(const SubtitleSetupOverlay&)            = delete;
    SubtitleSetupOverlay& operator=(const SubtitleSetupOverlay&) = delete;
    SubtitleSetupOverlay(SubtitleSetupOverlay&&)                 = delete;
    SubtitleSetupOverlay& operator=(SubtitleSetupOverlay&&)      = delete;

    void create(render::OverlayRenderer& renderer);

    // Bind the subtitle overlay we display state from.
    void set_subtitle_overlay(SubtitleOverlay* ov) noexcept
    {
        subtitles_ = ov;
    }

    void toggle_visible() noexcept;
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    void hide() noexcept { visible_ = false; }

    // Move the highlight through the track list. Wraps at the ends.
    // No-op if the list has fewer than two rows.
    void move_highlight(int delta) noexcept;

    // Flip the active state of the currently-highlighted track. For
    // embedded tracks this triggers on-demand extraction the first
    // time the track is activated.
    void toggle_highlighted() noexcept;

    void draw(ID2D1DeviceContext* ctx, UINT width, UINT height) noexcept;

private:
    SubtitleOverlay* subtitles_ = nullptr;
    bool             visible_   = false;

    // Index into the track list fetched from SubtitleOverlay.
    // Clamped at `draw` time in case the list shrinks while the
    // panel is open.
    std::size_t      highlight_row_ = 0;

    ComPtr<ID2D1SolidColorBrush> brush_bg_;
    ComPtr<ID2D1SolidColorBrush> brush_accent_;
    ComPtr<ID2D1SolidColorBrush> brush_text_;
    ComPtr<ID2D1SolidColorBrush> brush_muted_;
    ComPtr<ID2D1SolidColorBrush> brush_row_active_;
    ComPtr<ID2D1SolidColorBrush> brush_row_highlight_;
    ComPtr<IDWriteTextFormat>    text_title_;
    ComPtr<IDWriteTextFormat>    text_label_;
    ComPtr<IDWriteTextFormat>    text_value_;
    ComPtr<IDWriteTextFormat>    text_hint_;
    ComPtr<IDWriteTextFormat>    text_row_;
};

} // namespace freikino::app
