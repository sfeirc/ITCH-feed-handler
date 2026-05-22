#pragma once

#include "itch/message_view.hpp"
#include "itch/order_book.hpp"
#include "itch/book_event.hpp"
#include "itch/stock_locate.hpp"

#include <array>
#include <cstdint>
#include <cstddef>
#include <functional>

namespace itch {

// ---------------------------------------------------------------------------
// ITCH 5.0 message structs
// All fields decoded from big-endian wire format.
// ---------------------------------------------------------------------------

struct MsgSystemEvent {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    char     event_code;     // O=start of messages, S=start of system hours, etc.
};

struct MsgStockDirectory {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    char     stock[8];
    char     market_category;
    char     financial_status;
    uint32_t round_lot_size;
    char     round_lots_only;
    char     issue_classification;
    char     issue_subtype[2];
    char     authenticity;
    char     short_sale_threshold;
    char     ipo_flag;
    char     luld_ref_price_tier;
    char     etp_flag;
    uint32_t etp_leverage_factor;
    char     inverse_indicator;
};

struct MsgStockTradingAction {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    char     stock[8];
    char     trading_state;
    char     reserved;
    char     reason[4];
};

struct MsgRegSho {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    char     stock[8];
    char     reg_sho_action;
};

struct MsgMarketParticipantPosition {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    char     mpid[4];
    char     stock[8];
    char     primary_market_maker;
    char     market_maker_mode;
    char     market_participant_state;
};

struct MsgMwcbDeclineLevel {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    uint64_t level1;   // Price × 10000
    uint64_t level2;
    uint64_t level3;
};

struct MsgMwcbStatus {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    char     breached_level;
};

struct MsgIpoQuotingPeriod {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    char     stock[8];
    uint32_t ipo_quotation_release_time;
    char     ipo_quotation_release_qualifier;
    uint32_t ipo_price;   // Price × 10000
};

struct MsgLuldAuctionCollar {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    char     stock[8];
    uint32_t auction_collar_ref_price;
    uint32_t upper_auction_collar_price;
    uint32_t lower_auction_collar_price;
    uint32_t auction_collar_extension;
};

struct MsgOperationalHalt {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    char     stock[8];
    char     market_code;
    char     operational_halt_action;
};

struct MsgAddOrder {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    uint64_t order_ref_num;
    char     buy_sell;       // 'B' or 'S'
    uint32_t shares;
    char     stock[8];
    uint32_t price;          // Price × 10000
    // mpid field (AddOrderMpid 'F' only)
    char     mpid[4];
    bool     has_mpid;
};

struct MsgOrderExecuted {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    uint64_t order_ref_num;
    uint32_t executed_shares;
    uint64_t match_number;
};

struct MsgOrderExecutedWithPrice {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    uint64_t order_ref_num;
    uint32_t executed_shares;
    uint64_t match_number;
    char     printable;
    uint32_t execution_price;  // Price × 10000
};

struct MsgOrderCancel {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    uint64_t order_ref_num;
    uint32_t cancelled_shares;
};

struct MsgOrderDelete {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    uint64_t order_ref_num;
};

struct MsgOrderReplace {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    uint64_t orig_order_ref_num;
    uint64_t new_order_ref_num;
    uint32_t shares;
    uint32_t price;   // Price × 10000
};

struct MsgTrade {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    uint64_t order_ref_num;
    char     buy_sell;
    uint32_t shares;
    char     stock[8];
    uint32_t price;        // Price × 10000
    uint64_t match_number;
};

struct MsgCrossTrade {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    uint64_t shares;
    char     stock[8];
    uint32_t cross_price;   // Price × 10000
    uint64_t match_number;
    char     cross_type;
};

struct MsgBrokenTrade {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    uint64_t match_number;
};

struct MsgIpoAllocation {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    char     stock[8];
    uint32_t shares;
    char     market_code;
    uint32_t price;     // Price × 10000
};

struct MsgRetailInterest {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    char     stock[8];
    char     interest_flag;
};

struct MsgDirectListingWithCapital {
    uint16_t stock_locate;
    uint16_t tracking_number;
    uint64_t timestamp_ns;
    char     stock[8];
    char     open_eligibility_status;
    uint32_t min_allowed_price;
    uint32_t max_allowed_price;
    uint32_t near_execution_price;
    uint64_t near_execution_time;
    uint32_t lower_price_range_collar;
    uint32_t upper_price_range_collar;
};

// ---------------------------------------------------------------------------
// Callback types
// ---------------------------------------------------------------------------

using AddOrderCb              = std::function<void(const MsgAddOrder&)>;
using OrderExecutedCb         = std::function<void(const MsgOrderExecuted&)>;
using OrderExecutedWithPriceCb= std::function<void(const MsgOrderExecutedWithPrice&)>;
using OrderCancelCb           = std::function<void(const MsgOrderCancel&)>;
using OrderDeleteCb           = std::function<void(const MsgOrderDelete&)>;
using OrderReplaceCb          = std::function<void(const MsgOrderReplace&)>;
using TradeCb                 = std::function<void(const MsgTrade&)>;
using CrossTradeCb            = std::function<void(const MsgCrossTrade&)>;
using SystemEventCb           = std::function<void(const MsgSystemEvent&)>;
using UnknownMsgCb            = std::function<void(uint8_t type, const MessageView&)>;

/// Callback bundle — set whichever you need; unset handlers are no-ops.
struct Callbacks {
    AddOrderCb               on_add_order;
    OrderExecutedCb          on_order_executed;
    OrderExecutedWithPriceCb on_order_executed_with_price;
    OrderCancelCb            on_order_cancel;
    OrderDeleteCb            on_order_delete;
    OrderReplaceCb           on_order_replace;
    TradeCb                  on_trade;
    CrossTradeCb             on_cross_trade;
    SystemEventCb            on_system_event;
    UnknownMsgCb             on_unknown;
};

// ---------------------------------------------------------------------------
// Parser
// ---------------------------------------------------------------------------

/// ITCH 5.0 dispatch-table parser.
///
/// All supported message types are decoded and dispatched through the
/// Callbacks struct. The dispatch table is a constexpr array of function
/// pointers indexed by message-type byte (0–255).
///
/// OrderBook updates are applied internally; BBO changes are reflected in
/// the atomic BBO fields of the book.
class Parser {
public:
    /// Construct with a fixed pool of order books (one per stock-locate slot).
    /// books must outlive the parser.
    Parser(OrderBook* books, size_t book_count, Callbacks cb);

    /// Parse one ITCH message. Returns true if recognised.
    bool parse(const MessageView& msg, uint64_t hw_ts_ns) noexcept;

    /// Parse an entire ITCH binary stream (length-prefixed messages).
    /// cb is called for each message before dispatch.
    size_t parse_stream(const uint8_t* buf, size_t buf_len,
                        uint64_t hw_ts_ns = 0) noexcept;

    /// Access the stock-locate table (e.g. for symbol lookup after parsing).
    [[nodiscard]] const StockLocate& stock_locate() const noexcept { return locate_; }

    /// Statistics
    struct Stats {
        uint64_t total_messages   = 0;
        uint64_t add_orders       = 0;
        uint64_t cancels          = 0;
        uint64_t deletes          = 0;
        uint64_t replaces         = 0;
        uint64_t executions       = 0;
        uint64_t unknown_messages = 0;
    };
    [[nodiscard]] const Stats& stats() const noexcept { return stats_; }

private:
    using DispatchFn = void(*)(Parser&, const MessageView&, uint64_t);

    static const std::array<DispatchFn, 256> kDispatchTable;

    // Individual message decoders — static to avoid vtable overhead
    static void handle_system_event           (Parser&, const MessageView&, uint64_t);
    static void handle_stock_directory        (Parser&, const MessageView&, uint64_t);
    static void handle_stock_trading_action   (Parser&, const MessageView&, uint64_t);
    static void handle_reg_sho                (Parser&, const MessageView&, uint64_t);
    static void handle_market_participant      (Parser&, const MessageView&, uint64_t);
    static void handle_mwcb_decline_level     (Parser&, const MessageView&, uint64_t);
    static void handle_mwcb_status            (Parser&, const MessageView&, uint64_t);
    static void handle_ipo_quoting            (Parser&, const MessageView&, uint64_t);
    static void handle_luld_auction_collar    (Parser&, const MessageView&, uint64_t);
    static void handle_operational_halt       (Parser&, const MessageView&, uint64_t);
    static void handle_add_order              (Parser&, const MessageView&, uint64_t);
    static void handle_add_order_mpid         (Parser&, const MessageView&, uint64_t);
    static void handle_order_executed         (Parser&, const MessageView&, uint64_t);
    static void handle_order_executed_price   (Parser&, const MessageView&, uint64_t);
    static void handle_order_cancel           (Parser&, const MessageView&, uint64_t);
    static void handle_order_delete           (Parser&, const MessageView&, uint64_t);
    static void handle_order_replace          (Parser&, const MessageView&, uint64_t);
    static void handle_trade                  (Parser&, const MessageView&, uint64_t);
    static void handle_cross_trade            (Parser&, const MessageView&, uint64_t);
    static void handle_broken_trade           (Parser&, const MessageView&, uint64_t);
    static void handle_ipo_allocation         (Parser&, const MessageView&, uint64_t);
    static void handle_retail_interest        (Parser&, const MessageView&, uint64_t);
    static void handle_direct_listing         (Parser&, const MessageView&, uint64_t);
    static void handle_unknown                (Parser&, const MessageView&, uint64_t);

    [[nodiscard]] __attribute__((always_inline))
    OrderBook* book_for(uint16_t locate) noexcept {
        if (__builtin_expect(!!((locate >= book_count_)), 0)) return nullptr;
        return &books_[locate];
    }

    OrderBook*  books_;
    size_t      book_count_;
    Callbacks   cb_;
    StockLocate locate_;
    Stats       stats_;
};

} // namespace itch
