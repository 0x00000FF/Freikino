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

// Brief overlay that pops up near the top of the window showing the
// name of the file that just started. Fades in, holds for ~1 second,
// fades out. Fully passive: no input handling. Call `show()` to
// retrigger (e.g. on each file change). Call `draw()` from the
// renderer's overlay callback.
class TitleToast {
public:
    TitleToast() noexcept = default;
    ~TitleToast() = default;

    TitleToast(const TitleToast&)            = delete;
    TitleToast& operator=(const TitleToast&) = delete;
    TitleToast(TitleToast&&)                 = delete;
    TitleToast& operator=(TitleToast&&)      = delete;

    void create(render::OverlayRenderer& renderer);

    // Retriggers the toast with the given text. Safe to call while
    // the toast is already visible — the new text replaces the old
    // one and the hold window restarts from now.
    void show(std::wstring text) noexcept;

    void draw(ID2D1DeviceContext* ctx, UINT width, UINT height) noexcept;

private:
    std::wstring text_;
    ULONGLONG    shown_at_ms_ = 0;   // time `show` was last called
    float        alpha_       = 0.0f;
    ULONGLONG    last_tick_ms_ = 0;

    ComPtr<ID2D1SolidColorBrush> brush_bg_;
    ComPtr<ID2D1SolidColorBrush> brush_text_;
    ComPtr<IDWriteTextFormat>    text_format_;
};

} // namespace freikino::app
