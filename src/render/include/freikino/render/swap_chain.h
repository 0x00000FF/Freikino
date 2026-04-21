#pragma once

#include "freikino/common/com.h"

#include <d3d11.h>
#include <dxgi1_3.h>

namespace freikino::render {

class Device;

// Flip-model swapchain with a frame-latency waitable object.
//
// The waitable is what the render loop blocks on: it is signalled when DXGI
// can accept another Present without adding latency. Together with
// `SetMaximumFrameLatency(1)` this gives single-frame latency while still
// being paced by the compositor.
//
// Tearing is enabled when the OS+GPU support it (Win10 1703+ on flip-model);
// a present with `sync_interval=0` then bypasses the compositor for the
// lowest possible latency on variable-refresh displays.
class SwapChain {
public:
    SwapChain() noexcept = default;
    ~SwapChain();

    SwapChain(const SwapChain&) = delete;
    SwapChain& operator=(const SwapChain&) = delete;
    SwapChain(SwapChain&&) = delete;
    SwapChain& operator=(SwapChain&&) = delete;

    void create(Device& device, HWND hwnd, UINT width, UINT height);
    void resize(UINT width, UINT height);
    void present(bool vsync);

    ID3D11RenderTargetView* rtv()    const noexcept { return rtv_.Get(); }
    ID3D11Texture2D*        back()   const noexcept { return back_.Get(); }
    HANDLE                  waitable() const noexcept { return waitable_; }
    // Base-interface accessor so overlays (D2D) can wrap the back buffer
    // without needing IDXGISwapChain2-specific features.
    IDXGISwapChain1*        dxgi_swap_chain() const noexcept { return sc_.Get(); }

    UINT width()  const noexcept { return width_; }
    UINT height() const noexcept { return height_; }

private:
    void acquire_back_buffer();
    void close_waitable() noexcept;

    ID3D11Device*               d3d_    = nullptr; // non-owning
    ComPtr<IDXGISwapChain2>     sc_;
    ComPtr<ID3D11Texture2D>     back_;
    ComPtr<ID3D11RenderTargetView> rtv_;
    HANDLE                      waitable_ = nullptr;
    UINT                        width_    = 0;
    UINT                        height_   = 0;
    bool                        tearing_  = false;
};

} // namespace freikino::render
