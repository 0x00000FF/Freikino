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

// Centered "Opening <filename>…" card with an indeterminate
// progress bar. Shown while MediaSession's background worker is
// still probing the file — blocking network/OneDrive opens would
// otherwise freeze the UI with nothing on screen.
class OpeningOverlay {
public:
    OpeningOverlay() noexcept = default;
    ~OpeningOverlay() = default;

    OpeningOverlay(const OpeningOverlay&)            = delete;
    OpeningOverlay& operator=(const OpeningOverlay&) = delete;
    OpeningOverlay(OpeningOverlay&&)                 = delete;
    OpeningOverlay& operator=(OpeningOverlay&&)      = delete;

    void create(render::OverlayRenderer& renderer);

    void show(std::wstring name) noexcept;
    void hide() noexcept;

    [[nodiscard]] bool visible() const noexcept { return visible_; }

    void draw(ID2D1DeviceContext* ctx, UINT width, UINT height) noexcept;

private:
    std::wstring name_;
    bool         visible_ = false;

    ComPtr<ID2D1SolidColorBrush> brush_bg_;
    ComPtr<ID2D1SolidColorBrush> brush_card_;
    ComPtr<ID2D1SolidColorBrush> brush_text_;
    ComPtr<ID2D1SolidColorBrush> brush_track_;
    ComPtr<ID2D1SolidColorBrush> brush_bar_;
    ComPtr<IDWriteTextFormat>    text_title_;
    ComPtr<IDWriteTextFormat>    text_sub_;
};

} // namespace freikino::app
