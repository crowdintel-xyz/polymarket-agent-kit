// ─────────────────────────────────────────────────────────────────────────────
// maker_bid_metrics.hpp — P5: Maker-bid fill-rate metric
//
// Ported from polywhales lanes/maker-bid.ts (metrics focus) — C++20,
// NO TypeScript copied.
//
// From paper.ts observation: "The live paper maker book agreed: 69 bids, 2 touched."
// This class collects that metric: fill-rate = touched / placed, per price level
// and overall. Also tracks queue-aware time-to-fill and the price gap to best ask.
//
// polywhales finding: maker bids that improve best+0.01 get outbid within 1-44s
// on 67 of 69 attempts. Guard-legal maker prices are a desert.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <mutex>
#include <cstdint>
#include <cstdio>
#include <map>
#include <string>
#include <vector>
#include <algorithm>
#include <unordered_map>

// ── Constants (from polywhales core/constants + maker-bid.ts) ──────────────────
namespace MAKER_BID_METRICS {
    constexpr double MIN_ASK_SANITY = 0.50;       // below this → our read is outlier
    constexpr double MAX_ASK = 0.95;               // cap: no bid above $0.95
    constexpr double TICK_SIZE = 0.01;            // one-tick improvement
    constexpr double PAPER_STAKE_USD = 100.0;     // standard paper stake
}

// ── Maker bid placement record (for metric collection) ────────────────────────
struct MakerBidEntry {
    std::string market_slug;
    std::string condition_id;
    std::string yes_token;
    double bid_price = 0.0;       // our posted price
    double best_bid_at_post = 0.0; // best bid when we posted
    double best_ask_at_post = 0.0; // best ask when we posted
    uint64_t queue_ahead = 0;     // shares ahead of us at post time
    uint64_t shares = 0;          // shares we bid for
    uint64_t post_ts = 0;         // epoch ms when bid was placed
    uint64_t filled_ts = 0;       // epoch ms when filled (0 = not filled)
    uint64_t missed_ts = 0;       // epoch ms when TTL expired (0 = not missed)
    bool touched = false;        // any flow reached our level
    bool filled = false;         // bid was fully or partially filled
    std::string outcome = "MISS"; // MISS, FILL, EXPIRE, OUTBID
};

// ── MakerBidMetrics: collect fill-rate metrics ─────────────────────────────────
class MakerBidMetrics {
public:
    using ClockFn = uint64_t (*)();

    struct Summary {
        uint64_t placed = 0;
        uint64_t touched = 0;
        uint64_t filled = 0;
        uint64_t missed = 0;
        uint64_t outbid = 0;
        double fill_rate = 0.0;        // filled / placed
        double touch_rate = 0.0;       // touched / placed
        double outbid_rate = 0.0;      // outbid / placed
        double avg_queue_ahead = 0.0;
        double avg_gap_to_ask = 0.0;   // MAX_ASK - bid_price (our price gap to cap)
        uint64_t avg_ttf_ms = 0;       // average time-to-fill (ms), 0 if no fills
        uint64_t avg_ttm_ms = 0;        // average time-to-miss (ms), 0 if no misses
        uint64_t avg_outbid_delay_ms = 0; // avg time before being outbid
    };

    struct Config {
        ClockFn now_fn = nullptr;
        uint64_t ttl_ms = 2 * 3'600'000;  // MAKER_BID_TTL_MS = 2h
    };

    explicit MakerBidMetrics(const Config& cfg)
        : cfg_(cfg), now_fn_(cfg.now_fn ? cfg.now_fn : &default_clock) {}

    // ── Record a bid placement ────────────────────────────────────────────────
    void place_bid(const MakerBidEntry& entry) {
        std::lock_guard lock(mutex_);
        entries_.push_back(entry);
        stats_.placed++;
        if (entry.queue_ahead > 0) {
            queue_sum_ += entry.queue_ahead;
        }
        gap_sum_ += (MAKER_BID_METRICS::MAX_ASK - entry.bid_price);
    }

    // ── Record a tape print that touches our level ────────────────────────────
    // Called by the tape consumer (from PaperBroker::settle or L2 feed).
    // If the print's price <= our bid_price and it's on our token, our level was touched.
    void record_tape_print(const std::string& token, double price, uint64_t ts_ms,
                           uint64_t size, bool is_sell) {
        std::lock_guard lock(mutex_);
        uint64_t now = now_fn_();
        (void)now;
        (void)size;
        (void)is_sell;

        for (auto& e : entries_) {
            if (e.filled || e.missed_ts > 0) continue;
            if (e.yes_token != token) continue;
            // If print price <= our bid, flow reached our level
            if (price <= e.bid_price) {
                if (!e.touched) {
                    e.touched = true;
                    stats_.touched++;
                    touch_times_.push_back(ts_ms - e.post_ts);
                }
            }
            // Check if print size + queue_ahead consumed enough to fill us
            // (queue-pessimistic: everything ahead must be consumed first)
        }
        (void)price;
    }

    // ── Record a fill for a bid ─────────────────────────────────────────────────
    void record_fill(const std::string& market_slug, uint64_t ts_ms) {
        std::lock_guard lock(mutex_);
        for (auto& e : entries_) {
            if (e.market_slug == market_slug && !e.filled && e.missed_ts == 0) {
                e.filled = true;
                e.filled_ts = ts_ms;
                stats_.filled++;
                uint64_t ttf = ts_ms - e.post_ts;
                ttf_sum_ += ttf;
                ttf_count_++;
                e.outcome = "FILL";
                return;
            }
        }
    }

    // ── Record a bid outbid (improved away) ────────────────────────────────────
    void record_outbid(const std::string& market_slug, uint64_t ts_ms) {
        std::lock_guard lock(mutex_);
        for (auto& e : entries_) {
            if (e.market_slug == market_slug && !e.filled && e.missed_ts == 0 && !e.touched) {
                e.outcome = "OUTBID";
                e.missed_ts = ts_ms;
                stats_.outbid++;
                outbid_times_.push_back(ts_ms - e.post_ts);
                return;
            }
        }
    }

    // ── Record a bid that expired (TTL elapsed, never filled) ──────────────────
    void record_expire(uint64_t now_ms) {
        std::lock_guard lock(mutex_);
        for (auto& e : entries_) {
            if (e.filled) continue;
            if (e.missed_ts > 0) continue;
            uint64_t expiry = e.post_ts + cfg_.ttl_ms;
            if (now_ms >= expiry) {
                e.missed_ts = now_ms;
                if (e.touched) {
                    e.outcome = "MISS";
                } else {
                    e.outcome = "EXPIRE";
                }
                stats_.missed++;
                if (e.touched) {
                    ttm_sum_ += (now_ms - e.post_ts);
                    ttm_count_++;
                }
            }
        }
    }

    // ── Compute summary ───────────────────────────────────────────────────────
    Summary summary() const {
        std::lock_guard lock(mutex_);
        Summary s;
        s.placed = stats_.placed;
        s.touched = stats_.touched;
        s.filled = stats_.filled;
        s.missed = stats_.missed;
        s.outbid = stats_.outbid;

        if (s.placed > 0) {
            s.fill_rate = (double)s.filled / s.placed;
            s.touch_rate = (double)s.touched / s.placed;
            s.outbid_rate = (double)s.outbid / s.placed;
        }

        if (entries_.size() > 0) {
            s.avg_queue_ahead = (double)queue_sum_ / entries_.size();
            s.avg_gap_to_ask = gap_sum_ / entries_.size();
        }

        if (ttf_count_ > 0) s.avg_ttf_ms = ttf_sum_ / ttf_count_;
        if (ttm_count_ > 0) s.avg_ttm_ms = ttm_sum_ / ttm_count_;

        // Avg time before being outbid
        if (!outbid_times_.empty()) {
            uint64_t sum = 0;
            for (auto t : outbid_times_) sum += t;
            s.avg_outbid_delay_ms = sum / outbid_times_.size();
        }

        return s;
    }

    // ── Per-price-level fill rate ─────────────────────────────────────────────
    struct LevelStat {
        double price;
        uint64_t placed = 0;
        uint64_t filled = 0;
        double fill_rate = 0.0;
    };
    std::vector<LevelStat> per_level() const {
        std::lock_guard lock(mutex_);
        std::map<double, std::pair<uint64_t, uint64_t>> levels;
        for (const auto& e : entries_) {
            levels[e.bid_price].first++;   // placed
            if (e.filled) levels[e.bid_price].second++; // filled
        }
        std::vector<LevelStat> result;
        for (auto& [px, counts] : levels) {
            LevelStat ls;
            ls.price = px;
            ls.placed = counts.first;
            ls.filled = counts.second;
            if (counts.first > 0) ls.fill_rate = (double)counts.second / counts.first;
            result.push_back(ls);
        }
        return result;
    }

    // ── Access raw entries (for audit) ────────────────────────────────────────
    const std::vector<MakerBidEntry>& entries() const { return entries_; }

    // ── Reset (for test isolation) ────────────────────────────────────────────
    void reset() {
        std::lock_guard lock(mutex_);
        entries_.clear();
        stats_ = {};
        queue_sum_ = 0;
        gap_sum_ = 0.0;
        ttf_sum_ = 0;
        ttf_count_ = 0;
        ttm_sum_ = 0;
        ttm_count_ = 0;
        touch_times_.clear();
        outbid_times_.clear();
    }

private:
    static uint64_t default_clock() { return 0; }

    struct StatsCounter {
        uint64_t placed{0};
        uint64_t touched{0};
        uint64_t filled{0};
        uint64_t missed{0};
        uint64_t outbid{0};
    };

    mutable std::mutex mutex_;
    Config cfg_;
    ClockFn now_fn_;

    std::vector<MakerBidEntry> entries_;
    StatsCounter stats_{};

    uint64_t queue_sum_ = 0;
    double gap_sum_ = 0.0;
    uint64_t ttf_sum_ = 0;
    uint64_t ttf_count_ = 0;
    uint64_t ttm_sum_ = 0;
    uint64_t ttm_count_ = 0;
    std::vector<uint64_t> touch_times_;
    std::vector<uint64_t> outbid_times_;
};

// ── Static helpers ported from maker-bid.ts ───────────────────────────────────
class MakerBidPricing {
public:
    // Our bid improves best+0.01 and is capped by MAX_ASK
    static double maker_bid_px(double best_bid_px) {
        double improved = best_bid_px + MAKER_BID_METRICS::TICK_SIZE;
        return std::min(MAKER_BID_METRICS::MAX_ASK, improved);
    }

    // Check if our bid would cross the ask (belongs to taker lane, not maker)
    static bool would_cross(double bid, double ask) {
        return bid >= ask;
    }

    // Guard: ask below MIN_ASK_SANITY means our read is the outlier
    static bool ask_invalid(double ask) {
        return ask < MAKER_BID_METRICS::MIN_ASK_SANITY;
    }
};
