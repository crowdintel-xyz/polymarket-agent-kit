// ─────────────────────────────────────────────────────────────────────────────
// ofi_calculator.hpp — Order Flow Imbalance (Cont et al., 2014)
//
// Incremental O(1) calculator. Receives raw book changes from BinanceWSClient
// and emits MarketState structs (the CfC input vector).
//
// INVARIANTS:
//   - O(1) per market event (EWMA decay, no buffer scan)
//   - Zero heap allocation (stack + fixed ring buffer)
//   - Thread-safe: cold-path producer (Binance thread) → hot-path consumer
//     via SPSC ring (drop-oldest on overflow)
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>

// ── MarketState: the 6-9 feature vector consumed by CfC ──────────────────────
//
// Packed into two cache lines; the hot path drains one at a time from the SPSC
// ring.  All members are trivially copyable so ring transport is a single
// memcpy.  alignas(64) ensures the hot-path consumer lands on a clean
// cache-line boundary when draining the next snapshot.
struct alignas(64) MarketState {
    double ofi_normalized;      // OFI / (B_vol + A_vol), range [-1, 1]
    double trade_intensity;     // EWMA trades/sec
    double spread_bps;          // (ask - bid) / mid * 10000
    double depth_imbalance;     // (B_vol - A_vol) / (B_vol + A_vol)
    double microprice;          // (B_vol*ask + A_vol*bid) / (B_vol + A_vol)
    double mid_velocity;        // Δmid / Δt in bps/sec

    uint64_t timestamp_ns;     // CLOCK_MONOTONIC_RAW
    double delta_t_sec;        // time since last event

    // Phase-2 additions (optional, zero-initialized for backward compat):
    double realized_vol_1h = 0.0;
    double funding_rate = 0.0;
    double volume_zscore = 0.0;
};
static_assert(sizeof(MarketState) <= 128, "Cache-line aligned, no padding bloat");

// ── Configuration ────────────────────────────────────────────────────────────
struct OFIConfig {
    static constexpr double DECAY_LAMBDA = 0.95;       // EWMA decay
    static constexpr uint32_t HISTORY_DEPTH = 64;       // ring buffer for spikes
    static constexpr double MIN_PRICE_TICK = 0.01;      // USD
    static constexpr double DEPTH_SCALE = 100.0;        // normalize to basis
};

// ── OFI Calculator ───────────────────────────────────────────────────────────
// Cold-path only.  Processes Binance L2 depth updates + trade events on the cold
// thread and produces MarketState snapshots into a ring buffer for hot-path
// drain.  All arithmetic is O(1) per event.
//
// Event semantics:
//   Depth update (is_trade=false):
//     bid_vol/ask_vol are the NEW best-level volumes.  The OFI contribution
//     is event_delta = Δbid_vol − Δask_vol (order-flow imbalance).  A
//     buyer-initiated cancel on the ask shrinks ask_vol → positive delta.
//   Trade (is_trade=true):
//     bid_vol/ask_vol are the post-trade best-level volumes (the trade has
//     already consumed liquidity).  Same delta formula naturally yields
//     +size for buyer-initiated (ask consumed) and −size for
//     seller-initiated (bid consumed).
class OFICalculator {
public:
    explicit OFICalculator(const OFIConfig& cfg) : cfg_(cfg) {}

    // Process one market event (called from Binance thread).
    // bid_vol, ask_vol: best-level volumes in USD (quote currency).
    // bid_px, ask_px: best prices in USD (e.g., 26500.00).
    // is_trade: true if this is a trade event (drives trade_intensity EWMA).
    // now_ns: CLOCK_MONOTONIC_RAW timestamp.
    void on_event(double bid_px, double ask_px,
                  double bid_vol, double ask_vol,
                  bool is_trade, uint64_t now_ns) noexcept {
        constexpr double kHalf = 0.5;

        // --- Time delta ---------------------------------------------------------
        double dt_sec = 0.0;
        if (last_event_ns_ != 0) {
            dt_sec = static_cast<double>(now_ns - last_event_ns_) * 1e-9;
            if (dt_sec < 0.0) dt_sec = 0.0;  // clock-skew guard
        }
        last_event_ns_ = now_ns;
        delta_t_sec_ = dt_sec;

        // --- Price levels -------------------------------------------------------
        const double mid = (bid_px + ask_px) * kHalf;
        const double spread = ask_px - bid_px;

        // --- OFI event delta (Cont et al. 2014) ---------------------------------
        // event_delta = Δbid_vol − Δask_vol
        //
        // The first event after construction or reset establishes the baseline
        // (no prior depth to compare against), so its delta is zero.  This is the
        // correct Cont et al. behaviour: OFI measures *changes* in flow.
        double event_delta = 0.0;
        if (!has_baseline_) {
            has_baseline_ = true;
            prev_bid_vol_ = bid_vol;
            prev_ask_vol_ = ask_vol;
        } else {
            const double bid_delta = bid_vol - prev_bid_vol_;
            const double ask_delta = ask_vol - prev_ask_vol_;
            event_delta = bid_delta - ask_delta;
            prev_bid_vol_ = bid_vol;
            prev_ask_vol_ = ask_vol;
        }

        // --- EWMA accumulation (leaky integrator, O(1)) ------------------------
        ofi_running_ = cfg_.DECAY_LAMBDA * ofi_running_ + event_delta;
        ++event_count_;

        // --- Trade intensity (EWMA of trade rate, trades/sec) ------------------
        if (is_trade) {
            ++trade_event_count_;
            if (dt_sec > 0.0) {
                const double inst_rate = 1.0 / dt_sec;
                if (trade_intensity_ewma_ == 0.0)
                    trade_intensity_ewma_ = inst_rate;
                else
                    trade_intensity_ewma_ = cfg_.DECAY_LAMBDA *
                        trade_intensity_ewma_ +
                        (1.0 - cfg_.DECAY_LAMBDA) * inst_rate;
            }
        }

        // --- Derived features ---------------------------------------------------
        const double vol_sum = bid_vol + ask_vol;

        // Depth imbalance
        depth_imbalance_ = vol_sum > 0.0
            ? (bid_vol - ask_vol) / vol_sum
            : 0.0;

        // Microprice: liquidity-weighted midpoint
        microprice_ = vol_sum > 0.0
            ? (bid_vol * ask_px + ask_vol * bid_px) / vol_sum
            : mid;

        // Spread in basis points
        spread_bps_ = mid > 0.0
            ? (spread / mid) * 10000.0
            : 0.0;

        // Mid velocity: price change rate in bps/sec
        const double prev_mid = prev_mid_;
        if (prev_mid > 0.0 && dt_sec > 0.0) {
            const double mid_change_bps =
                ((mid - prev_mid) / prev_mid) * 10000.0;
            mid_velocity_ = mid_change_bps / dt_sec;
        } else {
            mid_velocity_ = 0.0;
        }
        prev_mid_ = mid;

        // Normalized OFI: divide by total depth to bound [-1, 1].
        // Guard against zero-volume (empty book edge) to avoid NaN.
        ofi_normalized_ = vol_sum > 0.0 ? ofi_running_ / vol_sum : 0.0;
        ofi_normalized_ = std::clamp(ofi_normalized_, -1.0, 1.0);

        prev_bid_px_ = bid_px;
        prev_ask_px_ = ask_px;

        // --- Price history (for spike / Hurst detection) -----------------------
        price_history_[price_head_ & (OFIConfig::HISTORY_DEPTH - 1)] = mid;
        price_head_ = (price_head_ + 1) & (OFIConfig::HISTORY_DEPTH - 1);
    }

    // Copy latest computed MarketState out.  Returns false if no event has
    // been processed yet (hot-path drain skips stale/zero states).
    bool current_state(MarketState& out) const noexcept {
        if (event_count_ == 0) return false;
        out.ofi_normalized = ofi_normalized_;
        out.trade_intensity = trade_intensity_ewma_;
        out.spread_bps = spread_bps_;
        out.depth_imbalance = depth_imbalance_;
        out.microprice = microprice_;
        out.mid_velocity = mid_velocity_;
        out.timestamp_ns = last_event_ns_;
        out.delta_t_sec = delta_t_sec_;
        out.realized_vol_1h = 0.0;  // reserved for P2
        out.funding_rate = 0.0;     // reserved for P2
        out.volume_zscore = 0.0;    // reserved for P2
        return true;
    }

    // Reset accumulators (called at window start by WindowShield).
    void reset() noexcept {
        ofi_running_ = 0.0;
        trade_intensity_ewma_ = 0.0;
        prev_bid_px_ = 0.0;
        prev_ask_px_ = 0.0;
        prev_mid_ = 0.0;
        prev_bid_vol_ = 0.0;
        prev_ask_vol_ = 0.0;
        depth_imbalance_ = 0.0;
        microprice_ = 0.0;
        spread_bps_ = 0.0;
        mid_velocity_ = 0.0;
        ofi_normalized_ = 0.0;
        delta_t_sec_ = 0.0;
        last_event_ns_ = 0;
        event_count_ = 0;
        trade_event_count_ = 0;
        has_baseline_ = false;
        price_head_ = 0;
        price_history_.fill(0.0);
    }

    // Accessors for diagnostics / cold-path inspection.
    uint64_t event_count() const noexcept { return event_count_; }
    uint64_t trade_event_count() const noexcept { return trade_event_count_; }
    double ofi_running() const noexcept { return ofi_running_; }

private:
    OFIConfig cfg_;

    // EWMA accumulators
    double ofi_running_ = 0.0;          // raw OFI (pre-normalization)
    double trade_intensity_ewma_ = 0.0;  // trades/sec EWMA

    // Price/volume state
    bool has_baseline_ = false;
    double prev_bid_px_ = 0.0;
    double prev_ask_px_ = 0.0;
    double prev_mid_ = 0.0;
    double prev_bid_vol_ = 0.0;
    double prev_ask_vol_ = 0.0;

    // Cached computed features (copied by current_state())
    double depth_imbalance_ = 0.0;
    double microprice_ = 0.0;
    double spread_bps_ = 0.0;
    double mid_velocity_ = 0.0;
    double ofi_normalized_ = 0.0;
    double delta_t_sec_ = 0.0;

    // Event tracking
    uint64_t last_event_ns_ = 0;
    uint64_t event_count_ = 0;
    uint64_t trade_event_count_ = 0;

    // Fixed ring for spike detection / Hurst exponent estimation.
    // Uses bitmask indexing (power-of-2 size) for branchless wrap.
    std::array<double, OFIConfig::HISTORY_DEPTH> price_history_{};
    uint32_t price_head_ = 0;
};
