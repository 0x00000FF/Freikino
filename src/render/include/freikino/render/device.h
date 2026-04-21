#pragma once

#include "freikino/common/com.h"

#include <d3d11_2.h>
#include <dxgi1_6.h>

namespace freikino::render {

// Owns the DXGI factory, hardware adapter, D3D11 device and immediate context.
//
// The device is multithread-protected (ID3D10Multithread::SetMultithreadProtected)
// so that Media Foundation's video decoders and FFmpeg's hwaccel paths can
// submit work on their own threads without racing our render loop.
class Device {
public:
    Device() noexcept = default;
    ~Device() = default;

    Device(const Device&) = delete;
    Device& operator=(const Device&) = delete;
    Device(Device&&) = delete;
    Device& operator=(Device&&) = delete;

    // `enable_debug` opts into the SDK debug layer (D3D11_CREATE_DEVICE_DEBUG).
    // Silently downgrades if the optional SDK layers aren't installed.
    void create(bool enable_debug);

    ID3D11Device*        d3d()           const noexcept { return d3d_.Get(); }
    ID3D11DeviceContext* context()       const noexcept { return ctx_.Get(); }
    IDXGIFactory2*       factory()       const noexcept { return factory_.Get(); }
    IDXGIAdapter1*       adapter()       const noexcept { return adapter_.Get(); }
    D3D_FEATURE_LEVEL    feature_level() const noexcept { return feature_level_; }
    bool                 tearing_supported() const noexcept { return tearing_; }

private:
    ComPtr<IDXGIFactory2>       factory_;
    ComPtr<IDXGIAdapter1>       adapter_;
    ComPtr<ID3D11Device>        d3d_;
    ComPtr<ID3D11DeviceContext> ctx_;
    D3D_FEATURE_LEVEL           feature_level_ = D3D_FEATURE_LEVEL_11_0;
    bool                        tearing_       = false;
};

} // namespace freikino::render
