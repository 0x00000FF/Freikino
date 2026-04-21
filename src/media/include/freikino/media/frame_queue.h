#pragma once

#include <array>
#include <atomic>
#include <cstddef>
#include <new>
#include <utility>

namespace freikino::media {

// Wait-free single-producer single-consumer ring queue.
//
// Exactly one thread may call `try_push`, and exactly one (different) thread
// may call `try_pop`. Multiple producers or multiple consumers are undefined
// behaviour. The capacity must be a power of two so that the head/tail
// wrap can be done with a cheap bitmask instead of a modulo.
//
// MSVC C4324: alignas on the head/tail atomics is intentional (cache-line
// padding to avoid false sharing between producer and consumer). Warning
// that the class was "padded because of alignas" is telling us the
// requested layout actually happened — scope a local suppression.
#if defined(_MSC_VER)
#pragma warning(push)
#pragma warning(disable : 4324)
#endif

template <class T, std::size_t Capacity>
class SpscQueue {
    static_assert(Capacity >= 2,                  "capacity must be >= 2");
    static_assert((Capacity & (Capacity - 1)) == 0, "capacity must be a power of two");

#ifdef __cpp_lib_hardware_interference_size
    static constexpr std::size_t kCacheLine =
        std::hardware_destructive_interference_size;
#else
    static constexpr std::size_t kCacheLine = 64;
#endif

    static constexpr std::size_t kMask = Capacity - 1;

public:
    SpscQueue() noexcept = default;
    ~SpscQueue() = default;

    SpscQueue(const SpscQueue&)            = delete;
    SpscQueue& operator=(const SpscQueue&) = delete;
    SpscQueue(SpscQueue&&)                 = delete;
    SpscQueue& operator=(SpscQueue&&)      = delete;

    [[nodiscard]] bool try_push(T value) noexcept
    {
        const std::size_t head = head_.load(std::memory_order_relaxed);
        const std::size_t next = (head + 1) & kMask;
        if (next == tail_.load(std::memory_order_acquire)) {
            return false; // full
        }
        slots_[head] = std::move(value);
        head_.store(next, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool try_pop(T& out) noexcept
    {
        const std::size_t tail = tail_.load(std::memory_order_relaxed);
        if (tail == head_.load(std::memory_order_acquire)) {
            return false; // empty
        }
        out = std::move(slots_[tail]);
        slots_[tail] = T{};  // drop any retained resources promptly
        tail_.store((tail + 1) & kMask, std::memory_order_release);
        return true;
    }

    // O(1) snapshot. Approximate under contention; exact when the caller is
    // the sole thread performing both producer and consumer operations.
    std::size_t size() const noexcept
    {
        const std::size_t h = head_.load(std::memory_order_acquire);
        const std::size_t t = tail_.load(std::memory_order_acquire);
        return (h - t) & kMask;
    }

    bool empty() const noexcept
    {
        return head_.load(std::memory_order_acquire)
            == tail_.load(std::memory_order_acquire);
    }

    static constexpr std::size_t capacity() noexcept { return Capacity - 1; }

private:
    alignas(kCacheLine) std::atomic<std::size_t> head_{0};
    alignas(kCacheLine) std::atomic<std::size_t> tail_{0};
    std::array<T, Capacity> slots_{};
};

#if defined(_MSC_VER)
#pragma warning(pop)
#endif

} // namespace freikino::media
