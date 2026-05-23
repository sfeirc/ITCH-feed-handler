#include "itch/order_book.hpp"

#include <algorithm>
#include <cassert>
#include <cstring>
#include <functional>
#include <immintrin.h>

namespace itch {

// ---------------------------------------------------------------------------
// Construction
// ---------------------------------------------------------------------------

OrderBook::OrderBook() {
    // Zero-fill order map (ref==0 marks empty)
    order_map.fill(OrderEntry{});
    bids.fill(Level{});
    asks.fill(Level{});
    std::memset(bid_prices, 0, sizeof(bid_prices));
    std::memset(ask_prices, 0, sizeof(ask_prices));
    std::memset(order_refs, 0, sizeof(order_refs));
}

// ---------------------------------------------------------------------------
// Internal: hash-map lookup  (Optimisation 3: SIMD 4-way probing)
// ---------------------------------------------------------------------------

size_t OrderBook::find_order(uint64_t ref) const noexcept {
    const size_t start = probe_start(ref);
#ifdef __AVX2__
    const __m256i vref   = _mm256_set1_epi64x(static_cast<int64_t>(ref));
    const __m256i empty4 = _mm256_set1_epi64x(static_cast<int64_t>(ORDER_EMPTY));

    for (size_t i = 0; i < ORDER_MAP_CAP; i += 4) {
        // Quadratic (triangular) probe positions for steps i, i+1, i+2, i+3
        const size_t s0 = (start + (i+0)*(i+1)/2) & ORDER_MAP_MASK;
        const size_t s1 = (start + (i+1)*(i+2)/2) & ORDER_MAP_MASK;
        const size_t s2 = (start + (i+2)*(i+3)/2) & ORDER_MAP_MASK;
        const size_t s3 = (start + (i+3)*(i+4)/2) & ORDER_MAP_MASK;

        __m256i slots = _mm256_set_epi64x(
            static_cast<int64_t>(order_refs[s3]),
            static_cast<int64_t>(order_refs[s2]),
            static_cast<int64_t>(order_refs[s1]),
            static_cast<int64_t>(order_refs[s0]));

        __m256i hit   = _mm256_cmpeq_epi64(slots, vref);
        __m256i empty = _mm256_cmpeq_epi64(slots, empty4);

        const int hit_mask   = _mm256_movemask_epi8(hit);
        const int empty_mask = _mm256_movemask_epi8(empty);

        if (__builtin_expect(hit_mask != 0, 0)) {
            const size_t lane = static_cast<size_t>(__builtin_ctz(static_cast<unsigned>(hit_mask))) / 8u;
            const size_t slots_arr[4] = {s0, s1, s2, s3};
            return slots_arr[lane];
        }
        if (empty_mask != 0) return ORDER_MAP_CAP;  // definitively absent
        // All four were tombstones or other refs — continue
    }
    return ORDER_MAP_CAP;  // not found after full scan
#else
    for (size_t i = 0; i < ORDER_MAP_CAP; ++i) {
        const size_t slot = (start + i * (i + 1) / 2) & ORDER_MAP_MASK;
        if (order_refs[slot] == ref) return slot;
        if (order_refs[slot] == ORDER_EMPTY) return ORDER_MAP_CAP;
    }
    return ORDER_MAP_CAP;
#endif
}

void OrderBook::insert_order(const OrderEntry& entry) noexcept {
    assert(order_count * 2 < ORDER_MAP_CAP && "Order map load factor exceeded 0.5");

    size_t idx = probe_start(entry.ref);
    size_t first_tomb = ORDER_MAP_CAP;  // first tombstone slot encountered

    for (size_t i = 0; i < ORDER_MAP_CAP; ++i) {
        size_t slot = (idx + i * (i + 1) / 2) & ORDER_MAP_MASK;
        const uint64_t slot_ref = order_map[slot].ref;

        if (slot_ref == ORDER_EMPTY) {
            // Use the first tombstone if we saw one; otherwise this slot
            size_t write_slot = (first_tomb < ORDER_MAP_CAP) ? first_tomb : slot;
            order_map[write_slot] = entry;
            order_refs[write_slot] = entry.ref;  // keep parallel array in sync
            assert(order_refs[write_slot] == order_map[write_slot].ref);
            ++order_count;
            return;
        }
        if (slot_ref == ORDER_TOMBSTONE && first_tomb == ORDER_MAP_CAP) {
            first_tomb = slot;
            // Continue probing to ensure no duplicate
        }
        if (slot_ref == entry.ref) {
            // Duplicate insert — overwrite (shouldn't happen in valid ITCH)
            order_map[slot] = entry;
            order_refs[slot] = entry.ref;
            return;
        }
    }
    // Use tombstone slot if available
    if (first_tomb < ORDER_MAP_CAP) {
        order_map[first_tomb] = entry;
        order_refs[first_tomb] = entry.ref;
        assert(order_refs[first_tomb] == order_map[first_tomb].ref);
        ++order_count;
        return;
    }
    assert(false && "Order map full — load factor must be <= 0.5");
}

// ---------------------------------------------------------------------------
// Internal: level management  (Optimisations 1 + 2: SIMD find + memmove)
// ---------------------------------------------------------------------------

// Binary search on bids[].price (stride=sizeof(Level)=24) for descending order.
// Returns lower_bound position. Since Level.price is at offset 0, compares bids[mid].price.
[[gnu::always_inline]] static inline
uint32_t bid_level_lower_bound(const Level* __restrict__ lvl,
                                uint32_t count, uint64_t price) noexcept {
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        const uint32_t mid = lo + ((hi - lo) >> 1);
        if (lvl[mid].price > price) lo = mid + 1;
        else                         hi = mid;
    }
    return lo;
}

// Binary search on asks[].price (ascending order).
[[gnu::always_inline]] static inline
uint32_t ask_level_lower_bound(const Level* __restrict__ lvl,
                                uint32_t count, uint64_t price) noexcept {
    uint32_t lo = 0, hi = count;
    while (lo < hi) {
        const uint32_t mid = lo + ((hi - lo) >> 1);
        if (lvl[mid].price < price) lo = mid + 1;
        else                         hi = mid;
    }
    return lo;
}

void OrderBook::add_to_level(uint64_t price, uint32_t qty, uint8_t side) noexcept {
    if (side == 'B') {
        assert(bid_count < MAX_LEVELS && "Bid level array full");
        // Binary search on bids[].price (no separate bid_prices[] scan needed)
        const uint32_t pos = bid_level_lower_bound(bids.data(), bid_count, price);
        if (pos < bid_count && bids[pos].price == price) {
            // Existing level — update in place
            bids[pos].total_qty += qty;
            ++bids[pos].order_count;
            return;
        }
        // New level — single memmove for bids[]; bid_prices[] updated for debug only
        const uint32_t tail = bid_count - pos;
        if (tail > 0)
            std::memmove(&bids[pos + 1], &bids[pos], tail * sizeof(Level));
        bids[pos] = {price, qty, 1};
#ifndef NDEBUG
        // Keep bid_prices[0] in sync for the debug assert in update_bbo
        bid_prices[0] = bids[0].price;
#endif
        ++bid_count;
    } else {
        assert(ask_count < MAX_LEVELS && "Ask level array full");
        const uint32_t pos = ask_level_lower_bound(asks.data(), ask_count, price);
        if (pos < ask_count && asks[pos].price == price) {
            asks[pos].total_qty += qty;
            ++asks[pos].order_count;
            return;
        }
        const uint32_t tail = ask_count - pos;
        if (tail > 0)
            std::memmove(&asks[pos + 1], &asks[pos], tail * sizeof(Level));
        asks[pos] = {price, qty, 1};
#ifndef NDEBUG
        ask_prices[0] = asks[0].price;
#endif
        ++ask_count;
    }
}

void OrderBook::remove_from_level(uint64_t price, uint32_t qty,
                                   uint8_t side, bool full_remove) noexcept {
    if (side == 'B') {
        // Binary search on bids[].price directly
        const uint32_t i = bid_level_lower_bound(bids.data(), bid_count, price);
        if (__builtin_expect(i >= bid_count || bids[i].price != price, 0)) return;
        bids[i].total_qty -= qty;
        if (full_remove && bids[i].order_count > 0) --bids[i].order_count;
        if (bids[i].total_qty == 0) {
            const uint32_t tail = bid_count - i - 1;
            if (tail > 0)
                std::memmove(&bids[i], &bids[i + 1], tail * sizeof(Level));
#ifndef NDEBUG
            if (bid_count > 1) bid_prices[0] = bids[0].price;
#endif
            --bid_count;
        }
    } else {
        const uint32_t i = ask_level_lower_bound(asks.data(), ask_count, price);
        if (__builtin_expect(i >= ask_count || asks[i].price != price, 0)) return;
        asks[i].total_qty -= qty;
        if (full_remove && asks[i].order_count > 0) --asks[i].order_count;
        if (asks[i].total_qty == 0) {
            const uint32_t tail = ask_count - i - 1;
            if (tail > 0)
                std::memmove(&asks[i], &asks[i + 1], tail * sizeof(Level));
#ifndef NDEBUG
            if (ask_count > 1) ask_prices[0] = asks[0].price;
#endif
            --ask_count;
        }
    }
}

// ---------------------------------------------------------------------------
// Internal: BBO update
// ---------------------------------------------------------------------------

void OrderBook::update_bbo(uint64_t ts_ns) noexcept {
    const uint64_t bp = (bid_count > 0) ? bids[0].price     : 0;
    const uint64_t bq = (bid_count > 0) ? bids[0].total_qty : 0;
    const uint64_t ap = (ask_count > 0) ? asks[0].price     : 0;
    const uint64_t aq = (ask_count > 0) ? asks[0].total_qty : 0;

#ifndef NDEBUG
    // In debug builds: assert bid <= ask when both exist
    if (bp != 0 && ap != 0) {
        assert(bp <= ap && "BBO invariant violated: bid > ask");
    }
    // Verify top-of-book parallel price arrays are in sync (spot-check index 0)
    assert((bid_count == 0 || bid_prices[0] == bids[0].price) && "bid_prices[0] out of sync");
    assert((ask_count == 0 || ask_prices[0] == asks[0].price) && "ask_prices[0] out of sync");
#endif

    // Relaxed stores for prices/qtys — release store for timestamp
    best_bid_price.store(bp, std::memory_order_relaxed);
    best_ask_price.store(ap, std::memory_order_relaxed);
    best_bid_qty.store(bq,   std::memory_order_relaxed);
    best_ask_qty.store(aq,   std::memory_order_relaxed);
    last_update_ns.store(ts_ns, std::memory_order_release);
}

// ---------------------------------------------------------------------------
// Hot-path operations
// ---------------------------------------------------------------------------

void OrderBook::add_order(uint64_t ref, uint64_t price, uint32_t qty,
                          uint8_t side, uint64_t ts_ns) noexcept {
    assert(ref != ORDER_EMPTY && ref != ORDER_TOMBSTONE);
    // Prefetch insert_order's hash slot before add_to_level's memmove (which is
    // long-latency and may evict it from L1). Locality=1 keeps it in L2/L1.
    const size_t insert_probe = probe_start(ref);
    __builtin_prefetch(&order_refs[insert_probe], 1, 1);
    __builtin_prefetch(&order_map[insert_probe],  1, 1);
    OrderEntry e{ref, price, qty, side, {0, 0, 0}};
    add_to_level(price, qty, side);  // memmove executes while prefetch completes
    insert_order(e);
    update_bbo(ts_ns);
}

void OrderBook::cancel_order(uint64_t ref, uint32_t cancelled_qty,
                              uint64_t ts_ns) noexcept {
    const size_t slot = find_order(ref);
    if (__builtin_expect(!!((slot == ORDER_MAP_CAP)), 0)) return;

    auto& e = order_map[slot];
    if (cancelled_qty >= e.qty) {
        // Full cancel — treat as delete
        remove_from_level(e.price, e.qty, e.side);
        e.ref = ORDER_TOMBSTONE;  // tombstone: slot re-usable but probe chain intact
        order_refs[slot] = ORDER_TOMBSTONE;
        --order_count;
    } else {
        // Partial cancel: remove qty from level but order remains (full_remove=false)
        remove_from_level(e.price, cancelled_qty, e.side, /*full_remove=*/false);
        e.qty -= cancelled_qty;
    }
    update_bbo(ts_ns);
}

void OrderBook::delete_order(uint64_t ref, uint64_t ts_ns) noexcept {
    const size_t slot = find_order(ref);
    if (__builtin_expect(!!((slot == ORDER_MAP_CAP)), 0)) return;

    const auto& e = order_map[slot];
    const uint64_t del_price = e.price;
    const uint32_t del_qty   = e.qty;
    const uint8_t  del_side  = e.side;

    // Tombstone BEFORE the memmove so order_refs[slot] stays in L1 during memmove
    order_map[slot].ref = ORDER_TOMBSTONE;
    order_refs[slot] = ORDER_TOMBSTONE;
    --order_count;

    remove_from_level(del_price, del_qty, del_side);
    update_bbo(ts_ns);
}

void OrderBook::replace_order(uint64_t orig_ref, uint64_t new_ref,
                               uint32_t new_qty, uint64_t new_price,
                               uint64_t ts_ns) noexcept {
    const size_t slot = find_order(orig_ref);
    if (__builtin_expect(!!((slot == ORDER_MAP_CAP)), 0)) return;

    // Capture old values before modifying
    const auto old_e = order_map[slot];

    // Remove old order from book
    remove_from_level(old_e.price, old_e.qty, old_e.side);
    order_map[slot].ref = ORDER_TOMBSTONE;  // tombstone to preserve probe chains
    order_refs[slot] = ORDER_TOMBSTONE;
    --order_count;

    // Insert new order
    OrderEntry new_e{new_ref, new_price, new_qty, old_e.side, {0, 0, 0}};
    insert_order(new_e);
    add_to_level(new_price, new_qty, old_e.side);
    update_bbo(ts_ns);
}

void OrderBook::execute_order(uint64_t ref, uint32_t executed_qty,
                               uint64_t ts_ns) noexcept {
    const size_t slot = find_order(ref);
    if (__builtin_expect(!!((slot == ORDER_MAP_CAP)), 0)) return;

    auto& e = order_map[slot];
    const uint32_t fill = (executed_qty < e.qty) ? executed_qty : e.qty;
    const bool full = (fill >= e.qty);
    // full_remove=true only when the entire order is consumed
    remove_from_level(e.price, fill, e.side, /*full_remove=*/full);
    if (full) {
        e.ref = ORDER_TOMBSTONE;
        order_refs[slot] = ORDER_TOMBSTONE;
        --order_count;
    } else {
        e.qty -= fill;
    }
    update_bbo(ts_ns);
}

} // namespace itch
