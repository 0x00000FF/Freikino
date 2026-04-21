#include "freikino/render/presenter.h"

#include "freikino/common/error.h"
#include "freikino/common/log.h"

#include <cstdint>

#include <windows.h>

namespace freikino::render {

namespace {

constexpr float kIdleClear[4]  = { 0.020f, 0.020f, 0.024f, 1.0f };
constexpr float kVideoClear[4] = { 0.0f,   0.0f,   0.0f,   1.0f };

// Present a frame a touch early rather than a touch late — ≤ half a vsync
// at 60 Hz. Frames more than `kLateWindowNs` behind are dropped silently.
constexpr int64_t kEarlyWindowNs = 8'000'000;   //  8 ms
constexpr int64_t kLateWindowNs  = 66'000'000;  // ~2 frames at 30 fps

} // namespace

void Presenter::create(HWND hwnd, UINT width, UINT height, bool enable_d3d_debug)
{
    device_.create(enable_d3d_debug);
    swap_.create(device_, hwnd, width, height);
    video_pipeline_.create(device_.d3d());
}

void Presenter::resize(UINT width, UINT height)
{
    swap_.resize(width, height);
}

void Presenter::drop_lookahead() noexcept
{
    pending_ = {};
}

void Presenter::flush_video_state() noexcept
{
    pending_ = {};
    video_pipeline_.set_frame({});
}

void Presenter::render()
{
    if (!ready()) {
        return;
    }

    // Heartbeat — render() runs only on the UI thread, so plain integers
    // suffice. Flushed once a second so we can see whether this thread is
    // alive and how many frames reached the pipeline.
    if (hb_last_tick_ == 0) {
        hb_last_tick_ = ::GetTickCount64();
    }
    ++hb_calls_;
    {
        const ULONGLONG now_ms = ::GetTickCount64();
        if (now_ms - hb_last_tick_ >= 1000) {
            const int64_t clk = clock_ != nullptr ? clock_->now_ns() : -1;
            log::info(
                "render hb: calls={} displayed={} held_pending={} "
                "has_current={} clock_ms={}",
                hb_calls_, hb_displayed_, hb_held_,
                video_pipeline_.has_frame() ? 1 : 0,
                clk / 1'000'000);
            hb_calls_ = 0;
            hb_displayed_ = 0;
            hb_held_ = 0;
            hb_last_tick_ = now_ms;
        }
    }

    if (source_ != nullptr) {
        if (clock_ != nullptr) {
            const int64_t now = clock_->now_ns();

            // If there's a held future frame and it's due, promote.
            for (;;) {
                if (!pending_.valid()) {
                    VideoFrame candidate{};
                    if (!source_->try_acquire_video_frame(candidate)) {
                        break;
                    }
                    pending_ = std::move(candidate);
                }

                const int64_t pts = pending_.pts_ns;
                if (pts <= now + kEarlyWindowNs) {
                    // Drop anything older than one frame interval behind —
                    // don't waste a draw on a frame the user won't perceive.
                    if (pts + kLateWindowNs < now) {
                        pending_ = {};
                        continue;
                    }
                    video_pipeline_.set_frame(std::move(pending_));
                    pending_ = {};
                    ++hb_displayed_;
                    displayed_frames_.fetch_add(1, std::memory_order_relaxed);
                    // Loop to see if an even fresher frame is already waiting.
                    continue;
                }
                // Held frame is still in the future; stop draining.
                ++hb_held_;
                break;
            }
        } else {
            // No clock attached yet — fall back to "display the freshest
            // available frame each tick". Used during bootstrap or when no
            // source/clock has been wired up by the app layer.
            VideoFrame candidate{};
            while (source_->try_acquire_video_frame(candidate)) {
                video_pipeline_.set_frame(std::move(candidate));
            }
        }
    }

    ID3D11DeviceContext*    ctx = device_.context();
    ID3D11RenderTargetView* rtv = swap_.rtv();

    ctx->ClearRenderTargetView(
        rtv,
        video_pipeline_.has_frame() ? kVideoClear : kIdleClear);

    if (video_pipeline_.has_frame()) {
        video_pipeline_.draw(ctx, rtv, swap_.width(), swap_.height());
    } else {
        D3D11_VIEWPORT vp{};
        vp.Width    = static_cast<float>(swap_.width());
        vp.Height   = static_cast<float>(swap_.height());
        vp.MinDepth = 0.0f;
        vp.MaxDepth = 1.0f;
        ctx->RSSetViewports(1, &vp);

        ID3D11RenderTargetView* rtvs[1] = { rtv };
        ctx->OMSetRenderTargets(1, rtvs, nullptr);
    }

    // Overlay draws over the composed video. D2D uses the swap chain's back
    // buffer as its target; order on the CPU side guarantees GPU command
    // ordering: video → overlay → Present.
    if (overlay_cb_) {
        try {
            overlay_cb_(swap_.width(), swap_.height());
        } catch (const hresult_error& e) {
            log::error("overlay: 0x{:08X}", static_cast<unsigned>(e.code()));
        } catch (const std::exception& e) {
            log::error("overlay: {}", e.what());
        } catch (...) {
            log::error("overlay: unknown");
        }
    }

    swap_.present(/* vsync */ true);
}

} // namespace freikino::render
