#pragma once

#include "freikino/common/com.h"

#include <cstdint>

#include <d3d11.h>
#include <dxgi.h>

namespace freikino::render {

// Narrow enum of formats the render path understands. Extended as decode
// paths are plumbed through.
enum class PixelFormat {
    unknown,
    nv12,     // 8-bit 4:2:0 in two planes (Y, UV) — standard D3D11VA hw output
    p010,     // 10-bit 4:2:0 HDR
    bgra8,    // software-decoded fallback, presentation-ready
};

// A single decoded video frame. Owns a D3D11 texture via ComPtr; the producer
// (decoder thread) hands off refcount to the consumer (render thread) through
// a frame queue. Pixel data stays on the GPU.
struct VideoFrame {
    ComPtr<ID3D11Texture2D> texture;

    // For texture arrays (D3D11VA decoder pools often hand back a single
    // Texture2DArray with per-frame slice indices). Zero when the texture is
    // a standalone 2D resource.
    UINT        array_slice = 0;

    PixelFormat format      = PixelFormat::unknown;
    UINT        width       = 0;
    UINT        height      = 0;

    // Presentation timestamp and nominal duration in nanoseconds on a
    // monotonic timeline rooted at the stream's first decoded frame.
    int64_t     pts_ns      = 0;
    int64_t     duration_ns = 0;

    // Colour-space hints from the decoder. Populated when available; the
    // render path uses them to pick the YUV→RGB matrix and transfer function.
    // Values follow the FFmpeg AVCOL_* numbering (ITU-R BT series).
    int         color_primaries = 0;
    int         color_transfer  = 0;
    int         color_space     = 0;
    int         color_range     = 0;

    bool valid() const noexcept { return texture != nullptr; }
};

} // namespace freikino::render
