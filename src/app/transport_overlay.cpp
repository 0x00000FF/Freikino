#include "transport_overlay.h"

#include "freikino/common/error.h"
#include "freikino/common/log.h"
#include "freikino/media/thumbnail_source.h"
#include "freikino/render/overlay_renderer.h"
#include "playback.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <string>

#include <d2d1.h>
#include <d2d1_1.h>
#include <dwrite.h>

namespace freikino::app {

namespace {

// Auto-hide tuning.
constexpr ULONGLONG kIdleHideMs = 2500;
constexpr float     kFadeInPerMs  = 1.0f / 180.0f;   // ~180 ms to fully fade in
constexpr float     kFadeOutPerMs = 1.0f / 260.0f;   // ~260 ms to fade out

// Layout constants (physical pixels, no DPI scaling yet).
constexpr float kBarHeight   = 96.0f;
constexpr float kEdgePad     = 24.0f;
constexpr float kBtnSize     = 44.0f;
constexpr float kBtnGap      = 6.0f;
constexpr float kTimeWidth   = 140.0f;
constexpr float kSeekGap     = 16.0f;
constexpr float kTrackThick  = 4.0f;
constexpr float kSeekHitTopBias = 12.0f;
constexpr float kKnobRadius  = 8.0f;

// Volume popup (vertical slider) constants.
constexpr float kVolPopupW      = 40.0f;
constexpr float kVolPopupH      = 140.0f;
constexpr float kVolPopupGap    = 14.0f;   // gap between popup and speaker
constexpr float kVolSliderThick = 4.0f;
constexpr float kVolSliderPadY  = 14.0f;
constexpr float kVolKnobRadius  = 7.0f;

// Re-request a thumbnail only when the mouse has moved by at least
// this much in stream time. Keeps the thumbnail decoder from thrashing
// on every single mouse-move pixel during a drag.
constexpr int64_t  kThumbRequestMinDeltaNs = 150'000'000LL; // 150 ms

// Pixel dimensions for the thumbnail frame around the bitmap.
constexpr float kThumbBorder    = 2.0f;
constexpr float kThumbAboveBar  = 28.0f;   // gap above scrub bar
constexpr float kThumbEdgeGuard = 8.0f;    // minimum clearance from window edges

std::wstring format_time_ns(int64_t ns) noexcept
{
    if (ns < 0) ns = 0;
    const int64_t secs = ns / 1'000'000'000LL;
    const int64_t h    = secs / 3600;
    const int64_t m    = (secs % 3600) / 60;
    const int64_t s    = secs % 60;
    wchar_t buf[32] = {};
    int n;
    if (h > 0) {
        n = std::swprintf(buf, 32, L"%lld:%02lld:%02lld",
                          static_cast<long long>(h),
                          static_cast<long long>(m),
                          static_cast<long long>(s));
    } else {
        n = std::swprintf(buf, 32, L"%lld:%02lld",
                          static_cast<long long>(m),
                          static_cast<long long>(s));
    }
    if (n <= 0) return L"";
    return std::wstring{buf, static_cast<std::size_t>(n)};
}

} // namespace

// ---------------------------------------------------------------------------

void TransportOverlay::create(render::OverlayRenderer& renderer)
{
    auto* ctx = renderer.context();
    if (ctx == nullptr) {
        throw_hresult(E_UNEXPECTED);
    }

    // Brushes — opacity is set per draw via SetOpacity.
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(0.0f, 0.0f, 0.0f, 0.60f), &brush_bg_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.30f), &brush_track_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.00f), &brush_progress_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.00f), &brush_icon_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 1.00f), &brush_text_));
    check_hr(ctx->CreateSolidColorBrush(
        D2D1::ColorF(1.0f, 1.0f, 1.0f, 0.75f), &brush_thumb_border_));

    // DWrite text format for the time readout.
    auto* dw = renderer.dwrite();
    if (dw == nullptr) {
        throw_hresult(E_UNEXPECTED);
    }
    check_hr(dw->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        14.0f,
        L"en-us",
        &text_format_));
    text_format_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_TRAILING);
    text_format_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // Hover-time label above the scrub thumbnail.
    check_hr(dw->CreateTextFormat(
        L"Segoe UI", nullptr,
        DWRITE_FONT_WEIGHT_SEMI_BOLD,
        DWRITE_FONT_STYLE_NORMAL,
        DWRITE_FONT_STRETCH_NORMAL,
        13.0f,
        L"en-us",
        &text_thumb_time_));
    text_thumb_time_->SetTextAlignment(DWRITE_TEXT_ALIGNMENT_CENTER);
    text_thumb_time_->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);

    // Play triangle geometry (pause is a pair of rects — no geometry needed).
    auto* factory = renderer.d2d_factory();
    if (factory == nullptr) {
        throw_hresult(E_UNEXPECTED);
    }
    check_hr(factory->CreatePathGeometry(&play_geo_));
    ComPtr<ID2D1GeometrySink> sink;
    check_hr(play_geo_->Open(&sink));
    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
    // Equilateral-ish triangle pointing right, drawn centred on (0, 0).
    const float tw = 14.0f;
    const float th = 16.0f;
    sink->BeginFigure(
        D2D1::Point2F(-tw * 0.35f, -th * 0.5f),
        D2D1_FIGURE_BEGIN_FILLED);
    sink->AddLine(D2D1::Point2F(+tw * 0.65f, 0.0f));
    sink->AddLine(D2D1::Point2F(-tw * 0.35f, +th * 0.5f));
    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
    check_hr(sink->Close());

    last_tick_ms_ = ::GetTickCount64();
}

// ---------------------------------------------------------------------------

bool TransportOverlay::has_source() const noexcept
{
    return playback_ != nullptr
        && playback_->state() != PlaybackController::State::no_source;
}

bool TransportOverlay::is_paused() const noexcept
{
    return playback_ != nullptr
        && playback_->state() == PlaybackController::State::paused;
}

TransportOverlay::Layout
TransportOverlay::compute_layout(UINT w, UINT h) const noexcept
{
    Layout l{};
    const float fw = static_cast<float>(w);
    const float fh = static_cast<float>(h);

    l.bar.left   = 0.0f;
    l.bar.top    = fh - kBarHeight;
    l.bar.right  = fw;
    l.bar.bottom = fh;

    const float row_y = fh - kBarHeight * 0.5f;

    l.play_button.left   = kEdgePad;
    l.play_button.top    = row_y - kBtnSize * 0.5f;
    l.play_button.right  = kEdgePad + kBtnSize;
    l.play_button.bottom = row_y + kBtnSize * 0.5f;
    l.play_center        = D2D1::Point2F(
        kEdgePad + kBtnSize * 0.5f, row_y);

    l.stop_button.left   = l.play_button.right + kBtnGap;
    l.stop_button.top    = row_y - kBtnSize * 0.5f;
    l.stop_button.right  = l.stop_button.left + kBtnSize;
    l.stop_button.bottom = row_y + kBtnSize * 0.5f;
    l.stop_center        = D2D1::Point2F(
        (l.stop_button.left + l.stop_button.right) * 0.5f, row_y);

    l.time_text.right  = fw - kEdgePad;
    l.time_text.left   = fw - kEdgePad - kTimeWidth;
    l.time_text.top    = row_y - 12.0f;
    l.time_text.bottom = row_y + 12.0f;

    l.volume_button.right  = l.time_text.left - 8.0f;
    l.volume_button.left   = l.volume_button.right - kBtnSize;
    l.volume_button.top    = row_y - kBtnSize * 0.5f;
    l.volume_button.bottom = row_y + kBtnSize * 0.5f;
    l.volume_center        = D2D1::Point2F(
        (l.volume_button.left + l.volume_button.right) * 0.5f, row_y);

    // Vertical popup slider centred above the speaker.
    l.volume_popup_panel.left   = l.volume_center.x - kVolPopupW * 0.5f;
    l.volume_popup_panel.right  = l.volume_center.x + kVolPopupW * 0.5f;
    l.volume_popup_panel.bottom = l.volume_button.top - kVolPopupGap;
    l.volume_popup_panel.top    = l.volume_popup_panel.bottom - kVolPopupH;
    // Hit region spans the popup + the gap + the speaker so moving
    // between them doesn't unhover-dismiss the slider.
    l.volume_popup_hit = l.volume_popup_panel;
    l.volume_popup_hit.left   -= 4.0f;
    l.volume_popup_hit.right  += 4.0f;
    l.volume_popup_hit.bottom  = l.volume_button.bottom;

    l.volume_slider_x  = (l.volume_popup_panel.left + l.volume_popup_panel.right) * 0.5f;
    l.volume_slider_y0 = l.volume_popup_panel.top    + kVolSliderPadY;
    l.volume_slider_y1 = l.volume_popup_panel.bottom - kVolSliderPadY;
    l.volume_slider_track.left   = l.volume_slider_x - kVolSliderThick * 0.5f;
    l.volume_slider_track.right  = l.volume_slider_x + kVolSliderThick * 0.5f;
    l.volume_slider_track.top    = l.volume_slider_y0;
    l.volume_slider_track.bottom = l.volume_slider_y1;

    l.seek_x0 = l.stop_button.right + kSeekGap;
    l.seek_x1 = l.volume_button.left - kSeekGap;
    l.seek_y  = row_y;
    if (l.seek_x1 < l.seek_x0 + 40.0f) {
        // Too narrow for a scrub bar; collapse the hit region.
        l.seek_x1 = l.seek_x0;
    }

    l.seek_track.left   = l.seek_x0;
    l.seek_track.right  = l.seek_x1;
    l.seek_track.top    = l.seek_y - kTrackThick * 0.5f;
    l.seek_track.bottom = l.seek_y + kTrackThick * 0.5f;

    l.seek_hit.left   = l.seek_x0 - 4.0f;
    l.seek_hit.right  = l.seek_x1 + 4.0f;
    l.seek_hit.top    = l.seek_y - kSeekHitTopBias;
    l.seek_hit.bottom = l.seek_y + kSeekHitTopBias;

    return l;
}

bool TransportOverlay::hit(const D2D1_RECT_F& r, int x, int y) noexcept
{
    const float fx = static_cast<float>(x);
    const float fy = static_cast<float>(y);
    return fx >= r.left && fx <= r.right && fy >= r.top && fy <= r.bottom;
}

float TransportOverlay::position_on_bar(int x, const Layout& l) const noexcept
{
    const float span = l.seek_x1 - l.seek_x0;
    if (span <= 0.0f) {
        return 0.0f;
    }
    float t = (static_cast<float>(x) - l.seek_x0) / span;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    return t;
}

float TransportOverlay::current_progress() const noexcept
{
    if (seek_dragging_) {
        return drag_progress_;
    }
    if (playback_ == nullptr) {
        return 0.0f;
    }
    const int64_t dur = playback_->duration_ns();
    if (dur <= 0) {
        return 0.0f;
    }
    const int64_t cur = playback_->current_time_ns();
    if (cur <= 0) return 0.0f;
    if (cur >= dur) return 1.0f;
    return static_cast<float>(
        static_cast<double>(cur) / static_cast<double>(dur));
}

// ---------------------------------------------------------------------------

void TransportOverlay::on_mouse_move(int x, int y, UINT w, UINT h) noexcept
{
    mouse_x_ = x;
    mouse_y_ = y;
    last_activity_ms_ = ::GetTickCount64();
    const Layout l = compute_layout(w, h);
    hover_play_ = hit(l.play_button, x, y);
    hover_stop_ = hit(l.stop_button, x, y);
    // Volume hover is sticky across speaker <-> popup so the slider
    // stays up while the user reaches for it. Popup panel is only
    // included in the hit once the user is already hovering; that way
    // the panel isn't drawn until the user actually targets the icon.
    const bool on_speaker = hit(l.volume_button, x, y);
    const bool on_popup_hit = hit(l.volume_popup_hit, x, y);
    if (!hover_volume_) {
        hover_volume_ = on_speaker;
    } else {
        hover_volume_ = on_speaker || on_popup_hit || volume_dragging_;
    }
    hover_seek_ = hit(l.seek_hit, x, y);
    if (seek_dragging_) {
        drag_progress_ = position_on_bar(x, l);
    }
    if (volume_dragging_ && playback_ != nullptr) {
        const float span = l.volume_slider_y1 - l.volume_slider_y0;
        if (span > 0.0f) {
            float t = (l.volume_slider_y1 - static_cast<float>(y)) / span;
            if (t < 0.0f) t = 0.0f;
            if (t > 1.0f) t = 1.0f;
            playback_->set_volume(t);
        }
    }

    // Thumbnail request — fires on any hover over the scrub bar,
    // whether or not the user is dragging. Real seeks are intentionally
    // NOT issued during drag: on heavy content (4K HW-decoded), each
    // real seek tears down the decoder and triggers a multi-frame
    // keyframe catch-up. Firing that at mouse-move rate leaves playback
    // frozen and the scene un-updated. The thumbnail preview gives
    // scrub feedback without perturbing the playback decoder; the real
    // seek happens once, on mouse-up.
    if (thumbnail_source_ != nullptr
        && playback_ != nullptr
        && (hover_seek_ || seek_dragging_)) {
        const int64_t dur = playback_->duration_ns();
        if (dur > 0) {
            const float t = position_on_bar(x, l);
            const int64_t target = static_cast<int64_t>(
                static_cast<double>(t) * static_cast<double>(dur));
            if (last_thumb_request_ns_ == INT64_MIN
                || std::llabs(target - last_thumb_request_ns_)
                    >= kThumbRequestMinDeltaNs) {
                last_thumb_request_ns_ = target;
                thumbnail_source_->request(target);
            }
        }
    }
}

void TransportOverlay::on_lbutton_down(int x, int y, UINT w, UINT h) noexcept
{
    last_activity_ms_ = ::GetTickCount64();
    if (!has_source()) {
        return;
    }
    const Layout l = compute_layout(w, h);

    if (hit(l.play_button, x, y)) {
        try {
            playback_->toggle_pause();
        } catch (const std::exception& e) {
            log::error("transport play click: {}", e.what());
        } catch (...) {
            log::error("transport play click: unknown");
        }
        return;
    }

    if (hit(l.stop_button, x, y)) {
        try {
            playback_->stop();
        } catch (const std::exception& e) {
            log::error("transport stop click: {}", e.what());
        } catch (...) {
            log::error("transport stop click: unknown");
        }
        return;
    }

    // Volume slider drag — start when clicking inside the popup panel
    // (it's only visible while hover_volume_ is true, but we still
    // check the rect to be defensive).
    if (hover_volume_ && hit(l.volume_popup_panel, x, y)) {
        volume_dragging_ = true;
        if (playback_ != nullptr) {
            const float span = l.volume_slider_y1 - l.volume_slider_y0;
            if (span > 0.0f) {
                float t = (l.volume_slider_y1 - static_cast<float>(y)) / span;
                if (t < 0.0f) t = 0.0f;
                if (t > 1.0f) t = 1.0f;
                playback_->set_volume(t);
            }
        }
        return;
    }

    // Click on the speaker itself toggles mute.
    if (hit(l.volume_button, x, y)) {
        try {
            playback_->toggle_mute();
        } catch (const std::exception& e) {
            log::error("transport mute click: {}", e.what());
        } catch (...) {
            log::error("transport mute click: unknown");
        }
        return;
    }

    if (hit(l.seek_hit, x, y)) {
        seek_dragging_ = true;
        drag_progress_ = position_on_bar(x, l);
    }
}

void TransportOverlay::on_lbutton_up(int x, int y, UINT w, UINT h) noexcept
{
    (void)y; // lift is along the x-axis on the scrub bar
    last_activity_ms_ = ::GetTickCount64();
    if (seek_dragging_ && playback_ != nullptr) {
        const Layout l = compute_layout(w, h);
        const float  t = position_on_bar(x, l);
        const int64_t dur = playback_->duration_ns();
        if (dur > 0) {
            const int64_t target = static_cast<int64_t>(
                static_cast<double>(t) * static_cast<double>(dur));
            try {
                playback_->seek_to(target);
            } catch (const std::exception& e) {
                log::error("transport seek: {}", e.what());
            } catch (...) {
                log::error("transport seek: unknown");
            }
        }
    }
    seek_dragging_   = false;
    volume_dragging_ = false;
}

void TransportOverlay::on_mouse_leave() noexcept
{
    // Don't drop drag state on mouse-leave — SetCapture keeps tracking even
    // when the cursor exits the window.
    if (!seek_dragging_ && !volume_dragging_) {
        mouse_x_      = -1;
        mouse_y_      = -1;
        hover_play_   = false;
        hover_stop_   = false;
        hover_seek_   = false;
        hover_volume_ = false;
    }
}

void TransportOverlay::refresh_thumb_bitmap(ID2D1DeviceContext* ctx) noexcept
{
    if (thumbnail_source_ == nullptr || ctx == nullptr) {
        return;
    }
    const int64_t pts = thumbnail_source_->latest_pts();
    if (pts < 0 || pts == thumb_bitmap_pts_) {
        return;
    }
    media::ThumbnailSource::Frame f;
    if (!thumbnail_source_->peek_latest(f) || f.width <= 0 || f.height <= 0) {
        return;
    }

    thumb_bitmap_.Reset();

    D2D1_BITMAP_PROPERTIES props{};
    props.pixelFormat.format    = DXGI_FORMAT_B8G8R8A8_UNORM;
    props.pixelFormat.alphaMode = D2D1_ALPHA_MODE_IGNORE;
    props.dpiX = 96.0f;
    props.dpiY = 96.0f;

    const HRESULT hr = ctx->CreateBitmap(
        D2D1::SizeU(static_cast<UINT32>(f.width),
                    static_cast<UINT32>(f.height)),
        f.pixels.data(),
        static_cast<UINT32>(f.width) * 4,
        props,
        &thumb_bitmap_);
    if (FAILED(hr)) {
        thumb_bitmap_.Reset();
        thumb_bitmap_pts_ = -1;
        return;
    }
    thumb_bitmap_pts_ = pts;
}

// ---------------------------------------------------------------------------

void TransportOverlay::tick_animation() noexcept
{
    const ULONGLONG now_ms = ::GetTickCount64();
    const ULONGLONG dt = (last_tick_ms_ == 0)
        ? 16
        : (now_ms > last_tick_ms_ ? now_ms - last_tick_ms_ : 0);
    last_tick_ms_ = now_ms;

    const bool idle_fresh = (now_ms - last_activity_ms_) < kIdleHideMs;
    const bool target_visible =
        has_source()
        && (idle_fresh || is_paused() || seek_dragging_);

    if (target_visible) {
        alpha_ = (std::min)(1.0f, alpha_ + kFadeInPerMs  * static_cast<float>(dt));
    } else {
        alpha_ = (std::max)(0.0f, alpha_ - kFadeOutPerMs * static_cast<float>(dt));
    }
}

void TransportOverlay::draw(ID2D1DeviceContext* ctx, UINT w, UINT h) noexcept
{
    tick_animation();

    if (ctx == nullptr || alpha_ <= 0.001f) {
        return;
    }
    if (!has_source()) {
        return;
    }

    const Layout l = compute_layout(w, h);

    // 1. Darkening fill behind the bar. Subtle — we want the video visible
    //    through the scrim.
    brush_bg_->SetOpacity(alpha_ * 0.7f);
    ctx->FillRectangle(l.bar, brush_bg_.Get());

    // 2. Scrub bar track.
    brush_track_->SetOpacity(alpha_);
    ctx->FillRectangle(l.seek_track, brush_track_.Get());

    // 3. Progress portion.
    const float progress = current_progress();
    if (progress > 0.0f && l.seek_x1 > l.seek_x0) {
        D2D1_RECT_F prog = l.seek_track;
        prog.right = l.seek_x0 + (l.seek_x1 - l.seek_x0) * progress;
        brush_progress_->SetOpacity(alpha_);
        ctx->FillRectangle(prog, brush_progress_.Get());

        // Knob when hovering the bar or dragging.
        if ((hover_seek_ || seek_dragging_) && l.seek_x1 > l.seek_x0) {
            D2D1_ELLIPSE k;
            k.point.x = prog.right;
            k.point.y = l.seek_y;
            k.radiusX = kKnobRadius;
            k.radiusY = kKnobRadius;
            ctx->FillEllipse(k, brush_progress_.Get());
        }
    }

    // 4. Play / pause glyph.
    brush_icon_->SetOpacity(alpha_ * (hover_play_ ? 1.0f : 0.85f));

    if (is_paused()) {
        // Triangle icon. Translate around (0,0)-centred geometry.
        D2D1::Matrix3x2F m =
            D2D1::Matrix3x2F::Translation(l.play_center.x, l.play_center.y);
        ctx->SetTransform(m);
        ctx->FillGeometry(play_geo_.Get(), brush_icon_.Get());
        ctx->SetTransform(D2D1::Matrix3x2F::Identity());
    } else {
        // Two vertical bars.
        const float bw = 4.0f;
        const float bh = 16.0f;
        const float gap = 4.0f;
        D2D1_RECT_F a{
            l.play_center.x - gap * 0.5f - bw,
            l.play_center.y - bh * 0.5f,
            l.play_center.x - gap * 0.5f,
            l.play_center.y + bh * 0.5f,
        };
        D2D1_RECT_F b{
            l.play_center.x + gap * 0.5f,
            l.play_center.y - bh * 0.5f,
            l.play_center.x + gap * 0.5f + bw,
            l.play_center.y + bh * 0.5f,
        };
        ctx->FillRectangle(a, brush_icon_.Get());
        ctx->FillRectangle(b, brush_icon_.Get());
    }

    // 4b. Stop glyph — a filled square.
    brush_icon_->SetOpacity(alpha_ * (hover_stop_ ? 1.0f : 0.85f));
    {
        const float sq = 14.0f;
        const D2D1_RECT_F s{
            l.stop_center.x - sq * 0.5f,
            l.stop_center.y - sq * 0.5f,
            l.stop_center.x + sq * 0.5f,
            l.stop_center.y + sq * 0.5f,
        };
        ctx->FillRectangle(s, brush_icon_.Get());
    }

    // 5. Speaker / mute icon. A small trapezoid cone + three stubby
    //    bars to the right that represent sound waves. When muted we
    //    draw a single diagonal line across the cone instead.
    const bool muted = playback_ != nullptr && playback_->muted();
    brush_icon_->SetOpacity(alpha_ * (hover_volume_ ? 1.0f : 0.85f));
    {
        const float cx = l.volume_center.x;
        const float cy = l.volume_center.y;
        const float body_h = 12.0f;
        const float body_w = 6.0f;
        const float cone_w = 10.0f;
        const float cone_h = 18.0f;

        // Speaker body (small rect).
        const D2D1_RECT_F body{
            cx - cone_w * 0.5f - body_w,
            cy - body_h * 0.5f,
            cx - cone_w * 0.5f,
            cy + body_h * 0.5f,
        };
        ctx->FillRectangle(body, brush_icon_.Get());

        // Cone — quick trapezoid via path geometry. Build on the fly
        // because it's tiny and avoids another cached resource.
        ComPtr<ID2D1PathGeometry> cone_geo;
        ID2D1Factory* f_factory = nullptr;
        ctx->GetFactory(&f_factory);
        if (f_factory != nullptr) {
            if (SUCCEEDED(f_factory->CreatePathGeometry(&cone_geo))) {
                ComPtr<ID2D1GeometrySink> sink;
                if (SUCCEEDED(cone_geo->Open(&sink))) {
                    sink->SetFillMode(D2D1_FILL_MODE_WINDING);
                    sink->BeginFigure(
                        D2D1::Point2F(cx - cone_w * 0.5f, cy - body_h * 0.5f),
                        D2D1_FIGURE_BEGIN_FILLED);
                    sink->AddLine(D2D1::Point2F(cx + cone_w * 0.5f, cy - cone_h * 0.5f));
                    sink->AddLine(D2D1::Point2F(cx + cone_w * 0.5f, cy + cone_h * 0.5f));
                    sink->AddLine(D2D1::Point2F(cx - cone_w * 0.5f, cy + body_h * 0.5f));
                    sink->EndFigure(D2D1_FIGURE_END_CLOSED);
                    (void)sink->Close();
                    ctx->FillGeometry(cone_geo.Get(), brush_icon_.Get());
                }
            }
            f_factory->Release();
        }

        if (muted) {
            // Red slash through the speaker.
            const D2D1_POINT_2F p0 = D2D1::Point2F(cx - cone_w, cy - cone_h * 0.5f);
            const D2D1_POINT_2F p1 = D2D1::Point2F(cx + cone_w, cy + cone_h * 0.5f);
            brush_progress_->SetOpacity(alpha_);
            ctx->DrawLine(p0, p1, brush_progress_.Get(), 2.5f);
        } else {
            // Three sound-wave ticks, length scaled by volume so the
            // icon hints at the current level at a glance.
            const float vol = playback_ != nullptr ? playback_->volume() : 0.0f;
            const int ticks =
                vol > 0.66f ? 3 : vol > 0.33f ? 2 : vol > 0.01f ? 1 : 0;
            for (int i = 0; i < ticks; ++i) {
                const float ox = cx + cone_w * 0.5f + 3.0f + i * 3.0f;
                const float oy = static_cast<float>(4 + i * 2);
                const D2D1_POINT_2F p0 = D2D1::Point2F(ox, cy - oy);
                const D2D1_POINT_2F p1 = D2D1::Point2F(ox, cy + oy);
                ctx->DrawLine(p0, p1, brush_icon_.Get(), 1.8f);
            }
        }
    }

    // 5b. Volume popup slider — only drawn while hovering the speaker
    //     or the popup panel, or while actively dragging.
    if ((hover_volume_ || volume_dragging_) && playback_ != nullptr) {
        // Dim background panel.
        brush_bg_->SetOpacity(alpha_ * 0.85f);
        ctx->FillRectangle(l.volume_popup_panel, brush_bg_.Get());

        // Track.
        brush_track_->SetOpacity(alpha_);
        ctx->FillRectangle(l.volume_slider_track, brush_track_.Get());

        const float vol = playback_->volume();
        const float clamped = vol < 0.0f ? 0.0f : (vol > 1.0f ? 1.0f : vol);
        const float span = l.volume_slider_y1 - l.volume_slider_y0;
        const float knob_y = l.volume_slider_y1 - span * clamped;

        // Filled portion (from knob down).
        D2D1_RECT_F filled = l.volume_slider_track;
        filled.top = knob_y;
        brush_progress_->SetOpacity(alpha_);
        ctx->FillRectangle(filled, brush_progress_.Get());

        D2D1_ELLIPSE knob;
        knob.point.x = l.volume_slider_x;
        knob.point.y = knob_y;
        knob.radiusX = kVolKnobRadius;
        knob.radiusY = kVolKnobRadius;
        ctx->FillEllipse(knob, brush_progress_.Get());
    }

    // 6. Thumbnail preview above the bar when the user is hovering on
    //    (or dragging) the scrub area. The thumbnail is decoded on a
    //    separate SW decoder that never touches the playback pipeline,
    //    so moving the cursor feels responsive regardless of how
    //    heavy the main content is.
    const bool show_thumb = (hover_seek_ || seek_dragging_) && mouse_x_ >= 0;
    if (show_thumb) {
        refresh_thumb_bitmap(ctx);
    }
    // Hover-time label y/x. Computed here so we can draw it even when
    // no thumbnail bitmap is available (file with no video, or
    // thumbnail decoder failed) — user still gets the time readout.
    float label_cx = 0.0f;
    float label_y  = 0.0f;
    bool  want_label = false;
    if (show_thumb && thumb_bitmap_) {
        const D2D1_SIZE_F bmp_size = thumb_bitmap_->GetSize();
        float tx = static_cast<float>(mouse_x_) - bmp_size.width * 0.5f;
        if (tx < kThumbEdgeGuard) tx = kThumbEdgeGuard;
        if (tx + bmp_size.width > static_cast<float>(w) - kThumbEdgeGuard) {
            tx = static_cast<float>(w) - kThumbEdgeGuard - bmp_size.width;
        }
        float ty = l.seek_y - kThumbAboveBar - bmp_size.height;
        if (ty < kThumbEdgeGuard) ty = kThumbEdgeGuard;

        const D2D1_RECT_F bmp_dest{
            tx, ty, tx + bmp_size.width, ty + bmp_size.height };

        // Subtle light border so the thumbnail reads as a distinct
        // element over bright/colourful video content.
        const D2D1_RECT_F border_rect{
            tx - kThumbBorder, ty - kThumbBorder,
            tx + bmp_size.width + kThumbBorder,
            ty + bmp_size.height + kThumbBorder };
        brush_thumb_border_->SetOpacity(alpha_ * 0.85f);
        ctx->FillRectangle(border_rect, brush_thumb_border_.Get());

        ctx->DrawBitmap(
            thumb_bitmap_.Get(),
            bmp_dest,
            alpha_,
            D2D1_BITMAP_INTERPOLATION_MODE_LINEAR);

        label_cx = tx + bmp_size.width * 0.5f;
        label_y  = ty - 4.0f;    // sit just above the border
        want_label = true;
    } else if (show_thumb) {
        // No thumbnail — park the label directly above the scrub bar
        // at the cursor x, clamped to the window edges.
        label_cx = static_cast<float>(mouse_x_);
        label_y  = l.seek_y - kThumbAboveBar;
        want_label = true;
    }

    // 6b. Hover-time label. Computed from the cursor's x-on-track, so
    //     it follows the scrub head precisely regardless of whether a
    //     thumbnail decoded in time.
    if (want_label
        && text_thumb_time_ != nullptr
        && playback_ != nullptr) {
        const int64_t dur = playback_->duration_ns();
        if (dur > 0) {
            const float t = position_on_bar(mouse_x_, l);
            const int64_t hover_pts = static_cast<int64_t>(
                static_cast<double>(t) * static_cast<double>(dur));
            const std::wstring time_str = format_time_ns(hover_pts);

            constexpr float kLabelHalfW = 50.0f;
            constexpr float kLabelH     = 18.0f;
            D2D1_RECT_F label_rect{
                label_cx - kLabelHalfW,
                label_y - kLabelH,
                label_cx + kLabelHalfW,
                label_y,
            };
            // Keep the label on-screen horizontally.
            const float fw = static_cast<float>(w);
            if (label_rect.left < kThumbEdgeGuard) {
                const float shift = kThumbEdgeGuard - label_rect.left;
                label_rect.left  += shift;
                label_rect.right += shift;
            } else if (label_rect.right > fw - kThumbEdgeGuard) {
                const float shift = label_rect.right - (fw - kThumbEdgeGuard);
                label_rect.left  -= shift;
                label_rect.right -= shift;
            }

            // Small dimmed pill behind the text so the label reads
            // on any background.
            brush_bg_->SetOpacity(alpha_ * 0.7f);
            ctx->FillRectangle(label_rect, brush_bg_.Get());

            brush_text_->SetOpacity(alpha_);
            ctx->DrawTextW(
                time_str.c_str(),
                static_cast<UINT32>(time_str.size()),
                text_thumb_time_.Get(),
                label_rect,
                brush_text_.Get(),
                D2D1_DRAW_TEXT_OPTIONS_CLIP,
                DWRITE_MEASURING_MODE_NATURAL);
        }
    }

    // 7. Time readout — either live playback position or drag preview.
    if (text_format_) {
        const int64_t dur = playback_ != nullptr ? playback_->duration_ns() : 0;
        int64_t cur;
        if (seek_dragging_ && dur > 0) {
            cur = static_cast<int64_t>(
                static_cast<double>(drag_progress_) * static_cast<double>(dur));
        } else {
            cur = playback_ != nullptr ? playback_->current_time_ns() : 0;
        }

        const std::wstring line =
            format_time_ns(cur) + L" / " + format_time_ns(dur);

        brush_text_->SetOpacity(alpha_);
        ctx->DrawTextW(
            line.c_str(),
            static_cast<UINT32>(line.size()),
            text_format_.Get(),
            l.time_text,
            brush_text_.Get(),
            D2D1_DRAW_TEXT_OPTIONS_CLIP,
            DWRITE_MEASURING_MODE_NATURAL);
    }
}

} // namespace freikino::app
