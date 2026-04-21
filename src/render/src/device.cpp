#include "freikino/render/device.h"

#include "freikino/common/error.h"
#include "freikino/common/log.h"

#include <array>

#include <d3d11.h>

namespace freikino::render {

namespace {

ComPtr<IDXGIAdapter1> pick_adapter(IDXGIFactory2* factory)
{
    // Prefer a high-performance adapter (discrete GPU) when available.
    ComPtr<IDXGIFactory6> factory6;
    if (SUCCEEDED(factory->QueryInterface(IID_PPV_ARGS(&factory6)))) {
        for (UINT i = 0;; ++i) {
            ComPtr<IDXGIAdapter1> adapter;
            const HRESULT hr = factory6->EnumAdapterByGpuPreference(
                i,
                DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE,
                IID_PPV_ARGS(&adapter));
            if (hr == DXGI_ERROR_NOT_FOUND) {
                break;
            }
            check_hr(hr);

            DXGI_ADAPTER_DESC1 desc{};
            if (SUCCEEDED(adapter->GetDesc1(&desc))
                && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
                return adapter;
            }
        }
    }

    // Fallback: first non-software adapter in default enumeration order.
    for (UINT i = 0;; ++i) {
        ComPtr<IDXGIAdapter1> adapter;
        const HRESULT hr = factory->EnumAdapters1(i, &adapter);
        if (hr == DXGI_ERROR_NOT_FOUND) {
            break;
        }
        check_hr(hr);

        DXGI_ADAPTER_DESC1 desc{};
        if (SUCCEEDED(adapter->GetDesc1(&desc))
            && (desc.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) == 0) {
            return adapter;
        }
    }
    throw_hresult(DXGI_ERROR_NOT_FOUND);
}

bool query_tearing_support(IDXGIFactory2* factory) noexcept
{
    ComPtr<IDXGIFactory5> factory5;
    if (FAILED(factory->QueryInterface(IID_PPV_ARGS(&factory5)))) {
        return false;
    }
    BOOL allow_tearing = FALSE;
    if (FAILED(factory5->CheckFeatureSupport(
            DXGI_FEATURE_PRESENT_ALLOW_TEARING,
            &allow_tearing, sizeof(allow_tearing)))) {
        return false;
    }
    return allow_tearing == TRUE;
}

} // namespace

void Device::create(bool enable_debug)
{
    UINT factory_flags = 0;
    if (enable_debug) {
        factory_flags |= DXGI_CREATE_FACTORY_DEBUG;
    }

    HRESULT hr = ::CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_));
    if (FAILED(hr) && (factory_flags & DXGI_CREATE_FACTORY_DEBUG)) {
        // SDK debug layer not installed — retry without it.
        factory_flags &= ~static_cast<UINT>(DXGI_CREATE_FACTORY_DEBUG);
        hr = ::CreateDXGIFactory2(factory_flags, IID_PPV_ARGS(&factory_));
    }
    check_hr(hr);

    tearing_ = query_tearing_support(factory_.Get());

    adapter_ = pick_adapter(factory_.Get());

    UINT device_flags =
        D3D11_CREATE_DEVICE_BGRA_SUPPORT |
        D3D11_CREATE_DEVICE_VIDEO_SUPPORT;
    if (enable_debug) {
        device_flags |= D3D11_CREATE_DEVICE_DEBUG;
    }

    constexpr std::array levels{
        D3D_FEATURE_LEVEL_11_1,
        D3D_FEATURE_LEVEL_11_0,
        D3D_FEATURE_LEVEL_10_1,
        D3D_FEATURE_LEVEL_10_0,
    };

    hr = ::D3D11CreateDevice(
        adapter_.Get(),
        D3D_DRIVER_TYPE_UNKNOWN, // required when a specific adapter is passed
        /* software module */ nullptr,
        device_flags,
        levels.data(),
        static_cast<UINT>(levels.size()),
        D3D11_SDK_VERSION,
        &d3d_,
        &feature_level_,
        &ctx_);

    if (hr == E_INVALIDARG && (device_flags & D3D11_CREATE_DEVICE_DEBUG)) {
        device_flags &= ~static_cast<UINT>(D3D11_CREATE_DEVICE_DEBUG);
        hr = ::D3D11CreateDevice(
            adapter_.Get(),
            D3D_DRIVER_TYPE_UNKNOWN,
            nullptr,
            device_flags,
            levels.data(),
            static_cast<UINT>(levels.size()),
            D3D11_SDK_VERSION,
            &d3d_,
            &feature_level_,
            &ctx_);
    }
    check_hr(hr);

    // Enable multithread protection on the immediate context so decoder
    // threads can submit commands without racing.
    ComPtr<ID3D10Multithread> mt;
    if (SUCCEEDED(ctx_.As(&mt))) {
        mt->SetMultithreadProtected(TRUE);
    }

    // Low input latency: one frame in flight maximum from the DXGI side.
    ComPtr<IDXGIDevice1> dxgi_device;
    if (SUCCEEDED(d3d_.As(&dxgi_device))) {
        (void)dxgi_device->SetMaximumFrameLatency(1);
    }

    DXGI_ADAPTER_DESC1 desc{};
    if (SUCCEEDED(adapter_->GetDesc1(&desc))) {
        log::info(
            "D3D11 device: adapter={:x}:{:x}, feature_level=0x{:x}, tearing={}",
            static_cast<unsigned>(desc.VendorId),
            static_cast<unsigned>(desc.DeviceId),
            static_cast<unsigned>(feature_level_),
            tearing_ ? "yes" : "no");
    }
}

} // namespace freikino::render
