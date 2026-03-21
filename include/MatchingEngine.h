// ─────────────────────────────────────────────────────────────────────────────
// MatchingEngine.h  –  Core price-time priority matching algorithm
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include "Order.h"
#include "OrderBook.h"
#include "fmt_compat.h"

#include <unordered_map>
#include <vector>
#include <functional>
#include <mutex>
#include <atomic>
#include <string>
#include <stdexcept>
#include <chrono>
#include <numeric>
#include <algorithm>
#include <cmath>

namespace trading {

// ─────────────────────────────────────────────────────────────────────────────
// MatchResult  –  outcome of a single matchOrder call
// ─────────────────────────────────────────────────────────────────────────────
struct MatchResult {
    long long           order_id;
    std::vector<Trade>  trades;
    long                executed_qty{0};
    long                rested_qty{0};
    bool                fully_filled{false};

    [[nodiscard]] std::size_t trade_count() const noexcept { return trades.size(); }
    [[nodiscard]] bool        has_trades()  const noexcept { return !trades.empty(); }
};

// ─────────────────────────────────────────────────────────────────────────────
// EngineStats  –  atomic counters safe to read from any thread
// ─────────────────────────────────────────────────────────────────────────────
struct EngineStats {
    std::atomic<long long> orders_received  {0};
    std::atomic<long long> orders_matched   {0};
    std::atomic<long long> orders_rested    {0};
    std::atomic<long long> orders_cancelled {0};
    std::atomic<long long> trades_generated {0};
    std::atomic<long long> total_volume     {0};

    // Latency samples in nanoseconds
    mutable std::mutex          latency_mutex;
    std::vector<long long>      latency_samples_ns;

    void record_latency(long long ns) {
        std::lock_guard<std::mutex> lock(latency_mutex);
        latency_samples_ns.push_back(ns);
    }

    struct LatencyReport {
        long long min_ns{0}, max_ns{0}, mean_ns{0};
        long long p50_ns{0}, p95_ns{0}, p99_ns{0};
        long long total_orders{0};
        double    total_time_us{0.0};
        double    avg_us{0.0};
    };

    [[nodiscard]] LatencyReport latency_report() const {
        std::lock_guard<std::mutex> lock(latency_mutex);
        if (latency_samples_ns.empty()) return {};

        auto samples = latency_samples_ns;
        std::sort(samples.begin(), samples.end());

        const long long N   = static_cast<long long>(samples.size());
        const long long sum = std::accumulate(samples.begin(), samples.end(), 0LL);

        auto pct = [&](double p) -> long long {
            auto idx = static_cast<std::size_t>(
                p / 100.0 * static_cast<double>(N - 1));
            return samples[idx];
        };

        LatencyReport r{};
        r.min_ns        = samples.front();
        r.max_ns        = samples.back();
        r.mean_ns       = sum / N;
        r.p50_ns        = pct(50.0);
        r.p95_ns        = pct(95.0);
        r.p99_ns        = pct(99.0);
        r.total_orders  = N;
        r.total_time_us = static_cast<double>(sum) / 1'000.0;
        r.avg_us        = static_cast<double>(sum) /
                          static_cast<double>(N) / 1'000.0;
        return r;
    }
};

// ─────────────────────────────────────────────────────────────────────────────
// MatchingEngine
// ─────────────────────────────────────────────────────────────────────────────
class MatchingEngine {
public:
    using TradeCallback = std::function<void(const Trade&)>;

    explicit MatchingEngine(std::string engine_name = "default")
        : name_{ std::move(engine_name) } {}

    MatchingEngine(const MatchingEngine&)            = delete;
    MatchingEngine& operator=(const MatchingEngine&) = delete;
    MatchingEngine(MatchingEngine&&)                 = default;
    MatchingEngine& operator=(MatchingEngine&&)      = default;

    // ── Configuration ─────────────────────────────────────────────────────────
    void set_trade_callback(TradeCallback cb) {
        trade_callback_ = std::move(cb);
    }

    // ── Primary entry point ───────────────────────────────────────────────────
    MatchResult matchOrder(Order incoming) {
        const auto t0 = std::chrono::steady_clock::now();

        stats_.orders_received.fetch_add(1, std::memory_order_relaxed);

        auto& book = get_or_create_book(incoming.symbol);

        MatchResult result{};
        result.order_id = incoming.order_id;

        if (incoming.side == Side::BUY)
            match_buy(incoming, book, result);
        else
            match_sell(incoming, book, result);

        result.fully_filled = incoming.is_fully_filled();

        // Rest any unfilled LIMIT remainder
        if (!incoming.is_fully_filled() && incoming.type == OrderType::LIMIT) {
            result.rested_qty = incoming.remaining();
            book.addOrder(incoming);
            stats_.orders_rested.fetch_add(1, std::memory_order_relaxed);
        }

        if (incoming.is_fully_filled())
            stats_.orders_matched.fetch_add(1, std::memory_order_relaxed);

        // ── Latency recording ─────────────────────────────────────────────────
        const auto t1 = std::chrono::steady_clock::now();
        const auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        stats_.record_latency(ns);

        return result;
    }

    bool cancelOrder(const std::string& symbol, long long order_id) {
        auto it = books_.find(symbol);
        if (it == books_.end()) return false;
        const bool ok = it->second.cancelOrder(order_id);
        if (ok) stats_.orders_cancelled.fetch_add(1, std::memory_order_relaxed);
        return ok;
    }

    // ── Accessors ─────────────────────────────────────────────────────────────
    [[nodiscard]] const OrderBook* book(const std::string& sym) const {
        auto it = books_.find(sym);
        return (it == books_.end()) ? nullptr : &it->second;
    }

    [[nodiscard]] const std::string& name()  const noexcept { return name_; }
    [[nodiscard]] EngineStats&       stats() const noexcept { return stats_; }

    // ── Statistics summary ────────────────────────────────────────────────────
    [[nodiscard]] std::string stats_summary() const {
        auto r = stats_.latency_report();
        std::string s;
        s += trading::fmt::sprintf(
            "\n╔══ Engine [%s] Statistics ══\n"
            "║  Orders received  : %12lld\n"
            "║  Orders matched   : %12lld\n"
            "║  Orders rested    : %12lld\n"
            "║  Orders cancelled : %12lld\n"
            "║  Trades generated : %12lld\n"
            "║  Total volume     : %12lld\n"
            "║\n"
            "║  Latency (per matchOrder call)\n"
            "║    Total orders   : %12lld\n"
            "║    Total time     : %12.2f µs\n"
            "║    Average        : %12.4f µs\n"
            "║    Min            : %12lld ns\n"
            "║    Median (p50)   : %12lld ns\n"
            "║    p95            : %12lld ns\n"
            "║    p99            : %12lld ns\n"
            "║    Max            : %12lld ns\n"
            "╚══════════════════════════════╝\n",
            name_.c_str(),
            stats_.orders_received .load(),
            stats_.orders_matched  .load(),
            stats_.orders_rested   .load(),
            stats_.orders_cancelled.load(),
            stats_.trades_generated.load(),
            stats_.total_volume    .load(),
            r.total_orders,
            r.total_time_us,
            r.avg_us,
            r.min_ns,
            r.p50_ns,
            r.p95_ns,
            r.p99_ns,
            r.max_ns
        );
        return s;
    }

private:
    // ── Match BUY order against ask side ─────────────────────────────────────
    void match_buy(Order& incoming, OrderBook& book, MatchResult& result) {
        auto& asks = book.asks();

        while (!incoming.is_fully_filled() && !asks.empty()) {
            auto best_ask_it = asks.begin();           // O(1): lowest ask
            const double ask_px = best_ask_it->first;

            // Price check: BUY price must be >= best ask price
            if (incoming.type == OrderType::LIMIT && incoming.price < ask_px)
                break;

            Level& level = best_ask_it->second;

            while (!incoming.is_fully_filled() && !level.orders.empty()) {
                Order& passive = level.orders.front(); // FIFO: earliest order

                const long fill_qty = std::min(incoming.remaining(),
                                               passive.remaining());

                incoming.fill(fill_qty);
                passive.fill(fill_qty);

                emit_trade(incoming, passive, ask_px, fill_qty, result);

                level.total_qty -= fill_qty;
                if (passive.is_fully_filled())
                    level.orders.pop_front();          // O(1) list erase
            }

            if (level.empty())
                asks.erase(best_ask_it);               // O(log P) map erase
        }
    }

    // ── Match SELL order against bid side ────────────────────────────────────
    void match_sell(Order& incoming, OrderBook& book, MatchResult& result) {
        auto& bids = book.bids();

        while (!incoming.is_fully_filled() && !bids.empty()) {
            auto best_bid_it = bids.begin();           // O(1): highest bid
            const double bid_px = best_bid_it->first;

            // Price check: SELL price must be <= best bid price
            if (incoming.type == OrderType::LIMIT && incoming.price > bid_px)
                break;

            Level& level = best_bid_it->second;

            while (!incoming.is_fully_filled() && !level.orders.empty()) {
                Order& passive = level.orders.front();

                const long fill_qty = std::min(incoming.remaining(),
                                               passive.remaining());

                incoming.fill(fill_qty);
                passive.fill(fill_qty);

                emit_trade(passive, incoming, bid_px, fill_qty, result);

                level.total_qty -= fill_qty;
                if (passive.is_fully_filled())
                    level.orders.pop_front();
            }

            if (level.empty())
                bids.erase(best_bid_it);
        }
    }

    void emit_trade(const Order& buyer, const Order& seller,
                    double exec_px, long exec_qty,
                    MatchResult& result) {
        Trade t{};
        t.trade_id      = next_trade_id();
        t.symbol        = buyer.symbol;
        t.buy_order_id  = buyer.order_id;
        t.sell_order_id = seller.order_id;
        t.exec_price    = exec_px;
        t.exec_qty      = exec_qty;
        t.exec_time     = std::chrono::steady_clock::now();

        result.executed_qty += exec_qty;
        result.trades.push_back(t);

        stats_.trades_generated.fetch_add(1, std::memory_order_relaxed);
        stats_.total_volume.fetch_add(
            static_cast<long long>(exec_qty), std::memory_order_relaxed);

        if (trade_callback_)
            trade_callback_(t);
    }

    OrderBook& get_or_create_book(const std::string& symbol) {
        auto it = books_.find(symbol);
        if (it != books_.end()) return it->second;
        auto [ins_it, ok] = books_.emplace(symbol, OrderBook{ symbol });
        (void)ok;
        return ins_it->second;
    }

    std::string                                name_;
    std::unordered_map<std::string, OrderBook> books_;
    TradeCallback                              trade_callback_;
    mutable EngineStats                        stats_;
};

} // namespace trading
