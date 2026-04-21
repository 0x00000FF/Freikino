#include "freikino/ui/window.h"

#include "freikino/common/error.h"
#include "freikino/common/log.h"

#include <windows.h>

namespace freikino::ui {

namespace {

constexpr int kSelfSlot = GWLP_USERDATA;

} // namespace

Window::~Window()
{
    // Defensive: the derived class should have called close() before ~Window.
    // If not, tear down the HWND here, accepting that our own on_message
    // overrides are already gone.
    if (hwnd_ != nullptr) {
        ::SetWindowLongPtrW(hwnd_, kSelfSlot, 0);
        ::DestroyWindow(hwnd_);
        hwnd_ = nullptr;
    }
    if (class_registered_ && instance_ != nullptr && !registered_class_.empty()) {
        ::UnregisterClassW(registered_class_.c_str(), instance_);
    }
}

void Window::show(int cmd_show) noexcept
{
    if (hwnd_ != nullptr) {
        ::ShowWindow(hwnd_, cmd_show);
        (void)::UpdateWindow(hwnd_);
    }
}

void Window::close() noexcept
{
    if (hwnd_ != nullptr) {
        ::DestroyWindow(hwnd_);
        // hwnd_ is cleared by the thunk's WM_NCDESTROY handling.
    }
}

LRESULT Window::on_message(UINT msg, WPARAM wparam, LPARAM lparam)
{
    return ::DefWindowProcW(hwnd_, msg, wparam, lparam);
}

void Window::create(HINSTANCE instance, const CreateParams& params)
{
    if (hwnd_ != nullptr) {
        throw_hresult(E_UNEXPECTED);
    }
    if (instance == nullptr) {
        throw_hresult(E_INVALIDARG);
    }
    if (params.class_name.empty()) {
        throw_hresult(E_INVALIDARG);
    }

    instance_         = instance;
    registered_class_ = params.class_name;

    WNDCLASSEXW wc{};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_OWNDC | CS_DBLCLKS;
    wc.lpfnWndProc   = &Window::wnd_proc_thunk;
    wc.hInstance     = instance;
    wc.hIcon         = params.icon;
    wc.hIconSm       = params.small_icon;
    wc.hCursor       = params.cursor != nullptr
                           ? params.cursor
                           : ::LoadCursorW(nullptr, IDC_ARROW);
    wc.hbrBackground = params.background;
    wc.lpszClassName = registered_class_.c_str();

    if (::RegisterClassExW(&wc) == 0) {
        const DWORD err = ::GetLastError();
        if (err != ERROR_CLASS_ALREADY_EXISTS) {
            throw_hresult(HRESULT_FROM_WIN32(err));
        }
        // Someone else (e.g. a prior instance of this class) already
        // registered it. Reuse, but don't take ownership for unregistration.
    } else {
        class_registered_ = true;
    }

    HWND created = ::CreateWindowExW(
        params.ex_style,
        registered_class_.c_str(),
        params.title.c_str(),
        params.style,
        params.x, params.y,
        params.width, params.height,
        params.parent,
        /* menu */ nullptr,
        instance,
        this);
    if (created == nullptr) {
        throw_last_error();
    }
    // hwnd_ was set in WM_NCCREATE via the thunk; sanity-check.
    if (hwnd_ != created) {
        hwnd_ = created;
    }
}

LRESULT CALLBACK Window::wnd_proc_thunk(
    HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) noexcept
{
    Window* self = nullptr;

    if (msg == WM_NCCREATE) {
        auto* cs = reinterpret_cast<CREATESTRUCTW*>(lparam);
        if (cs != nullptr) {
            self = static_cast<Window*>(cs->lpCreateParams);
        }
        if (self != nullptr) {
            self->hwnd_ = hwnd;
            ::SetLastError(0);
            const LONG_PTR prev = ::SetWindowLongPtrW(
                hwnd, kSelfSlot, reinterpret_cast<LONG_PTR>(self));
            if (prev == 0 && ::GetLastError() != 0) {
                // Failed to attach self-pointer; abandon window creation.
                return FALSE;
            }
        }
    } else {
        self = reinterpret_cast<Window*>(
            ::GetWindowLongPtrW(hwnd, kSelfSlot));
    }

    if (self == nullptr) {
        return ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    LRESULT result = 0;
    try {
        result = self->on_message(msg, wparam, lparam);
    } catch (const hresult_error& e) {
        log::error(
            "unhandled hresult in wnd_proc: 0x{:08X} at {}:{}",
            static_cast<unsigned>(e.code()),
            e.where().file_name() != nullptr ? e.where().file_name() : "?",
            e.where().line());
        result = ::DefWindowProcW(hwnd, msg, wparam, lparam);
    } catch (const std::exception& e) {
        log::error("unhandled exception in wnd_proc: {}", e.what());
        result = ::DefWindowProcW(hwnd, msg, wparam, lparam);
    } catch (...) {
        log::error("unknown exception in wnd_proc");
        result = ::DefWindowProcW(hwnd, msg, wparam, lparam);
    }

    if (msg == WM_NCDESTROY) {
        ::SetWindowLongPtrW(hwnd, kSelfSlot, 0);
        self->hwnd_ = nullptr;
    }
    return result;
}

} // namespace freikino::ui
