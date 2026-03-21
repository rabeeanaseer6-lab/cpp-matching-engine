// ─────────────────────────────────────────────────────────────────────────────
// Order.h  –  Core data model for the High-Performance Matching Engine
//
// Design decisions
// ────────────────
//  • OrderID  : long long   – supports 9.2 × 10¹⁸ unique orders; generated
//                             via std::atomic<long long> for lock-free,
//                             thread-safe ID assignment.
//  • Price    : double      – convenient for simulation; production uses
//                             int64_t ticks to avoid FP rounding errors.
//  • Quantity : long        – signed to allow easy delta arithmetic.
//  • Side     : enum class  – strongly typed; prevents integer mix-ups.
//  • timestamp: std::chrono::steady_clock::time_point
//               Nanosecond arrival time for Price-Time Priority (FIFO within
//               a price level).  steady_clock is monotonic and never jumps
//               backward due to NTP corrections.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "fmt_compat.h"

#include <chrono>
#include <string>
#include <atomic>
#include <stdexcept>

namespace trading {

// ── Side ─────────────────────────────────────────────────────────────────────
enum class Side : uint8_t {
    BUY  = 0,
    SELL = 1
};

inline const char* to_cstr(Side s) noexcept {
    return s == Side::BUY ? "BUY" : "SELL";
}

// ── OrderStatus ───────────────────────────────────────────────────────────────
enum class OrderStatus : uint8_t {
    NEW           = 0,
    PARTIALLY_FILLED,
    FILLED,
    CANCELLED
};

inline const char* to_cstr(OrderStatus st) noexcept {
    switch (st) {
        case OrderStatus::NEW:              return "NEW";
        case OrderStatus::PARTIALLY_FILLED: return "PART_FILL";
        case OrderStatus::FILLED:           return "FILLED";
        case OrderStatus::CANCELLED:        return "CANCELLED";
    }
    return "UNKNOWN";
}

// ── OrderType ─────────────────────────────────────────────────────────────────
enum class OrderType : uint8_t {
    LIMIT  = 0,   ///< Rests on the book if not immediately matchable
    MARKET = 1    ///< Fills against best available price; never rests
};

inline const char* to_cstr(OrderType ot) noexcept {
    return ot == OrderType::LIMIT ? "LIMIT" : "MARKET";
}

// ── Global atomic ID counter ──────────────────────────────────────────────────
//  Thread-safe, lock-free order-ID generator.
//  fetch_add with relaxed ordering is sufficient: we only need uniqueness,
//  not happens-before synchronisation with other memory operations.
inline std::atomic<long long> g_next_order_id{1};

[[nodiscard]] inline long long next_order_id() noexcept {
    return g_next_order_id.fetch_add(1, std::memory_order_relaxed);
}

// ── Order ─────────────────────────────────────────────────────────────────────
struct Order {
    // ── Identity ──────────────────────────────────────────────────────────────
    long long   order_id;      ///< Unique monotonically-increasing identifier
    std::string symbol;        ///< Instrument e.g. "AAPL", "BTC-USD"

    // ── Pricing & quantity ────────────────────────────────────────────────────
    double      price;         ///< Limit price (ignored for MARKET orders)
    long        quantity;      ///< Original order size
    long        filled_qty;    ///< How much has executed so far

    // ── Classification ────────────────────────────────────────────────────────
    Side        side;
    OrderType   type;
    OrderStatus status;

    // ── Timing (Price-Time Priority) ──────────────────────────────────────────
    std::chrono::steady_clock::time_point timestamp;

    // ── Constructors ──────────────────────────────────────────────────────────

    Order(std::string  sym,
          double       px,
          long         qty,
          Side         sd,
          OrderType    ot = OrderType::LIMIT)
        : order_id   { next_order_id()  }
        , symbol     { std::move(sym)   }
        , price      { px               }
        , quantity   { qty              }
        , filled_qty { 0                }
        , side       { sd               }
        , type       { ot               }
        , status     { OrderStatus::NEW }
        , timestamp  { std::chrono::steady_clock::now() }
    {
        if (qty <= 0)
            throw std::invalid_argument(
                trading::fmt::sprintf("Order qty must be > 0, got %ld", qty));
        if (ot == OrderType::LIMIT && px <= 0.0)
            throw std::invalid_argument(
                trading::fmt::sprintf("Limit price must be > 0, got %.4f", px));
    }

    // Default / copy / move
    Order()                            = default;
    Order(const Order&)                = default;
    Order(Order&&)                     = default;
    Order& operator=(const Order&)     = default;
    Order& operator=(Order&&)          = default;
    ~Order()                           = default;

    // ── Accessors ─────────────────────────────────────────────────────────────

    [[nodiscard]] long remaining() const noexcept { return quantity - filled_qty; }
    [[nodiscard]] bool is_fully_filled() const noexcept { return remaining() <= 0; }

    /// Fill qty units and update status.
    void fill(long qty) {
        if (qty <= 0 || qty > remaining())
            throw std::logic_error(
                trading::fmt::sprintf(
                    "Invalid fill qty=%ld for order_id=%lld remaining=%ld",
                    qty, order_id, remaining()));
        filled_qty += qty;
        status = is_fully_filled() ? OrderStatus::FILLED
                                   : OrderStatus::PARTIALLY_FILLED;
    }

    // ── Formatted display ─────────────────────────────────────────────────────
    [[nodiscard]] std::string to_string() const {
        return trading::fmt::sprintf(
            "[Order id=%10lld  sym=%-8s  side=%-4s  type=%-6s  "
            "px=%10.4f  qty=%6ld/%6ld  status=%s]",
            order_id,
            symbol.c_str(),
            to_cstr(side),
            to_cstr(type),
            price,
            filled_qty, quantity,
            to_cstr(status)
        );
    }
};

// ── Trade record ──────────────────────────────────────────────────────────────
struct Trade {
    long long   trade_id;
    std::string symbol;
    long long   buy_order_id;
    long long   sell_order_id;
    double      exec_price;
    long        exec_qty;
    std::chrono::steady_clock::time_point exec_time;

    [[nodiscard]] std::string to_string() const {
        return trading::fmt::sprintf(
            "[Trade id=%8lld  sym=%-8s  buy=%10lld  sell=%10lld  "
            "px=%10.4f  qty=%6ld]",
            trade_id, symbol.c_str(),
            buy_order_id, sell_order_id,
            exec_price, exec_qty
        );
    }
};

inline std::atomic<long long> g_next_trade_id{1};
[[nodiscard]] inline long long next_trade_id() noexcept {
    return g_next_trade_id.fetch_add(1, std::memory_order_relaxed);
}

} // namespace trading
