#include "freikino/render/video_pipeline.h"

#include "freikino/common/error.h"
#include "freikino/common/log.h"

#include <algorithm>
#include <cstring>

#include <d3d11.h>
#include <d3dcompiler.h>

namespace freikino::render {

namespace {

// ---- HLSL -------------------------------------------------------------------
//
// Fullscreen-triangle VS with no vertex buffer — vertices are synthesised
// from SV_VertexID.  PS samples the two NV12 planes and converts BT.709
// limited-range YCbCr to linear RGB.  Multiple colour paths (BT.601,
// BT.2020, full-range) will branch on a constant buffer in a later pass.

constexpr const char kYuvVS[] = R"hlsl(
struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

VSOut main(uint vid : SV_VertexID)
{
    VSOut o;
    o.uv  = float2((vid << 1) & 2, vid & 2);
    o.pos = float4(o.uv * float2(2.0, -2.0) + float2(-1.0, 1.0), 0.0, 1.0);
    return o;
}
)hlsl";

constexpr const char kYuvPS[] = R"hlsl(
Texture2D<float>  y_plane  : register(t0);
Texture2D<float2> uv_plane : register(t1);
SamplerState      samp     : register(s0);

struct VSOut {
    float4 pos : SV_POSITION;
    float2 uv  : TEXCOORD0;
};

float4 main(VSOut input) : SV_TARGET
{
    float  y  = y_plane.Sample(samp, input.uv).r;
    float2 uv = uv_plane.Sample(samp, input.uv).rg;

    // Limited-range normalisation.
    y  = saturate((y  - 16.0 / 255.0) * (255.0 / 219.0));
    uv = (uv - 128.0 / 255.0) * (255.0 / 224.0);

    // BT.709 YCbCr -> RGB (video-range)
    float3 rgb;
    rgb.r = y                 + 1.5748 * uv.y;
    rgb.g = y - 0.1873 * uv.x - 0.4681 * uv.y;
    rgb.b = y + 1.8556 * uv.x;

    return float4(saturate(rgb), 1.0);
}
)hlsl";

ComPtr<ID3DBlob> compile_shader(
    const char* src, std::size_t size, const char* entry, const char* profile)
{
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> errors;

    UINT flags = D3DCOMPILE_OPTIMIZATION_LEVEL3 | D3DCOMPILE_ENABLE_STRICTNESS;
#ifndef NDEBUG
    flags |= D3DCOMPILE_DEBUG | D3DCOMPILE_SKIP_OPTIMIZATION;
    flags &= ~static_cast<UINT>(D3DCOMPILE_OPTIMIZATION_LEVEL3);
#endif

    const HRESULT hr = ::D3DCompile(
        src, size,
        /* sourceName */ nullptr,
        /* defines */    nullptr,
        /* include */    nullptr,
        entry, profile,
        flags, 0,
        &code, &errors);

    if (FAILED(hr)) {
        if (errors != nullptr && errors->GetBufferSize() > 0) {
            const char* msg = static_cast<const char*>(errors->GetBufferPointer());
            log::error("shader compile: {}", msg != nullptr ? msg : "?");
        }
        throw_hresult(hr);
    }
    return code;
}

DXGI_FORMAT y_plane_format(DXGI_FORMAT nv_fmt) noexcept
{
    switch (nv_fmt) {
        case DXGI_FORMAT_NV12: return DXGI_FORMAT_R8_UNORM;
        case DXGI_FORMAT_P010: return DXGI_FORMAT_R16_UNORM;
        default:               return DXGI_FORMAT_UNKNOWN;
    }
}

DXGI_FORMAT uv_plane_format(DXGI_FORMAT nv_fmt) noexcept
{
    switch (nv_fmt) {
        case DXGI_FORMAT_NV12: return DXGI_FORMAT_R8G8_UNORM;
        case DXGI_FORMAT_P010: return DXGI_FORMAT_R16G16_UNORM;
        default:               return DXGI_FORMAT_UNKNOWN;
    }
}

} // namespace

void VideoPipeline::create(ID3D11Device* d3d)
{
    if (d3d == nullptr) {
        throw_hresult(E_INVALIDARG);
    }
    if (d3d_ != nullptr) {
        throw_hresult(E_UNEXPECTED);
    }
    d3d_ = d3d;

    const auto vs_blob = compile_shader(
        kYuvVS, sizeof(kYuvVS) - 1, "main", "vs_5_0");
    const auto ps_blob = compile_shader(
        kYuvPS, sizeof(kYuvPS) - 1, "main", "ps_5_0");

    check_hr(d3d->CreateVertexShader(
        vs_blob->GetBufferPointer(), vs_blob->GetBufferSize(),
        nullptr, &vs_));
    check_hr(d3d->CreatePixelShader(
        ps_blob->GetBufferPointer(), ps_blob->GetBufferSize(),
        nullptr, &ps_));

    // Linear sampler, clamp to edge (fractional UV on the frame border must
    // not wrap into the opposite edge).
    D3D11_SAMPLER_DESC sd{};
    sd.Filter         = D3D11_FILTER_MIN_MAG_LINEAR_MIP_POINT;
    sd.AddressU       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressV       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.AddressW       = D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MaxAnisotropy  = 1;
    sd.ComparisonFunc = D3D11_COMPARISON_NEVER;
    sd.MinLOD         = 0.0f;
    sd.MaxLOD         = D3D11_FLOAT32_MAX;
    check_hr(d3d->CreateSamplerState(&sd, &sampler_));

    D3D11_RASTERIZER_DESC rd{};
    rd.FillMode              = D3D11_FILL_SOLID;
    rd.CullMode              = D3D11_CULL_NONE;
    rd.FrontCounterClockwise = FALSE;
    rd.DepthClipEnable       = TRUE;
    rd.ScissorEnable         = FALSE;
    rd.MultisampleEnable     = FALSE;
    rd.AntialiasedLineEnable = FALSE;
    check_hr(d3d->CreateRasterizerState(&rd, &rs_));

    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable           = FALSE;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    check_hr(d3d->CreateBlendState(&bd, &bs_));

    D3D11_DEPTH_STENCIL_DESC dsd{};
    dsd.DepthEnable   = FALSE;
    dsd.StencilEnable = FALSE;
    check_hr(d3d->CreateDepthStencilState(&dsd, &dss_));
}

void VideoPipeline::set_frame(VideoFrame frame)
{
    if (!frame.valid()) {
        current_     = {};
        y_srv_.Reset();
        uv_srv_.Reset();
        cached_tex_  = nullptr;
        return;
    }
    current_ = std::move(frame);
}

void VideoPipeline::ensure_srvs()
{
    if (!current_.valid()) {
        return;
    }
    ID3D11Texture2D* tex = current_.texture.Get();
    if (tex == cached_tex_ && y_srv_ != nullptr && uv_srv_ != nullptr) {
        return;
    }
    cached_tex_ = tex;

    D3D11_TEXTURE2D_DESC td{};
    tex->GetDesc(&td);

    const DXGI_FORMAT y_fmt  = y_plane_format(td.Format);
    const DXGI_FORMAT uv_fmt = uv_plane_format(td.Format);
    if (y_fmt == DXGI_FORMAT_UNKNOWN || uv_fmt == DXGI_FORMAT_UNKNOWN) {
        log::error(
            "video_pipeline: unsupported texture format {}",
            static_cast<int>(td.Format));
        throw_hresult(E_NOTIMPL);
    }

    D3D11_SHADER_RESOURCE_VIEW_DESC yd{};
    yd.Format                    = y_fmt;
    yd.ViewDimension             = D3D11_SRV_DIMENSION_TEXTURE2D;
    yd.Texture2D.MostDetailedMip = 0;
    yd.Texture2D.MipLevels       = 1;

    D3D11_SHADER_RESOURCE_VIEW_DESC ud = yd;
    ud.Format = uv_fmt;

    y_srv_.Reset();
    uv_srv_.Reset();
    check_hr(d3d_->CreateShaderResourceView(tex, &yd, &y_srv_));
    check_hr(d3d_->CreateShaderResourceView(tex, &ud, &uv_srv_));
}

void VideoPipeline::draw(
    ID3D11DeviceContext* ctx,
    ID3D11RenderTargetView* rtv,
    UINT dst_width,
    UINT dst_height)
{
    if (ctx == nullptr || rtv == nullptr) {
        return;
    }
    if (!current_.valid() || vs_ == nullptr || ps_ == nullptr) {
        return;
    }
    if (dst_width == 0 || dst_height == 0) {
        return;
    }

    ensure_srvs();

    // Aspect-correct viewport inside the destination. Black outside it is
    // provided by the RTV clear done before this draw.
    const float src_w = static_cast<float>(current_.width);
    const float src_h = static_cast<float>(current_.height);
    const float dst_w = static_cast<float>(dst_width);
    const float dst_h = static_cast<float>(dst_height);

    float draw_w = dst_w;
    float draw_h = dst_h;
    float draw_x = 0.0f;
    float draw_y = 0.0f;
    if (src_w > 0.0f && src_h > 0.0f) {
        const float src_aspect = src_w / src_h;
        const float dst_aspect = dst_w / dst_h;
        if (src_aspect > dst_aspect) {
            draw_w = dst_w;
            draw_h = dst_w / src_aspect;
            draw_x = 0.0f;
            draw_y = (dst_h - draw_h) * 0.5f;
        } else {
            draw_h = dst_h;
            draw_w = dst_h * src_aspect;
            draw_x = (dst_w - draw_w) * 0.5f;
            draw_y = 0.0f;
        }
    }

    D3D11_VIEWPORT vp{};
    vp.TopLeftX = draw_x;
    vp.TopLeftY = draw_y;
    vp.Width    = draw_w;
    vp.Height   = draw_h;
    vp.MinDepth = 0.0f;
    vp.MaxDepth = 1.0f;
    ctx->RSSetViewports(1, &vp);

    // Bind pipeline state.
    ctx->IASetInputLayout(nullptr);
    ctx->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx->IASetVertexBuffers(0, 0, nullptr, nullptr, nullptr);
    ctx->IASetIndexBuffer(nullptr, DXGI_FORMAT_UNKNOWN, 0);

    ctx->VSSetShader(vs_.Get(), nullptr, 0);
    ctx->PSSetShader(ps_.Get(), nullptr, 0);
    ctx->GSSetShader(nullptr, nullptr, 0);
    ctx->HSSetShader(nullptr, nullptr, 0);
    ctx->DSSetShader(nullptr, nullptr, 0);

    ID3D11ShaderResourceView* srvs[2] = { y_srv_.Get(), uv_srv_.Get() };
    ctx->PSSetShaderResources(0, 2, srvs);
    ID3D11SamplerState* samplers[1] = { sampler_.Get() };
    ctx->PSSetSamplers(0, 1, samplers);

    ctx->RSSetState(rs_.Get());

    constexpr float kBlendFactor[4] = { 0.0f, 0.0f, 0.0f, 0.0f };
    ctx->OMSetBlendState(bs_.Get(), kBlendFactor, 0xFFFFFFFFu);
    ctx->OMSetDepthStencilState(dss_.Get(), 0);

    ID3D11RenderTargetView* rtvs[1] = { rtv };
    ctx->OMSetRenderTargets(1, rtvs, nullptr);

    ctx->Draw(3, 0);

    // Unbind the SRVs so the engine's hazard tracking doesn't complain when
    // the same texture is later used as a copy destination.
    ID3D11ShaderResourceView* null_srvs[2] = { nullptr, nullptr };
    ctx->PSSetShaderResources(0, 2, null_srvs);
}

} // namespace freikino::render
