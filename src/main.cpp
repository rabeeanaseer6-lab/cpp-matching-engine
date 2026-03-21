// ─────────────────────────────────────────────────────────────────────────────
// main.cpp  –  High-Performance Matching Engine Simulation & Benchmark
//
// Simulation design
// ─────────────────
//  Phase 1 – Order generation (std::jthread producer)
//    Generates N_ORDERS random LIMIT orders and pushes them into a shared queue.
//
//  Phase 2 – Order processing (main consumer thread)
//    Drains the queue calling engine.matchOrder() for each order, recording
//    per-call nanosecond latency via steady_clock.
//
//  Phase 3 – Reporting
//    Prints wall-clock summary, latency percentiles, histogram, book snapshot.
// ─────────────────────────────────────────────────────────────────────────────

#include "Order.h"
#include "OrderBook.h"
#include "MatchingEngine.h"
#include "fmt_compat.h"

#include <iostream>
#include <vector>
#include <queue>
#include <mutex>
#include <condition_variable>
#include <thread>
#include <atomic>
#include <random>
#include <chrono>
#include <string>
#include <cassert>
#include <algorithm>
#include <numeric>
#include <cmath>
#include <cstdio>

// ── ANSI colour helpers ───────────────────────────────────────────────────────
namespace clr {
    constexpr const char* RESET  = "\033[0m";
    constexpr const char* BOLD   = "\033[1m";
    constexpr const char* GREEN  = "\033[32m";
    constexpr const char* YELLOW = "\033[33m";
    constexpr const char* CYAN   = "\033[36m";
    constexpr const char* BLUE   = "\033[34m";
    constexpr const char* WHITE  = "\033[97m";
}

// ─────────────────────────────────────────────────────────────────────────────
// Thread-safe order queue (SPSC pattern using mutex + condvar)
//
//  NOTE: In a production LMAX Disruptor architecture this would be a lock-free
//  ring buffer.  A mutex-based queue is used here for clarity.
// ─────────────────────────────────────────────────────────────────────────────
class OrderQueue {
public:
    void push(trading::Order order) {
        {
            std::lock_guard<std::mutex> lock(mtx_);
            q_.push(std::move(order));
        }
        cv_.notify_one();
    }

    /// Blocking pop; returns false only when the queue is drained AND done.
    bool pop(trading::Order& out) {
        std::unique_lock<std::mutex> lock(mtx_);
        cv_.wait(lock, [this]{ return !q_.empty() || done_; });
        if (q_.empty()) return false;
        out = std::move(q_.front());
        q_.pop();
        return true;
    }

    void set_done() {
        { std::lock_guard<std::mutex> lock(mtx_); done_ = true; }
        cv_.notify_all();
    }

    [[nodiscard]] std::size_t size() const {
        std::lock_guard<std::mutex> lock(mtx_);
        return q_.size();
    }

private:
    mutable std::mutex              mtx_;
    std::condition_variable         cv_;
    std::queue<trading::Order>      q_;
    bool                            done_{false};
};

// ─────────────────────────────────────────────────────────────────────────────
// Banner / section helpers
// ─────────────────────────────────────────────────────────────────────────────
static void print_banner() {
    std::cout << clr::CYAN << clr::BOLD << R"(
╔═══════════════════════════════════════════════════════════════════════╗
║                                                                       ║
║   ██╗  ██╗██████╗ ███████╗    ███╗   ███╗ █████╗ ████████╗ ██████╗   ║
║   ██║  ██║██╔══██╗██╔════╝    ████╗ ████║██╔══██╗╚══██╔══╝██╔════╝   ║
║   ███████║██████╔╝███████╗    ██╔████╔██║███████║   ██║   ██║        ║
║   ██╔══██║██╔═══╝ ╚════██║    ██║╚██╔╝██║██╔══██║   ██║   ██║        ║
║   ██║  ██║██║     ███████║    ██║ ╚═╝ ██║██║  ██║   ██║   ╚██████╗   ║
║   ╚═╝  ╚═╝╚═╝     ╚══════╝    ╚═╝     ╚═╝╚═╝  ╚═╝   ╚═╝    ╚═════╝   ║
║                                                                       ║
║        C++20 High-Performance Price-Time Priority Matching Engine     ║
║        ─────────────────────────────────────────────────────────      ║
║        Features: std::jthread · std::atomic · steady_clock            ║
║                  std::map<Price,Level> · O(1) cancel cache             ║
╚═══════════════════════════════════════════════════════════════════════╝
)" << clr::RESET << "\n";
}

static void print_section(const char* title) {
    std::cout << clr::YELLOW << clr::BOLD
              << "\n━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
              << "  " << title << "\n"
              << "━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n"
              << clr::RESET;
}

// ─────────────────────────────────────────────────────────────────────────────
// ASCII latency histogram
// ─────────────────────────────────────────────────────────────────────────────
static void print_histogram(const std::vector<long long>& samples_ns,
                             int buckets = 20) {
    if (samples_ns.empty()) return;

    auto sorted = samples_ns;
    std::sort(sorted.begin(), sorted.end());

    const long long lo  = sorted.front();
    const long long hi  = sorted.back();
    const long long rng = hi - lo;

    if (rng == 0) {
        std::cout << trading::fmt::sprintf("  All samples at %lld ns\n", lo);
        return;
    }

    std::vector<int> counts(static_cast<std::size_t>(buckets), 0);
    for (auto v : sorted) {
        int b = static_cast<int>(
            static_cast<double>(v - lo) / static_cast<double>(rng)
            * static_cast<double>(buckets - 1));
        ++counts[static_cast<std::size_t>(b)];
    }
    const int max_cnt = *std::max_element(counts.begin(), counts.end());
    constexpr int BW  = 40;

    std::cout << clr::BLUE;
    for (int i = 0; i < buckets; ++i) {
        long long blo = lo + static_cast<long long>(
            static_cast<double>(i)   / static_cast<double>(buckets) * static_cast<double>(rng));
        long long bhi = lo + static_cast<long long>(
            static_cast<double>(i+1) / static_cast<double>(buckets) * static_cast<double>(rng));

        int bar = (max_cnt > 0)
                  ? (counts[static_cast<std::size_t>(i)] * BW / max_cnt)
                  : 0;

        // Build bar string
        std::string bar_str(static_cast<std::size_t>(bar), '#');
        bar_str.resize(static_cast<std::size_t>(BW), ' ');

        std::cout << trading::fmt::sprintf(
            "  %6lld-%6lld ns |%s| %d\n",
            blo, bhi,
            bar_str.c_str(),
            counts[static_cast<std::size_t>(i)]);
    }
    std::cout << clr::RESET;
}

// ─────────────────────────────────────────────────────────────────────────────
// Order generator
//  500 BUY  orders: price ∈ [98.00, 103.00] (0.50 increments, 11 levels)
//  500 SELL orders: price ∈ [97.00, 102.00] (0.50 increments, 11 levels)
//  Overlap at 98–102 guarantees aggressive crossing.
// ─────────────────────────────────────────────────────────────────────────────
static std::vector<trading::Order>
generate_orders(const std::string& symbol, int n, unsigned seed = 42u) {
    std::mt19937_64 rng(seed);
    std::uniform_int_distribution<int>  buy_idx (0, 10);
    std::uniform_int_distribution<int>  sell_idx(0, 10);
    std::uniform_int_distribution<long> qty_dist(50, 500);
    std::bernoulli_distribution         is_buy  (0.5);

    std::vector<trading::Order> orders;
    orders.reserve(static_cast<std::size_t>(n));

    for (int i = 0; i < n; ++i) {
        const bool   buy = is_buy(rng);
        const long   qty = qty_dist(rng);
        const double px  = buy
            ? 98.00 + buy_idx (rng) * 0.50
            : 97.00 + sell_idx(rng) * 0.50;

        orders.emplace_back(symbol, px, qty,
                            buy ? trading::Side::BUY : trading::Side::SELL,
                            trading::OrderType::LIMIT);
    }
    return orders;
}

// ─────────────────────────────────────────────────────────────────────────────
// main
// ─────────────────────────────────────────────────────────────────────────────
int main() {
    print_banner();

    // ── Configuration ─────────────────────────────────────────────────────────
    constexpr int  N_ORDERS = 1'000;
    const std::string SYMBOL = "HPS-MATCH";

    // ── Engine setup ──────────────────────────────────────────────────────────
    print_section("Engine Initialisation");
    trading::MatchingEngine engine("primary");

    std::mutex                        trade_log_mtx;
    std::vector<trading::Trade>       trade_log;
    trade_log.reserve(N_ORDERS * 4);

    engine.set_trade_callback([&](const trading::Trade& t) {
        std::lock_guard<std::mutex> lock(trade_log_mtx);
        trade_log.push_back(t);
    });

    std::cout << trading::fmt::sprintf(
        "  Engine '%s' ready — processing %d orders on [%s]\n",
        engine.name().c_str(), N_ORDERS, SYMBOL.c_str());

    // ── Phase 1: Producer thread (std::jthread) ───────────────────────────────
    print_section("Phase 1 — Order Generation  (std::jthread producer)");

    OrderQueue          order_queue;
    std::atomic<bool>   generation_done{false};

    // std::jthread automatically joins on destruction (C++20 feature).
    // The stop_token parameter enables cooperative cancellation.
    std::jthread producer([&](std::stop_token /*st*/) {
        auto orders = generate_orders(SYMBOL, N_ORDERS, /*seed=*/2024u);

        std::cout << trading::fmt::sprintf(
            "  [Producer] Generated %zu orders — pushing to queue...\n",
            orders.size());

        for (auto& o : orders)
            order_queue.push(std::move(o));

        order_queue.set_done();
        generation_done.store(true, std::memory_order_release);
        std::cout << "  [Producer] Done.\n";
    });

    // ── Phase 2: Consumer loop (main thread) ──────────────────────────────────
    print_section("Phase 2 — Matching  (main-thread consumer)");

    long long orders_processed = 0;
    long long total_trades     = 0;

    const auto wall_start = std::chrono::steady_clock::now();

    trading::Order incoming;
    while (order_queue.pop(incoming)) {
        auto result = engine.matchOrder(std::move(incoming));

        ++orders_processed;
        total_trades += static_cast<long long>(result.trade_count());

        // Progress every 100 orders
        if (orders_processed % 100 == 0) {
            std::cout << trading::fmt::sprintf(
                "  [Consumer] processed=%5lld  trades=%7lld  queue=%zu\n",
                orders_processed, total_trades, order_queue.size());
        }
    }

    const auto wall_end = std::chrono::steady_clock::now();
    producer.join();   // jthread also joins on destruction, but be explicit

    // ── Phase 3: Performance report ───────────────────────────────────────────
    print_section("Phase 3 — Performance Report");

    const long long wall_us = std::chrono::duration_cast<std::chrono::microseconds>(
                                  wall_end - wall_start).count();

    // ── Wall-clock summary ────────────────────────────────────────────────────
    std::cout << clr::GREEN << clr::BOLD;
    std::cout << trading::fmt::sprintf(
        "\n  ┌─ Wall-Clock Summary ──────────────────────────────────┐\n"
        "  │  Orders processed   : %8lld                          │\n"
        "  │  Trade events       : %8lld                          │\n"
        "  │  Total wall time    : %8lld µs  (%.3f ms)            │\n"
        "  │  Throughput         : %8.0f orders/sec               │\n"
        "  │  Avg latency/order  : %8.4f µs                       │\n"
        "  └──────────────────────────────────────────────────────┘\n",
        orders_processed,
        total_trades,
        wall_us,
        static_cast<double>(wall_us) / 1000.0,
        static_cast<double>(orders_processed) /
            (static_cast<double>(wall_us) / 1e6),
        static_cast<double>(wall_us) /
            static_cast<double>(orders_processed)
    );
    std::cout << clr::RESET;

    // ── Engine internal statistics ────────────────────────────────────────────
    std::cout << engine.stats_summary();

    // ── Latency histogram ─────────────────────────────────────────────────────
    print_section("Latency Histogram  (ns per matchOrder call)");
    {
        auto report = engine.stats().latency_report();

        std::vector<long long> samples;
        {
            std::lock_guard<std::mutex> lock(engine.stats().latency_mutex);
            samples = engine.stats().latency_samples_ns;
        }

        print_histogram(samples);

        if (!samples.empty()) {
            const double mean = static_cast<double>(report.mean_ns);
            double sq = 0.0;
            for (auto s : samples)
                sq += (static_cast<double>(s) - mean) *
                      (static_cast<double>(s) - mean);
            const double stddev = std::sqrt(sq / static_cast<double>(samples.size()));
            std::cout << clr::WHITE
                      << trading::fmt::sprintf("\n  Std-dev : %.1f ns\n", stddev)
                      << clr::RESET;
        }
    }

    // ── Final order book depth ────────────────────────────────────────────────
    print_section("Final Order Book Depth Snapshot");
    if (const auto* bk = engine.book(SYMBOL)) {
        std::cout << bk->snapshot(10);
        std::cout << trading::fmt::sprintf(
            "\n  Resting orders : %zu\n"
            "  Bid levels     : %zu\n"
            "  Ask levels     : %zu\n"
            "  Best bid       : %s\n"
            "  Best ask       : %s\n",
            bk->resting_orders(),
            bk->bid_levels(),
            bk->ask_levels(),
            bk->best_bid()
                ? trading::fmt::sprintf("%.4f", *bk->best_bid()).c_str()
                : "none",
            bk->best_ask()
                ? trading::fmt::sprintf("%.4f", *bk->best_ask()).c_str()
                : "none"
        );
    }

    // ── Last 10 trades ────────────────────────────────────────────────────────
    print_section("Last 10 Executed Trades");
    {
        std::lock_guard<std::mutex> lock(trade_log_mtx);
        const std::size_t show  = std::min<std::size_t>(10, trade_log.size());
        const std::size_t start = trade_log.size() - show;
        for (std::size_t i = start; i < trade_log.size(); ++i)
            std::cout << "  " << trade_log[i].to_string() << "\n";
        std::cout << trading::fmt::sprintf(
            "\n  Total trade records in log: %zu\n", trade_log.size());
    }

    // ── Architecture reference table ──────────────────────────────────────────
    print_section("Data Structure Complexity Reference");
    std::cout << clr::CYAN
              << R"(
  ┌──────────────────────────────┬──────────────┬───────────────────────┐
  │ Operation                    │ This Engine  │ Sorted std::vector    │
  ├──────────────────────────────┼──────────────┼───────────────────────┤
  │ Insert new price level       │ O(log P)     │ O(P)  – shift/memmove │
  │ Delete price level           │ O(log P)     │ O(P)  – shift/memmove │
  │ Best bid/ask lookup          │ O(1)         │ O(1)                  │
  │ Cancel order by ID           │ O(1)         │ O(P·N) – linear scan  │
  │ FIFO within price level      │ O(1) list    │ O(1) deque            │
  └──────────────────────────────┴──────────────┴───────────────────────┘
  P = distinct price levels   N = orders at one level

  Next-step latency improvements:
    1. Lock-free SPSC ring buffer (Disruptor pattern) → replace OrderQueue
    2. Kernel-bypass networking (DPDK / RDMA) → sub-microsecond NIC→app
    3. Integer tick prices (int64_t) → exact comparison, no FP rounding
    4. Custom slab allocator for list nodes → eliminate malloc hotspot
    5. CPU affinity + NUMA pinning + huge pages → cache/TLB optimisation
    6. Profile-Guided Optimisation (PGO) → 5-15% additional throughput
)"
              << clr::RESET << "\n";

    assert(orders_processed == N_ORDERS &&
           "Not all orders were consumed from the queue");

    std::cout << clr::GREEN << clr::BOLD
              << "\n  ✓ Simulation complete.\n\n"
              << clr::RESET;

    return 0;
}
