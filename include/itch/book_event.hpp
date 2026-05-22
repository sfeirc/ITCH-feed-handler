#pragma once

#include <cstdint>
#include <cstddef>

namespace itch {

/// Event types for BookEvent
enum class EventType : uint8_t {
    ADD     = 0,
    CANCEL  = 1,
    EXECUTE = 2,
    REPLACE = 3,
    DELETE  = 4,
};

/// BookEvent must fit in exactly one 64-byte cache line.
/// Readers can poll without locking — each field is plain value copy.
struct alignas(64) BookEvent {
    uint64_t hardware_ts_ns;   ///< NIC hardware timestamp (ns)
    uint64_t itch_ts_ns;       ///< ITCH wire timestamp (ns since midnight)
    uint64_t price;            ///< Price × 10000 (e.g. $1.2345 → 12345)
    uint64_t order_ref;        ///< ITCH order reference number
    uint64_t best_bid;         ///< BBO bid price × 10000
    uint64_t best_ask;         ///< BBO ask price × 10000
    uint32_t qty;              ///< Quantity for this event
    uint16_t stock_locate;     ///< ITCH stock-locate token
    uint8_t  event_type;       ///< EventType cast to uint8_t
    uint8_t  side;             ///< 'B' or 'S'
    uint32_t best_bid_qty;     ///< Quantity at best bid
    uint32_t best_ask_qty;     ///< Quantity at best ask
};

static_assert(sizeof(BookEvent) == 64, "BookEvent must be exactly one cache line");
static_assert(alignof(BookEvent) == 64, "BookEvent must be cache-line aligned");

} // namespace itch
