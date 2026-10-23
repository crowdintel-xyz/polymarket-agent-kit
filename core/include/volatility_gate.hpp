#ifndef VOLATILITY_GATE_HPP
#define VOLATILITY_GATE_HPP

// ─────────────────────────────────────────────────────────────────────────────
// VolatilityGate: the adverse-selection brake (P3).
//
// Two cooperating halves sharing one object but zero shared mutable fields:
//
//   * Regime sampler (COLD writer): the presign thread feeds book tops at its
//     native ~100 Hz loop.  The gate classifies the current regime
//     (NORMAL / ELEVATED / EXTREME) from spread width (BOT_VOL_MAX_SPREAD_BPS)
//     and mid-change rate (BOT_VOL_MAX_TICKS_PER_SEC) and publishes three
//     outputs through atomics: an effective pre-signed ladder TTL
//     (BOT_POOL_VOL_TTL_MS while hot), a size scale (BOT_VOL_SIZE_MULTIPLIER),
//     and a pause flag.  The hot path reads them for ~10 ns and never
//     contends the writer.
//
//   * Shock guard (HOT owned): a mid-price jump of BOT_VOL_MID_GAP_BPS or
//     more inside a SHOCK_WINDOW (100 ms — the acceptance window) arms a
//     cooldown during which NO passive flow fires.  This is the mechanism
//     against firing stale ladder orders on toxic flow: a 5%+ mid jump in
//     under 100 ms suppresses consumption entirely while the cold sampler
//     simultaneously shrinks the refreshed ladder.
//
//   * Slippage gate (HOT, pure): rejects orders whose target price deviates
//     from the CURRENT mid by more than BOT_POOL_MAX_DEV_BPS.  A stale pool
//     slot can only be consumed when its signed price is this close to fair.
//
// All quantities fixed-point x1e6 (prices) or basis points.  No allocation,
// no blocking, no virtuals.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <cstdint>

#include "../src/market_config.hpp"
#include "time_utils.hpp"

class VolatilityGate {
public:
    static constexpr uint64_t SHOCK_WINDOW_NS = 100000000ULL;   // 100 ms
    static constexpr uint64_t SHOCK_COOLDOWN_NS = 250000000ULL;  // 250 ms

    explicit VolatilityGate(const MarketConfig& cfg)
        : pool_vol_ttl_ms_(cfg.pool_vol_ttl_ms),
          max_dev_bps_(cfg.pool_max_dev_bps),
          spread_wide_bps_(cfg.vol_max_spread_bps),
          tick_high_hz_(cfg.vol_max_ticks_per_sec),
          mid_gap_bps_(cfg.vol_mid_gap_bps),
          size_mult_permille_(
              static_cast<uint32_t>(cfg.vol_size_multiplier * 1000.0)) {
        publish(0, cfg.presign_ttl_ms, 1000, false);
    }

    // ── Cold sampler (presign thread only) ──────────────────────────────────
    void sample(uint64_t bid, uint64_t ask, uint64_t mono_ns,
                uint64_t base_ttl_ms) noexcept {
        if (bid == 0 || ask == 0 || bid >= ask) return;
        const uint64_t mid = (bid + ask) / 2;
        if (mid != last_mid_) {
            ++mid_changes_;
            last_mid_ = mid;
        }
        // 1-second window rate estimate.
        if (mono_ns - window_start_ns_ >= 1000000000ULL) {
            rate_hz_ = mid_changes_;
            mid_changes_ = 0;
            window_start_ns_ = mono_ns;
        }
        const uint64_t spread_bps =
            (ask - bid) * 10000ULL / (mid ? mid : 1);
        const bool wide = spread_wide_bps_ > 0.0 &&
                          static_cast<double>(spread_bps) >= spread_wide_bps_;
        const bool fast = tick_high_hz_ != 0 && rate_hz_ >= tick_high_hz_;
        uint32_t regime = 0;
        if (wide && fast && tick_high_hz_ != 0 &&
            rate_hz_ >= 2 * tick_high_hz_)
            regime = 2;  // both, plus extreme rate: pause passive flow
        else if (wide || fast)
            regime = 1;
        if (regime != regime_mode_) {
            regime_mode_ = regime;
            uint64_t ttl = base_ttl_ms;
            uint32_t permille = 1000;
            bool pause = false;
            if (regime == 1) {
                ttl = pool_vol_ttl_ms_ < base_ttl_ms ? pool_vol_ttl_ms_
                                                     : base_ttl_ms;
                permille = size_mult_permille_ == 0 ? 100 : size_mult_permille_;
            } else if (regime == 2) {
                ttl = pool_vol_ttl_ms_ / 2 > 100 ? pool_vol_ttl_ms_ / 2 : 100;
                permille = size_mult_permille_ / 2 < 100
                    ? 100 : size_mult_permille_ / 2;
                pause = true;
            }
            publish(regime, ttl, permille, pause);
        }
    }

    // ── Hot readers (atomic, ~10 ns) ────────────────────────────────────────
    uint64_t effective_ttl_ms() const noexcept {
        return ttl_ms_.load(std::memory_order_acquire);
    }
    uint32_t size_permille() const noexcept {
        return permille_.load(std::memory_order_acquire);
    }
    bool paused() const noexcept {
        return pause_.load(std::memory_order_acquire);
    }
    uint32_t regime() const noexcept {
        return regime_pub_.load(std::memory_order_acquire);
    }

    // ── Hot shock guard (hot loop only: single writer of these fields) ──────
    // Returns true while passive flow is suppressed (cooldown armed).
    bool observe_mid(uint64_t mid, uint64_t mono_ns) noexcept {
        if (mid != 0 && prev_mid_ != 0 && prev_ns_ != 0) {
            const uint64_t dt = mono_ns - prev_ns_;
            const uint64_t diff = mid > prev_mid_ ? mid - prev_mid_
                                                  : prev_mid_ - mid;
            const uint64_t gap_bps = diff * 10000ULL / prev_mid_;
            if (dt <= SHOCK_WINDOW_NS &&
                static_cast<double>(gap_bps) >= mid_gap_bps_) {
                if (mono_ns >= shock_until_ns_)
                    shocks_.fetch_add(1, std::memory_order_relaxed);
                shock_until_ns_ = mono_ns + SHOCK_COOLDOWN_NS;
            }
        }
        prev_mid_ = mid;
        prev_ns_ = mono_ns;
        return mono_ns < shock_until_ns_;
    }

    // Pure slippage check against the CURRENT mid price.  A taker order
    // priced further than max_dev_bps from mid is toxic flow, not an edge.
    // max_dev_bps <= 0 disables the check (per configuration contract).
    bool slippage_ok(uint8_t side, uint64_t price, uint64_t mid) const
        noexcept {
        if (max_dev_bps_ <= 0.0 || mid == 0 || price == 0) return true;
        const uint64_t slip = side == 0
            ? (price > mid ? price - mid : 0)
            : (mid > price ? mid - price : 0);
        const uint64_t dev_bps = slip * 10000ULL / mid;
        return static_cast<double>(dev_bps) <= max_dev_bps_;
    }
    double max_dev_bps() const noexcept { return max_dev_bps_; }

    void note_stale_abort() noexcept {
        aborts_.fetch_add(1, std::memory_order_relaxed);
    }

    uint64_t shocks() const noexcept {
        return shocks_.load(std::memory_order_relaxed);
    }
    uint64_t aborts() const noexcept {
        return aborts_.load(std::memory_order_relaxed);
    }
    uint32_t rate_hz() const noexcept { return rate_hz_; }

private:
    void publish(uint32_t regime, uint64_t ttl, uint32_t permille,
                 bool pause) noexcept {
        regime_pub_.store(regime, std::memory_order_release);
        ttl_ms_.store(ttl, std::memory_order_release);
        permille_.store(permille, std::memory_order_release);
        pause_.store(pause, std::memory_order_release);
    }

    // Static configuration (constructor-owned, immutable afterwards).
    const uint64_t pool_vol_ttl_ms_;
    const double max_dev_bps_;
    const double spread_wide_bps_;
    const uint64_t tick_high_hz_;
    const double mid_gap_bps_;
    const uint32_t size_mult_permille_;

    // Cold-sampler state (owned by the presign thread).
    uint64_t last_mid_ = 0;
    uint64_t mid_changes_ = 0;
    uint64_t window_start_ns_ = 0;
    uint32_t rate_hz_ = 0;
    uint32_t regime_mode_ = 0;

    // Hot shock-guard state (owned by the hot loop).
    uint64_t prev_mid_ = 0;
    uint64_t prev_ns_ = 0;
    uint64_t shock_until_ns_ = 0;

    // Published outputs (cold writer -> hot readers).
    alignas(64) std::atomic<uint32_t> regime_pub_{0};
    std::atomic<uint64_t> ttl_ms_{0};
    std::atomic<uint32_t> permille_{1000};
    alignas(64) std::atomic<bool> pause_{false};
    alignas(64) std::atomic<uint64_t> shocks_{0};
    std::atomic<uint64_t> aborts_{0};
};

#endif  // VOLATILITY_GATE_HPP
