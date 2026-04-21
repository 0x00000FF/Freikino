#include "freikino/render/overlay_renderer.h"

#include "freikino/common/error.h"
#include "freikino/common/log.h"

#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <dxgi.h>
#include <dxgi1_2.h>

namespace freikino::render {

void OverlayRenderer::create(ID3D11Device* d3d)
{
    if (d3d == nullptr) {
        throw_hresult(E_INVALIDARG);
    }
    if (d2d_ctx_) {
        throw_hresult(E_UNEXPECTED);
    }

    D2D1_FACTORY_OPTIONS fopts{};
#ifndef NDEBUG
    fopts.debugLevel = D2D1_DEBUG_LEVEL_INFORMATION;
#endif

    check_hr(::D2D1CreateFactory(
        D2D1_FACTORY_TYPE_SINGLE_THREADED,
        __uuidof(ID2D1Factory1),
        &fopts,
        reinterpret_cast<void**>(d2d_factory_.GetAddressOf())));

    ComPtr<IDXGIDevice> dxgi_device;
    check_hr(d3d->QueryInterface(IID_PPV_ARGS(&dxgi_device)));

    check_hr(d2d_factory_->CreateDevice(dxgi_device.Get(), &d2d_device_));
    check_hr(d2d_device_->CreateDeviceContext(
        D2D1_DEVICE_CONTEXT_OPTIONS_NONE, &d2d_ctx_));

    check_hr(::DWriteCreateFactory(
        DWRITE_FACTORY_TYPE_SHARED,
        __uuidof(IDWriteFactory),
        reinterpret_cast<IUnknown**>(dwrite_.GetAddressOf())));
}

void OverlayRenderer::attach_swap_chain(IDXGISwapChain1* swap)
{
    swap_.Reset();
    target_.Reset();
    cached_w_ = 0;
    cached_h_ = 0;
    if (swap != nullptr) {
        swap_ = swap;
    }
}

ID2D1DeviceContext* OverlayRenderer::begin_draw()
{
    if (!swap_ || !d2d_ctx_) {
        return nullptr;
    }

    ComPtr<IDXGISurface> surface;
    const HRESULT gb = swap_->GetBuffer(0, IID_PPV_ARGS(&surface));
    if (FAILED(gb)) {
        log::warn("overlay: GetBuffer failed 0x{:08X}",
                  static_cast<unsigned>(gb));
        return nullptr;
    }

    DXGI_SURFACE_DESC desc{};
    if (FAILED(surface->GetDesc(&desc))) {
        return nullptr;
    }

    if (!target_ || desc.Width != cached_w_ || desc.Height != cached_h_) {
        target_.Reset();

        D2D1_BITMAP_PROPERTIES1 props{};
        props.pixelFormat.format    = DXGI_FORMAT_R8G8B8A8_UNORM;
        props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
        props.bitmapOptions =
            D2D1_BITMAP_OPTIONS_TARGET | D2D1_BITMAP_OPTIONS_CANNOT_DRAW;
        props.dpiX = 96.0f;
        props.dpiY = 96.0f;

        const HRESULT cb = d2d_ctx_->CreateBitmapFromDxgiSurface(
            surface.Get(), &props, &target_);
        if (FAILED(cb)) {
            log::warn("overlay: CreateBitmapFromDxgiSurface failed 0x{:08X}",
                      static_cast<unsigned>(cb));
            return nullptr;
        }
        cached_w_ = desc.Width;
        cached_h_ = desc.Height;
    }

    d2d_ctx_->SetTarget(target_.Get());
    d2d_ctx_->BeginDraw();
    in_draw_ = true;
    return d2d_ctx_.Get();
}

void OverlayRenderer::end_draw() noexcept
{
    if (!in_draw_ || !d2d_ctx_) {
        return;
    }
    in_draw_ = false;

    const HRESULT hr = d2d_ctx_->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
        target_.Reset();
        cached_w_ = 0;
        cached_h_ = 0;
    } else if (FAILED(hr)) {
        log::warn("overlay: EndDraw failed 0x{:08X}",
                  static_cast<unsigned>(hr));
    }
    d2d_ctx_->SetTarget(nullptr);
}

void OverlayRenderer::invalidate_target() noexcept
{
    if (d2d_ctx_) {
        d2d_ctx_->SetTarget(nullptr);
    }
    target_.Reset();
    cached_w_ = 0;
    cached_h_ = 0;
}

} // namespace freikino::render
