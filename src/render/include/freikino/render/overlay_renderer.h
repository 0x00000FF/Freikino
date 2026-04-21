#pragma once

#include "freikino/common/com.h"

#include <d2d1_1.h>
#include <d3d11.h>
#include <dwrite.h>
#include <dxgi1_2.h>

namespace freikino::render {

// Direct2D render host layered over an existing DXGI swap chain.
//
// The overlay shares the D3D11 device the video pipeline already uses, so
// every D2D draw goes to the same GPU queue as the video present. Order
// within a frame is: video draw (D3D11) → overlay draw (D2D) → Present.
//
// `begin_draw()` binds the current back buffer as the D2D target and enters
// a D2D draw scope. `end_draw()` flushes and unbinds. The target bitmap is
// recreated when the back buffer's dimensions change (e.g., after swap
// chain resize).
class OverlayRenderer {
public:
    OverlayRenderer() noexcept = default;
    ~OverlayRenderer() = default;

    OverlayRenderer(const OverlayRenderer&)            = delete;
    OverlayRenderer& operator=(const OverlayRenderer&) = delete;
    OverlayRenderer(OverlayRenderer&&)                 = delete;
    OverlayRenderer& operator=(OverlayRenderer&&)      = delete;

    // Build D2D factory/device + DWrite factory. `d3d` must stay alive for
    // the lifetime of this object.
    void create(ID3D11Device* d3d);

    // Bind to the DXGI swap chain whose back buffer will become the D2D
    // target. Can be called multiple times (e.g., after a device loss);
    // invalidates the cached bitmap.
    void attach_swap_chain(IDXGISwapChain1* swap);

    // Begin D2D drawing into the swap chain's current back buffer. Returns
    // the device context; caller issues D2D commands, then calls `end_draw`.
    // Returns nullptr if the target could not be obtained.
    [[nodiscard]] ID2D1DeviceContext* begin_draw();

    // Commit D2D commands. If EndDraw reports a recreate-target condition,
    // the cached bitmap is dropped; the next `begin_draw` rebuilds it.
    void end_draw() noexcept;

    // Release the cached D2D target bitmap. Must be called before the swap
    // chain's `ResizeBuffers` — DXGI rejects a resize while any reference
    // to buffer 0 is still live, and our bitmap is such a reference. The
    // next `begin_draw` transparently rebuilds the target from the (now
    // resized) back buffer.
    void invalidate_target() noexcept;

    [[nodiscard]] IDWriteFactory*    dwrite()      const noexcept { return dwrite_.Get(); }
    [[nodiscard]] ID2D1Factory1*     d2d_factory() const noexcept { return d2d_factory_.Get(); }
    // Exposed so overlays can allocate D2D resources (brushes, text formats,
    // geometries) that live alongside the renderer's lifetime.
    [[nodiscard]] ID2D1DeviceContext* context()    const noexcept { return d2d_ctx_.Get(); }

private:
    ComPtr<ID2D1Factory1>      d2d_factory_;
    ComPtr<ID2D1Device>        d2d_device_;
    ComPtr<ID2D1DeviceContext> d2d_ctx_;
    ComPtr<IDWriteFactory>     dwrite_;
    ComPtr<IDXGISwapChain1>    swap_;
    ComPtr<ID2D1Bitmap1>       target_;
    UINT                       cached_w_ = 0;
    UINT                       cached_h_ = 0;
    bool                       in_draw_  = false;
};

} // namespace freikino::render
