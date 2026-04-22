#include "main_window.h"

#include "freikino/audio/wasapi_renderer.h"
#include "freikino/common/error.h"
#include "freikino/common/log.h"
#include "freikino/common/strings.h"
#include "freikino/platform/dpi.h"
#include "freikino/subtitle/subtitle_source.h"
#include "media_session.h"
#include "playback.h"
#include "playlist.h"

#include <memory>
#include <vector>

#include <windows.h>
#include <windowsx.h>
#include <commdlg.h>
#include <dwmapi.h>
#include <shellapi.h>

#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_WINDOW_CORNER_PREFERENCE
#define DWMWA_WINDOW_CORNER_PREFERENCE 33
#endif
#ifndef DWMWCP_DEFAULT
#define DWMWCP_DEFAULT 0
#endif
#ifndef DWMWCP_ROUND
#define DWMWCP_ROUND 2
#endif
#ifndef DWMWA_FORCE_ICONIC_REPRESENTATION
#define DWMWA_FORCE_ICONIC_REPRESENTATION 7
#endif
#ifndef DWMWA_DISALLOW_PEEK
#define DWMWA_DISALLOW_PEEK 11
#endif
#ifndef DWMWA_EXCLUDED_FROM_PEEK
#define DWMWA_EXCLUDED_FROM_PEEK 12
#endif
#ifndef DWMWA_HAS_ICONIC_BITMAP
#define DWMWA_HAS_ICONIC_BITMAP 10
#endif
#ifndef WM_DWMSENDICONICTHUMBNAIL
#define WM_DWMSENDICONICTHUMBNAIL 0x0323
#endif
#ifndef WM_DWMSENDICONICLIVEPREVIEWBITMAP
#define WM_DWMSENDICONICLIVEPREVIEWBITMAP 0x0326
#endif

namespace freikino::app {

namespace {

constexpr const wchar_t* kClassName = L"Freikino.MainWindow";
constexpr const wchar_t* kTitle     = L"Freikino";
constexpr int kDesignWidth  = 1280;
constexpr int kDesignHeight = 720;

#ifdef NDEBUG
constexpr bool kD3DDebug = false;
#else
constexpr bool kD3DDebug = true;
#endif

constexpr int64_t kStepShort = 5LL * 1'000'000'000LL;
constexpr int64_t kStepLong  = 30LL * 1'000'000'000LL;

// Match `kBarHeight` in transport_overlay.cpp — bottom of the client
// area that belongs to the transport controls. Above this is "video
// area" and can be dragged to move the window.
constexpr int kTransportBarHeightPx = 96;

} // namespace

// Private window message used by MediaSession's background open
// worker to hand its result back to the UI thread. Named (not a bare
// WM_APP+N) so grep finds every use site.
extern const UINT kMsgOpenComplete   = WM_APP + 1;
const UINT kMsgDeviceChanged = WM_APP + 2;

MainWindow::~MainWindow()
{
    if (album_thumb_bmp_ != nullptr) {
        ::DeleteObject(album_thumb_bmp_);
        album_thumb_bmp_ = nullptr;
    }
    if (incognito_icon_big_ != nullptr) {
        ::DestroyIcon(incognito_icon_big_);
        incognito_icon_big_ = nullptr;
    }
    if (incognito_icon_small_ != nullptr) {
        ::DestroyIcon(incognito_icon_small_);
        incognito_icon_small_ = nullptr;
    }
}

void MainWindow::create(HINSTANCE instance)
{
    CreateParams p;
    p.class_name = kClassName;
    p.title      = kTitle;
    p.style      = WS_OVERLAPPEDWINDOW;
    p.background = nullptr;

    const UINT dpi = ::GetDpiForSystem();
    p.width  = platform::scale(kDesignWidth,  dpi);
    p.height = platform::scale(kDesignHeight, dpi);

    Window::create(instance, p);

    RECT client{};
    check_bool(::GetClientRect(handle(), &client));
    const UINT cw = static_cast<UINT>(client.right  - client.left);
    const UINT ch = static_cast<UINT>(client.bottom - client.top);
    presenter_.create(handle(), cw, ch, kD3DDebug);
    presenter_created_ = true;

    // Bring up the D2D overlay on top of the same swap chain.
    try {
        overlay_renderer_.create(presenter_.d3d());
        overlay_renderer_.attach_swap_chain(presenter_.swap_chain());
        transport_overlay_.create(overlay_renderer_);
        transport_overlay_.set_fullscreen_toggle(
            [](void* user) noexcept {
                auto* self = static_cast<MainWindow*>(user);
                if (self != nullptr) self->toggle_fullscreen();
            },
            this);
        playlist_overlay_.create(overlay_renderer_);
        playlist_overlay_.set_play_request(
            &MainWindow::playlist_play_request, this);
        subtitle_overlay_.create(overlay_renderer_);
        subtitle_setup_overlay_.create(overlay_renderer_);
        subtitle_setup_overlay_.set_subtitle_overlay(&subtitle_overlay_);
        audio_tracks_overlay_.create(overlay_renderer_);
        spectrum_.create(overlay_renderer_);
        audio_info_overlay_.create(overlay_renderer_);
        title_toast_.create(overlay_renderer_);
        opening_overlay_.create(overlay_renderer_);
        volume_osd_.create(overlay_renderer_);
        debug_overlay_.create(overlay_renderer_, presenter_.adapter());
        overlay_created_ = true;

        presenter_.set_overlay_callback(
            [this](UINT w, UINT h) {
                ID2D1DeviceContext* ctx = overlay_renderer_.begin_draw();
                if (ctx != nullptr) {
                    // Subtitles paint first — they're part of the
                    // video content layer — so chrome (playlist,
                    // transport, toasts) sits on top without being
                    // obscured by a caption.
                    // Audio-only path draws first: spectrum (full-
                    // area backdrop) then the info card on top of
                    // it. No-ops when a video file is playing.
                    spectrum_.draw(ctx, w, h);
                    audio_info_overlay_.draw(ctx, w, h);
                    subtitle_overlay_.draw(ctx, w, h);
                    playlist_overlay_.draw(ctx, w, h);
                    title_toast_.draw(ctx, w, h);
                    volume_osd_.draw(ctx, w, h);
                    // Opening overlay paints on top of the playlist +
                    // toast so the user sees the load state even if
                    // they were mid-interaction when the open kicked
                    // off. Below transport so the bar still reads.
                    opening_overlay_.draw(ctx, w, h);
                    transport_overlay_.draw(ctx, w, h);
                    subtitle_setup_overlay_.draw(ctx, w, h);
                    audio_tracks_overlay_.draw(ctx, w, h);
                    debug_overlay_.draw(ctx, w, h);
                    overlay_renderer_.end_draw();
                }
            });
    } catch (const hresult_error& e) {
        // Non-fatal: the player still works without the transport bar.
        log::warn(
            "overlay setup failed 0x{:08X} — continuing without on-screen UI",
            static_cast<unsigned>(e.code()));
        overlay_created_ = false;
    } catch (...) {
        log::warn("overlay setup failed — continuing without on-screen UI");
        overlay_created_ = false;
    }
}

void MainWindow::set_playback(PlaybackController* controller) noexcept
{
    playback_ = controller;
    transport_overlay_.set_playback(controller);
    subtitle_overlay_.set_playback(controller);
    debug_overlay_.set_runtime(
        presenter_created_ ? &presenter_ : nullptr, controller);

    if (controller != nullptr) {
        controller->set_state_change_callback(
            [](void* user, PlaybackController::Transition t) noexcept {
                auto* self = static_cast<MainWindow*>(user);
                if (self != nullptr) self->show_state_toast(t);
            },
            this);
    }
}

void MainWindow::set_playlist(Playlist* playlist, MediaSession* session) noexcept
{
    playlist_       = playlist;
    media_session_  = session;
    playlist_overlay_.set_playlist(playlist);
}

void MainWindow::playlist_play_request(void* user, std::size_t index) noexcept
{
    auto* self = static_cast<MainWindow*>(user);
    if (self != nullptr) {
        self->play_playlist_index(index);
    }
}

void MainWindow::play_playlist_index(std::size_t index) noexcept
{
    if (playlist_ == nullptr || media_session_ == nullptr) {
        return;
    }
    const auto* entry = playlist_->at(index);
    if (entry == nullptr) {
        return;
    }
    if (media_session_->open(entry->path)) {
        playlist_->set_current_index(index);
        refresh_title();
    }
}

LRESULT MainWindow::on_message(UINT msg, WPARAM wparam, LPARAM lparam)
{
    switch (msg) {
    case WM_CREATE:
        on_create();
        return 0;

    case WM_ERASEBKGND:
        return 1;

    case WM_SIZE:
        on_size(LOWORD(lparam), HIWORD(lparam));
        return 0;

    case WM_PAINT: {
        PAINTSTRUCT ps{};
        const HDC hdc = ::BeginPaint(handle(), &ps);
        if (hdc != nullptr) {
            ::EndPaint(handle(), &ps);
        }
        return 0;
    }

    case WM_DPICHANGED: {
        const auto* suggested = reinterpret_cast<const RECT*>(lparam);
        if (suggested != nullptr) {
            ::SetWindowPos(
                handle(), nullptr,
                suggested->left, suggested->top,
                suggested->right  - suggested->left,
                suggested->bottom - suggested->top,
                SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
    }

    case WM_KEYDOWN:
        on_keydown(wparam, (lparam & (1 << 30)) != 0);
        return 0;

    case WM_SYSKEYDOWN:
        break;

    case WM_MOUSEMOVE: {
        ensure_mouse_tracking();
        on_mouse_move(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;
    }

    case WM_LBUTTONDOWN:
        on_lbutton_down(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;

    case WM_LBUTTONDBLCLK: {
        const int x = GET_X_LPARAM(lparam);
        const int y = GET_Y_LPARAM(lparam);
        RECT rc{};
        if (::GetClientRect(handle(), &rc)) {
            const UINT w = static_cast<UINT>(rc.right);
            const UINT h = static_cast<UINT>(rc.bottom);
            if (playlist_overlay_.hit_panel(x, y, w, h)) {
                playlist_overlay_.on_lbutton_dblclk(x, y, w, h);
                return 0;
            }
        }
        // Fall through to treat as two single clicks elsewhere so the
        // transport bar's click handling isn't surprised by dblclks.
        on_lbutton_down(x, y);
        return 0;
    }

    case WM_LBUTTONUP:
        on_lbutton_up(GET_X_LPARAM(lparam), GET_Y_LPARAM(lparam));
        return 0;

    case WM_MOUSELEAVE:
        mouse_tracked_ = false;
        on_mouse_leave();
        return 0;

    case WM_MOUSEWHEEL: {
        if (playback_ != nullptr) {
            const int delta = GET_WHEEL_DELTA_WPARAM(wparam);
            // One notch = WHEEL_DELTA (120). One notch → 1 %. Keyboard
            // VK_UP/VK_DOWN stays at 5 % for a more intentional feel.
            const float step = static_cast<float>(delta)
                / static_cast<float>(WHEEL_DELTA) * 0.01f;
            playback_->adjust_volume(step);
            transport_overlay_.bump_activity();
            pop_volume_osd();
        }
        return 0;
    }

    case WM_NCHITTEST:
        return on_nchittest(lparam);

    case WM_SETCURSOR: {
        // Swap cursor for interactive regions:
        //   - Hand (IDC_HAND) over transport buttons + scrub track.
        //   - East-west arrow over the playlist resize grip.
        // Everything else falls through to default handling.
        if (reinterpret_cast<HWND>(wparam) == handle()
            && LOWORD(lparam) == HTCLIENT) {
            POINT pt{};
            RECT rc{};
            if (::GetCursorPos(&pt)
                && ::ScreenToClient(handle(), &pt)
                && ::GetClientRect(handle(), &rc)) {
                const UINT cw = static_cast<UINT>(rc.right);
                const UINT ch = static_cast<UINT>(rc.bottom);
                if (playlist_overlay_.hit_resize_grip(pt.x, pt.y, cw, ch)) {
                    ::SetCursor(::LoadCursorW(nullptr, IDC_SIZEWE));
                    return TRUE;
                }
                if (transport_overlay_.hit_interactive(pt.x, pt.y, cw, ch)) {
                    ::SetCursor(::LoadCursorW(nullptr, IDC_HAND));
                    return TRUE;
                }
            }
        }
        break;
    }

    case WM_NCLBUTTONDBLCLK:
        // Double-click is HTCAPTION for both the real title bar AND
        // our video-area override. Only intercept for the video area;
        // let the real title bar keep its native maximize/restore
        // behaviour. Distinguish by the click's client-space Y: ≥ 0
        // means inside the client area (our draggable video), < 0
        // means on the real non-client title bar above it.
        if (wparam == HTCAPTION && playback_ != nullptr) {
            POINT pt{};
            pt.x = GET_X_LPARAM(lparam);
            pt.y = GET_Y_LPARAM(lparam);
            ::ScreenToClient(handle(), &pt);
            if (pt.y >= 0) {
                try {
                    playback_->toggle_pause();
                    refresh_title();
                    transport_overlay_.bump_activity();
                } catch (...) {
                }
                return 0;
            }
        }
        break;

    case WM_CAPTURECHANGED:
        // Another window took capture (task switch, drag); treat the drag
        // as ended but leave the cursor state alone.
        transport_overlay_.on_lbutton_up(-1, -1, 0, 0);
        playlist_overlay_.on_lbutton_up(-1, -1, 0, 0);
        return 0;

    case WM_DROPFILES:
        on_dropfiles(reinterpret_cast<HDROP>(wparam));
        return 0;

    case WM_DWMSENDICONICTHUMBNAIL:
        dispatch_iconic_thumbnail_request(
            HIWORD(lparam), LOWORD(lparam));
        return 0;

    case WM_DWMSENDICONICLIVEPREVIEWBITMAP:
        // Same bitmap for the full Aero Peek preview — the album art
        // scales up fine.
        dispatch_iconic_thumbnail_request(0, 0);
        return 0;

    case WM_DESTROY:
        ::PostQuitMessage(0);
        return 0;

    default:
        if (msg == kMsgOpenComplete) {
            auto* raw = reinterpret_cast<PendingOpen*>(lparam);
            std::unique_ptr<PendingOpen> p{raw};
            if (media_session_ != nullptr) {
                (void)media_session_->complete_open(std::move(p));
                refresh_title();
            }
            return 0;
        }
        if (msg == kMsgDeviceChanged) {
            on_audio_device_changed();
            return 0;
        }
        break;
    }
    return Window::on_message(msg, wparam, lparam);
}

void MainWindow::on_create() noexcept
{
    apply_window_chrome();
    // Opt into Explorer drag-drop. Windows will post WM_DROPFILES to
    // this window when the user releases a drag over the client area.
    ::DragAcceptFiles(handle(), TRUE);
}

void MainWindow::on_size(UINT width, UINT height) noexcept
{
    if (!presenter_created_) {
        return;
    }
    try {
        // DXGI's ResizeBuffers rejects the call if any reference to the
        // current back buffer (index 0) is still live. The overlay's D2D
        // bitmap wraps that surface, so drop it first; it will be
        // rebuilt on the next begin_draw() against the resized buffer.
        if (overlay_created_) {
            overlay_renderer_.invalidate_target();
        }
        presenter_.resize(width, height);
    } catch (const hresult_error& e) {
        log::error(
            "swapchain resize failed: 0x{:08X}",
            static_cast<unsigned>(e.code()));
    } catch (...) {
        log::error("swapchain resize: unknown exception");
    }
}

void MainWindow::on_keydown(WPARAM vk, bool repeat) noexcept
{
    if (vk == VK_ESCAPE) {
        // Escape closes the audio-tracks picker first (so it can act
        // like a modal panel). Otherwise: exit fullscreen, then close.
        if (audio_tracks_overlay_.visible()) {
            audio_tracks_overlay_.hide();
            transport_overlay_.bump_activity();
            return;
        }
        if (fs_active_) {
            toggle_fullscreen();
        } else {
            ::PostMessageW(handle(), WM_CLOSE, 0, 0);
        }
        return;
    }

    // Audio-tracks picker captures navigation keys while open — Up/Down
    // move the highlight, Enter applies. Handle this before falling
    // into the general transport switch so the keys don't also trigger
    // their usual volume/fullscreen bindings.
    if (audio_tracks_overlay_.visible()) {
        if (vk == VK_UP) {
            audio_tracks_overlay_.move_highlight(-1);
            transport_overlay_.bump_activity();
            return;
        }
        if (vk == VK_DOWN) {
            audio_tracks_overlay_.move_highlight(+1);
            transport_overlay_.bump_activity();
            return;
        }
        if (vk == VK_RETURN) {
            if (!repeat) {
                const int idx =
                    audio_tracks_overlay_.highlighted_stream_index();
                if (idx >= 0 && playback_ != nullptr) {
                    (void)playback_->change_audio_track(idx);
                }
                audio_tracks_overlay_.hide();
                transport_overlay_.bump_activity();
            }
            return;
        }
        if (vk == 'A') {
            if (!repeat) {
                audio_tracks_overlay_.hide();
                transport_overlay_.bump_activity();
            }
            return;
        }
        // Any other key falls through to the normal handler below.
    }

    // Playlist edit keys — only active when the panel is visible so
    // Backspace doesn't silently delete selections the user can't see.
    if ((vk == VK_DELETE || vk == VK_BACK)
        && playlist_overlay_.visible()
        && playlist_overlay_.has_selection()) {
        if (!repeat) {
            (void)playlist_overlay_.delete_selected();
            transport_overlay_.bump_activity();
        }
        return;
    }

    if (playback_ == nullptr) {
        return;
    }

    const bool shift = (::GetKeyState(VK_SHIFT)   & 0x8000) != 0;
    const bool ctrl  = (::GetKeyState(VK_CONTROL) & 0x8000) != 0;

    try {
        switch (vk) {
        case VK_SPACE:
            if (!repeat) {
                playback_->toggle_pause();
                refresh_title();
            }
            break;
        case VK_LEFT:
            if (shift) {
                playback_->frame_step(-1);
                refresh_title();
            } else if (ctrl) {
                playback_->seek_by(-kStepLong);
            } else {
                playback_->seek_by(-kStepShort);
            }
            break;
        case VK_RIGHT:
            if (shift) {
                playback_->frame_step(+1);
                refresh_title();
            } else if (ctrl) {
                playback_->seek_by(+kStepLong);
            } else {
                playback_->seek_by(+kStepShort);
            }
            break;
        case VK_HOME:
            playback_->seek_to(0);
            break;
        case VK_END: {
            const int64_t dur = playback_->duration_ns();
            if (dur > 0) {
                playback_->seek_to(dur);
            }
            break;
        }
        case VK_UP:
            playback_->adjust_volume(+0.05f);
            transport_overlay_.bump_activity();
            pop_volume_osd();
            break;
        case VK_DOWN:
            playback_->adjust_volume(-0.05f);
            transport_overlay_.bump_activity();
            pop_volume_osd();
            break;
        case 'M':
            if (!repeat) {
                playback_->toggle_mute();
                transport_overlay_.bump_activity();
                pop_volume_osd();
            }
            break;
        case VK_RETURN:
            if (!repeat) {
                toggle_fullscreen();
            }
            break;
        case VK_F3:
            if (!repeat) {
                debug_overlay_.toggle_visible();
            }
            break;
        case 'L':
            if (!repeat) {
                playlist_overlay_.toggle_visible();
                transport_overlay_.bump_activity();
            }
            break;
        case 'P':
            if (!repeat) {
                toggle_incognito();
            }
            break;
        case 'S':
            if (!repeat) {
                subtitle_setup_overlay_.toggle_visible();
                transport_overlay_.bump_activity();
            }
            break;
        case 'A':
            if (!repeat) {
                audio_tracks_overlay_.toggle_visible();
                transport_overlay_.bump_activity();
            }
            break;
        case VK_OEM_COMMA:
            // ',' — nudge subtitle delay earlier by 100ms. Only while
            // the setup panel is open so this key stays free for
            // future bindings in the normal viewing state.
            if (subtitle_setup_overlay_.visible()
                && subtitle_overlay_.loaded()) {
                subtitle_overlay_.set_delay_ns(
                    subtitle_overlay_.delay_ns() - 100'000'000LL);
                transport_overlay_.bump_activity();
            }
            break;
        case VK_OEM_PERIOD:
            // '.' — nudge subtitle delay later by 100ms.
            if (subtitle_setup_overlay_.visible()
                && subtitle_overlay_.loaded()) {
                subtitle_overlay_.set_delay_ns(
                    subtitle_overlay_.delay_ns() + 100'000'000LL);
                transport_overlay_.bump_activity();
            }
            break;
        case VK_OEM_MINUS:
            // '-' — shrink subtitle font.
            if (subtitle_setup_overlay_.visible()
                && subtitle_overlay_.loaded()) {
                subtitle_overlay_.set_font_scale(
                    subtitle_overlay_.font_scale() - 0.1f);
                transport_overlay_.bump_activity();
            }
            break;
        case VK_OEM_PLUS:
            // '=' (same physical key as '+') — grow subtitle font.
            if (subtitle_setup_overlay_.visible()
                && subtitle_overlay_.loaded()) {
                subtitle_overlay_.set_font_scale(
                    subtitle_overlay_.font_scale() + 0.1f);
                transport_overlay_.bump_activity();
            }
            break;
        case '0':
            if (subtitle_setup_overlay_.visible()
                && subtitle_overlay_.loaded()) {
                subtitle_overlay_.set_delay_ns(0);
                transport_overlay_.bump_activity();
            }
            break;
        case 'E':
            // Cycle the forced subtitle encoding. Heuristics can't
            // always tell CP949-that-happens-to-be-valid-UTF-8 from
            // real UTF-8, so the user gets an explicit override.
            // Order: auto → utf-8 → utf-16le → utf-16be → cp949 →
            // cp932 → cp936 → cp1252 → auto. The subtitle reloads
            // from the same path on each press.
            if (!repeat
                && subtitle_setup_overlay_.visible()
                && subtitle_overlay_.loaded()) {
                static constexpr const char* kOrder[] = {
                    "", "utf-8", "utf-16le", "utf-16be",
                    "cp949", "cp932", "cp936", "cp1252",
                };
                const std::string& cur = subtitle_overlay_.forced_encoding();
                std::size_t idx = 0;
                for (std::size_t i = 0;
                     i < sizeof(kOrder) / sizeof(kOrder[0]); ++i) {
                    if (cur == kOrder[i]) { idx = i; break; }
                }
                idx = (idx + 1) % (sizeof(kOrder) / sizeof(kOrder[0]));
                subtitle_overlay_.set_forced_encoding(kOrder[idx]);
                transport_overlay_.bump_activity();
            }
            break;
        case 'F':
            // Pops the system font picker so the user can swap the
            // subtitle face to something with the glyphs their track
            // actually needs — Arial lacks Hebrew / many CJK ranges,
            // which is what triggers libass's "failed to find any
            // fallback with glyph" fontselect warnings.
            if (!repeat
                && subtitle_setup_overlay_.visible()
                && subtitle_overlay_.loaded()) {
                LOGFONTW lf{};
                lf.lfHeight = -16;
                lf.lfCharSet = DEFAULT_CHARSET;
                // Pre-fill with the current override so the dialog
                // opens on the user's last choice.
                const std::string& cur = subtitle_overlay_.font_override();
                if (!cur.empty()) {
                    try {
                        const std::wstring w = utf8_to_wide(cur);
                        const std::size_t n = (std::min<std::size_t>)(
                            w.size(), LF_FACESIZE - 1);
                        std::copy_n(w.begin(), n, lf.lfFaceName);
                        lf.lfFaceName[n] = L'\0';
                    } catch (...) {
                        lf.lfFaceName[0] = L'\0';
                    }
                }

                CHOOSEFONTW cf{};
                cf.lStructSize = sizeof(cf);
                cf.hwndOwner   = handle();
                cf.lpLogFont   = &lf;
                cf.Flags       = CF_SCREENFONTS | CF_SCALABLEONLY
                               | CF_INITTOLOGFONTSTRUCT | CF_NOVERTFONTS;

                if (::ChooseFontW(&cf)) {
                    try {
                        subtitle_overlay_.set_font_override(
                            wide_to_utf8(lf.lfFaceName));
                        log::info("subtitle: font override = '{}'",
                                  wide_to_utf8(lf.lfFaceName));
                    } catch (...) {
                        log::warn("subtitle: font name conversion failed");
                    }
                }
                transport_overlay_.bump_activity();
            }
            break;
        default:
            break;
        }
    } catch (const hresult_error& e) {
        log::error(
            "transport key: 0x{:08X} at {}:{}",
            static_cast<unsigned>(e.code()),
            e.where().file_name() != nullptr ? e.where().file_name() : "?",
            e.where().line());
    } catch (const std::exception& e) {
        log::error("transport key: {}", e.what());
    } catch (...) {
        log::error("transport key: unknown exception");
    }
}

void MainWindow::on_mouse_move(int x, int y) noexcept
{
    if (!overlay_created_) return;
    RECT rc{};
    if (!::GetClientRect(handle(), &rc)) return;
    const UINT w = static_cast<UINT>(rc.right);
    const UINT h = static_cast<UINT>(rc.bottom);
    transport_overlay_.on_mouse_move(x, y, w, h);
    playlist_overlay_.on_mouse_move(x, y, w, h);
}

void MainWindow::on_lbutton_down(int x, int y) noexcept
{
    if (!overlay_created_) return;
    RECT rc{};
    if (!::GetClientRect(handle(), &rc)) return;
    const UINT w = static_cast<UINT>(rc.right);
    const UINT h = static_cast<UINT>(rc.bottom);
    // Route to the playlist first when the click is on its panel —
    // transport doesn't overlap that region, so no need to forward
    // both when the playlist consumes it. The resize grip sits just
    // outside the panel rect on its left edge, so check it separately.
    if (playlist_overlay_.hit_panel(x, y, w, h)
        || playlist_overlay_.hit_resize_grip(x, y, w, h)) {
        const bool ctrl =
            (::GetKeyState(VK_CONTROL) & 0x8000) != 0;
        const bool shift =
            (::GetKeyState(VK_SHIFT)   & 0x8000) != 0;
        playlist_overlay_.on_lbutton_down(x, y, w, h, ctrl, shift);
        if (playlist_overlay_.is_resizing()) {
            (void)::SetCapture(handle());
        }
        return;
    }
    transport_overlay_.on_lbutton_down(x, y, w, h);
    if (transport_overlay_.wants_mouse_capture()) {
        (void)::SetCapture(handle());
    }
}

void MainWindow::on_lbutton_up(int x, int y) noexcept
{
    if (!overlay_created_) return;
    RECT rc{};
    if (!::GetClientRect(handle(), &rc)) return;
    const UINT w = static_cast<UINT>(rc.right);
    const UINT h = static_cast<UINT>(rc.bottom);
    transport_overlay_.on_lbutton_up(x, y, w, h);
    playlist_overlay_.on_lbutton_up(x, y, w, h);
    if (::GetCapture() == handle()) {
        (void)::ReleaseCapture();
    }
    if (playback_ != nullptr) {
        refresh_title();
    }
}

void MainWindow::on_mouse_leave() noexcept
{
    transport_overlay_.on_mouse_leave();
    playlist_overlay_.on_mouse_leave();
}

void MainWindow::on_dropfiles(HDROP drop) noexcept
{
    if (drop == nullptr) {
        return;
    }

    // Drop position (in screen or client coords — query for client).
    POINT pt{};
    (void)::DragQueryPoint(drop, &pt);

    RECT rc{};
    const bool have_rect = (::GetClientRect(handle(), &rc) != FALSE);
    const UINT w = have_rect ? static_cast<UINT>(rc.right)  : 0;
    const UINT h = have_rect ? static_cast<UINT>(rc.bottom) : 0;

    // Decide whether the drop targets the playlist panel. If so, we
    // append every dropped file. Otherwise, we still append (so the
    // user can queue) and additionally open the FIRST file so
    // playback starts immediately on a drag-to-play gesture.
    const bool into_playlist =
        have_rect && playlist_overlay_.hit_panel(pt.x, pt.y, w, h);

    const UINT count = ::DragQueryFileW(drop, 0xFFFFFFFFu, nullptr, 0);

    std::vector<std::wstring> paths;
    paths.reserve(count);
    for (UINT i = 0; i < count; ++i) {
        const UINT needed = ::DragQueryFileW(drop, i, nullptr, 0);
        if (needed == 0) continue;
        std::wstring p;
        p.resize(needed);
        // DragQueryFileW's size arg includes the trailing NUL — pass
        // needed+1 and trim the NUL afterward.
        const UINT copied = ::DragQueryFileW(
            drop, i, p.data(), needed + 1);
        if (copied == 0) continue;
        p.resize(copied);
        paths.push_back(std::move(p));
    }
    ::DragFinish(drop);

    if (paths.empty()) {
        return;
    }

    // Split into subtitles vs media. Subtitle files (.srt/.smi/.ass/…)
    // apply to the currently-playing file without touching the
    // playlist; media files go through the normal queue-and-play
    // path. Mixed drops handle each bucket independently.
    std::vector<std::wstring> media_paths;
    std::vector<std::wstring> subtitle_paths;
    media_paths.reserve(paths.size());
    subtitle_paths.reserve(paths.size());
    for (auto& p : paths) {
        if (subtitle::looks_like_subtitle_path(p)) {
            subtitle_paths.push_back(std::move(p));
        } else {
            media_paths.push_back(std::move(p));
        }
    }

    // Apply subtitles. Only the most-recent one wins if the user
    // drops several at once — a multi-subtitle UX would need a
    // picker, which is out of scope here.
    if (!subtitle_paths.empty()) {
        (void)subtitle_overlay_.load(subtitle_paths.back());
    }

    if (media_paths.empty()
        || playlist_ == nullptr
        || media_session_ == nullptr) {
        transport_overlay_.bump_activity();
        return;
    }

    std::size_t first_added = Playlist::npos();
    for (const auto& p : media_paths) {
        const std::size_t idx = playlist_->append(p);
        if (first_added == Playlist::npos()) {
            first_added = idx;
        }
    }

    if (!into_playlist && first_added != Playlist::npos()) {
        // Open the first dropped file — if the user drops multiple
        // files into the video area, they almost always mean "play
        // this one and queue the rest".
        const auto* entry = playlist_->at(first_added);
        if (entry != nullptr && media_session_->open(entry->path)) {
            playlist_->set_current_index(first_added);
            refresh_title();
        }
    }

    transport_overlay_.bump_activity();
}

void MainWindow::ensure_mouse_tracking() noexcept
{
    if (mouse_tracked_) {
        return;
    }
    TRACKMOUSEEVENT tme{};
    tme.cbSize    = sizeof(tme);
    tme.dwFlags   = TME_LEAVE;
    tme.hwndTrack = handle();
    if (::TrackMouseEvent(&tme)) {
        mouse_tracked_ = true;
    }
}

void MainWindow::show_title_toast(std::wstring text) noexcept
{
    current_name_ = text;
    if (!incognito_) {
        // Annotate the filename toast with "(Subtitled)" when a
        // sibling subtitle was picked up for this open. MediaSession
        // runs auto_load_sibling_subtitle BEFORE calling this, so
        // the very first toast after opening a video reflects the
        // subtitle state on its own.
        std::wstring display = text;
        if (subtitle_overlay_.loaded()) {
            display += L" (Subtitled)";
        }
        title_toast_.show(std::move(display));
    }
    refresh_title();
}

void MainWindow::show_state_toast(
    PlaybackController::Transition t) noexcept
{
    const wchar_t* label = L"";
    switch (t) {
    case PlaybackController::Transition::Playing: label = L"Playing"; break;
    case PlaybackController::Transition::Paused:  label = L"Paused";  break;
    case PlaybackController::Transition::Stopped: label = L"Stopped"; break;
    }
    // Bypass show_title_toast's incognito check — state changes are
    // about UI feedback, not file identity.
    title_toast_.show(label);
    transport_overlay_.bump_activity();
}

void MainWindow::auto_load_sibling_subtitle(
    const std::wstring& video_path) noexcept
{
    if (video_path.empty()) return;

    // Strip the extension off the video path (keep the directory
    // part intact). Guard against a lone dot in a directory name —
    // the dot must be AFTER the last slash.
    const std::size_t slash = video_path.find_last_of(L"\\/");
    const std::size_t dot   = video_path.find_last_of(L'.');
    if (dot == std::wstring::npos
        || (slash != std::wstring::npos && dot < slash)) {
        return;
    }
    const std::wstring base = video_path.substr(0, dot);

    // Priority order matches the request: ASS/SSA first (most
    // feature-complete), then SRT, then SMI/SAMI. Subtitle module
    // doesn't support anything else yet, so the list is closed.
    static constexpr const wchar_t* kExts[] = {
        L".ass", L".ssa", L".srt", L".smi", L".sami",
    };
    for (const auto* ext : kExts) {
        std::wstring candidate = base + ext;
        const DWORD attrs = ::GetFileAttributesW(candidate.c_str());
        if (attrs == INVALID_FILE_ATTRIBUTES
            || (attrs & FILE_ATTRIBUTE_DIRECTORY) != 0) {
            continue;
        }
        if (subtitle_overlay_.load(candidate)) {
            log::info("auto-loaded subtitle: {}",
                      wide_to_utf8(candidate));
        }
        return;
    }
}

void MainWindow::set_audio_only_mode(
    bool on, AudioInfoOverlay::Track track) noexcept
{
    if (!overlay_created_) return;
    spectrum_.set_active(on);
    if (on) {
        // Copy art out before moving the track into the overlay so
        // we can push the same buffer to the taskbar bitmap.
        const std::vector<std::uint8_t> art_bgra = track.art_bgra;
        const int art_w = track.art_width;
        const int art_h = track.art_height;

        audio_info_overlay_.set_track(std::move(track));

        if (!art_bgra.empty() && art_w > 0 && art_h > 0) {
            set_album_thumbnail(art_bgra, art_w, art_h);
        } else {
            clear_album_thumbnail();
        }
    } else {
        audio_info_overlay_.clear();
        clear_album_thumbnail();
    }
}

void MainWindow::set_audio_renderer(audio::WasapiRenderer* r) noexcept
{
    audio_renderer_ = r;
    spectrum_.set_source(r);
    if (r != nullptr) {
        r->set_device_change_notify(handle(), kMsgDeviceChanged);
    }
}

void MainWindow::on_audio_device_changed() noexcept
{
    if (audio_renderer_ == nullptr) return;

    // Pause first so the user isn't dropped into mid-playback on the
    // new endpoint mid-word. The reload path will tear down the pump
    // thread anyway, but explicit pause keeps the transport state
    // consistent and stops the decoder from filling queues against a
    // disconnected sink.
    if (playback_ != nullptr
        && playback_->state() == PlaybackController::State::playing) {
        playback_->pause();
        refresh_title();
    }

    try {
        audio_renderer_->reload_default_device();
    } catch (...) {
        log::error("on_audio_device_changed: reload threw");
    }

    const std::wstring name = audio_renderer_->device_friendly_name();
    // Reuse the title-toast overlay for the device-change banner.
    // Prefixed so the user knows why it popped — a bare device name
    // would look like the title_toast for a newly-opened file.
    std::wstring msg = L"Audio device: ";
    msg += name.empty() ? std::wstring{L"default"} : name;
    title_toast_.show(std::move(msg));

    transport_overlay_.bump_activity();
    log::info("audio device changed to '{}'", wide_to_utf8(name));
}

void MainWindow::set_opening(bool opening, std::wstring name) noexcept
{
    if (!overlay_created_) return;
    if (opening) {
        // Show just the basename to keep the card readable.
        const auto slash = name.find_last_of(L"\\/");
        std::wstring display =
            (slash == std::wstring::npos) ? name : name.substr(slash + 1);
        opening_overlay_.show(std::move(display));
    } else {
        opening_overlay_.hide();
    }
}

void MainWindow::refresh_title() noexcept
{
    const bool paused = playback_ != nullptr
        && playback_->state() == PlaybackController::State::paused;

    std::wstring title;
    if (incognito_) {
        // Disguise as Windows Explorer — no filename, no paused
        // suffix, nothing that would betray the player.
        title = L"Windows Explorer";
    } else if (!current_name_.empty()) {
        title = current_name_;
        if (paused) {
            title += L" (Paused)";
        }
        title += L" \x2014 ";
        title += kTitle;
    } else if (paused) {
        title = std::wstring{kTitle} + L" \x2014 Paused";
    } else {
        title = kTitle;
    }
    ::SetWindowTextW(handle(), title.c_str());
}

LRESULT MainWindow::on_nchittest(LPARAM lparam) noexcept
{
    // Let the default handler decide whether this is a frame hit
    // (title bar, border, resize handle, etc.). Only override when the
    // default says "client area" — then promote the upper portion to
    // HTCAPTION so clicks in the video area drag the window, while the
    // bottom transport-bar strip stays interactive.
    const LRESULT hit = ::DefWindowProcW(handle(), WM_NCHITTEST, 0, lparam);
    if (hit != HTCLIENT) {
        return hit;
    }

    POINT pt{};
    pt.x = GET_X_LPARAM(lparam);
    pt.y = GET_Y_LPARAM(lparam);
    ::ScreenToClient(handle(), &pt);

    RECT rc{};
    if (!::GetClientRect(handle(), &rc)) {
        return HTCLIENT;
    }
    // Playlist panel is interactive — exempt it from the drag-to-move
    // override so rows can be clicked. Same for the resize grip on
    // its left edge, which sits just outside the panel rect.
    if (playlist_overlay_.hit_panel(
            pt.x, pt.y,
            static_cast<UINT>(rc.right),
            static_cast<UINT>(rc.bottom))
        || playlist_overlay_.hit_resize_grip(
            pt.x, pt.y,
            static_cast<UINT>(rc.right),
            static_cast<UINT>(rc.bottom))) {
        return HTCLIENT;
    }
    if (pt.y < rc.bottom - kTransportBarHeightPx) {
        return HTCAPTION;
    }
    return HTCLIENT;
}

void MainWindow::toggle_fullscreen() noexcept
{
    if (!fs_active_) {
        // Enter fullscreen. Remember everything we change so we can
        // restore it precisely.
        fs_prev_placement_.length = sizeof(fs_prev_placement_);
        ::GetWindowPlacement(handle(), &fs_prev_placement_);
        fs_prev_style_   = ::GetWindowLongPtrW(handle(), GWL_STYLE);
        fs_prev_exstyle_ = ::GetWindowLongPtrW(handle(), GWL_EXSTYLE);

        HMONITOR mon = ::MonitorFromWindow(handle(), MONITOR_DEFAULTTONEAREST);
        MONITORINFO mi{};
        mi.cbSize = sizeof(mi);
        if (!::GetMonitorInfoW(mon, &mi)) {
            return;
        }

        const LONG_PTR fs_style =
            fs_prev_style_ & ~static_cast<LONG_PTR>(
                WS_OVERLAPPEDWINDOW | WS_DLGFRAME);
        ::SetWindowLongPtrW(handle(), GWL_STYLE, fs_style | WS_POPUP);

        ::SetWindowPos(handle(), HWND_TOP,
            mi.rcMonitor.left, mi.rcMonitor.top,
            mi.rcMonitor.right  - mi.rcMonitor.left,
            mi.rcMonitor.bottom - mi.rcMonitor.top,
            SWP_NOOWNERZORDER | SWP_FRAMECHANGED | SWP_SHOWWINDOW);

        fs_active_ = true;
    } else {
        ::SetWindowLongPtrW(handle(), GWL_STYLE,   fs_prev_style_);
        ::SetWindowLongPtrW(handle(), GWL_EXSTYLE, fs_prev_exstyle_);
        ::SetWindowPlacement(handle(), &fs_prev_placement_);
        ::SetWindowPos(handle(), nullptr, 0, 0, 0, 0,
            SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER
                | SWP_NOOWNERZORDER | SWP_FRAMECHANGED);
        fs_active_ = false;
    }

    transport_overlay_.bump_activity();
}

void MainWindow::apply_window_chrome() noexcept
{
    const BOOL dark = TRUE;
    (void)::DwmSetWindowAttribute(
        handle(), DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));

    const DWORD corner = DWMWCP_ROUND;
    (void)::DwmSetWindowAttribute(
        handle(), DWMWA_WINDOW_CORNER_PREFERENCE, &corner, sizeof(corner));
}

void MainWindow::toggle_incognito() noexcept
{
    incognito_ = !incognito_;
    apply_dwm_chrome();
    apply_incognito_icon();
    refresh_title();
    transport_overlay_.bump_activity();
    log::info("incognito: {}", incognito_ ? "on" : "off");
}

void MainWindow::apply_incognito_icon() noexcept
{
    HICON new_big   = nullptr;
    HICON new_small = nullptr;

    if (incognito_) {
        // Pull the File Explorer icon straight out of explorer.exe.
        // Composed from %SystemRoot% so it works no matter where
        // Windows is installed.
        wchar_t windir[MAX_PATH] = {};
        const UINT n = ::GetWindowsDirectoryW(windir, MAX_PATH);
        if (n > 0 && n < MAX_PATH) {
            std::wstring exe_path{windir, n};
            exe_path += L"\\explorer.exe";
            (void)::ExtractIconExW(
                exe_path.c_str(), 0, &new_big, &new_small, 1);
        }
    } else {
        // On toggle-off, WM_SETICON NULL is not enough: once the
        // taskbar has latched onto an explicit HICON it keeps
        // showing it instead of falling back to the class icon.
        // Install IDI_APPLICATION explicitly to force a refresh —
        // that matches what the taskbar would show pre-incognito
        // (our window class has no icon of its own). These are
        // shared system icons so they must not be DestroyIcon'd.
        new_big   = ::LoadIconW(nullptr, IDI_APPLICATION);
        new_small = new_big;
    }

    ::SendMessageW(handle(), WM_SETICON, ICON_SMALL,
        reinterpret_cast<LPARAM>(new_small));
    ::SendMessageW(handle(), WM_SETICON, ICON_BIG,
        reinterpret_cast<LPARAM>(new_big));

    if (incognito_icon_big_ != nullptr) {
        ::DestroyIcon(incognito_icon_big_);
    }
    if (incognito_icon_small_ != nullptr) {
        ::DestroyIcon(incognito_icon_small_);
    }
    // Only record HICONs we own (from ExtractIconExW). IDI_APPLICATION
    // is a shared system icon — don't stash it for later destruction.
    incognito_icon_big_   = incognito_ ? new_big   : nullptr;
    incognito_icon_small_ = incognito_ ? new_small : nullptr;
}

void MainWindow::pop_volume_osd() noexcept
{
    if (!overlay_created_ || playback_ == nullptr) {
        return;
    }
    volume_osd_.show(playback_->volume(), playback_->muted());
}

void MainWindow::apply_dwm_chrome() noexcept
{
    // Taskbar thumbnail policy:
    //   - Incognito always wins: force-iconic + NO custom bitmap, so
    //     DWM falls back to the bare app icon (nothing about the
    //     currently-playing file leaks to the taskbar).
    //   - Otherwise, if an album-art bitmap is present (audio-only
    //     with cover art), force-iconic + custom bitmap so the cover
    //     appears in the taskbar preview.
    //   - Otherwise, let DWM render the native live thumbnail.
    //
    // Aero Peek (hover-to-full-preview) is suppressed only in
    // incognito — when showing album art we still allow Peek to
    // show the full player (which is just the visualizer anyway).
    const bool use_iconic =
        incognito_ || album_thumb_bmp_ != nullptr;
    const bool use_custom_bitmap =
        !incognito_ && album_thumb_bmp_ != nullptr;

    const BOOL iconic_flag  = use_iconic ? TRUE : FALSE;
    const BOOL has_bmp_flag = use_custom_bitmap ? TRUE : FALSE;

    (void)::DwmSetWindowAttribute(
        handle(), DWMWA_FORCE_ICONIC_REPRESENTATION,
        &iconic_flag, sizeof(iconic_flag));
    (void)::DwmSetWindowAttribute(
        handle(), DWMWA_HAS_ICONIC_BITMAP,
        &has_bmp_flag, sizeof(has_bmp_flag));

    const BOOL peek_flag = incognito_ ? TRUE : FALSE;
    (void)::DwmSetWindowAttribute(
        handle(), DWMWA_DISALLOW_PEEK, &peek_flag, sizeof(peek_flag));
    (void)::DwmSetWindowAttribute(
        handle(), DWMWA_EXCLUDED_FROM_PEEK, &peek_flag, sizeof(peek_flag));

    if (use_custom_bitmap) {
        // Invalidate DWM's cache so it re-requests the thumbnail
        // on its next redraw via WM_DWMSENDICONICTHUMBNAIL.
        (void)::DwmInvalidateIconicBitmaps(handle());
    }
}

namespace {

// BGRA buffer → 32-bit top-down DIB section. Returned HBITMAP is
// owned by the caller and must be DeleteObject'd. The longer edge
// is scaled to `max_side`; aspect ratio is preserved.
HBITMAP make_dib_from_bgra(
    const std::vector<std::uint8_t>& bgra,
    int src_w, int src_h, int max_side) noexcept
{
    if (bgra.empty() || src_w <= 0 || src_h <= 0 || max_side <= 0) {
        return nullptr;
    }

    float scale = static_cast<float>(max_side)
        / static_cast<float>((std::max)(src_w, src_h));
    if (scale > 1.0f) scale = 1.0f;   // don't upscale
    int dst_w = static_cast<int>(static_cast<float>(src_w) * scale);
    int dst_h = static_cast<int>(static_cast<float>(src_h) * scale);
    if (dst_w < 1) dst_w = 1;
    if (dst_h < 1) dst_h = 1;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = dst_w;
    bi.bmiHeader.biHeight      = -dst_h;   // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP bmp = ::CreateDIBSection(
        nullptr, &bi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    if (bmp == nullptr || pixels == nullptr) {
        if (bmp != nullptr) ::DeleteObject(bmp);
        return nullptr;
    }

    // Nearest-neighbour scale into the DIB. Preview dimensions are
    // small enough that sampling artifacts aren't visible; keeps
    // this dependency-free.
    auto* out = static_cast<std::uint8_t*>(pixels);
    for (int y = 0; y < dst_h; ++y) {
        const int sy = (y * src_h) / dst_h;
        for (int x = 0; x < dst_w; ++x) {
            const int sx = (x * src_w) / dst_w;
            const std::uint8_t* src =
                bgra.data() + (sy * src_w + sx) * 4;
            std::uint8_t* dst = out + (y * dst_w + x) * 4;
            dst[0] = src[0];     // B
            dst[1] = src[1];     // G
            dst[2] = src[2];     // R
            // Opaque alpha — DwmSetIconicThumbnail wants
            // premultiplied, and with A=0xFF premultiplication is a
            // no-op.
            dst[3] = 0xFF;
        }
    }
    return bmp;
}

} // namespace

void MainWindow::set_album_thumbnail(
    const std::vector<std::uint8_t>& bgra, int w, int h) noexcept
{
    HBITMAP new_bmp = make_dib_from_bgra(bgra, w, h, 256);
    if (new_bmp == nullptr) {
        clear_album_thumbnail();
        return;
    }
    if (album_thumb_bmp_ != nullptr) {
        ::DeleteObject(album_thumb_bmp_);
    }
    album_thumb_bmp_ = new_bmp;

    apply_dwm_chrome();

    // Push once now so the user sees the cover immediately; further
    // refreshes come through WM_DWMSENDICONICTHUMBNAIL.
    if (!incognito_) {
        (void)::DwmSetIconicThumbnail(
            handle(), album_thumb_bmp_, 0);
        (void)::DwmSetIconicLivePreviewBitmap(
            handle(), album_thumb_bmp_, nullptr, 0);
    }
}

void MainWindow::clear_album_thumbnail() noexcept
{
    if (album_thumb_bmp_ == nullptr) {
        return;
    }
    ::DeleteObject(album_thumb_bmp_);
    album_thumb_bmp_ = nullptr;
    apply_dwm_chrome();
}

void MainWindow::dispatch_iconic_thumbnail_request(int w, int h) noexcept
{
    (void)w;
    (void)h;
    if (incognito_ || album_thumb_bmp_ == nullptr) {
        return;
    }
    // DWM downscales from the stored 256-edge bitmap to whatever
    // the request asked for.
    (void)::DwmSetIconicThumbnail(
        handle(), album_thumb_bmp_, 0);
    (void)::DwmSetIconicLivePreviewBitmap(
        handle(), album_thumb_bmp_, nullptr, 0);
}

} // namespace freikino::app
