#include "freikino/platform/dpi.h"

#include <windows.h>

namespace freikino::platform {

namespace {

using SetProcessDpiAwarenessContext_t = BOOL (WINAPI*)(DPI_AWARENESS_CONTEXT);

} // namespace

void ensure_per_monitor_v2_dpi() noexcept
{
    // Resolve dynamically so the binary still loads on hosts that predate
    // the API (Windows 10 < 1703). We target Win10+ in the manifest, but
    // defensive dynamic resolution costs nothing.
    HMODULE user32 = ::GetModuleHandleW(L"user32.dll");
    if (user32 == nullptr) {
        return;
    }
    const auto set_ctx = reinterpret_cast<SetProcessDpiAwarenessContext_t>(
        reinterpret_cast<void*>(
            ::GetProcAddress(user32, "SetProcessDpiAwarenessContext")));
    if (set_ctx == nullptr) {
        return;
    }
    // Return value ignored: fails if awareness is already set (e.g. by the
    // manifest), which is the expected path.
    (void)set_ctx(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
}

UINT dpi_for_window(HWND hwnd) noexcept
{
    if (hwnd == nullptr) {
        return USER_DEFAULT_SCREEN_DPI;
    }
    const UINT dpi = ::GetDpiForWindow(hwnd);
    return dpi != 0 ? dpi : static_cast<UINT>(USER_DEFAULT_SCREEN_DPI);
}

int scale(int value, UINT dpi) noexcept
{
    return ::MulDiv(value, static_cast<int>(dpi), USER_DEFAULT_SCREEN_DPI);
}

} // namespace freikino::platform
