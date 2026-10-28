#ifndef EXECUTION_ENGINE_HPP
#define EXECUTION_ENGINE_HPP

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <ctime>

#include "../crypto/eip712_signer.hpp"
#include "../crypto/fast_random.hpp"
#include "../include/account_events.hpp"
#include "../include/journal.hpp"
#include "../include/order_book.hpp"
#include "../include/position_tracker.hpp"
#include "../include/bayesian_engine.hpp"
#include "../include/evidence.hpp"
#include "../include/risk_manager.hpp"
#include "../include/source_reliability.hpp"
#include "../include/volatility_gate.hpp"
#include "../include/spsc_ring_buffer.hpp"
#include "../include/time_utils.hpp"
#include "alpha_receiver.hpp"
#include "kelly_engine.hpp"
#include "market_config.hpp"
#include "polymarket_order.hpp"
#include "presigned_pool.hpp"

enum class TickResult : uint8_t {
    NO_SIGNAL = 0,
    FILTERED_STATS,
    WRONG_MARKET,
    STALE_SIGNAL,
    DUPLICATE_SIGNAL,
    NO_BOOK,
    NO_EDGE,
    NO_INVENTORY,
    RISK_REJECTED,
    TOO_SMALL,
    SIGN_FAILED,
    BODY_FAILED,
    SUBMIT_FAILED,
    QUEUED,
    SUBMITTED,
    ACCOUNT_APPLIED,     // housekeeping consumed user-channel facts
    RISK_STOP_LOSS,      // protective close emitted without alpha input
    RISK_HEDGE,          // hedge order emitted on the complement token
    RISK_KILL_SWITCH,    // daily-loss kill switch was latched this tick
    STALE_PRICE_ABORT,   // slippage-vs-mid guard rejected a stale/toxic order
    VOLATILITY_PAUSED,   // volatility regime suppressed passive flow
    BAYES_SIGNAL,        // a bayesian posterior fired an order this tick
    COUNT
};

inline const char* tick_result_name(TickResult result) {
    switch (result) {
        case TickResult::NO_SIGNAL: return "no_signal";
        case TickResult::FILTERED_STATS: return "filtered_stats";
        case TickResult::WRONG_MARKET: return "wrong_market";
        case TickResult::STALE_SIGNAL: return "stale_signal";
        case TickResult::DUPLICATE_SIGNAL: return "duplicate_signal";
        case TickResult::NO_BOOK: return "no_book";
        case TickResult::NO_EDGE: return "no_edge";
        case TickResult::NO_INVENTORY: return "no_inventory";
        case TickResult::RISK_REJECTED: return "risk_rejected";
        case TickResult::TOO_SMALL: return "too_small";
        case TickResult::SIGN_FAILED: return "sign_failed";
        case TickResult::BODY_FAILED: return "body_failed";
        case TickResult::SUBMIT_FAILED: return "submit_failed";
        case TickResult::QUEUED: return "queued";
        case TickResult::SUBMITTED: return "submitted";
        case TickResult::ACCOUNT_APPLIED: return "account_applied";
        case TickResult::RISK_STOP_LOSS: return "risk_stop_loss";
        case TickResult::RISK_HEDGE: return "risk_hedge";
        case TickResult::RISK_KILL_SWITCH: return "risk_kill_switch";
        case TickResult::STALE_PRICE_ABORT: return "stale_price_abort";
        case TickResult::VOLATILITY_PAUSED: return "volatility_paused";
        case TickResult::BAYES_SIGNAL: return "bayes_signal";
        case TickResult::COUNT: break;
    }
    return "unknown";
}

// Forward declarations of the P2–P4 layers (defined in their own headers).
class RiskManager;
class VolatilityGate;
class BayesianEngine;
class SourceReliability;
struct EvidenceEvent;

// Authoritative restatement from the cold REST reconciler.  Travels on its
// own dedicated SPSC queue (a different producer always gets its own queue).
struct TrackerRestate {
    uint64_t yes_shares = 0;
    uint64_t yes_avg = 0;
    uint64_t hedge_shares = 0;
    uint64_t hedge_avg = 0;
    uint64_t drift_exceeded = 0;  // non-zero: latch the kill switch
};
static_assert(std::is_trivially_copyable_v<TrackerRestate>);

// Returns a monotonically increasing day number based on the current UTC
// calendar date.  Two calls on the same UTC day return the same value; a call
// after local midnight UTC returns a larger value.  This lets the daily
// buy-volume cap roll over at the correct boundary without depending on
// process restarts.  The value is never interpreted as a real date, so it is
// Y2038-safe on 64-bit time_t.
inline uint64_t utc_day_number() noexcept {
    const std::time_t now = std::time(nullptr);
    std::tm utc{};
    // gmtime_r is POSIX; on all target platforms time_t is 64-bit.
    gmtime_r(&now, &utc);
    const uint64_t year = static_cast<uint64_t>(utc.tm_year + 1900);
    const uint64_t month = static_cast<uint64_t>(utc.tm_mon + 1);
    const uint64_t day = static_cast<uint64_t>(utc.tm_mday);
    // Days from epoch (1970-01-01) using the civil-from-days algorithm.
    const uint64_t y = (month <= 2) ? year - 1 : year;
    const uint64_t era = (y >= 0 ? y : y - 399) / 400;
    const uint64_t yoe = y - (era * 400);                             // [0, 399]
    const uint64_t doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const uint64_t doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;       // [0, 146096]
    return era * 36524 + static_cast<uint64_t>(doe);
}

// Optional second-generation layers.  Every pointer is nullable; an absent
// layer preserves the exact pre-existing (legacy) engine behavior, so all
// original tests and the mock path keep their semantics byte-for-byte.
struct EngineLayers {
    SPSC_RingBuffer<AccountEvent>* account_q = nullptr;  // user-channel facts
    PositionTracker* tracker = nullptr;                  // inventory/P&L state
    SPSC_RingBuffer<JournalEvent>* journal_q = nullptr;  // audit journal sink
    SPSC_RingBuffer<TrackerRestate, 16>* restate_q = nullptr;  // REST reconcile
    RiskManager* risk = nullptr;                         // brakes (P2)
    VolatilityGate* volatility = nullptr;                // adverse selection (P3)
    SPSC_RingBuffer<EvidenceEvent>* evidence_q = nullptr;  // brain (P4)
    BayesianEngine* bayes = nullptr;
    SourceReliability* sources = nullptr;
    OrderBookL2* hedge_book = nullptr;                   // complement book (P2)
};

template <typename Client>
class ExecutionEngine {
public:
    ExecutionEngine(const MarketConfig& cfg,
                    OrderBookL2& book,
                    SPSC_RingBuffer<AlphaSignal>& signals,
                    const EIP712Signer& signer,
                    PresignedOrderPool& pool,
                    Client& client,
                    std::atomic<bool>* trading_enabled = nullptr,
                    const EngineLayers* layers = nullptr)
        : cfg_(cfg), book_(book), signals_(signals), signer_(signer),
          pool_(pool), client_(client), trading_enabled_(trading_enabled),
          confirmed_inventory_(cfg.initial_position_shares) {
        if (layers) {
            account_q_ = layers->account_q;
            tracker_ = layers->tracker;
            journal_q_ = layers->journal_q;
            restate_q_ = layers->restate_q;
            risk_ = layers->risk;
            volatility_ = layers->volatility;
            evidence_q_ = layers->evidence_q;
            bayes_ = layers->bayes;
            sources_ = layers->sources;
            hedge_book_ = layers->hedge_book;
        }
        // The account queue and tracker are inseparable: accepting facts
        // without state to apply them to would silently lose fills.
        if (account_q_ && !tracker_) tracker_ = nullptr, account_q_ = nullptr;
    }

    TickResult run_tick() {
        // PRIORITY 1+2 (eyes and brakes): user-channel facts are drained and
        // protective risk actions execute BEFORE any alpha decision, so
        // orders never fire against stale inventory or a breached stop.
        const TickResult housework = housekeeping();
        if (housework == TickResult::RISK_KILL_SWITCH ||
            housework == TickResult::RISK_STOP_LOSS ||
            housework == TickResult::RISK_HEDGE)
            return housework;  // protective action owns this tick

        // ── P3 (adverse selection) ─────────────────────────────────────────
        // Mid observation must be CONTINUOUS: the shock FSM only detects a
        // >5%/100ms jump if it sees the mid every tick, not only when alpha
        // happens to arrive.  One extra best-effort seqlock read (~tens of
        // ns) far from the +5us budget, and only when the gate is attached.
        bool flow_suppressed = false;
        if (volatility_) {
            OrderBookL2::Top obs{};
            const uint64_t max_age_ns = cfg_.max_book_age_ms * 1000000ULL;
            if (book_.read_top(obs, max_age_ns) && obs.bid.size != 0 &&
                obs.ask.size != 0 && obs.bid.price < obs.ask.price) {
                const uint64_t mid_obs = (obs.bid.price + obs.ask.price) / 2;
                const bool shocked = volatility_->observe_mid(
                    mid_obs, crowdintel::mono_ns());
                flow_suppressed = shocked || volatility_->paused();
                const uint32_t regime = volatility_->regime();
                if (regime != last_regime_) {
                    journal(regime != 0 ? JournalEvent::Type::VOLATILITY_ENTER
                                        : JournalEvent::Type::VOLATILITY_EXIT,
                            0, regime,
                            obs.ask.price - obs.bid.price, 0);
                    last_regime_ = regime;
                }
                // Every newly-armed shock is journaled exactly once: the
                // discarded ladder slots are auditable (P3 acceptance:
                // "logs and adapts" on a >5% jump inside 100 ms).
                const uint64_t shock_count = volatility_->shocks();
                if (shock_count != shocks_seen_) {
                    journal(JournalEvent::Type::POOL_STALE_DROP, 0,
                            mid_obs, shock_count, 0);
                    shocks_seen_ = shock_count;
                }
            }
        }

        // ── P4 (brain) ────────────────────────────────────────────────────
        // Evidence ingestion precedes the alpha pop on every tick: posterior
        // state must track facts even while trading flow is busy.  Each
        // event folds into the closed form in tens of nanoseconds.
        drain_evidence();

        AlphaSignal signal{};
        if (signals_.try_pop(signal)) {
            if (volatility_ && flow_suppressed)
                return TickResult::VOLATILITY_PAUSED;
            return execute_signal_pipeline(signal, false);
        }

        // No queued alpha: evaluate the posterior trigger against the book.
        if (bayes_ && sources_ && cfg_.bayes_enable &&
            !(volatility_ && flow_suppressed)) {
            const TickResult brain = evaluate_posterior_trigger();
            if (brain != TickResult::NO_SIGNAL) return brain;
        }

        return housework != TickResult::NO_SIGNAL ? housework
                                                  : TickResult::NO_SIGNAL;
    }

    // Shared dispatch for queued alpha and posterior-synthesized signals.
    // A bayesian-tagged signal maps SUBMITTED to BAYES_SIGNAL so the brain's
    // orders stay accountable against plain alpha in the session report.
    TickResult execute_signal_pipeline(const AlphaSignal& signal,
                                       bool from_bayes) {
        if (risk_ && risk_->killed()) return TickResult::RISK_REJECTED;

        if (!std::isfinite(signal.p_win) || !std::isfinite(signal.confidence) ||
            !std::isfinite(signal.q_value) ||
            !(signal.p_win > 0.0 && signal.p_win < 1.0) ||
            signal.confidence < cfg_.min_confidence ||
            signal.q_value > cfg_.max_q_value || signal.direction_hint > 2)
            return TickResult::FILTERED_STATS;
        if (signal.market_hash != cfg_.market_hash)
            return TickResult::WRONG_MARKET;

        const uint64_t now_ns = realtime_ns();
        const uint64_t ttl_ns = cfg_.signal_ttl_ms * 1000000ULL;
        constexpr uint64_t MAX_FUTURE_SKEW_NS = 1000000000ULL;
        if (signal.timestamp_ns == 0 ||
            (signal.timestamp_ns > now_ns &&
             signal.timestamp_ns - now_ns > MAX_FUTURE_SKEW_NS) ||
            (now_ns > signal.timestamp_ns && now_ns - signal.timestamp_ns > ttl_ns))
            return TickResult::STALE_SIGNAL;
        if (seen_before(signal.signal_id, now_ns, ttl_ns))
            return TickResult::DUPLICATE_SIGNAL;
        if (trading_enabled_ && !trading_enabled_->load(std::memory_order_acquire))
            return TickResult::RISK_REJECTED;

        OrderBookL2::Top top{};
        bool have_book = false;
        const uint64_t max_age_ns = cfg_.max_book_age_ms * 1000000ULL;
        for (int attempt = 0; attempt < 4 && !have_book; ++attempt)
            have_book = book_.read_top(top, max_age_ns);
        if (!have_book || top.bid.size == 0 || top.ask.size == 0 ||
            top.bid.price >= top.ask.price)
            return TickResult::NO_BOOK;

        uint8_t side = K_SIDE_BUY;
        if (signal.direction_hint == 0) {
            side = K_SIDE_BUY;
        } else if (signal.direction_hint == 1) {
            side = K_SIDE_SELL;
        } else {
            const double buy_edge = net_edge(K_SIDE_BUY, signal.p_win,
                                             static_cast<double>(top.ask.price) * 1e-6);
            const double sell_edge = net_edge(K_SIDE_SELL, signal.p_win,
                                              static_cast<double>(top.bid.price) * 1e-6);
            side = buy_edge >= sell_edge ? K_SIDE_BUY : K_SIDE_SELL;
        }

        if (side == K_SIDE_SELL && sellable_inventory() < cfg_.min_size_shares)
            return TickResult::NO_INVENTORY;

        const uint64_t tick = book_.tick_size(cfg_.tick_size);
        const uint64_t raw_book_price = side == K_SIDE_BUY
                                      ? top.ask.price : top.bid.price;
        const uint64_t price_raw = round_price_to_tick(raw_book_price, tick);
        const double price = static_cast<double>(price_raw) * 1e-6;
        const double edge = net_edge(side, signal.p_win, price);
        if (edge < cfg_.min_edge) return TickResult::NO_EDGE;

        // Pre-sign slippage guard (P3): the signed target may never sit
        // further than BOT_POOL_MAX_DEV_BPS from the CURRENT mid.  A pool
        // slot whose price drifted that far away is a stale order; firing it
        // would realize exactly the adverse selection this layer exists for.
        const uint64_t mid_now = (top.bid.price + top.ask.price) / 2;
        if (volatility_ &&
            !volatility_->slippage_ok(side, price_raw, mid_now)) {
            volatility_->note_stale_abort();
            journal(JournalEvent::Type::STALE_PRICE_ABORT, 0, price_raw,
                    mid_now, 0);
            return TickResult::STALE_PRICE_ABORT;
        }

        // Size against the fee-adjusted execution price.  This is conservative:
        // fees reduce both the gate and the Kelly fraction.
        const double fee_per_share = cfg_.taker_fee_rate * price * (1.0 - price);
        const double sizing_price = side == K_SIDE_BUY
            ? std::min(0.999999, price + fee_per_share)
            : std::max(0.000001, price - fee_per_share);
        const double kelly = side == K_SIDE_BUY
            ? KellyEngine::kelly_buy(signal.p_win, sizing_price)
            : KellyEngine::kelly_sell(signal.p_win, sizing_price);
        double usd = KellyEngine::position_usd(
            kelly, cfg_.kelly_fraction, cfg_.bankroll_usd);
        // BUG FIX (A3): Reset daily buy-volume counter on UTC day rollover so
        // the daily-loss cap window always matches the calendar day.
        check_daily_reset();
        double available_budget = cfg_.max_order_usd;
        if (side == K_SIDE_BUY) {
            available_budget = std::min(
                available_budget,
                std::max(0.0, cfg_.max_exposure_usd - exposure_now_usd()));
            available_budget = std::min(
                available_budget,
                std::max(0.0, cfg_.max_daily_loss_usd - daily_buy_volume_.load(std::memory_order_acquire)));
        }
        usd = std::min(usd, available_budget);
        // Volatile regime shrinks passive size (P3): the cold sampler scales
        // by BOT_VOL_SIZE_MULTIPLIER; a pause never reaches this line.
        if (volatility_) {
            const uint32_t shrink = volatility_->size_permille();
            if (shrink < 1000)
                usd = usd * static_cast<double>(shrink) / 1000.0;
        }
        // BUG FIX (A2-pre-size): Size shares against the FEE-ADJUSTED price
        // (sizing_price, which includes taker fees for BUY) so that the
        // realized notional + fee stays within max_order_usd.  Using raw
        // `price` would over-allocate shares whose fee pushes the total
        // above the cap, causing spurious RISK_REJECTED at the boundary.
        uint64_t requested_shares = KellyEngine::usd_to_shares_fixed(usd, sizing_price);

        const uint64_t visible = side == K_SIDE_BUY ? top.ask.size : top.bid.size;
        requested_shares = std::min(requested_shares, visible);
        if (side == K_SIDE_SELL)
            requested_shares = std::min(requested_shares, sellable_inventory());
        if (requested_shares < cfg_.min_size_shares)
            return TickResult::TOO_SMALL;

        const bool market_order = std::strcmp(cfg_.order_type, "FAK") == 0 ||
                                  std::strcmp(cfg_.order_type, "FOK") == 0;
        uint64_t maker_amount = 0, taker_amount = 0, effective_shares = 0;
        if (!compute_order_amounts(side, price_raw, requested_shares, tick,
                                   market_order, maker_amount, taker_amount,
                                   effective_shares) ||
            effective_shares < cfg_.min_size_shares)
            return TickResult::TOO_SMALL;

        // Hard per-order and portfolio caps.  With the tracker layer attached
        // these read the reconciled worst-cost exposure; without it they keep
        // the legacy local-reservation semantics.
        //
        // BUG FIX (A2): order_notional must include the taker fee.
        // Previously the cap check was fee-blind, so BUY orders could
        // exceed BOT_MAX_ORDER_USD by the fee amount.  max_order_usd is a
        // per-order worst-cost cap, so the fee must be included:
        //   - BUY:  total_cost = notional + fee (fee is additional outflow)
        //   - SELL: net_value  = notional - fee (conservative: fee reduces
        //            realized proceeds, lowering economic exposure)
        // effective_shares is in micro-shares (×1e6); fee_per_share is USD,
        // so total_fee = fee_per_share * effective_shares * 1e-6 → USD.
        const double total_fee = fee_per_share *
            static_cast<double>(effective_shares) * 1e-6;
        const double base_notional = static_cast<double>(
            side == K_SIDE_BUY ? maker_amount : taker_amount) * 1e-6;
        const double order_notional = side == K_SIDE_BUY
            ? base_notional + total_fee
            : base_notional - total_fee;
        if (order_notional > cfg_.max_order_usd + 1e-9 ||
            (side == K_SIDE_BUY &&
             (exposure_now_usd() + order_notional > cfg_.max_exposure_usd ||
              daily_buy_volume_.load(std::memory_order_acquire) + order_notional > cfg_.max_daily_loss_usd)))
            return TickResult::RISK_REJECTED;
        // RiskManager authorization is the ALWAYS-ON line consulted before
        // any signature is produced.  A kill latch or a cap breach denies
        // the order even when every legacy budget still looks available.
        if (risk_ && !risk_->authorize(side,
                side == K_SIDE_BUY ? order_notional : 0.0,
                side == K_SIDE_BUY ? exposure_now_usd() : 0.0,
                side == K_SIDE_BUY ? exposure_now_usd() : 0.0))
            return TickResult::RISK_REJECTED;

        WireBody body{};
        uint64_t pool_size = 0, pool_maker = 0, pool_taker = 0;
        // Dynamic ladder TTL (P3): volatile regimes tighten the freshness
        // window (BOT_POOL_VOL_TTL_MS); volatile=off keeps the static TTL.
        bool presigned = pool_.acquire_at_most(
            side, price_raw, tick, effective_shares, body,
            pool_size, pool_maker, pool_taker,
            volatility_ ? volatility_->effective_ttl_ms() : 0);
        if (presigned) {
            effective_shares = pool_size;
            maker_amount = pool_maker;
            taker_amount = pool_taker;
        } else {
            OrderV2 order{};
            fill_order(order, side, maker_amount, taker_amount);
            uint8_t signature[65];
            if (!signer_.sign_order(order, signature))
                return TickResult::SIGN_FAILED;
            const uint64_t expiration = cfg_.wire_expiration(order.timestamp_ms / 1000ULL);
            if (!build_wire_body(order, signature, cfg_.token_id_dec,
                                 cfg_.maker_hex, cfg_.signer_hex,
                                 cfg_.owner_api_key, cfg_.order_type, body,
                                 expiration))
                return TickResult::BODY_FAILED;
        }

        // With the tracker attached, reserve against the shared inventory
        // BEFORE the wire: a second order can never duplicate exposure even
        // while the venue's fill event is still in flight (P1 acceptance).
        if (tracker_) {
            if (side == K_SIDE_BUY)
                tracker_->reserve_buy(effective_shares, price_raw);
            else
                tracker_->reserve_sell(effective_shares);
        }
        const SubmitResult response = client_.submit(body);
        if (!response.ok) {
            if (tracker_) {
                if (side == K_SIDE_BUY)
                    tracker_->release_buy(effective_shares, price_raw);
                else
                    tracker_->release_sell(effective_shares);
            }
            ++submit_failed_;
            journal(JournalEvent::Type::ORDER_FAILED, 0,
                    static_cast<uint64_t>(response.http_code), price_raw,
                    effective_shares);
            return TickResult::SUBMIT_FAILED;
        }
        journal(JournalEvent::Type::ORDER_ACCEPTED, 0, side, price_raw,
                effective_shares);

        // T1.1 FIX (ID-C02): daily_buy_volume_ MUST be updated regardless of
        // tracker attachment. Previously gated by `if (!tracker_)`, when the
        // tracker was attached (always, per main_hot_path.cpp), the counter
        // never incremented, making max_daily_loss_usd a no-op.
        const double accepted_notional = order_notional;
        if (side == K_SIDE_BUY) {
            daily_buy_volume_.fetch_add(accepted_notional, std::memory_order_acq_rel);
        }

        // Legacy-mode exposure tracking (only when tracker absent)
        if (!tracker_) {
            committed_exposure_usd_ += accepted_notional;
            if (side == K_SIDE_BUY) {
                // Already counted above in daily_buy_volume_
            } else {
                confirmed_inventory_ = effective_shares >= confirmed_inventory_
                    ? 0 : confirmed_inventory_ - effective_shares;
            }
        }
        if (!response.final) {
            ++queued_;
            return TickResult::QUEUED;
        }
        ++submitted_;
        if (from_bayes && bayes_) {
            journal(JournalEvent::Type::BAYES_SIGNAL, 0,
                    static_cast<uint64_t>(bayes_->posterior() * 1000000.0),
                    price_raw, effective_shares);
            return TickResult::BAYES_SIGNAL;
        }
        return TickResult::SUBMITTED;
    }

    uint64_t submitted() const noexcept { return submitted_; }
    uint64_t queued() const noexcept { return queued_; }
    uint64_t submit_failed() const noexcept { return submit_failed_; }
    uint64_t confirmed_inventory() const noexcept {
        return tracker_ ? tracker_->net_yes() : confirmed_inventory_;
    }
    double committed_exposure_usd() const noexcept {
        return exposure_now_usd();
    }

private:
    static uint64_t realtime_ns() noexcept {
        return crowdintel::realtime_ns();
    }

    // ── Brain (P4) ──────────────────────────────────────────────────────────
    // Fold queued evidence into the closed-form posterior.  Cost when idle:
    // one atomic load; per event: one weight load + conjugate update.
    void drain_evidence() noexcept {
        if (!evidence_q_ || !bayes_ || !sources_ || !cfg_.bayes_enable)
            return;
        EvidenceEvent ev{};
        const uint32_t floor_x1e6 = static_cast<uint32_t>(
            cfg_.bayes_min_reliability * 1000000.0);
        while (evidence_q_->try_pop(ev)) {
            const uint32_t w = sources_->weight_x1e6(ev.source_id);
            if (w < floor_x1e6) {
                // Reliable-source gate: untrusted evidence never moves the
                // posterior; it lands in the journal for later calibration.
                journal(JournalEvent::Type::BAYES_LOW_RELIABILITY, 0,
                        ev.source_id, w, ev.event_hash);
                continue;
            }
            if (ev.event_hash != 0 && seen_evidence_before(ev.event_hash))
                continue;
            if (ev.kind == EvidenceEvent::Kind::COUNT)
                bayes_->update_count(ev.outcome, ev.count_n, ev.count_k, w);
            else
                bayes_->update_lr(ev.lr_x1e6, w);
            ++evidence_generation_;
            journal(JournalEvent::Type::BAYES_UPDATE, 0,
                    static_cast<uint64_t>(bayes_->posterior() * 1000000.0),
                    ev.source_id, ev.event_hash);
        }
    }

    bool seen_evidence_before(uint32_t hash) noexcept {
        for (const uint32_t seen : seen_evidence_)
            if (seen == hash) return true;
        seen_evidence_[evidence_seen_idx_++ & 31] = hash;
        return false;
    }

    // Fire exactly one signal per new batch of trusted evidence when the
    // posterior diverges from executable prices beyond the threshold.  The
    // synthesized signal traverses the SAME alpha pipeline, so Kelly sizing,
    // risk authorization, the volatility gate, and inventory caps all apply
    // with no special case — the brain can never outmaneuver the brakes.
    TickResult evaluate_posterior_trigger() noexcept {
        if (evidence_generation_ == bayes_trigger_gen_)
            return TickResult::NO_SIGNAL;  // one shot per new evidence batch
        OrderBookL2::Top top{};
        const uint64_t max_age_ns = cfg_.max_book_age_ms * 1000000ULL;
        if (!book_.read_top(top, max_age_ns) || top.bid.size == 0 ||
            top.ask.size == 0 || top.bid.price >= top.ask.price)
            return TickResult::NO_SIGNAL;
        if (!bayes_->has_prior()) {
            const double mid = static_cast<double>(
                (top.bid.price + top.ask.price) / 2) * 1e-6;
            bayes_->ensure_prior(mid, cfg_.bayes_prior_strength);
            return TickResult::NO_SIGNAL;  // seed only; trigger on next fact
        }
        const double posterior = bayes_->posterior();
        if (posterior < 0.0) return TickResult::NO_SIGNAL;
        const double ask = static_cast<double>(top.ask.price) * 1e-6;
        const double bid = static_cast<double>(top.bid.price) * 1e-6;
        uint8_t direction = 255;
        if (posterior - ask > cfg_.bayes_signal_threshold)
            direction = K_SIDE_BUY;
        else if (bid - posterior > cfg_.bayes_signal_threshold)
            direction = K_SIDE_SELL;
        if (direction == 255) return TickResult::NO_SIGNAL;
        bayes_trigger_gen_ = evidence_generation_;
        AlphaSignal syn{};
        syn.direction_hint = direction;
        syn.p_win = posterior;
        syn.confidence = 1.0;
        syn.q_value = 0.0;
        syn.timestamp_ns = realtime_ns();
        syn.market_hash = cfg_.market_hash;
        syn.signal_id = 0xB4E5000000000000ULL ^ (++bayes_synth_seq_);
        return execute_signal_pipeline(syn, true);
    }

    // Drain all pending user-channel facts into the tracker, then run the
    // protective risk evaluation.  Runs before any decision every tick so a
    // partial fill (e.g. 3,000 of 10,000) is visible to the very next order
    // evaluation; applies in well under 1 ms because each event is O(1) plus
    // a bounded 16-slot scan.
    TickResult housekeeping() noexcept {
        TickResult result = TickResult::NO_SIGNAL;
        if (restate_q_ && tracker_) {
            // Authoritative REST restatement: adopt venue truth, then let the
            // user channel continue streaming deltas on top of it.
            TrackerRestate restate{};
            while (restate_q_->try_pop(restate)) {
                tracker_->override_inventory(
                    restate.yes_shares, restate.yes_avg, restate.hedge_shares,
                    restate.hedge_avg);
                journal(JournalEvent::Type::RECONCILE_DRIFT, 0,
                        restate.yes_shares, restate.yes_avg,
                        restate.drift_exceeded);
                if (restate.drift_exceeded && risk_) {
                    risk_->latch_kill();
                    if (trading_enabled_)
                        trading_enabled_->store(false,
                            std::memory_order_release);
                }
                result = TickResult::ACCOUNT_APPLIED;
            }
        }
        if (account_q_ && tracker_) {
            AccountEvent event{};
            while (account_q_->try_pop(event)) {
                tracker_->apply(event);
                journal(event.type == AccountEvent::Type::REJECT ||
                                event.type == AccountEvent::Type::FAILED
                            ? JournalEvent::Type::ACCOUNT_REJECT
                            : JournalEvent::Type::ACCOUNT_FILL,
                        tracker_->realized_pnl(), event.price, event.size,
                        event.order_hash);
                result = TickResult::ACCOUNT_APPLIED;
            }
            const size_t released = tracker_->release_stale(
                crowdintel::mono_ns(), cfg_.reservation_ttl_ms * 1000000ULL);
            if (released) {
                journal(JournalEvent::Type::RESERVATION_STALE_RELEASE, 0,
                        static_cast<uint64_t>(released), 0, 0);
                result = TickResult::ACCOUNT_APPLIED;
            }
        }

        // ── The brakes (P2) ────────────────────────────────────────────────
        if (risk_ && tracker_) {
            if (risk_->maintain_day_anchor(tracker_->realized_pnl()))
                journal(JournalEvent::Type::DAY_RESET,
                        tracker_->realized_pnl(), 0, 0, 0);
            if (!risk_->killed()) {
                OrderBookL2::Top top{};
                const uint64_t max_age_ns =
                    cfg_.max_book_age_ms * 1000000ULL;
                if (book_.read_top(top, max_age_ns) && top.bid.size != 0 &&
                    top.ask.size != 0) {
                    uint64_t hedge_bid = 0, hedge_ask = 0;
                    if (hedge_book_) {
                        OrderBookL2::Top hedge_top{};
                        if (hedge_book_->read_top(hedge_top, max_age_ns)) {
                            hedge_bid = hedge_top.bid.price;
                            hedge_ask = hedge_top.ask.price;
                        }
                    }
                    const RiskDecision decision = risk_->evaluate(
                        top, *tracker_, hedge_bid, hedge_ask);
                    if (decision.action == RiskAction::KILL) {
                        if (trading_enabled_)
                            trading_enabled_->store(false,
                                std::memory_order_release);
                        journal(JournalEvent::Type::KILL_SWITCH,
                                decision.projected_loss, 0, 0, 0);
                        return TickResult::RISK_KILL_SWITCH;
                    }
                    if (decision.action == RiskAction::CLOSE ||
                        decision.action == RiskAction::HEDGE) {
                        const TickResult action_result =
                            execute_protective(decision);
                        if (action_result != TickResult::NO_SIGNAL)
                            return action_result;
                    }
                }
            } else if (risk_->killed() && trading_enabled_ &&
                       trading_enabled_->load(std::memory_order_acquire)) {
                trading_enabled_->store(false, std::memory_order_release);
                return TickResult::RISK_KILL_SWITCH;
            }
        }
        return result;
    }

    // Execute a protective CLOSE (stop-loss) or HEDGE order.  Protective
    // exits are always inline-signed taker orders ("FAK"): they must not
    // consume the pre-signed ladder (stale-price risk) nor wait for a cold
    // queue — the panic path is synchronous by design and fully journaled.
    TickResult execute_protective(const RiskDecision& decision) noexcept {
        const bool is_close = decision.action == RiskAction::CLOSE;
        const bool is_hedge = decision.action == RiskAction::HEDGE;
        if (!is_close && !is_hedge) return TickResult::NO_SIGNAL;

        const uint8_t side = is_hedge ? K_SIDE_BUY : K_SIDE_SELL;
        const uint64_t tick = book_.tick_size(cfg_.tick_size);
        const uint64_t price_raw = round_price_to_tick(decision.limit_price,
                                                       tick);
        uint64_t shares = decision.shares;
        if (is_close) {
            const uint64_t sellable = tracker_ ? tracker_->sellable() : 0;
            shares = shares > sellable ? sellable : shares;
        }
        if (shares < cfg_.min_size_shares) return TickResult::NO_SIGNAL;

        uint64_t maker_amount = 0, taker_amount = 0, effective_shares = 0;
        if (!compute_order_amounts(side, price_raw, shares, tick,
                                   /*market_order=*/true, maker_amount,
                                   taker_amount, effective_shares) ||
            effective_shares < cfg_.min_size_shares)
            return is_close ? TickResult::RISK_STOP_LOSS
                            : TickResult::RISK_HEDGE;

        // Hedges increase gross exposure and must pass authorization; closes
        // are reduce-only and are never blocked by caps.
        if (is_hedge) {
            const double hedge_notional =
                static_cast<double>(maker_amount) * 1e-6;
            if (risk_ && !risk_->authorize(side, hedge_notional,
                                           exposure_now_usd(),
                                           exposure_now_usd())) {
                journal(JournalEvent::Type::ORDER_FAILED, 0, 1, price_raw,
                        effective_shares);
                return TickResult::RISK_REJECTED;
            }
        }

        const uint8_t* token = is_hedge ? cfg_.hedge_token_id_be
                                        : cfg_.token_id_be;
        const char* token_dec = is_hedge ? cfg_.hedge_token_id_dec
                                         : cfg_.token_id_dec;
        OrderV2 order{};
        order.salt = rng_.next_salt();
        order.timestamp_ms = PresignedOrderPool::now_ms();
        std::memcpy(order.maker, cfg_.maker, 20);
        std::memcpy(order.signer, cfg_.signer, 20);
        std::memcpy(order.token_id, token, 32);
        order.maker_amount = maker_amount;
        order.taker_amount = taker_amount;
        order.side = side;
        order.signature_type = cfg_.signature_type;
        uint8_t signature[65];
        if (!signer_.sign_order(order, signature))
            return TickResult::SIGN_FAILED;

        WireBody body{};
        const uint64_t expiration = cfg_.wire_expiration(order.timestamp_ms / 1000ULL);
        if (!build_wire_body(order, signature, token_dec, cfg_.maker_hex,
                             cfg_.signer_hex, cfg_.owner_api_key, "FAK",
                             body, expiration))
            return TickResult::BODY_FAILED;

        if (tracker_) {
            if (side == K_SIDE_BUY)
                tracker_->reserve_buy(effective_shares, price_raw);
            else
                tracker_->reserve_sell(effective_shares);
        }
        const SubmitResult response = client_.submit(body);
        if (!response.ok) {
            if (tracker_) {
                if (side == K_SIDE_BUY)
                    tracker_->release_buy(effective_shares, price_raw);
                else
                    tracker_->release_sell(effective_shares);
            }
            journal(JournalEvent::Type::ORDER_FAILED, 0,
                    static_cast<uint64_t>(response.http_code), price_raw,
                    effective_shares);
            // A failed close is still reported as an attempted stop so the
            // next tick re-evaluates against the moved market.
        }
        journal(is_close ? JournalEvent::Type::STOP_LOSS_TRIGGERED
                         : JournalEvent::Type::HEDGE_TRIGGERED,
                decision.projected_loss, price_raw, effective_shares,
                static_cast<uint64_t>(response.ok ? 1 : 0));
        return is_close ? TickResult::RISK_STOP_LOSS
                        : TickResult::RISK_HEDGE;
    }

    void journal(JournalEvent::Type type, int64_t pnl, uint64_t aux0,
                 uint64_t aux1, uint64_t aux2) noexcept {
        if (!journal_q_) return;
        JournalEvent event{};
        event.type = type;
        event.pnl = pnl;
        event.aux0 = aux0;
        event.aux1 = aux1;
        event.aux2 = aux2;
        event.mono_ns = crowdintel::mono_ns();
        (void)journal_q_->try_push(event);  // bounded; drops are never fatal
    }

    uint64_t sellable_inventory() const noexcept {
        return tracker_ ? tracker_->sellable() : confirmed_inventory_;
    }

    double exposure_now_usd() const noexcept {
        return tracker_ ? static_cast<double>(tracker_->exposure_worst_cost()) *
                              1e-6
                        : committed_exposure_usd_;
    }

public:
    // BUG FIX (A3): daily buy-volume accumulator replaces the old
    // worst_case_loss_usd_.  Only BUY orders (which consume USD) count
    // toward this daily gross-volume cap.  The counter resets at UTC
    // midnight so the cap window aligns with the calendar day.
    double daily_buy_volume() const noexcept { 
        return daily_buy_volume_.load(std::memory_order_acquire); 
    }

    // Resets the daily buy-volume counter when the UTC calendar day rolls
    // over.  Called once per tick, before budgeting.  O(1), no allocation.
    void check_daily_reset() noexcept {
        const uint64_t day = utc_day_number();
        if (daily_buy_volume_day_.load(std::memory_order_acquire) != day) {
            daily_buy_volume_.store(0.0, std::memory_order_release);
            daily_buy_volume_day_.store(day, std::memory_order_release);
        }
    }

    // T1.3: Persist daily buy-volume state to file (atomic write: temp + rename)
    bool save_daily_volume_state(const char* path) const noexcept {
        char tmp_path[512];
        std::snprintf(tmp_path, sizeof(tmp_path), "%s.tmp", path);

        FILE* f = std::fopen(tmp_path, "wb");
        if (!f) return false;

        const double vol = daily_buy_volume_.load(std::memory_order_acquire);
        const uint64_t day = daily_buy_volume_day_.load(std::memory_order_acquire);

        bool ok = true;
        ok &= (std::fwrite(&vol, sizeof(vol), 1, f) == 1);
        ok &= (std::fwrite(&day, sizeof(day), 1, f) == 1);
        std::fclose(f);

        if (!ok) {
            std::remove(tmp_path);
            return false;
        }
        // Atomic rename (POSIX)
        return std::rename(tmp_path, path) == 0;
    }

    // T1.3: Load daily buy-volume state from file
    bool load_daily_volume_state(const char* path) noexcept {
        FILE* f = std::fopen(path, "rb");
        if (!f) return false;

        double vol = 0.0;
        uint64_t day = 0;
        bool ok = true;
        ok &= (std::fread(&vol, sizeof(vol), 1, f) == 1);
        ok &= (std::fread(&day, sizeof(day), 1, f) == 1);
        std::fclose(f);

        if (!ok) return false;

        daily_buy_volume_.store(vol, std::memory_order_release);
        daily_buy_volume_day_.store(day, std::memory_order_release);
        return true;
    }

    double net_edge(uint8_t side, double p_win, double price) const noexcept {
        const double fee = cfg_.taker_fee_rate * price * (1.0 - price);
        return side == K_SIDE_BUY ? p_win - price - fee
                                  : price - p_win - fee;
    }

    static uint64_t dedupe_hash(uint64_t value) noexcept {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    static bool bloom_contains(
            const std::array<uint64_t, 65536>& filter,
            const std::array<size_t, 3>& positions) noexcept {
        for (const size_t position : positions)
            if ((filter[position >> 6] & (1ULL << (position & 63U))) == 0)
                return false;
        return true;
    }

    bool seen_before(uint64_t id, uint64_t now_ns,
                     uint64_t ttl_ns) noexcept {
        if (id == 0 || ttl_ns == 0) return true;
        const uint64_t epoch = now_ns / ttl_ns;
        if (dedupe_epoch_ == 0) {
            dedupe_epoch_ = epoch;
        } else if (epoch > dedupe_epoch_) {
            if (epoch == dedupe_epoch_ + 1) {
                dedupe_previous_ = dedupe_current_;
            } else {
                dedupe_previous_.fill(0);
            }
            dedupe_current_.fill(0);
            dedupe_epoch_ = epoch;
        }

        static constexpr size_t MASK = (1U << 22) - 1U;
        const std::array<size_t, 3> positions = {
            static_cast<size_t>(dedupe_hash(id)) & MASK,
            static_cast<size_t>(dedupe_hash(id ^ 0xa0761d6478bd642fULL)) & MASK,
            static_cast<size_t>(dedupe_hash(id ^ 0xe7037ed1a0b428dbULL)) & MASK};
        if (bloom_contains(dedupe_current_, positions) ||
            bloom_contains(dedupe_previous_, positions))
            return true;
        for (const size_t position : positions)
            dedupe_current_[position >> 6] |= 1ULL << (position & 63U);
        return false;
    }

    void fill_order(OrderV2& order, uint8_t side,
                    uint64_t maker_amount, uint64_t taker_amount) {
        order.salt = rng_.next_salt();
        order.timestamp_ms = PresignedOrderPool::now_ms();
        std::memcpy(order.maker, cfg_.maker, 20);
        std::memcpy(order.signer, cfg_.signer, 20);
        std::memcpy(order.token_id, cfg_.token_id_be, 32);
        order.maker_amount = maker_amount;
        order.taker_amount = taker_amount;
        order.side = side;
        order.signature_type = cfg_.signature_type;
    }

    const MarketConfig& cfg_;
    OrderBookL2& book_;
    SPSC_RingBuffer<AlphaSignal>& signals_;
    const EIP712Signer& signer_;
    PresignedOrderPool& pool_;
    Client& client_;
    std::atomic<bool>* trading_enabled_;  // risk kill switch latches it
    SPSC_RingBuffer<AccountEvent>* account_q_ = nullptr;
    PositionTracker* tracker_ = nullptr;
    SPSC_RingBuffer<JournalEvent>* journal_q_ = nullptr;
    SPSC_RingBuffer<TrackerRestate, 16>* restate_q_ = nullptr;
    RiskManager* risk_ = nullptr;
    VolatilityGate* volatility_ = nullptr;
    uint32_t last_regime_ = 0;
    uint64_t shocks_seen_ = 0;
    SPSC_RingBuffer<EvidenceEvent>* evidence_q_ = nullptr;
    BayesianEngine* bayes_ = nullptr;
    SourceReliability* sources_ = nullptr;
    uint32_t seen_evidence_[32]{};
    uint32_t evidence_seen_idx_ = 0;
    uint64_t evidence_generation_ = 0;
    uint64_t bayes_trigger_gen_ = UINT64_MAX;
    uint64_t bayes_synth_seq_ = 0;
    OrderBookL2* hedge_book_ = nullptr;
    FastRandom rng_;
    // Two rotating Bloom epochs guarantee no false negatives inside the
    // configured TTL. False positives only reject work, which is fail-safe.
    std::array<uint64_t, 65536> dedupe_current_{};
    std::array<uint64_t, 65536> dedupe_previous_{};
    uint64_t dedupe_epoch_ = 0;
    uint64_t confirmed_inventory_ = 0;
    double committed_exposure_usd_ = 0.0;
    std::atomic<double> daily_buy_volume_{0.0};
    std::atomic<uint64_t> daily_buy_volume_day_{0};
    uint64_t submitted_ = 0;
    uint64_t queued_ = 0;
    uint64_t submit_failed_ = 0;
};

#endif  // EXECUTION_ENGINE_HPP
