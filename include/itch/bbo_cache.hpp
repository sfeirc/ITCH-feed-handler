#pragma once

#include <atomic>
#include <cstdint>

namespace itch {

/// Standalone atomic BBO cache suitable for sharing across threads.
///
/// Writer protocol (producer):
///   1. Relaxed-store bid/ask prices and quantities.
///   2. Release-store timestamp to publish the update.
///
/// Reader protocol (consumer):
///   1. Acquire-load timestamp (synchronises with writer's release-store).
///   2. Relaxed-load prices and quantities.
///
/// This is a seqlock-free approach because only one writer updates at a time
/// (the parser thread). Multiple readers get a consistent snapshot provided
/// they observe the same timestamp before and after reading prices.
/// For single-writer / multiple-reader use, this is sufficient.
struct BboCache {
    alignas(64) std::atomic<uint64_t> bid_price{0};
    char _p0[64 - sizeof(std::atomic<uint64_t>)];

    alignas(64) std::atomic<uint64_t> ask_price{0};
    char _p1[64 - sizeof(std::atomic<uint64_t>)];

    alignas(64) std::atomic<uint64_t> bid_qty{0};
    char _p2[64 - sizeof(std::atomic<uint64_t>)];

    alignas(64) std::atomic<uint64_t> ask_qty{0};
    char _p3[64 - sizeof(std::atomic<uint64_t>)];

    alignas(64) std::atomic<uint64_t> ts_ns{0};
    char _p4[64 - sizeof(std::atomic<uint64_t>)];

    struct Snapshot {
        uint64_t bid_price;
        uint64_t ask_price;
        uint64_t bid_qty;
        uint64_t ask_qty;
        uint64_t ts_ns;
        [[nodiscard]] bool valid() const noexcept { return ts_ns != 0; }
    };

    __attribute__((always_inline))
    void store(uint64_t bp, uint64_t aq, uint64_t bq, uint64_t aqq, uint64_t ts) noexcept {
        bid_price.store(bp,  std::memory_order_relaxed);
        ask_price.store(aq,  std::memory_order_relaxed);
        bid_qty.store(bq,    std::memory_order_relaxed);
        ask_qty.store(aqq,   std::memory_order_relaxed);
        ts_ns.store(ts,      std::memory_order_release);
    }

    [[nodiscard]] __attribute__((always_inline))
    Snapshot load() const noexcept {
        const uint64_t t  = ts_ns.load(std::memory_order_acquire);
        const uint64_t bp = bid_price.load(std::memory_order_relaxed);
        const uint64_t ap = ask_price.load(std::memory_order_relaxed);
        const uint64_t bq = bid_qty.load(std::memory_order_relaxed);
        const uint64_t aq = ask_qty.load(std::memory_order_relaxed);
        return {bp, ap, bq, aq, t};
    }
};

} // namespace itch
