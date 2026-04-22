#pragma once

#include "freikino/common/com.h"

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

    void toggle_visible() noexcept { visible_ = !visible_; }
    [[nodiscard]] bool visible() const noexcept { return visible_; }
    void hide() noexcept { visible_ = false; }

    void draw(ID2D1DeviceContext* ctx, UINT width, UINT height) noexcept;

private:
    SubtitleOverlay* subtitles_ = nullptr;
    bool             visible_   = false;

    ComPtr<ID2D1SolidColorBrush> brush_bg_;
    ComPtr<ID2D1SolidColorBrush> brush_accent_;
    ComPtr<ID2D1SolidColorBrush> brush_text_;
    ComPtr<ID2D1SolidColorBrush> brush_muted_;
    ComPtr<IDWriteTextFormat>    text_title_;
    ComPtr<IDWriteTextFormat>    text_label_;
    ComPtr<IDWriteTextFormat>    text_value_;
    ComPtr<IDWriteTextFormat>    text_hint_;
};

} // namespace freikino::app
