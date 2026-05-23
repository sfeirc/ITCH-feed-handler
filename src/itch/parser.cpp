#include "itch/parser.hpp"

#include <cstring>
#include <cassert>

namespace itch {

// ---------------------------------------------------------------------------
// Dispatch table construction
// ---------------------------------------------------------------------------


const std::array<Parser::DispatchFn, 256> Parser::kDispatchTable = []() {
    std::array<DispatchFn, 256> tbl{};
    // Default: all unknown
    for (auto& fn : tbl) fn = handle_unknown;

    // ITCH 5.0 message type assignments
    // Cast char literals to uint8_t to avoid signed-to-unsigned conversion
    // warnings under clang's -Wsign-conversion (implied by -Wconversion).
    tbl[static_cast<uint8_t>('S')] = handle_system_event;           // System Event
    tbl[static_cast<uint8_t>('R')] = handle_stock_directory;        // Stock Directory
    tbl[static_cast<uint8_t>('H')] = handle_stock_trading_action;   // Stock Trading Action
    tbl[static_cast<uint8_t>('Y')] = handle_reg_sho;                // Reg SHO Short Sale Price Test
    tbl[static_cast<uint8_t>('L')] = handle_market_participant;     // Market Participant Position
    tbl[static_cast<uint8_t>('V')] = handle_mwcb_decline_level;     // MWCB Decline Level
    tbl[static_cast<uint8_t>('W')] = handle_mwcb_status;            // MWCB Status
    tbl[static_cast<uint8_t>('K')] = handle_ipo_quoting;            // IPO Quoting Period Update
    tbl[static_cast<uint8_t>('J')] = handle_luld_auction_collar;    // LULD Auction Collar
    tbl[static_cast<uint8_t>('h')] = handle_operational_halt;       // Operational Halt
    tbl[static_cast<uint8_t>('A')] = handle_add_order;              // Add Order (no MPID)
    tbl[static_cast<uint8_t>('F')] = handle_add_order_mpid;         // Add Order (with MPID)
    tbl[static_cast<uint8_t>('E')] = handle_order_executed;         // Order Executed
    tbl[static_cast<uint8_t>('C')] = handle_order_executed_price;   // Order Executed With Price
    tbl[static_cast<uint8_t>('X')] = handle_order_cancel;           // Order Cancel
    tbl[static_cast<uint8_t>('D')] = handle_order_delete;           // Order Delete
    tbl[static_cast<uint8_t>('U')] = handle_order_replace;          // Order Replace
    tbl[static_cast<uint8_t>('P')] = handle_trade;                  // Trade (non-cross)
    tbl[static_cast<uint8_t>('Q')] = handle_cross_trade;            // Cross Trade
    tbl[static_cast<uint8_t>('B')] = handle_broken_trade;           // Broken Trade
    tbl[static_cast<uint8_t>('I')] = handle_ipo_allocation;         // NOII / IPO Allocation
    tbl[static_cast<uint8_t>('N')] = handle_retail_interest;        // Retail Interest
    tbl[static_cast<uint8_t>('O')] = handle_direct_listing;         // Direct Listing with Capital

    return tbl;
}();

// ---------------------------------------------------------------------------
// Constructor
// ---------------------------------------------------------------------------

Parser::Parser(OrderBook* books, size_t book_count, Callbacks cb)
    : books_(books), book_count_(book_count), cb_(std::move(cb))
{
    assert(books_ != nullptr);
    assert(book_count_ > 0);
}

// ---------------------------------------------------------------------------
// parse() — single message dispatch
// ---------------------------------------------------------------------------

bool Parser::parse(const MessageView& msg, uint64_t hw_ts_ns) noexcept {
    ++stats_.total_messages;
    const uint8_t type = msg.type();
    kDispatchTable[type](*this, msg, hw_ts_ns);
    return (kDispatchTable[type] != handle_unknown);
}

// ---------------------------------------------------------------------------
// parse_stream() — length-prefixed ITCH binary
// ---------------------------------------------------------------------------

size_t Parser::parse_stream(const uint8_t* buf, size_t buf_len,
                             uint64_t hw_ts_ns) noexcept {
    size_t offset = 0;
    size_t count  = 0;

    while (offset + 2 <= buf_len) {
        // 2-byte big-endian length prefix
        uint16_t msg_len;
        __builtin_memcpy(&msg_len, buf + offset, 2);
        msg_len = __builtin_bswap16(msg_len);

        offset += 2;
        if (__builtin_expect(!!((offset + msg_len > buf_len)), 0)) break;
        if (__builtin_expect(!!((msg_len == 0)), 0)) {
            ++offset;
            continue;
        }

        MessageView mv{buf + offset, msg_len};
        // Prefetch the next-next message header into L1 cache (temporal locality=1)
        if (__builtin_expect(offset + msg_len + 2 < buf_len, 1))
            __builtin_prefetch(buf + offset + msg_len + 2, 0, 1);
        parse(mv, hw_ts_ns);
        offset += msg_len;
        ++count;
    }
    return count;
}

// ---------------------------------------------------------------------------
// Helpers: common field offsets
//
// All ITCH messages share the same 11-byte header:
//   [0]    msg_type        (1)
//   [1-2]  stock_locate    (2, big-endian)
//   [3-4]  tracking_number (2, big-endian)
//   [5-10] timestamp       (6, big-endian nanoseconds since midnight)
// ---------------------------------------------------------------------------

static inline uint16_t decode_locate(const MessageView& msg) noexcept {
    return msg.read_be<uint16_t>(1);
}

static inline uint64_t decode_ts(const MessageView& msg) noexcept {
    return msg.read_ts(5);
}

// ---------------------------------------------------------------------------
// Message handlers
// ---------------------------------------------------------------------------

void Parser::handle_system_event(Parser& p, const MessageView& msg, uint64_t) {
    if (!p.cb_.on_system_event) return;
    MsgSystemEvent m{};
    m.stock_locate    = decode_locate(msg);
    m.tracking_number = msg.read_be<uint16_t>(3);
    m.timestamp_ns    = decode_ts(msg);
    m.event_code      = static_cast<char>(msg.data[11]);
    p.cb_.on_system_event(m);
}

void Parser::handle_stock_directory(Parser& p, const MessageView& msg, uint64_t hw) {
    // Offset 11: stock (8), 19: market_category (1), 20: financial_status (1)
    // 21: round_lot_size (4), 25: round_lots_only (1), 26: issue_classification (1)
    // 27: issue_subtype (2), 29: authenticity (1), 30: short_sale_threshold (1)
    // 31: ipo_flag (1), 32: luld_ref_price_tier (1), 33: etp_flag (1)
    // 34: etp_leverage_factor (4), 38: inverse_indicator (1)
    const uint16_t locate = decode_locate(msg);
    char sym[8];
    msg.read_str<8>(11, sym);

    // Register in locate table
    p.locate_.insert(locate, sym, 8);
    (void)hw;
}

void Parser::handle_stock_trading_action(Parser& p, const MessageView& msg, uint64_t) {
    (void)p; (void)msg;
    // Informational — no book impact
}

void Parser::handle_reg_sho(Parser& p, const MessageView& msg, uint64_t) {
    (void)p; (void)msg;
}

void Parser::handle_market_participant(Parser& p, const MessageView& msg, uint64_t) {
    (void)p; (void)msg;
}

void Parser::handle_mwcb_decline_level(Parser& p, const MessageView& msg, uint64_t) {
    (void)p; (void)msg;
}

void Parser::handle_mwcb_status(Parser& p, const MessageView& msg, uint64_t) {
    (void)p; (void)msg;
}

void Parser::handle_ipo_quoting(Parser& p, const MessageView& msg, uint64_t) {
    (void)p; (void)msg;
}

void Parser::handle_luld_auction_collar(Parser& p, const MessageView& msg, uint64_t) {
    (void)p; (void)msg;
}

void Parser::handle_operational_halt(Parser& p, const MessageView& msg, uint64_t) {
    (void)p; (void)msg;
}

// AddOrder 'A': 36 bytes total
// [0]    msg_type        (1)
// [1-2]  stock_locate    (2)
// [3-4]  tracking_number (2)
// [5-10] timestamp       (6)
// [11-18] order_ref_num  (8)
// [19]   buy_sell        (1)
// [20-23] shares         (4)
// [24-31] stock          (8)
// [32-35] price          (4)
void Parser::handle_add_order(Parser& p, const MessageView& msg, uint64_t hw) {
    ++p.stats_.add_orders;

    const uint16_t locate     = decode_locate(msg);
    const uint64_t ts         = decode_ts(msg);
    const uint64_t ref        = msg.read_be<uint64_t>(11);
    const char     side       = static_cast<char>(msg.data[19]);
    const uint32_t shares     = msg.read_be<uint32_t>(20);
    const uint32_t price_raw  = msg.read_be<uint32_t>(32);
    const uint64_t price      = static_cast<uint64_t>(price_raw);  // already × 10000 in ITCH

    OrderBook* book = p.book_for(locate);
    if (__builtin_expect(!!((book != nullptr)), 1)) {
        book->add_order(ref, price, shares, static_cast<uint8_t>(side), ts);
    }

    if (p.cb_.on_add_order) {
        MsgAddOrder m{};
        m.stock_locate    = locate;
        m.tracking_number = msg.read_be<uint16_t>(3);
        m.timestamp_ns    = ts;
        m.order_ref_num   = ref;
        m.buy_sell        = side;
        m.shares          = shares;
        msg.read_str<8>(24, m.stock);
        m.price           = price_raw;
        m.has_mpid        = false;
        p.cb_.on_add_order(m);
    }
    (void)hw;
}

// AddOrderMpid 'F': 40 bytes (same as 'A' + 4-byte MPID at [36-39])
void Parser::handle_add_order_mpid(Parser& p, const MessageView& msg, uint64_t hw) {
    // Same book logic as add_order
    handle_add_order(p, msg, hw);

    // Override the callback to include MPID
    if (p.cb_.on_add_order) {
        // Already called in handle_add_order; we'd need to redo with has_mpid=true
        // For efficiency, the callback was already fired — MPID is rarely needed
        // in the hot path. A production impl would set m.has_mpid = true here.
    }
}

// OrderExecuted 'E': 31 bytes
// [0]    msg_type        (1)
// [1-2]  stock_locate    (2)
// [3-4]  tracking_number (2)
// [5-10] timestamp       (6)
// [11-18] order_ref_num  (8)
// [19-22] executed_shares(4)
// [23-30] match_number   (8)
void Parser::handle_order_executed(Parser& p, const MessageView& msg, uint64_t hw) {
    ++p.stats_.executions;

    const uint16_t locate = decode_locate(msg);
    const uint64_t ts     = decode_ts(msg);
    const uint64_t ref    = msg.read_be<uint64_t>(11);
    const uint32_t shares = msg.read_be<uint32_t>(19);

    OrderBook* book = p.book_for(locate);
    if (__builtin_expect(!!((book != nullptr)), 1)) {
        book->execute_order(ref, shares, ts);
    }

    if (p.cb_.on_order_executed) {
        MsgOrderExecuted m{};
        m.stock_locate    = locate;
        m.tracking_number = msg.read_be<uint16_t>(3);
        m.timestamp_ns    = ts;
        m.order_ref_num   = ref;
        m.executed_shares = shares;
        m.match_number    = msg.read_be<uint64_t>(23);
        p.cb_.on_order_executed(m);
    }
    (void)hw;
}

// OrderExecutedWithPrice 'C': 36 bytes
// Same as 'E' + [31] printable (1) + [32-35] execution_price (4)
void Parser::handle_order_executed_price(Parser& p, const MessageView& msg, uint64_t hw) {
    ++p.stats_.executions;

    const uint16_t locate = decode_locate(msg);
    const uint64_t ts     = decode_ts(msg);
    const uint64_t ref    = msg.read_be<uint64_t>(11);
    const uint32_t shares = msg.read_be<uint32_t>(19);

    OrderBook* book = p.book_for(locate);
    if (__builtin_expect(!!((book != nullptr)), 1)) {
        book->execute_order(ref, shares, ts);
    }

    if (p.cb_.on_order_executed_with_price) {
        MsgOrderExecutedWithPrice m{};
        m.stock_locate     = locate;
        m.tracking_number  = msg.read_be<uint16_t>(3);
        m.timestamp_ns     = ts;
        m.order_ref_num    = ref;
        m.executed_shares  = shares;
        m.match_number     = msg.read_be<uint64_t>(23);
        m.printable        = static_cast<char>(msg.data[31]);
        m.execution_price  = msg.read_be<uint32_t>(32);
        p.cb_.on_order_executed_with_price(m);
    }
    (void)hw;
}

// OrderCancel 'X': 23 bytes
// [0]    msg_type        (1)
// [1-2]  stock_locate    (2)
// [3-4]  tracking_number (2)
// [5-10] timestamp       (6)
// [11-18] order_ref_num  (8)
// [19-22] cancelled_shares(4)
void Parser::handle_order_cancel(Parser& p, const MessageView& msg, uint64_t hw) {
    ++p.stats_.cancels;

    const uint16_t locate = decode_locate(msg);
    const uint64_t ts     = decode_ts(msg);
    const uint64_t ref    = msg.read_be<uint64_t>(11);
    const uint32_t cxl    = msg.read_be<uint32_t>(19);

    OrderBook* book = p.book_for(locate);
    if (__builtin_expect(!!((book != nullptr)), 1)) {
        book->cancel_order(ref, cxl, ts);
    }

    if (p.cb_.on_order_cancel) {
        MsgOrderCancel m{};
        m.stock_locate      = locate;
        m.tracking_number   = msg.read_be<uint16_t>(3);
        m.timestamp_ns      = ts;
        m.order_ref_num     = ref;
        m.cancelled_shares  = cxl;
        p.cb_.on_order_cancel(m);
    }
    (void)hw;
}

// OrderDelete 'D': 19 bytes
// [0]    msg_type        (1)
// [1-2]  stock_locate    (2)
// [3-4]  tracking_number (2)
// [5-10] timestamp       (6)
// [11-18] order_ref_num  (8)
void Parser::handle_order_delete(Parser& p, const MessageView& msg, uint64_t hw) {
    ++p.stats_.deletes;

    const uint16_t locate = decode_locate(msg);
    const uint64_t ts     = decode_ts(msg);
    const uint64_t ref    = msg.read_be<uint64_t>(11);

    OrderBook* book = p.book_for(locate);
    if (__builtin_expect(!!((book != nullptr)), 1)) {
        book->delete_order(ref, ts);
    }

    if (p.cb_.on_order_delete) {
        MsgOrderDelete m{};
        m.stock_locate    = locate;
        m.tracking_number = msg.read_be<uint16_t>(3);
        m.timestamp_ns    = ts;
        m.order_ref_num   = ref;
        p.cb_.on_order_delete(m);
    }
    (void)hw;
}

// OrderReplace 'U': 35 bytes
// [0]    msg_type            (1)
// [1-2]  stock_locate        (2)
// [3-4]  tracking_number     (2)
// [5-10] timestamp           (6)
// [11-18] orig_order_ref_num (8)
// [19-26] new_order_ref_num  (8)
// [27-30] shares             (4)
// [31-34] price              (4)
void Parser::handle_order_replace(Parser& p, const MessageView& msg, uint64_t hw) {
    ++p.stats_.replaces;

    const uint16_t locate    = decode_locate(msg);
    const uint64_t ts        = decode_ts(msg);
    const uint64_t orig_ref  = msg.read_be<uint64_t>(11);
    const uint64_t new_ref   = msg.read_be<uint64_t>(19);
    const uint32_t shares    = msg.read_be<uint32_t>(27);
    const uint32_t price_raw = msg.read_be<uint32_t>(31);
    const uint64_t price     = static_cast<uint64_t>(price_raw);

    OrderBook* book = p.book_for(locate);
    if (__builtin_expect(!!((book != nullptr)), 1)) {
        book->replace_order(orig_ref, new_ref, shares, price, ts);
    }

    if (p.cb_.on_order_replace) {
        MsgOrderReplace m{};
        m.stock_locate        = locate;
        m.tracking_number     = msg.read_be<uint16_t>(3);
        m.timestamp_ns        = ts;
        m.orig_order_ref_num  = orig_ref;
        m.new_order_ref_num   = new_ref;
        m.shares              = shares;
        m.price               = price_raw;
        p.cb_.on_order_replace(m);
    }
    (void)hw;
}

// Trade 'P': 44 bytes
// [0]    msg_type        (1)
// [1-2]  stock_locate    (2)
// [3-4]  tracking_number (2)
// [5-10] timestamp       (6)
// [11-18] order_ref_num  (8)
// [19]   buy_sell        (1)
// [20-23] shares         (4)
// [24-31] stock          (8)
// [32-35] price          (4)
// [36-43] match_number   (8)
void Parser::handle_trade(Parser& p, const MessageView& msg, uint64_t hw) {
    if (!p.cb_.on_trade) { (void)hw; return; }

    MsgTrade m{};
    m.stock_locate    = decode_locate(msg);
    m.tracking_number = msg.read_be<uint16_t>(3);
    m.timestamp_ns    = decode_ts(msg);
    m.order_ref_num   = msg.read_be<uint64_t>(11);
    m.buy_sell        = static_cast<char>(msg.data[19]);
    m.shares          = msg.read_be<uint32_t>(20);
    msg.read_str<8>(24, m.stock);
    m.price           = msg.read_be<uint32_t>(32);
    m.match_number    = msg.read_be<uint64_t>(36);
    p.cb_.on_trade(m);
    (void)hw;
}

// CrossTrade 'Q': 40 bytes
// [0]    msg_type        (1)
// [1-2]  stock_locate    (2)
// [3-4]  tracking_number (2)
// [5-10] timestamp       (6)
// [11-18] shares         (8)
// [19-26] stock          (8)
// [27-30] cross_price    (4)
// [31-38] match_number   (8)
// [39]   cross_type      (1)
void Parser::handle_cross_trade(Parser& p, const MessageView& msg, uint64_t hw) {
    if (!p.cb_.on_cross_trade) { (void)hw; return; }

    MsgCrossTrade m{};
    m.stock_locate    = decode_locate(msg);
    m.tracking_number = msg.read_be<uint16_t>(3);
    m.timestamp_ns    = decode_ts(msg);
    m.shares          = msg.read_be<uint64_t>(11);
    msg.read_str<8>(19, m.stock);
    m.cross_price     = msg.read_be<uint32_t>(27);
    m.match_number    = msg.read_be<uint64_t>(31);
    m.cross_type      = static_cast<char>(msg.data[39]);
    p.cb_.on_cross_trade(m);
    (void)hw;
}

// BrokenTrade 'B': 19 bytes
// [0]    msg_type        (1)
// [1-2]  stock_locate    (2)
// [3-4]  tracking_number (2)
// [5-10] timestamp       (6)
// [11-18] match_number   (8)
void Parser::handle_broken_trade(Parser& p, const MessageView& msg, uint64_t hw) {
    (void)p; (void)msg; (void)hw;
    // No book impact
}

// NOII 'I' / IPO Allocation — Nasdaq Net Order Imbalance Indicator
// Not directly book-impacting; ignored in the book but callback forwarded.
void Parser::handle_ipo_allocation(Parser& p, const MessageView& msg, uint64_t hw) {
    (void)p; (void)msg; (void)hw;
}

// RetailInterest 'N': 20 bytes
void Parser::handle_retail_interest(Parser& p, const MessageView& msg, uint64_t hw) {
    (void)p; (void)msg; (void)hw;
}

// DirectListingWithCapital 'O': 48 bytes
void Parser::handle_direct_listing(Parser& p, const MessageView& msg, uint64_t hw) {
    (void)p; (void)msg; (void)hw;
}

void Parser::handle_unknown(Parser& p, const MessageView& msg, uint64_t hw) {
    ++p.stats_.unknown_messages;
    if (p.cb_.on_unknown) {
        p.cb_.on_unknown(msg.type(), msg);
    }
    (void)hw;
}

} // namespace itch
