#pragma once

#include "freikino/common/com.h"
#include "freikino/render/video_frame.h"

#include <d3d11.h>

namespace freikino::render {

// Fixed graphics pipeline that samples an NV12 (or P010) texture through a
// YUV→RGB shader and draws the result as a fullscreen, aspect-correct quad.
//
// The pipeline does not own frames — it holds a transient copy of the most
// recent frame's texture via ComPtr so that it can redraw the same content
// across multiple Present calls (paused playback, occluded window, UI-only
// repaints). Frames arrive through `set_frame`.
class VideoPipeline {
public:
    VideoPipeline() noexcept = default;
    ~VideoPipeline() = default;

    VideoPipeline(const VideoPipeline&)            = delete;
    VideoPipeline& operator=(const VideoPipeline&) = delete;
    VideoPipeline(VideoPipeline&&)                 = delete;
    VideoPipeline& operator=(VideoPipeline&&)      = delete;

    // Compile shaders, allocate sampler / rasteriser / blend state.
    void create(ID3D11Device* d3d);

    // Replaces the frame to be drawn on the next `draw` call. Setting an
    // invalid frame (no texture) clears the current frame; subsequent draws
    // are no-ops.
    void set_frame(VideoFrame frame);

    // Issues the draw into `rtv`. The RTV must already be bound for output.
    // `dst_width` / `dst_height` are the backing render target's dimensions
    // in pixels; the pipeline computes an aspect-correct viewport inside it.
    void draw(ID3D11DeviceContext* ctx,
              ID3D11RenderTargetView* rtv,
              UINT dst_width,
              UINT dst_height);

    // True once `create` has succeeded and `set_frame` has been given a
    // valid frame at least once.
    bool has_frame() const noexcept { return current_.valid(); }

private:
    void ensure_srvs();

    ID3D11Device* d3d_ = nullptr; // non-owning

    ComPtr<ID3D11VertexShader>    vs_;
    ComPtr<ID3D11PixelShader>     ps_;
    ComPtr<ID3D11SamplerState>    sampler_;
    ComPtr<ID3D11RasterizerState> rs_;
    ComPtr<ID3D11BlendState>      bs_;
    ComPtr<ID3D11DepthStencilState> dss_;

    VideoFrame                       current_;
    ComPtr<ID3D11ShaderResourceView> y_srv_;
    ComPtr<ID3D11ShaderResourceView> uv_srv_;
    ID3D11Texture2D*                 cached_tex_ = nullptr; // identity-only, not refcounted
};

} // namespace freikino::render
