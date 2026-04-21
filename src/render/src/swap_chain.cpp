#include "freikino/render/swap_chain.h"

#include "freikino/common/error.h"
#include "freikino/render/device.h"

namespace freikino::render {

namespace {

constexpr DXGI_FORMAT kBackBufferFormat = DXGI_FORMAT_R8G8B8A8_UNORM;

UINT swap_flags(bool tearing) noexcept
{
    UINT f = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    if (tearing) {
        f |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    }
    return f;
}

} // namespace

SwapChain::~SwapChain()
{
    close_waitable();
}

void SwapChain::close_waitable() noexcept
{
    if (waitable_ != nullptr) {
        ::CloseHandle(waitable_);
        waitable_ = nullptr;
    }
}

void SwapChain::create(Device& device, HWND hwnd, UINT width, UINT height)
{
    if (sc_) {
        throw_hresult(E_UNEXPECTED);
    }
    if (hwnd == nullptr) {
        throw_hresult(E_INVALIDARG);
    }
    if (width == 0) {
        width = 1;
    }
    if (height == 0) {
        height = 1;
    }

    d3d_     = device.d3d();
    tearing_ = device.tearing_supported();

    DXGI_SWAP_CHAIN_DESC1 desc{};
    desc.Width              = width;
    desc.Height             = height;
    desc.Format             = kBackBufferFormat;
    desc.Stereo             = FALSE;
    desc.SampleDesc.Count   = 1;
    desc.SampleDesc.Quality = 0;
    desc.BufferUsage        = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    desc.BufferCount        = 2;
    desc.Scaling            = DXGI_SCALING_STRETCH;
    desc.SwapEffect         = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    desc.AlphaMode          = DXGI_ALPHA_MODE_IGNORE;
    desc.Flags              = swap_flags(tearing_);

    ComPtr<IDXGISwapChain1> sc1;
    check_hr(device.factory()->CreateSwapChainForHwnd(
        d3d_, hwnd, &desc, /* fullscreen desc */ nullptr,
        /* restrict output */ nullptr, &sc1));

    // DXGI's built-in Alt+Enter toggle would clash with our own fullscreen
    // handling — disable it on this HWND.
    check_hr(device.factory()->MakeWindowAssociation(
        hwnd,
        DXGI_MWA_NO_WINDOW_CHANGES | DXGI_MWA_NO_ALT_ENTER));

    check_hr(sc1.As(&sc_));
    check_hr(sc_->SetMaximumFrameLatency(1));

    waitable_ = sc_->GetFrameLatencyWaitableObject();
    if (waitable_ == nullptr) {
        throw_last_error();
    }

    width_  = width;
    height_ = height;
    acquire_back_buffer();
}

void SwapChain::acquire_back_buffer()
{
    check_hr(sc_->GetBuffer(0, IID_PPV_ARGS(&back_)));

    D3D11_RENDER_TARGET_VIEW_DESC rtv_desc{};
    rtv_desc.Format        = kBackBufferFormat;
    rtv_desc.ViewDimension = D3D11_RTV_DIMENSION_TEXTURE2D;
    rtv_desc.Texture2D.MipSlice = 0;

    check_hr(d3d_->CreateRenderTargetView(back_.Get(), &rtv_desc, &rtv_));
}

void SwapChain::resize(UINT width, UINT height)
{
    if (!sc_) {
        return;
    }
    if (width == 0 || height == 0) {
        // Window is minimised; keep the current buffers and skip until a real
        // size returns.
        return;
    }
    if (width == width_ && height == height_) {
        return;
    }

    rtv_.Reset();
    back_.Reset();

    check_hr(sc_->ResizeBuffers(
        0, // keep buffer count
        width, height,
        DXGI_FORMAT_UNKNOWN, // keep format
        swap_flags(tearing_)));

    width_  = width;
    height_ = height;
    acquire_back_buffer();
}

void SwapChain::present(bool vsync)
{
    if (!sc_) {
        return;
    }
    const UINT sync_interval = vsync ? 1u : 0u;
    const UINT present_flags = (!vsync && tearing_)
        ? static_cast<UINT>(DXGI_PRESENT_ALLOW_TEARING)
        : 0u;

    const HRESULT hr = sc_->Present(sync_interval, present_flags);
    if (hr == DXGI_STATUS_OCCLUDED) {
        // Window is fully obscured; the next Present will unstall. Not an
        // error, but we report it so the driver can back off rendering.
        return;
    }
    check_hr(hr);
}

} // namespace freikino::render
