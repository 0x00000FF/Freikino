#pragma once

#include <string>

#include <windows.h>

namespace freikino::ui {

// RAII owner of a single HWND + its associated window class.
//
// Threading: a window must be created, messaged, and destroyed on the same
// thread — Win32 requirement, not enforced here.
//
// Lifetime: the destructor will DestroyWindow on any HWND that outlives this
// object, but derived-class message handlers will NOT run at that point (the
// derived subobject is already gone). Always close the window explicitly
// before the Window object goes out of scope.
class Window {
public:
    Window() noexcept = default;
    virtual ~Window();

    Window(const Window&) = delete;
    Window& operator=(const Window&) = delete;
    Window(Window&&) = delete;
    Window& operator=(Window&&) = delete;

    HWND handle() const noexcept { return hwnd_; }
    bool is_valid() const noexcept { return hwnd_ != nullptr; }

    void show(int cmd_show) noexcept;
    void close() noexcept;

protected:
    struct CreateParams {
        std::wstring class_name;
        std::wstring title;
        DWORD style        = WS_OVERLAPPEDWINDOW;
        DWORD ex_style     = 0;
        int   x            = CW_USEDEFAULT;
        int   y            = CW_USEDEFAULT;
        int   width        = CW_USEDEFAULT;
        int   height       = CW_USEDEFAULT;
        HWND  parent       = nullptr;
        HICON icon         = nullptr;
        HICON small_icon   = nullptr;
        HCURSOR cursor     = nullptr;
        HBRUSH  background = nullptr;
    };

    void create(HINSTANCE instance, const CreateParams& params);

    // Override to handle specific messages. Default forwards to DefWindowProcW.
    virtual LRESULT on_message(UINT msg, WPARAM wparam, LPARAM lparam);

private:
    static LRESULT CALLBACK wnd_proc_thunk(
        HWND hwnd, UINT msg, WPARAM wparam, LPARAM lparam) noexcept;

    HWND       hwnd_             = nullptr;
    HINSTANCE  instance_         = nullptr;
    std::wstring registered_class_;
    bool       class_registered_ = false;
};

} // namespace freikino::ui
