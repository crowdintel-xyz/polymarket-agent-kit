#ifndef ORDER_BOOK_HPP
#define ORDER_BOOK_HPP

// Fixed-capacity L2 book with an atomic, freshness-aware top-of-book view.
//
// The engine never reads the mutable depth arrays.  The WebSocket writer
// updates depth under a cold-path mutex and publishes bid+ask in one seqlock
// transaction whose fields are atomic.  This is valid in the C++ memory model
// (a seqlock over non-atomic payloads would still be a data race).  Cold-path
// depth snapshots are copied while holding the mutex.

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <mutex>

struct Level2Entry {
    uint64_t price = 0;  // fixed-point x1e6
    uint64_t size = 0;   // fixed-point x1e6
};

class OrderBookL2 {
public:
    static constexpr size_t MAX_LEVELS = 100;

    struct Top {
        Level2Entry bid;
        Level2Entry ask;
        uint64_t sequence = 0;
        uint64_t updated_ns = 0;  // CLOCK_MONOTONIC
    };

    // Publish both sides as one coherent market-data event.
    void set_book(const Level2Entry* bids, size_t nb,
                  const Level2Entry* asks, size_t na) {
        std::lock_guard<std::mutex> lock(depth_mu_);
        copy_side(bids_, bids, nb);
        copy_side(asks_, asks, na);
        publish_top_locked(now_mono_ns());
    }

    // Compatibility helpers for tests/cold callers.  Feed handlers should use
    // set_book() so a snapshot/delta cannot expose a mixed generation.
    void set_bids(const Level2Entry* levels, size_t n) {
        std::lock_guard<std::mutex> lock(depth_mu_);
        copy_side(bids_, levels, n);
        publish_top_locked(now_mono_ns());
    }

    void set_asks(const Level2Entry* levels, size_t n) {
        std::lock_guard<std::mutex> lock(depth_mu_);
        copy_side(asks_, levels, n);
        publish_top_locked(now_mono_ns());
    }

    // Clear tradable state immediately on disconnect.  The engine therefore
    // cannot trade a snapshot left behind by a dead socket.
    void invalidate() {
        std::lock_guard<std::mutex> lock(depth_mu_);
        bids_.fill({0, 0});
        asks_.fill({0, 0});
        publish_top_locked(0);
    }

    // Returns false on a concurrent publication.  max_age_ns==0 disables the
    // age check; live engines pass a configured freshness budget.
    inline bool read_top(Top& out, uint64_t max_age_ns = 0) const noexcept {
        // Sequential consistency gives one total order across the version and
        // atomic payload fields. Equal even versions therefore bracket one
        // coherent generation without a fence that TSan cannot model.
        const uint64_t s1 = seq_.load(std::memory_order_seq_cst);
        if (s1 & 1U) return false;

        out.bid.price = bid_price_.load(std::memory_order_seq_cst);
        out.bid.size = bid_size_.load(std::memory_order_seq_cst);
        out.ask.price = ask_price_.load(std::memory_order_seq_cst);
        out.ask.size = ask_size_.load(std::memory_order_seq_cst);
        out.updated_ns = updated_ns_.load(std::memory_order_seq_cst);

        const uint64_t s2 = seq_.load(std::memory_order_seq_cst);
        if (s1 != s2 || (s2 & 1U)) return false;
        out.sequence = s2;

        if (max_age_ns != 0) {
            if (out.updated_ns == 0) return false;
            const uint64_t now = now_mono_ns();
            if (now < out.updated_ns || now - out.updated_ns > max_age_ns)
                return false;
        }
        return true;
    }

    // Cold-path coherent depth copy used by the WS delta merger.
    void snapshot(Level2Entry* bids, size_t& nb,
                  Level2Entry* asks, size_t& na) const {
        std::lock_guard<std::mutex> lock(depth_mu_);
        nb = copy_out(bids_, bids);
        na = copy_out(asks_, asks);
    }

    Level2Entry get_bid(size_t level) const {
        std::lock_guard<std::mutex> lock(depth_mu_);
        return level < MAX_LEVELS ? bids_[level] : Level2Entry{};
    }

    Level2Entry get_ask(size_t level) const {
        std::lock_guard<std::mutex> lock(depth_mu_);
        return level < MAX_LEVELS ? asks_[level] : Level2Entry{};
    }

    uint64_t sequence() const noexcept {
        return seq_.load(std::memory_order_acquire);
    }

    void set_tick_size(uint64_t tick) noexcept {
        if (tick > 0 && tick < 1000000)
            tick_size_.store(tick, std::memory_order_release);
    }

    uint64_t tick_size(uint64_t fallback) const noexcept {
        const uint64_t v = tick_size_.load(std::memory_order_acquire);
        return v ? v : fallback;
    }

    static uint64_t now_mono_ns() noexcept {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

private:
    static void copy_side(std::array<Level2Entry, MAX_LEVELS>& dst,
                          const Level2Entry* src, size_t n) {
        n = std::min(n, MAX_LEVELS);
        for (size_t i = 0; i < n; ++i) dst[i] = src[i];
        for (size_t i = n; i < MAX_LEVELS; ++i) dst[i] = {0, 0};
    }

    static size_t copy_out(const std::array<Level2Entry, MAX_LEVELS>& src,
                           Level2Entry* dst) {
        size_t n = 0;
        while (n < MAX_LEVELS && src[n].size != 0) {
            dst[n] = src[n];
            ++n;
        }
        return n;
    }

    void publish_top_locked(uint64_t updated) noexcept {
        seq_.fetch_add(1, std::memory_order_seq_cst);
        bid_price_.store(bids_[0].price, std::memory_order_seq_cst);
        bid_size_.store(bids_[0].size, std::memory_order_seq_cst);
        ask_price_.store(asks_[0].price, std::memory_order_seq_cst);
        ask_size_.store(asks_[0].size, std::memory_order_seq_cst);
        updated_ns_.store(updated, std::memory_order_seq_cst);
        seq_.fetch_add(1, std::memory_order_seq_cst);
    }

    mutable std::mutex depth_mu_;
    std::array<Level2Entry, MAX_LEVELS> bids_{};
    std::array<Level2Entry, MAX_LEVELS> asks_{};

    alignas(64) std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> bid_price_{0};
    std::atomic<uint64_t> bid_size_{0};
    std::atomic<uint64_t> ask_price_{0};
    std::atomic<uint64_t> ask_size_{0};
    std::atomic<uint64_t> updated_ns_{0};
    std::atomic<uint64_t> tick_size_{0};
};

#endif  // ORDER_BOOK_HPP
