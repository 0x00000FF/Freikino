#pragma once

#include "freikino/render/presentation_clock.h"

#include <atomic>

#include <windows.h>

namespace freikino::render {

// QPC-backed presentation clock. `now_ns()` returns the number of
// nanoseconds since `start()` was called, frozen while `pause()` is in
// effect and continuing from the frozen value after `resume()`.
//
// Thread-safe. Reads and writes use acquire/release ordering.
class WallClock final : public PresentationClock {
public:
    WallClock() noexcept;

    // (Re)start ticking from zero.
    void start() noexcept;

    // Freeze `now_ns()` at its current value. Idempotent.
    void pause() noexcept;

    // Resume counting from where `pause()` froze the clock. No-op if not
    // paused.
    void resume() noexcept;

    // Jump to an absolute stream time. Useful after a seek.
    void set_now_ns(int64_t value) noexcept;

    [[nodiscard]] bool is_paused() const noexcept
    {
        return paused_.load(std::memory_order_acquire);
    }

    [[nodiscard]] int64_t now_ns() const noexcept override;

private:
    // Convert an elapsed ns value into a QPC delta, without overflowing
    // int64 on long playback sessions.
    int64_t ns_to_qpc(int64_t ns) const noexcept;

    LARGE_INTEGER         freq_{};
    std::atomic<int64_t>  origin_qpc_{-1};
    std::atomic<int64_t>  paused_at_ns_{0};
    std::atomic<bool>     paused_{false};
};

} // namespace freikino::render
