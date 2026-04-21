#include "freikino/render/wall_clock.h"

namespace freikino::render {

WallClock::WallClock() noexcept
{
    ::QueryPerformanceFrequency(&freq_);
}

int64_t WallClock::ns_to_qpc(int64_t ns) const noexcept
{
    if (freq_.QuadPart == 0) {
        return 0;
    }
    // Split so `seconds * freq` and `remain * freq` each fit in int64.
    const int64_t seconds = ns / 1'000'000'000LL;
    const int64_t remain  = ns % 1'000'000'000LL;
    return seconds * freq_.QuadPart
         + (remain * freq_.QuadPart) / 1'000'000'000LL;
}

void WallClock::start() noexcept
{
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    origin_qpc_.store(now.QuadPart, std::memory_order_release);
    paused_at_ns_.store(0, std::memory_order_release);
    paused_.store(false, std::memory_order_release);
}

void WallClock::pause() noexcept
{
    if (paused_.load(std::memory_order_acquire)) {
        return;
    }
    paused_at_ns_.store(now_ns(), std::memory_order_release);
    paused_.store(true, std::memory_order_release);
}

void WallClock::resume() noexcept
{
    if (!paused_.load(std::memory_order_acquire)) {
        return;
    }
    const int64_t frozen = paused_at_ns_.load(std::memory_order_acquire);
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    origin_qpc_.store(now.QuadPart - ns_to_qpc(frozen),
                      std::memory_order_release);
    paused_.store(false, std::memory_order_release);
}

void WallClock::set_now_ns(int64_t value) noexcept
{
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    origin_qpc_.store(now.QuadPart - ns_to_qpc(value),
                      std::memory_order_release);
    paused_at_ns_.store(value, std::memory_order_release);
}

int64_t WallClock::now_ns() const noexcept
{
    if (paused_.load(std::memory_order_acquire)) {
        return paused_at_ns_.load(std::memory_order_acquire);
    }
    const int64_t origin = origin_qpc_.load(std::memory_order_acquire);
    if (origin < 0 || freq_.QuadPart == 0) {
        return 0;
    }
    LARGE_INTEGER now{};
    ::QueryPerformanceCounter(&now);
    const int64_t delta = now.QuadPart - origin;
    const int64_t seconds = delta / freq_.QuadPart;
    const int64_t remain  = delta % freq_.QuadPart;
    return seconds * 1'000'000'000LL
         + (remain * 1'000'000'000LL) / freq_.QuadPart;
}

} // namespace freikino::render
