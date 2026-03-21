// ─────────────────────────────────────────────────────────────────────────────
// OrderBook.h  –  Price-Time Priority Order Book
//
// Data structure choice: std::map<double, Level>
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "Order.h"
#include "fmt_compat.h"

#include <map>
#include <list>
#include <unordered_map>
#include <string>
#include <optional>
#include <functional>
#include <stdexcept>
#include <vector>

namespace trading {

// ── Level ─────────────────────────────────────────────────────────────────────
struct Level {
    std::list<Order> orders;      ///< FIFO queue; front() was first to arrive
    long             total_qty{0};

    void push_back(Order o) {
        total_qty += o.remaining();
        orders.push_back(std::move(o));
    }

    [[nodiscard]] bool empty() const noexcept { return orders.empty(); }
};

// ── Map type aliases ──────────────────────────────────────────────────────────
//  BidMap: highest price at begin()  → use std::greater<double>
//  AskMap: lowest  price at begin()  → use std::less<double>  (default)
using BidMap = std::map<double, Level, std::greater<double>>;
using AskMap = std::map<double, Level>;

// ── OrderLocation cache entry ─────────────────────────────────────────────────
struct OrderLocation {
    Side                       side;
    double                     price;
    std::list<Order>::iterator order_iter;
};

// ─────────────────────────────────────────────────────────────────────────────
// OrderBook
// ─────────────────────────────────────────────────────────────────────────────
class OrderBook {
public:
    explicit OrderBook(std::string symbol)
        : symbol_{ std::move(symbol) } {}

    // Non-copyable; books are large move-only resources.
    OrderBook(const OrderBook&)            = delete;
    OrderBook& operator=(const OrderBook&) = delete;
    OrderBook(OrderBook&&)                 = default;
    OrderBook& operator=(OrderBook&&)      = default;

    // ── Accessors ─────────────────────────────────────────────────────────────
    [[nodiscard]] const std::string& symbol() const noexcept { return symbol_; }
    [[nodiscard]] BidMap& bids() noexcept { return bids_; }
    [[nodiscard]] AskMap& asks() noexcept { return asks_; }
    [[nodiscard]] const BidMap& bids() const noexcept { return bids_; }
    [[nodiscard]] const AskMap& asks() const noexcept { return asks_; }

    // ── Best bid / ask ────────────────────────────────────────────────────────
    [[nodiscard]] std::optional<double> best_bid() const noexcept {
        if (bids_.empty()) return std::nullopt;
        return bids_.begin()->first;
    }

    [[nodiscard]] std::optional<double> best_ask() const noexcept {
        if (asks_.empty()) return std::nullopt;
        return asks_.begin()->first;
    }

    [[nodiscard]] std::optional<double> spread() const noexcept {
        auto b = best_bid();
        auto a = best_ask();
        if (!b || !a) return std::nullopt;
        return *a - *b;
    }

    // ── addOrder ──────────────────────────────────────────────────────────────
    //  Complexity: O(log P) map lookup + O(1) list push_back
    void addOrder(Order order) {
        const long long id    = order.order_id;
        const Side      side  = order.side;
        const double    price = order.price;

        if (side == Side::BUY) {
            auto& level = bids_[price];
            auto  it    = level.orders.insert(level.orders.end(), std::move(order));
            level.total_qty += it->remaining();
            location_cache_[id] = { Side::BUY, price, it };
        } else {
            auto& level = asks_[price];
            auto  it    = level.orders.insert(level.orders.end(), std::move(order));
            level.total_qty += it->remaining();
            location_cache_[id] = { Side::SELL, price, it };
        }
        ++total_orders_;
    }

    // ── cancelOrder ───────────────────────────────────────────────────────────
    //  O(1) via location cache  –  no book scan required
    bool cancelOrder(long long order_id) {
        auto cache_it = location_cache_.find(order_id);
        if (cache_it == location_cache_.end()) return false;

        auto& loc = cache_it->second;
        loc.order_iter->status = OrderStatus::CANCELLED;

        if (loc.side == Side::BUY) {
            auto map_it = bids_.find(loc.price);
            if (map_it != bids_.end()) {
                map_it->second.total_qty -= loc.order_iter->remaining();
                map_it->second.orders.erase(loc.order_iter);
                if (map_it->second.empty()) bids_.erase(map_it);
            }
        } else {
            auto map_it = asks_.find(loc.price);
            if (map_it != asks_.end()) {
                map_it->second.total_qty -= loc.order_iter->remaining();
                map_it->second.orders.erase(loc.order_iter);
                if (map_it->second.empty()) asks_.erase(map_it);
            }
        }

        location_cache_.erase(cache_it);
        --total_orders_;
        ++total_cancels_;
        return true;
    }

    // ── Statistics ────────────────────────────────────────────────────────────
    [[nodiscard]] std::size_t total_orders()   const noexcept { return total_orders_;  }
    [[nodiscard]] std::size_t total_cancels()  const noexcept { return total_cancels_; }
    [[nodiscard]] std::size_t bid_levels()     const noexcept { return bids_.size();   }
    [[nodiscard]] std::size_t ask_levels()     const noexcept { return asks_.size();   }
    [[nodiscard]] std::size_t resting_orders() const noexcept { return location_cache_.size(); }

    // ── Book snapshot ─────────────────────────────────────────────────────────
    [[nodiscard]] std::string snapshot(std::size_t depth = 5) const {
        std::string out;
        out += trading::fmt::sprintf("\n╔══ OrderBook [%s] ══\n", symbol_.c_str());
        out += "║  ASKS  (low → high)\n";
        std::size_t n = 0;
        for (auto& [px, lvl] : asks_) {
            if (n++ >= depth) break;
            out += trading::fmt::sprintf(
                "║    ASK %10.4f  qty=%8ld  orders=%zu\n",
                px, lvl.total_qty, lvl.orders.size());
        }
        if (auto sp = spread())
            out += trading::fmt::sprintf("║  ── spread=%.4f ──\n", *sp);
        else
            out += "║  ── no spread ──\n";
        out += "║  BIDS  (high → low)\n";
        n = 0;
        for (auto& [px, lvl] : bids_) {
            if (n++ >= depth) break;
            out += trading::fmt::sprintf(
                "║    BID %10.4f  qty=%8ld  orders=%zu\n",
                px, lvl.total_qty, lvl.orders.size());
        }
        out += "╚═══════════════════╝\n";
        return out;
    }

private:
    std::string symbol_;

    // ── The two sides of the book ─────────────────────────────────────────────
    BidMap bids_;   // sorted high → low  (std::greater<double>)
    AskMap asks_;   // sorted low  → high (std::less<double>)

    // ── O(1) cancel cache ─────────────────────────────────────────────────────
    std::unordered_map<long long, OrderLocation> location_cache_;

    std::size_t total_orders_  {0};
    std::size_t total_cancels_ {0};
};

} // namespace trading
