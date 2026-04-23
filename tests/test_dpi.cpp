#include "freikino/platform/dpi.h"

#include <gtest/gtest.h>

#include <chrono>

#include <windows.h>

namespace {

using freikino::platform::dpi_for_window;
using freikino::platform::ensure_per_monitor_v2_dpi;
using freikino::platform::scale;

// ---------------------------------------------------------------------------
// Feature correctness: scaling must be exact at the common DPI ratios
// because layout pixel counts downstream (hit-test rectangles, toast
// margins, overlay font sizes) depend on integer results.

TEST(Dpi, ScaleIsIdentityAtDefault) {
    EXPECT_EQ(scale(0, 96), 0);
    EXPECT_EQ(scale(1, 96), 1);
    EXPECT_EQ(scale(100, 96), 100);
    EXPECT_EQ(scale(-5, 96), -5);
}

TEST(Dpi, ScaleDoublesAt192) {
    EXPECT_EQ(scale(1, 192), 2);
    EXPECT_EQ(scale(100, 192), 200);
    EXPECT_EQ(scale(-50, 192), -100);
}

TEST(Dpi, ScaleOneAndAHalfAt144) {
    EXPECT_EQ(scale(10, 144), 15);
    EXPECT_EQ(scale(100, 144), 150);
}

TEST(Dpi, ScaleAtFractionalRatioRoundsNearest) {
    // MulDiv rounds to nearest (with ties away from zero).
    EXPECT_EQ(scale(10, 120), 13);  // 10 * 120 / 96 = 12.5 -> 13
    EXPECT_EQ(scale(1, 120), 1);    // 1 * 120 / 96 = 1.25 -> 1
}

TEST(Dpi, NullHwndReturnsDefaultDpi) {
    // Documented contract: fall back to 96 for an invalid HWND so
    // callers can use the return value unconditionally.
    EXPECT_EQ(dpi_for_window(nullptr),
              static_cast<UINT>(USER_DEFAULT_SCREEN_DPI));
}

TEST(Dpi, EnsurePerMonitorV2IsIdempotent) {
    // Calling twice must not crash; the second call is a no-op once
    // awareness has been set. `noexcept` — relied on at startup.
    ensure_per_monitor_v2_dpi();
    ensure_per_monitor_v2_dpi();
    SUCCEED();
}

// ---------------------------------------------------------------------------
// Performance: `scale` sits in hot paint paths (every overlay rectangle
// that needs DPI-adjusting per-paint). It must stay branch-free integer
// math. Ten million invocations should complete in tens of milliseconds;
// the assertion is loose (2s) so only a catastrophic regression trips.

TEST(DpiPerf, ScaleIsCheapInHotLoop) {
    volatile int sink = 0;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < 10'000'000; ++i) {
        sink ^= scale(i, 144);
    }
    const auto elapsed = std::chrono::steady_clock::now() - t0;
    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        elapsed).count();
    EXPECT_LT(ms, 2'000);
    (void)sink;
}

} // namespace
