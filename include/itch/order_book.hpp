#pragma once

#include <array>
#include <atomic>
#include <cstdint>
#include <cstddef>
#include <cassert>
#include <algorithm>
#include <cstring>

namespace itch {

/// A single price level in the order book.
/// Kept sorted by price: bids descending, asks ascending.
/// Packed to 16 bytes so memmove touches fewer cache lines during level shifts.
struct Level {
    uint64_t price;        ///< Price × 10000
    uint32_t total_qty;    ///< Sum of quantities at this level (saturates at ~4.3B)
    uint32_t order_count;  ///< Number of live orders at this level
};

/// Sentinel values for the order map.
/// ITCH order reference numbers start at 1 so 0 is safe as "empty".
/// UINT64_MAX is used as a tombstone for deleted slots.
static constexpr uint64_t ORDER_EMPTY    = 0;
static constexpr uint64_t ORDER_TOMBSTONE = UINT64_MAX;

/// One entry in the open-addressed order map.
/// Uses ref==ORDER_EMPTY as empty slot, ref==ORDER_TOMBSTONE as deleted.
struct OrderEntry {
    uint64_t ref;       ///< Order ref number; ORDER_EMPTY=free, ORDER_TOMBSTONE=deleted
    uint64_t price;     ///< Price × 10000
    uint32_t qty;       ///< Current resting quantity
    uint8_t  side;      ///< 'B' = bid, 'S' = ask
    uint8_t  _pad[3];
};

/// Maximum number of price levels stored per side.
/// ITCH 5.0 real data: Nasdaq top-of-book typically has < 50 levels per side.
/// 1024 covers extreme fragmentation scenarios.
static constexpr size_t MAX_LEVELS = 1024;

/// Open-addressed hash-map capacity for order lookup.
/// Quadratic probing; load factor must stay <= 0.5.
/// 1<<17 = 131072 slots → max 65536 live orders per symbol.
/// Real Nasdaq data: ~3000-8000 live orders per busy symbol at peak.
static constexpr size_t ORDER_MAP_CAP  = 1u << 17;  // 131072
static constexpr size_t ORDER_MAP_MASK = ORDER_MAP_CAP - 1;

/// Order book for a single instrument.
///
/// Hot-path operations (add_order, cancel_order, delete_order, replace_order,
/// execute_order) are zero-heap-allocation. All storage is pre-allocated as
/// value members.
///
/// BBO cache uses sequenced atomic stores:
///   Writer: prices/qtys with relaxed, timestamp with release.
///   Reader: acquire-load timestamp, then relaxed-load prices.
class OrderBook {
public:
    OrderBook();
    OrderBook(const OrderBook&) = delete;
    OrderBook& operator=(const OrderBook&) = delete;

    // ---- Hot-path operations ----

    void add_order    (uint64_t ref, uint64_t price, uint32_t qty,
                       uint8_t side, uint64_t ts_ns) noexcept;

    void cancel_order (uint64_t ref, uint32_t cancelled_qty,
                       uint64_t ts_ns) noexcept;

    void delete_order (uint64_t ref, uint64_t ts_ns) noexcept;

    void replace_order(uint64_t orig_ref, uint64_t new_ref,
                       uint32_t new_qty, uint64_t new_price,
                       uint64_t ts_ns) noexcept;

    void execute_order(uint64_t ref, uint32_t executed_qty,
                       uint64_t ts_ns) noexcept;

    // ---- BBO accessors (reader-side) ----

    /// Snapshot the BBO. Returns false if the book has never been updated.
    struct BBO {
        uint64_t bid_price;
        uint64_t ask_price;
        uint64_t bid_qty;
        uint64_t ask_qty;
        uint64_t ts_ns;
    };

    [[nodiscard]] BBO bbo() const noexcept {
        // Acquire-load timestamp as the synchronization point
        const uint64_t ts = last_update_ns.load(std::memory_order_acquire);
        BBO b{};
        b.bid_price = best_bid_price.load(std::memory_order_relaxed);
        b.ask_price = best_ask_price.load(std::memory_order_relaxed);
        b.bid_qty   = best_bid_qty.load(std::memory_order_relaxed);
        b.ask_qty   = best_ask_qty.load(std::memory_order_relaxed);
        b.ts_ns     = ts;
        return b;
    }

    // ---- Public storage (inspectable by benchmarks) ----

    std::array<Level, MAX_LEVELS> bids{};
    std::array<Level, MAX_LEVELS> asks{};
    uint32_t bid_count = 0;
    uint32_t ask_count = 0;

    // Parallel flat price arrays for SIMD level search — kept in sync with bids[]/asks[].
    alignas(64) uint64_t bid_prices[MAX_LEVELS]{};
    alignas(64) uint64_t ask_prices[MAX_LEVELS]{};

    std::array<OrderEntry, ORDER_MAP_CAP> order_map{};
    uint32_t order_count = 0;

    // Parallel ref array for SIMD hash-map probing — mirrors order_map[i].ref.
    // Kept on its own cache lines so SIMD comparisons stay hot while full entry
    // data is only fetched on a confirmed hit.
    alignas(64) uint64_t order_refs[ORDER_MAP_CAP]{};

    // Atomic BBO cache — each on its own cache line to prevent false sharing
    alignas(64) std::atomic<uint64_t> best_bid_price{0};
    alignas(64) std::atomic<uint64_t> best_ask_price{0};
    alignas(64) std::atomic<uint64_t> best_bid_qty{0};
    alignas(64) std::atomic<uint64_t> best_ask_qty{0};
    alignas(64) std::atomic<uint64_t> last_update_ns{0};

private:
    // ---- Internals ----

    void update_bbo(uint64_t ts_ns) noexcept;

    [[nodiscard]]
    size_t find_order(uint64_t ref) const noexcept;

    void insert_order(const OrderEntry& e) noexcept;

    // full_remove=true: decrements level order_count (order deleted entirely).
    // full_remove=false: only removes qty (order partially cancelled, order remains).
    void remove_from_level(uint64_t price, uint32_t qty, uint8_t side,
                           bool full_remove = true) noexcept;

    void add_to_level(uint64_t price, uint32_t qty, uint8_t side) noexcept;

    static __attribute__((always_inline))
    size_t probe_start(uint64_t ref) noexcept {
        // Fibonacci hashing for good dispersion
        return static_cast<size_t>((ref * 11400714819323198485ULL) >> (64 - 17));
    }
};

} // namespace itch
