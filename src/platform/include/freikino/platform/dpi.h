#pragma once

#include <windows.h>

namespace freikino::platform {

// The manifest declares PerMonitorV2 DPI awareness, which is the primary and
// authoritative mechanism. This call is a defensive fallback for hosts whose
// manifest is not honoured (embedded scenarios, non-standard loaders); it is
// a no-op once DPI awareness has already been set for the process.
void ensure_per_monitor_v2_dpi() noexcept;

// Returns the effective DPI of the monitor hosting `hwnd`, falling back to
// the system default (96) if the HWND is invalid or the API is unavailable.
UINT dpi_for_window(HWND hwnd) noexcept;

// Scales `value` from design pixels (96 DPI) to device pixels at `dpi`.
int scale(int value, UINT dpi) noexcept;

} // namespace freikino::platform
