#pragma once

#include "freikino/common/com.h"

#include <cstdint>

#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>
#include <windows.h>

namespace freikino::render { class OverlayRenderer; }
namespace freikino::media  { class ThumbnailSource; }

namespace freikino::app {

class PlaybackController;

// Bottom-of-window transport bar. Auto-hides after ~2.5 s of mouse inactivity
// (kept visible while paused or while the cursor is over the bar). Hosts a
// play/pause button, a scrub bar, and a time readout.
//
// Not thread-safe. All methods run on the UI thread.
class TransportOverlay {
public:
    TransportOverlay() noexcept = default;
    ~TransportOverlay() = default;

    TransportOverlay(const TransportOverlay&)            = delete;
    TransportOverlay& operator=(const TransportOverlay&) = delete;
    TransportOverlay(TransportOverlay&&)                 = delete;
    TransportOverlay& operator=(TransportOverlay&&)      = delete;

    // Allocate D2D resources (brushes, text format, geometries) from the
    // renderer. The renderer + its underlying D2D context must outlive this
    // overlay.
    void create(render::OverlayRenderer& renderer);

    void set_playback(PlaybackController* pc) noexcept { playback_ = pc; }

    // Hook for the fullscreen button. MainWindow registers a
    // function + opaque user pointer; clicking the button invokes
    // it. Kept as a raw callback rather than a MainWindow* so the
    // overlay doesn't take a direct dependency on MainWindow.
    using FullscreenToggleFn = void(*)(void* user);
    void set_fullscreen_toggle(FullscreenToggleFn fn, void* user) noexcept
    {
        fs_toggle_     = fn;
        fs_toggle_user_ = user;
    }

    // Optional thumbnail provider. When set AND the mouse hovers over
    // the scrub bar, the overlay requests previews at the hover time
    // and draws the most recent one above the bar. Null = no preview
    // (e.g. file has no video stream or thumbnail decoder failed).
    void set_thumbnail_source(media::ThumbnailSource* src) noexcept
    {
        thumbnail_source_ = src;
    }

    // Mouse input, all in client-area pixel coordinates.
    void on_mouse_move(int x, int y, UINT width, UINT height) noexcept;
    void on_lbutton_down(int x, int y, UINT width, UINT height) noexcept;
    void on_lbutton_up(int x, int y, UINT width, UINT height) noexcept;
    void on_mouse_leave() noexcept;

    // Returns true while the overlay is mid-drag on the scrub bar or the
    // volume slider, so the window knows to SetCapture / ReleaseCapture
    // appropriately.
    [[nodiscard]] bool wants_mouse_capture() const noexcept
    {
        return seek_dragging_ || volume_dragging_;
    }

    // Public hook so the message window can keep the bar visible on
    // non-mouse activity (volume keys, pause toggle, etc.).
    void bump_activity() noexcept { last_activity_ms_ = ::GetTickCount64(); }

    // Called from the renderer's overlay callback. Handles auto-hide
    // animation internally; if the overlay is invisible the method is a
    // near-no-op.
    void draw(ID2D1DeviceContext* ctx, UINT width, UINT height) noexcept;

private:
    struct Layout {
        D2D1_RECT_F   bar;
        D2D1_RECT_F   play_button;
        D2D1_POINT_2F play_center;
        D2D1_RECT_F   stop_button;
        D2D1_POINT_2F stop_center;
        D2D1_RECT_F   seek_track;
        D2D1_RECT_F   seek_hit;
        float         seek_x0;
        float         seek_x1;
        float         seek_y;
        D2D1_RECT_F   fs_button;
        D2D1_POINT_2F fs_center;
        D2D1_RECT_F   volume_button;
        D2D1_POINT_2F volume_center;
        // Vertical volume slider popup. `volume_popup_hit` is the mouse
        // hit region (includes the speaker so hovering doesn't jitter);
        // `volume_popup_panel` is the drawn rectangle above the speaker.
        D2D1_RECT_F   volume_popup_hit;
        D2D1_RECT_F   volume_popup_panel;
        D2D1_RECT_F   volume_slider_track;
        float         volume_slider_x;
        float         volume_slider_y0;
        float         volume_slider_y1;
        D2D1_RECT_F   time_text;
    };

    // Upload / refresh the cached D2D bitmap for the current thumbnail.
    // Called from `draw()` — must run on the render thread since it
    // touches the D2D device context.
    void refresh_thumb_bitmap(ID2D1DeviceContext* ctx) noexcept;

    Layout compute_layout(UINT w, UINT h) const noexcept;

    void   tick_animation() noexcept;
    bool   is_paused() const noexcept;
    bool   has_source() const noexcept;
    float  current_progress() const noexcept;          // 0..1 (clamped)
    float  position_on_bar(int x, const Layout& l) const noexcept;
    static bool hit(const D2D1_RECT_F& r, int x, int y) noexcept;

    PlaybackController* playback_ = nullptr;

    // Cached D2D resources.
    ComPtr<ID2D1SolidColorBrush> brush_bg_;
    ComPtr<ID2D1SolidColorBrush> brush_track_;
    ComPtr<ID2D1SolidColorBrush> brush_progress_;
    ComPtr<ID2D1SolidColorBrush> brush_icon_;
    ComPtr<ID2D1SolidColorBrush> brush_text_;
    ComPtr<ID2D1PathGeometry>    play_geo_;
    ComPtr<IDWriteTextFormat>    text_format_;
    // Dedicated format for the hover-time label drawn above the
    // scrub thumbnail. Center-aligned so `position_on_bar` can hand
    // in the cursor x as the midpoint.
    ComPtr<IDWriteTextFormat>    text_thumb_time_;

    // Input state.
    int   mouse_x_         = -1;
    int   mouse_y_         = -1;
    bool  hover_play_      = false;
    bool  hover_stop_      = false;
    bool  hover_fs_        = false;
    bool  hover_seek_      = false;
    bool  hover_volume_    = false;   // over speaker OR popup panel
    bool  volume_dragging_ = false;   // mouse-down inside slider track
    bool  seek_dragging_   = false;
    float drag_progress_   = 0.0f;

    FullscreenToggleFn fs_toggle_      = nullptr;
    void*              fs_toggle_user_ = nullptr;
    // Last hover time (in stream pts) we asked the thumbnail decoder
    // for. Used so we don't re-request while the mouse is sitting on
    // the same spot.
    int64_t last_thumb_request_ns_ = INT64_MIN;

    // Cached D2D bitmap for the current thumbnail, keyed by its pts so
    // we only re-upload the BGRA buffer when a newer preview arrives.
    ComPtr<ID2D1Bitmap>   thumb_bitmap_;
    int64_t               thumb_bitmap_pts_ = -1;
    ComPtr<ID2D1SolidColorBrush> brush_thumb_border_;

    media::ThumbnailSource*      thumbnail_source_ = nullptr;

    // Animation state.
    float     alpha_             = 0.0f;
    ULONGLONG last_activity_ms_  = 0;
    ULONGLONG last_tick_ms_      = 0;
};

} // namespace freikino::app
