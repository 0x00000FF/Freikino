#include "freikino/media/frame_queue.h"

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <memory>
#include <stop_token>
#include <thread>

namespace {

using freikino::media::SpscQueue;

// ---------------------------------------------------------------------------
// Feature correctness: FIFO ordering, empty/full semantics, wraparound.

TEST(SpscQueue, CapacityExposesNMinusOneSlots) {
    // A power-of-two size N exposes N-1 usable slots because one slot
    // is reserved to disambiguate full from empty under the head/tail
    // scheme.
    constexpr std::size_t cap = SpscQueue<int, 8>::capacity();
    static_assert(cap == 7);
    EXPECT_EQ(cap, 7u);
}

TEST(SpscQueue, StartsEmpty) {
    SpscQueue<int, 4> q;
    EXPECT_TRUE(q.empty());
    EXPECT_EQ(q.size(), 0u);
    int v = 0;
    EXPECT_FALSE(q.try_pop(v));
}

TEST(SpscQueue, PushPopPreservesFifoOrder) {
    SpscQueue<int, 8> q;
    for (int i = 0; i < 7; ++i) {
        ASSERT_TRUE(q.try_push(i));
    }
    EXPECT_EQ(q.size(), 7u);
    for (int i = 0; i < 7; ++i) {
        int v = -1;
        ASSERT_TRUE(q.try_pop(v));
        EXPECT_EQ(v, i);
    }
    EXPECT_TRUE(q.empty());
}

TEST(SpscQueue, RejectsPushWhenFull) {
    SpscQueue<int, 4> q;
    EXPECT_TRUE(q.try_push(1));
    EXPECT_TRUE(q.try_push(2));
    EXPECT_TRUE(q.try_push(3));
    // The fourth push would close the ring on a size-4 queue.
    EXPECT_FALSE(q.try_push(4));
    EXPECT_EQ(q.size(), 3u);
}

TEST(SpscQueue, RejectsPopWhenEmptyAndLeavesOutputUntouched) {
    SpscQueue<int, 4> q;
    int v = 42;
    EXPECT_FALSE(q.try_pop(v));
    EXPECT_EQ(v, 42);
}

TEST(SpscQueue, WrapsAroundManyTimes) {
    // Alternating push/pop drives the head index well past the
    // capacity bound, exercising the bitmask-wrap branches.
    SpscQueue<int, 4> q;
    for (int cycle = 0; cycle < 1000; ++cycle) {
        ASSERT_TRUE(q.try_push(cycle));
        int v = -1;
        ASSERT_TRUE(q.try_pop(v));
        ASSERT_EQ(v, cycle);
    }
    EXPECT_TRUE(q.empty());
}

TEST(SpscQueue, AcceptsMoveOnlyPayload) {
    SpscQueue<std::unique_ptr<int>, 4> q;
    ASSERT_TRUE(q.try_push(std::make_unique<int>(42)));
    std::unique_ptr<int> out;
    ASSERT_TRUE(q.try_pop(out));
    ASSERT_TRUE(out);
    EXPECT_EQ(*out, 42);
}

TEST(SpscQueue, ClearsSlotOnPopSoResourcesReleaseImmediately) {
    // `try_pop` assigns `slots_[tail] = T{}` after moving out — that
    // matters for refcounted payloads (shared_ptr, ComPtr) which
    // would otherwise cling to the slot until a future wrap
    // overwrites it. Under playback that can pin a full-frame
    // texture in VRAM.
    SpscQueue<std::shared_ptr<int>, 4> q;
    auto p = std::make_shared<int>(7);
    ASSERT_TRUE(q.try_push(p));
    // Push copy-constructs into the parameter, moves into the slot.
    // Expect `p` + slot alive.
    EXPECT_EQ(p.use_count(), 2L);

    std::shared_ptr<int> out;
    ASSERT_TRUE(q.try_pop(out));
    // Slot was cleared in-place; only `p` + `out` remain.
    EXPECT_EQ(p.use_count(), 2L);
    out.reset();
    EXPECT_EQ(p.use_count(), 1L);
}

// ---------------------------------------------------------------------------
// Performance: end-to-end SPSC throughput of 1 M items. The queue is
// on the hot audio/video path; any regression that makes push/pop
// synchronous (mutex, wait) will push this well past the ceiling.
//
// The producer is spawned as a `std::jthread` so that an ASSERT_EQ
// failure in the consumer loop aborts the test without orphaning the
// thread — jthread's destructor signals the stop_token and joins.

TEST(SpscQueuePerf, OneMillionItemsCompleteQuickly) {
    SpscQueue<std::uint64_t, 1024> q;
    constexpr std::uint64_t kN = 1'000'000;

    std::jthread producer([&](std::stop_token st) {
        for (std::uint64_t i = 0; i < kN; ++i) {
            while (!q.try_push(i)) {
                if (st.stop_requested()) return;
                std::this_thread::yield();
            }
        }
    });

    const auto t0 = std::chrono::steady_clock::now();
    std::uint64_t expected = 0;
    while (expected < kN) {
        std::uint64_t v = 0;
        if (q.try_pop(v)) {
            ASSERT_EQ(v, expected) << "queue reordered at " << expected;
            ++expected;
        } else {
            std::this_thread::yield();
        }
    }
    const auto elapsed = std::chrono::steady_clock::now() - t0;

    const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
        elapsed).count();
    // Loose ceiling — a modern box runs this in well under a second
    // even with sanitizers. 10s alarms on a hang (e.g. ordering bug
    // causing a spin-forever) without flaking on slow CI runners.
    EXPECT_LT(ms, 10'000);
    EXPECT_TRUE(q.empty());
}

// ---------------------------------------------------------------------------
// Concurrency invariant: under adversarial scheduling (a tiny 64-slot
// ring forces the producer to stall on full frequently), every
// produced item is popped exactly once and in order.

TEST(SpscQueueConcurrency, NoLossNoDuplicationUnderPressure) {
    SpscQueue<std::uint32_t, 64> q;
    constexpr std::uint32_t kN = 200'000;

    std::jthread producer([&](std::stop_token st) {
        for (std::uint32_t i = 0; i < kN; ++i) {
            while (!q.try_push(i)) {
                if (st.stop_requested()) return;
                // Occasional yield while the ring is full.
                if ((i & 0xFFFu) == 0) {
                    std::this_thread::yield();
                }
            }
        }
    });

    std::uint64_t checksum = 0;
    std::uint32_t seen = 0;
    while (seen < kN) {
        std::uint32_t v = 0;
        if (q.try_pop(v)) {
            ASSERT_EQ(v, seen) << "queue reordered at " << seen;
            checksum += v;
            ++seen;
        }
    }

    // Gauss: sum of 0..kN-1.
    const std::uint64_t expected_sum =
        static_cast<std::uint64_t>(kN) * (kN - 1) / 2;
    EXPECT_EQ(checksum, expected_sum);
}

} // namespace
