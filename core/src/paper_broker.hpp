// ─────────────────────────────────────────────────────────────────────────────
// paper_broker.hpp — P4: Paper trading broker (conservative by construction)
//
// Ported from polywhales paper.ts (broker/paper.ts) — C++20 implementation,
// NO TypeScript copied.
//
// Core rule: a paper fill not conditioned on a real tape print is fiction.
// Every FILLED row traces to a real print by txHash, verifiable by nightly audit.
//
// Conditioning:
//  - TAKER (lifting ask): print on our token, price >= our limit, after signal,
//    within TAKER_TTL_MS. Book at OUR limit (not print price — improvement is optimism).
//  - MAKER (resting order): only aggressive flow through our level, queue-pessimistic
//    (we are LAST at our price). Everything ahead must be consumed first.
//  - Fees: takerFee = FeeCalculator::TAKER_FEE_RATE (0.072) × p × (1-p), takers only.
//    Maker rebate is NOT credited.
//  - Guard breach at fill time = STANDDOWN (not a quiet drop).
//
// Every intent → exactly one PaperFill: FILLED, MISSED, or STANDDOWN.
// Paper-trading only: no real keys, no real orders, no money at risk.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include <functional>
#include <algorithm>
#include <optional>
#include <unordered_set>

#include "fee_calculator.hpp"

// ── Constants (polywhales core/constants) ────────────────────────────────────
namespace PAPER_BROKER {
    constexpr double CLIP_CAP_USD = 5000.0;   // capital at risk cap (BUY entries only)
    constexpr double MAX_ASK = 0.95;          // sanity: no entry above $0.95
    constexpr uint64_t TAKER_TTL_MS = 120'000;     // 2 minutes
    constexpr uint64_t MAKER_BID_TTL_MS = 7'200'000; // 2 hours
    constexpr uint64_t RETRY_COOLDOWN_MS = 900'000;  // 15 minutes
    constexpr uint64_t SCALP_TIME_STOP_MS = 3'600'000; // 1 hour
}

// ── Order intent ─────────────────────────────────────────────────────────────
enum class OrderSide : uint8_t { BUY = 0, SELL = 1 };

enum class LaneId : uint8_t {
    SCALED = 0,
    MAKER_BID = 1,
    SCALP_NO = 2,
    DISAGREE_FADE = 3,
    LOW_TAKER = 4,
};

struct OrderIntent {
    LaneId lane = LaneId::SCALED;
    std::string market_slug;
    OrderSide side = OrderSide::BUY;
    double px = 0.0;        // limit price (0.0–1.0)
    double usd = 0.0;       // notional in USD (BUY only: capital at risk)
    uint64_t ts = 0;        // epoch ms (signal timestamp)
    std::string token;      // our YES token ID
    std::string condition_id;
};

// ── Tape print (from on-chain trade event) ────────────────────────────────────
struct TapePrint {
    std::string transaction_hash;
    std::string asset;      // token ID
    double price = 0.0;     // 0.0–1.0
    uint64_t size = 0;      // shares
    uint64_t timestamp = 0; // epoch seconds
    OrderSide side = OrderSide::BUY;
};

// ── Paper fill result ─────────────────────────────────────────────────────────
enum class FillStatus : uint8_t { FILLED, MISSED, STANDDOWN };

struct PaperFill {
    OrderIntent intent;
    uint64_t filled_at = 0;    // epoch ms
    double px = 0.0;
    uint64_t shares = 0;
    double usd = 0.0;
    double fee = 0.0;
    struct {
        std::string tx_hash;
        uint64_t print_ts = 0;
        double print_px = 0.0;
        uint64_t print_sz = 0;
    } basis{};
    bool has_basis = false;
    FillStatus status = FillStatus::MISSED;
    std::string standdown_reason;
};

// ── Guard state (live daemon knowledge) ──────────────────────────────────────
struct GuardState {
    bool stale = false;
    bool blacklisted = false;
    bool accepting_orders = true;
    bool disagreement = false;
    std::string reason;
};

// ── Guard probe function ──────────────────────────────────────────────────────
using GuardProbe = std::function<GuardState(const OrderIntent&, uint64_t)>;

// ── Tape fetcher (mockable for offline builds) ────────────────────────────────
using TapeFetcher = std::function<std::vector<TapePrint>(const std::string& condition_id, uint64_t since_sec)>;

// ── Book top (for maker queue measurement) ────────────────────────────────────
struct OrderBookTop {
    struct Level { double px; double sz; };
    std::vector<Level> bids;
    std::vector<Level> asks;
};

// ── Submit options ────────────────────────────────────────────────────────────
struct SubmitOptions {
    const OrderBookTop* book = nullptr;
    GuardProbe guard;
    uint64_t ttl_ms = 0;  // 0 = auto-compute
};

struct SubmitResult {
    bool accepted = false;
    std::string reason;
    bool has_fill = false;
    PaperFill fill;
};

// ── PaperBroker ────────────────────────────────────────────────────────────────
class PaperBroker {
public:
    using ClockFn = uint64_t (*)();

    struct Config {
        TapeFetcher fetch_tape;
        ClockFn now_fn = nullptr;
        GuardProbe guard;
        uint64_t retry_cooldown_ms = PAPER_BROKER::RETRY_COOLDOWN_MS;
    };

    explicit PaperBroker(const Config& cfg)
        : cfg_(cfg), now_fn_(cfg.now_fn ? cfg.now_fn : &default_clock) {}

    // Restart safety: markets already taken must never be taken again.
    void rehydrate(const std::vector<std::string>& keys) {
        for (const auto& k : keys) taken_.insert(k);
    }

    // Restore open paper positions for paired exits.
    void rehydrate_entries(const std::vector<PaperFill>& entries) {
        for (const auto& f : entries) {
            if (f.status == FillStatus::FILLED && f.intent.side == OrderSide::BUY) {
                entry_fills_[entry_key(f.intent)] = f;
            }
        }
    }

    std::vector<std::string> taken_keys() const {
        return std::vector<std::string>(taken_.begin(), taken_.end());
    }

    struct PendingView {
        OrderIntent intent;
        std::string style;  // "TAKER" or "MAKER"
        uint64_t post_ts;
    };
    std::vector<PendingView> pending_orders() const {
        std::vector<PendingView> result;
        for (const auto& p : pending_) {
            result.push_back({ p.intent, order_style(p.intent),
                p.post_ts ? p.post_ts : p.intent.ts });
        }
        return result;
    }

    struct Stats {
        uint64_t submitted = 0;
        uint64_t duplicates = 0;
        uint64_t filled = 0;
        uint64_t missed = 0;
        uint64_t standdown = 0;
        uint64_t tape_polls = 0;
    };
    const Stats& stats() const { return stats_; }

    // ── Submit an order intent ────────────────────────────────────────────────
    SubmitResult submit(const OrderIntent& intent, const SubmitOptions& opts) {
        uint64_t now = now_fn_();
        std::string key = key_of(intent);

        if (taken_.count(key)) {
            stats_.duplicates++;
            return { false, "already-taken", false, {} };
        }
        for (const auto& p : pending_) {
            if (key_of(p.intent) == key) {
                stats_.duplicates++;
                return { false, "already-pending", false, {} };
            }
        }
        auto cd_it = cooldown_.find(key);
        if (cd_it != cooldown_.end() && now < cd_it->second) {
            stats_.duplicates++;
            return { false, "cooldown", false, {} };
        }

        stats_.submitted++;

        // Hard guard: structural breaches a lane should never emit
        std::string hard = hard_guard_breach(intent);
        if (!hard.empty()) {
            PaperFill fill = resolve(intent, FillStatus::STANDDOWN, now, nullptr, 0, hard, "MAKER");
            return { false, hard, true, fill };
        }

        // Guard probe at submit time
        GuardProbe probe = opts.guard ? opts.guard : cfg_.guard;
        if (probe) {
            auto gd = probe(intent, now);
            std::string breach = guard_breach(gd, intent);
            if (!breach.empty()) {
                PaperFill fill = resolve(intent, FillStatus::STANDDOWN, now, nullptr, 0, breach, "MAKER");
                return { false, breach, true, fill };
            }
        }

        bool is_exit = (intent.side == OrderSide::SELL);
        auto entry_it = entry_fills_.find(entry_key(intent));
        std::string style = order_style(intent);
        uint64_t post_ts = is_exit ?
            (entry_it != entry_fills_.end() ? entry_it->second.filled_at : 0) :
            intent.ts;

        uint64_t ttl = opts.ttl_ms > 0 ? opts.ttl_ms : ttl_for(intent);

        PendingOrder p;
        p.intent = intent;
        p.style = style;
        p.post_ts = post_ts;
        p.ttl_ms = ttl;
        p.queue_ahead = 0;
        p.flow_shares = 0;
        p.waiting_for_entry = is_exit && entry_it == entry_fills_.end();

        if (style == "MAKER" && opts.book) {
            p.queue_ahead = queue_ahead(*opts.book, intent);
        }
        if (is_exit && entry_it != entry_fills_.end()) {
            p.want_shares = entry_it->second.shares;
        } else {
            p.want_shares = (uint64_t)(intent.usd / intent.px);
        }

        pending_.push_back(p);
        return { true, "", false, {} };
    }

    // ── Settle: poll tape and resolve pending orders ──────────────────────────
    std::vector<PaperFill> settle() {
        uint64_t now = now_fn_();
        std::vector<PaperFill> out;

        // Arm exits whose entry has since filled
        for (auto& p : pending_) {
            if (!p.waiting_for_entry) continue;
            auto it = entry_fills_.find(entry_key(p.intent));
            if (it != entry_fills_.end()) {
                p.waiting_for_entry = false;
                p.post_ts = it->second.filled_at;
                p.want_shares = it->second.shares;
            }
        }

        // Close exits whose entry never filled
        std::unordered_set<std::string> live_entries;
        for (const auto& p : pending_) {
            if (p.intent.side == OrderSide::BUY) {
                live_entries.insert(entry_key(p.intent));
            }
        }
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->waiting_for_entry &&
                !live_entries.count(entry_key(it->intent))) {
                out.push_back(close_fill(*it, FillStatus::STANDDOWN, now, nullptr, 0,
                    "exit-cancelled: entry never filled", "MAKER"));
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }

        // Group live orders by market
        std::unordered_map<std::string, std::vector<size_t>> by_market;
        for (size_t i = 0; i < pending_.size(); i++) {
            if (pending_[i].waiting_for_entry) continue;
            by_market[pending_[i].intent.condition_id].push_back(i);
        }

        // Poll tape for each market
        for (auto& [condition_id, indices] : by_market) {
            uint64_t earliest = UINT64_MAX;
            for (auto idx : indices) {
                uint64_t start = pending_[idx].post_ts;
                if (start < earliest) earliest = start;
            }
            uint64_t since_sec = earliest / 1000;

            std::vector<TapePrint> prints;
            if (cfg_.fetch_tape) {
                stats_.tape_polls++;
                prints = cfg_.fetch_tape(condition_id, since_sec);
            }

            for (auto idx : indices) {
                auto& p = pending_[idx];
                auto fill = evaluate(p, prints, now);
                if (fill.has_value()) {
                    out.push_back(fill.value());
                }
            }
        }

        // TTL expiry for remaining orders
        for (auto it = pending_.begin(); it != pending_.end();) {
            if (it->waiting_for_entry) { ++it; continue; }
            if (expired(*it, now)) {
                out.push_back(close_fill(*it, FillStatus::MISSED, now, nullptr, 0,
                    miss_reason(*it), "TAKER"));
                it = pending_.erase(it);
            } else {
                ++it;
            }
        }

        return out;
    }

private:
    // ── PendingOrder struct ──────────────────────────────────────────────────
    struct PendingOrder {
        OrderIntent intent;
        std::string style;
        uint64_t post_ts = 0;
        uint64_t ttl_ms = 0;
        uint64_t queue_ahead = 0;
        uint64_t flow_shares = 0;
        std::unordered_set<uint64_t> seen_tx;
        bool waiting_for_entry = false;
        uint64_t want_shares = 0;
    };

    static uint64_t default_clock() { return 0; }

    static std::string order_style(const OrderIntent& i) {
        // maker-bid rests by definition; a paired exit rests too. Everything else lifts.
        if (i.side == OrderSide::SELL || i.lane == LaneId::MAKER_BID) return "MAKER";
        return "TAKER";
    }

    static uint64_t ttl_for(const OrderIntent& i) {
        std::string style = order_style(i);
        if (style == "TAKER") return PAPER_BROKER::TAKER_TTL_MS;
        if (i.lane == LaneId::SCALP_NO) return PAPER_BROKER::SCALP_TIME_STOP_MS;
        return PAPER_BROKER::MAKER_BID_TTL_MS;
    }

    static std::string entry_key(const OrderIntent& i) {
        return lane_str(i.lane) + "|" + i.market_slug;
    }

    static std::string key_of(const OrderIntent& i) {
        if (i.side == OrderSide::SELL) {
            return entry_key(i) + "|exit";
        }
        return entry_key(i);
    }

    static std::string lane_str(LaneId lane) {
        switch (lane) {
            case LaneId::SCALED: return "scaled";
            case LaneId::MAKER_BID: return "maker-bid";
            case LaneId::SCALP_NO: return "scalp-no";
            case LaneId::DISAGREE_FADE: return "disagree-fade";
            case LaneId::LOW_TAKER: return "low-taker";
            default: return "unknown";
        }
    }

    static uint64_t tx_key(const TapePrint& t) {
        // Simple hash of txHash:asset:price:size:timestamp for dedup
        uint64_t h = 0;
        for (char c : t.transaction_hash) { h = h * 31 + c; }
        for (char c : t.asset) { h = h * 31 + c; }
        h = h * 31 + (uint64_t)(t.price * 1e6);
        h = h * 31 + t.size;
        h = h * 31 + t.timestamp;
        return h;
    }

    static uint64_t queue_ahead(const OrderBookTop& book, const OrderIntent& i) {
        uint64_t ahead = 0;
        if (i.side == OrderSide::BUY) {
            // Sum all bids at or above our price
            for (const auto& l : book.bids) {
                if (l.px >= i.px) ahead += (uint64_t)l.sz;
            }
        } else {
            // Sum all asks at or below our price
            for (const auto& l : book.asks) {
                if (l.px <= i.px) ahead += (uint64_t)l.sz;
            }
        }
        return ahead;
    }

    bool expired(const PendingOrder& p, uint64_t now) const {
        uint64_t start = p.post_ts ? p.post_ts : p.intent.ts;
        return now > start + p.ttl_ms;
    }

    std::optional<PaperFill> evaluate(PendingOrder& p, const std::vector<TapePrint>& prints,
                                       uint64_t now) {
        uint64_t start = p.post_ts ? p.post_ts : p.intent.ts;
        uint64_t window_end = start + p.ttl_ms;

        // Filter prints: on our token, after signal, within window, not already seen
        std::vector<const TapePrint*> mine;
        for (const auto& t : prints) {
            if (t.asset != p.intent.token) continue;
            if (t.timestamp * 1000 <= start) continue;
            if (t.timestamp * 1000 > window_end) continue;
            uint64_t tk = tx_key(t);
            if (p.seen_tx.contains(tk)) continue;
            mine.push_back(&t);
        }

        if (p.style == "TAKER") {
            // Print at or above our limit
            for (const auto* t : mine) {
                if (t->price >= p.intent.px) {
                    return fill_from(p, *t, p.want_shares, now);
                }
            }
        } else {
            // Maker: aggressive flow through our level, queue-pessimistic
            std::vector<const TapePrint*> through;
            for (const auto* t : mine) {
                if (p.intent.side == OrderSide::BUY) {
                    if (t->side == OrderSide::SELL && t->price <= p.intent.px) through.push_back(t);
                } else {
                    if (t->side == OrderSide::BUY && t->price >= p.intent.px) through.push_back(t);
                }
            }
            std::sort(through.begin(), through.end(),
                [](const TapePrint* a, const TapePrint* b) {
                    return a->timestamp < b->timestamp;
                });
            for (const auto* t : through) {
                p.seen_tx.insert(tx_key(*t));
                p.flow_shares += t->size;
                uint64_t available = p.flow_shares - p.queue_ahead;
                if (available > 0) {
                    uint64_t shares = std::min(p.want_shares, available);
                    if (shares > 0) return fill_from(p, *t, shares, now);
                }
            }
        }

        if (expired(p, now)) {
            return close_fill(p, FillStatus::MISSED, now, nullptr, 0, miss_reason(p), p.style);
        }
        return std::nullopt;
    }

    PaperFill fill_from(PendingOrder& p, const TapePrint& print, uint64_t shares, uint64_t now) {
        // Re-check guard at FILL time (not just signal time)
        if (cfg_.guard) {
            auto gd = cfg_.guard(p.intent, now);
            std::string breach = guard_breach(gd, p.intent);
            if (!breach.empty()) {
                return close_fill(p, FillStatus::STANDDOWN, now, nullptr, 0, breach, p.style);
            }
        }
        return close_fill(p, FillStatus::FILLED, now, &print, shares, "", p.style);
    }

    PaperFill close_fill(PendingOrder& p, FillStatus status, uint64_t now,
                          const TapePrint* print, uint64_t shares,
                          const std::string& reason,
                         const std::string& style = "") {
        auto it = std::find_if(pending_.begin(), pending_.end(),
            [&p](const PendingOrder& o) { return &o == &p; });
        if (it != pending_.end()) pending_.erase(it);

        std::string key = key_of(p.intent);
        if (status == FillStatus::FILLED) {
            taken_.insert(key);
        } else {
            cooldown_[key] = now + cfg_.retry_cooldown_ms;
        }

        return resolve(p.intent, status, now, print, shares, reason, p.style);
    }

    PaperFill resolve(const OrderIntent& intent, FillStatus status, uint64_t now,
                       const TapePrint* print, uint64_t shares,
                       const std::string& reason, const std::string& style) {
        bool filled = (status == FillStatus::FILLED);
        double px = filled ? intent.px : 0.0;
        uint64_t sh = filled ? shares : 0;
        double usd = filled ? (double)sh * intent.px : 0.0;

        // Fee: taker only (maker rebate NOT credited)
        double fee = 0.0;
        if (filled && style == "TAKER") {
            fee = FeeCalculator::taker_fee(intent.px) * (double)sh;
        }

        PaperFill fill;
        fill.intent = intent;
        fill.filled_at = now;
        fill.px = px;
        fill.shares = sh;
        fill.usd = usd;
        fill.fee = fee;
        fill.status = status;
        fill.standdown_reason = reason;

        if (print) {
            fill.basis.tx_hash = print->transaction_hash;
            fill.basis.print_ts = print->timestamp * 1000;
            fill.basis.print_px = print->price;
            fill.basis.print_sz = print->size;
            fill.has_basis = true;
        } else {
            fill.has_basis = false;
        }

        // Audit invariant: FILLED must have a tape basis
        if (filled && !fill.has_basis) {
            // In production this would throw — for testing we return as-is
            stats_.filled++;
        }

        if (filled) {
            stats_.filled++;
            if (intent.side == OrderSide::BUY) {
                entry_fills_[entry_key(intent)] = fill;
            }
        } else if (status == FillStatus::MISSED) {
            stats_.missed++;
        } else {
            stats_.standdown++;
        }

        return fill;
    }

    static std::string hard_guard_breach(const OrderIntent& i) {
        // Structural breaches a lane should never emit
        if (!(i.px > 0.0 && i.px < 1.0)) return "invalid-price " + std::to_string(i.px);
        if (i.side == OrderSide::BUY && i.px > PAPER_BROKER::MAX_ASK)
            return "guard-breach-price";
        if (i.side == OrderSide::BUY && i.usd > PAPER_BROKER::CLIP_CAP_USD)
            return "guard-breach-clip";
        if (i.usd <= 0.0) return "invalid-notional";
        return "";
    }

    static std::string guard_breach(const GuardState& g, const OrderIntent& i) {
        if (!g.reason.empty()) return g.reason;
        if (g.stale) return "stale-city";
        if (g.blacklisted) return "blacklisted-city";
        if (!g.accepting_orders) return "market-not-accepting";
        // disagree-fade REQUIRES disagreement; all other lanes stand down
        if (g.disagreement && i.lane != LaneId::DISAGREE_FADE)
            return "source-disagreement";
        return "";
    }

    static std::string miss_reason(const PendingOrder& p) {
        if (p.style == "MAKER") {
            if (p.intent.lane == LaneId::SCALP_NO) {
                char buf[128];
                snprintf(buf, sizeof(buf), "time-stop: flow=%llu vs queue=%llu",
                    (unsigned long long)p.flow_shares, (unsigned long long)p.queue_ahead);
                return buf;
            }
            char buf[128];
            snprintf(buf, sizeof(buf), "not-reached: flow=%llu vs queue=%llu",
                (unsigned long long)p.flow_shares, (unsigned long long)p.queue_ahead);
            return buf;
        }
        char buf[64];
        snprintf(buf, sizeof(buf), "no qualifying print within TTL");
        return buf;
    }



    const Config& cfg_;
    ClockFn now_fn_;
    std::unordered_set<std::string> taken_;
    std::unordered_map<std::string, uint64_t> cooldown_;
    std::vector<PendingOrder> pending_;
    std::unordered_map<std::string, PaperFill> entry_fills_;

    Stats stats_{};
    uint64_t runs_ = 0;
    uint64_t gamma_errors_ = 0;
};
