#pragma once

#include "freikino/common/com.h"

#include <cstdint>

#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <windows.h>

namespace freikino::render { class OverlayRenderer; }

namespace freikino::app {

// Small top-right pill showing the current volume level when it
// changes. Auto-fades after ~800 ms of no further changes. Driven by
// `show()`; passive otherwise. Not used while the user is dragging
// the transport bar's vertical slider — that surface is visual
// already and a duplicate OSD would fight the cursor.
class VolumeOsd {
public:
    VolumeOsd() noexcept = default;
    ~VolumeOsd() = default;

    VolumeOsd(const VolumeOsd&)            = delete;
    VolumeOsd& operator=(const VolumeOsd&) = delete;
    VolumeOsd(VolumeOsd&&)                 = delete;
    VolumeOsd& operator=(VolumeOsd&&)      = delete;

    void create(render::OverlayRenderer& renderer);

    // `volume` is 0.0–2.0 (matches WasapiRenderer's range). `muted`
    // forces the "Muted" label regardless of the level.
    void show(float volume, bool muted) noexcept;

    void draw(ID2D1DeviceContext* ctx, UINT width, UINT height) noexcept;

private:
    float     volume_     = 1.0f;
    bool      muted_      = false;
    ULONGLONG shown_at_ms_ = 0;
    float     alpha_      = 0.0f;
    ULONGLONG last_tick_ms_ = 0;

    ComPtr<ID2D1SolidColorBrush> brush_bg_;
    ComPtr<ID2D1SolidColorBrush> brush_track_;
    ComPtr<ID2D1SolidColorBrush> brush_bar_;
    ComPtr<ID2D1SolidColorBrush> brush_text_;
    ComPtr<IDWriteTextFormat>    text_format_;
};

} // namespace freikino::app
