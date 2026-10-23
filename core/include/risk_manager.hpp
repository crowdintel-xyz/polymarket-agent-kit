#ifndef RISK_MANAGER_HPP
#define RISK_MANAGER_HPP

// ─────────────────────────────────────────────────────────────────────────────
// RiskManager: the brakes.  Two faces by design:
//
//   * HOT authorize(): read-mostly evaluation consulted before EVERY order is
//     signed.  All mutable state it touches is either owned by the hot loop or
//     a single atomic; recalibration from the cold path lands through a
//     seqlock-published RiskLimits POD (same publication idiom as the book).
//
//   * evaluate(): per-tick mark-to-market.  Computes unrealized P&L at the
//     liquidation-conservative mark (best bid for long inventory), applies the
//     protective ladder KILL > CLOSE(stop-loss) > HEDGE, and returns ONE
//     action the engine executes immediately.  Protective exits never wait
//     for alpha — that is the entire point of the brakes.
//
// Day-loss accounting: real-time day P&L = (realized_pnl − day_realized_base)
// + unrealized P&L.  The base anchors at UTC midnight rollover and at startup.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <cstdint>

#include "order_book.hpp"
#include "polymarket_order.hpp"
#include "position_tracker.hpp"
#include "time_utils.hpp"

// Cold-recalibrable limits.  Copied by value under a seqlock: hot decision
// paths never read individual mutable fields.
struct RiskLimits {
    double stop_loss_pct = 0.15;         // mark drop vs VWAP entry; 0 disables
    double hedge_trigger_pct = 0.0;      // earlier hedge trigger; 0 disables
    double max_daily_loss_usd = 50.0;
    double max_market_exposure_usd = 250.0;
    double max_portfolio_exposure_usd = 250.0;
    uint64_t version = 0;                // monotonic publication counter
};

enum class RiskAction : uint8_t { NONE = 0, CLOSE = 1, HEDGE = 2, KILL = 3 };

struct RiskDecision {
    RiskAction action = RiskAction::NONE;
    uint64_t shares = 0;        // quantity to close / hedge
    uint64_t limit_price = 0;   // x1e6 conservative reference for the action
    int64_t projected_loss = 0; // x1e6 USD loss if action executes at the mark
};

class RiskManager {
public:
    explicit RiskManager(const RiskLimits& initial) : limits_plain_(initial) {
        publish_limits();
    }

    // Cold path: recalibrate limits atomically (hot picks them up coherently).
    void configure(const RiskLimits& limits) noexcept {
        limits_plain_ = limits;
        publish_limits();
    }

    bool read_limits(RiskLimits& out) const noexcept {
        const uint64_t s1 = limits_seq_.load(std::memory_order_seq_cst);
        if (s1 & 1U) return false;
        out = published_;
        const uint64_t s2 = limits_seq_.load(std::memory_order_seq_cst);
        if (s1 != s2 || (s2 & 1U)) return false;
        return true;
    }

    // ── Hot: pre-sign authorization (read-mostly) ───────────────────────────
    bool killed() const noexcept {
        return kill_.load(std::memory_order_acquire);
    }

    void latch_kill() noexcept {
        kill_.store(true, std::memory_order_release);
    }

    bool authorize(uint8_t /*side*/, double notional_usd,
                   double exposure_now_usd,
                   double portfolio_exposure_usd) const noexcept {
        if (killed()) return false;
        RiskLimits limits{};
        if (!read_limits(limits)) return false;  // mid-publish: deny, retry
        if (notional_usd > 0.0 &&
            (exposure_now_usd + notional_usd >
                 limits.max_market_exposure_usd + 1e-9 ||
             portfolio_exposure_usd + notional_usd >
                 limits.max_portfolio_exposure_usd + 1e-9))
            return false;
        return true;
    }

    // ── Hot: day accounting on top of tracker state ─────────────────────────
    // Returns true when the UTC day rolled and the base was re-anchored.
    bool maintain_day_anchor(int64_t tracker_realized) noexcept {
        const uint64_t today = crowdintel::realtime_ns() / 86400000000000ULL;
        if (day_ == 0) {
            day_ = today;
            day_realized_base_ = tracker_realized;
            return true;
        }
        if (today != day_) {
            day_ = today;
            day_realized_base_ = tracker_realized;
            day_kill_latched_ = false;
            return true;
        }
        return false;
    }

    int64_t day_realized(int64_t tracker_realized) const noexcept {
        return tracker_realized - day_realized_base_;
    }

    static int64_t unrealized_pnl(uint64_t net, uint64_t avg,
                                  uint64_t mark_bid) noexcept {
        if (net == 0 || avg == 0) return 0;
        const crowd_int128_t diff =
            static_cast<crowd_int128_t>(mark_bid) -
            static_cast<crowd_int128_t>(avg);
        return static_cast<int64_t>(diff * static_cast<crowd_int128_t>(net) /
                                    static_cast<crowd_int128_t>(1000000));
    }

    // ── Hot: per-tick protective evaluation ─────────────────────────────────
    // hedge_bid/hedge_ask describe the complement book (0 when unavailable).
    RiskDecision evaluate(const OrderBookL2::Top& top,
                          const PositionTracker& tracker,
                          uint64_t hedge_bid, uint64_t hedge_ask) noexcept {
        RiskDecision decision{};
        RiskLimits limits{};
        if (!read_limits(limits)) return decision;  // transient: act next tick

        const int64_t realized_today = day_realized(tracker.realized_pnl());
        const int64_t unreal_yes = unrealized_pnl(
            tracker.net_yes(), tracker.yes_avg(), top.bid.price);
        const int64_t unreal_hedge = unrealized_pnl(
            tracker.net_hedge(), tracker.hedge_avg(), hedge_bid);
        const int64_t day_pnl = realized_today + unreal_yes + unreal_hedge;

        // (d) Global kill switch: realized + floating loss breaches the budget.
        const int64_t loss_budget =
            static_cast<int64_t>(limits.max_daily_loss_usd * 1000000.0);
        if (loss_budget > 0 && day_pnl <= -loss_budget && !day_kill_latched_) {
            day_kill_latched_ = true;
            kill_.store(true, std::memory_order_release);
            decision.action = RiskAction::KILL;
            decision.projected_loss = day_pnl;
            return decision;
        }

        const uint64_t net = tracker.net_yes();
        const uint64_t avg = tracker.yes_avg();
        if (net == 0 || avg == 0 || top.bid.price == 0) return decision;

        // (b) Stop-loss and (c) hedging share the same adverse-move metric:
        // drop of the liquidation mark versus the tracked VWAP entry.
        const double drop = static_cast<double>(avg - top.bid.price) /
                            static_cast<double>(avg);
        if (drop <= 0.0) return decision;
        const int64_t floating = unreal_yes;  // negative when underwater

        const bool stop_enabled = limits.stop_loss_pct > 0.0;
        const bool hedge_enabled = limits.hedge_trigger_pct > 0.0 &&
                                   hedge_ask != 0 && hedge_ask < 1000000;
        const bool stop_hit = stop_enabled && drop >= limits.stop_loss_pct;
        const bool hedge_hit = hedge_enabled &&
                               drop >= limits.hedge_trigger_pct &&
                               tracker.net_hedge() < net;
        if (stop_hit) {
            decision.action = RiskAction::CLOSE;
            decision.shares = net;
            decision.limit_price = top.bid.price;
            decision.projected_loss = floating;
            return decision;
        }
        if (hedge_hit) {
            decision.action = RiskAction::HEDGE;
            decision.shares = net - tracker.net_hedge();
            decision.limit_price = hedge_ask;
            decision.projected_loss = floating;
            return decision;
        }
        return decision;
    }

private:
    void publish_limits() noexcept {
        limits_seq_.fetch_add(1, std::memory_order_seq_cst);
        published_ = limits_plain_;
        published_.version =
            limits_seq_.load(std::memory_order_seq_cst);
        limits_seq_.fetch_add(1, std::memory_order_seq_cst);
    }

    RiskLimits limits_plain_{};
    RiskLimits published_{};
    alignas(64) std::atomic<uint64_t> limits_seq_{0};
    alignas(64) std::atomic<bool> kill_{false};
    uint64_t day_ = 0;
    int64_t day_realized_base_ = 0;
    bool day_kill_latched_ = false;
};

#endif  // RISK_MANAGER_HPP
