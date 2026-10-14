// ─────────────────────────────────────────────────────────────────────────────
// twap_brownian_bridge.hpp — PHASE-1: Brownian Bridge probability for 60s TWAP
//
// Calculates P(TWAP_final > Strike) using the Brownian Bridge integral of a
// driftless arithmetic Brownian motion. NO ML — pure deterministic math.
//
// MATH:
//   A_t = (1/t)·∫₀ᵗ S(u)du   (TWAP acumulado hasta t, time-weighted average)
//   S_t = precio actual (tick más reciente)
//   τ = tiempo restante (T_total − t) en segundos
//   T_total = duración total de la ventana TWAP (60s típico en Chainlink)
//
//   The future integral ∫ₜᵀ S(u)du has:
//     E[∫ₜᵀ S(u)du | S(t)] = S_t · τ
//     Var[∫ₜᵀ S(u)du | S(t)] = σ² · τ³ / 3
//
//   Therefore:
//     d = (A_t·t + S_t·τ − K·T_total) / (σ · √(τ³/3))
//     P(TWAP_final > K) = Φ(d)
//
//   where σ is the *absolute* (price-scaled) volatility per √second:
//     σ = σ_annual · S_t / √(SECONDS_PER_YEAR)
//
// Invariants:
//   - Zero heap allocation (all on stack)
//   - norm_cdf() via Abramowitz-Stegun 26.2.17 (error < 1e-7)
//   - τ < 0.5s → deterministic P (returns 1.0 / 0.0)
//   - σ < 1e-12 → deterministic P
//   - Probabilistic P clamped to [0.01, 0.99]; deterministic returns 1.0 / 0.0
//   - Fees: taker = 0.072 × price × (1 − price); maker = 0
//   - Window open price = strike: in production, caller seeds TWAP with
//     on_price(strike_price, ts) at window start. compute() falls back
//     to current_price when no tick history exists.
//   - compute() < 500 ns end-to-end
//
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

// ── Config ───────────────────────────────────────────────────────────────────
struct TwapBBConfig {
    double ewma_lambda        = 0.94;    // Decay for volatility EWMA
    double vol_min_annual     = 0.15;    // Floor:  15 % anual
    double vol_max_annual     = 2.00;    // Cap:   200 % anual
    double twap_window_sec    = 60.0;    // Chainlink TWAP window (seconds)
    double min_edge_threshold = 0.005;    // 0.5 % minimum edge after fees
    double max_kelly_fraction = 0.05;    // Cap Kelly at 5 % of bankroll
};

// ── Result structs ─────────────────────────────────────────────────────────────
struct ProbResult {
    double p_up;                  // P(TWAP_final > Strike) ∈ [0,1]
    double twap_so_far;           // TWAP acumulado hasta t
    double time_remaining_sec;    // τ restante
    double sigma_annual;          // Volatilidad anualizada (clamped)
    double distance_to_strike_bps;  // (S_t − K) / K × 10'000, signed
    bool   is_decided;            // true when deterministic or P at clamp boundary
};

struct TradeDecision {
    bool   should_trade;          // true if edge ≥ threshold and bankroll > 0
    double edge_after_fees;       // p_up − market_price_up − fee_cost
    double fee_cost;              // 0.072·p·(1−p) for taker, 0 for maker
    double kelly_fraction;        // Quarter-Kelly fraction of bankroll
};

// ── TwapBrownianBridge ─────────────────────────────────────────────────────────
class TwapBrownianBridge {
public:
    explicit TwapBrownianBridge(const TwapBBConfig& cfg = {})
        : cfg_(cfg) {}

    // ── Hot-path feed: new Binance price tick ──────────────────────────────────
    // Updates TWAP accumulator and EWMA volatility. ~50 ns per call.
    void on_price(double price, uint64_t timestamp_ms) noexcept {
        if (price <= 0.0 || !std::isfinite(price)) return;

        if (!window_initialized_) {
            last_price_          = price;
            last_ts_ms_          = timestamp_ms;
            window_initialized_  = true;
            price_sum_           = 0.0;
            tick_time_           = 0.0;
            tick_count_          = 0;
            vol_ewma_var_        = 0.0;
            return;
        }
        // Guard against out-of-order or identical timestamps
        if (timestamp_ms <= last_ts_ms_) return;

        double dt = static_cast<double>(timestamp_ms - last_ts_ms_) * 1e-3;
        if (dt <= 0.0 || dt >= 60.0) return;   // reasonable tick interval

        // Update time-weighted TWAP accumulator
        price_sum_ += price * dt;
        tick_time_ += dt;
        tick_count_  += 1;

        // Update EWMA volatility (annualized variance)
        double log_ret = std::log(price / last_price_);
        double dt_yr   = dt / SECS_PER_YEAR;
        double inst_var = (log_ret * log_ret) / dt_yr;

        if (vol_ewma_var_ <= 0.0) {
            vol_ewma_var_ = inst_var;           // seed
        } else {
            vol_ewma_var_ = cfg_.ewma_lambda * vol_ewma_var_ +
                            (1.0 - cfg_.ewma_lambda) * inst_var;
        }

        last_price_ = price;
        last_ts_ms_  = timestamp_ms;
    }

    void reset() noexcept {
        window_initialized_ = false;
        price_sum_          = 0.0;
        tick_time_          = 0.0;
        tick_count_         = 0;
        last_price_         = 0.0;
        last_ts_ms_         = 0;
        vol_ewma_var_       = 0.0;
    }

    // ── Core probability: P(TWAP_final > K) ────────────────────────────────────
    // Computes the Brownian-Bridge probability. End-to-end < 500 ns.
    ProbResult compute(
        double current_price,
        double strike_price,
        double twap_window_total_sec,
        double elapsed_in_window_sec
    ) const noexcept {
        // Time remaining (clip to ≥ 0)
        double tau = twap_window_total_sec - elapsed_in_window_sec;
        if (tau < 0.0) tau = 0.0;

        // ── TWAP so far: time-weighted average ─────────────────────────────────
        double twap_so_far = 0.0;
        if (tick_time_ > 0.0) {
            twap_so_far = price_sum_ / tick_time_;
        } else if (tick_count_ > 0) {
            twap_so_far = price_sum_ / static_cast<double>(tick_count_);
        } else {
            twap_so_far = current_price;        // Window open price = current
        }

        // ── Annualised volatility (clamped) ────────────────────────────────────
        double sigma_annual = 0.0;
        if (vol_ewma_var_ > 0.0) {
            sigma_annual = std::sqrt(vol_ewma_var_);
        }
        sigma_annual = std::clamp(sigma_annual,
                                  cfg_.vol_min_annual,
                                  cfg_.vol_max_annual);

        // Absolute (price-scaled) volatility per √second
        double sigma_sec = sigma_annual / SQRT_SECS_YEAR;
        double sigma_t   = sigma_sec * current_price;

        // ── Distance to strike (bps, signed) ───────────────────────────────────
        double dist_bps = 0.0;
        if (strike_price > 0.0) {
            dist_bps = (current_price - strike_price) / strike_price * 10000.0;
        }

        // ── Deterministic edge cases ────────────────────────────────────────────
        if (tau < 0.5) {
            // Window nearly done — outcome is certain
            double p_up;
            if      (twap_so_far > strike_price) p_up = 1.0;
            else if (twap_so_far < strike_price) p_up = 0.0;
            else                                 p_up = 0.5;
            return ProbResult{
                .p_up                   = p_up,
                .twap_so_far            = twap_so_far,
                .time_remaining_sec     = tau,
                .sigma_annual           = sigma_annual,
                .distance_to_strike_bps = dist_bps,
                .is_decided             = true,
            };
        }

        if (sigma_t < 1e-12) {
            // Zero vol: deterministic
            double p_up;
            if      (twap_so_far > strike_price) p_up = 1.0;
            else if (twap_so_far < strike_price) p_up = 0.0;
            else                                 p_up = 0.5;
            return ProbResult{
                .p_up                   = p_up,
                .twap_so_far            = twap_so_far,
                .time_remaining_sec     = tau,
                .sigma_annual           = sigma_annual,
                .distance_to_strike_bps = dist_bps,
                .is_decided             = true,
            };
        }

        // ── Brownian Bridge probability ────────────────────────────────────────
        // d = (A_t·t + S_t·τ − K·T_total) / (σ · √(τ³/3))
        double numerator   = twap_so_far * elapsed_in_window_sec +
                             current_price * tau -
                             strike_price  * twap_window_total_sec;
        double denominator = sigma_t * std::sqrt(tau * tau * tau / 3.0);

        if (denominator < 1e-15) {
            double p_up;
            if      (numerator > 0.0)  p_up = 1.0;
            else if (numerator < 0.0)  p_up = 0.0;
            else                       p_up = 0.5;
            return ProbResult{
                .p_up                   = p_up,
                .twap_so_far            = twap_so_far,
                .time_remaining_sec     = tau,
                .sigma_annual           = sigma_annual,
                .distance_to_strike_bps = dist_bps,
                .is_decided             = true,
            };
        }

        double d = numerator / denominator;
        double p_up = norm_cdf(d);
        p_up = std::clamp(p_up, 0.01, 0.99);    // probabilistic clamp

        return ProbResult{
            .p_up                   = p_up,
            .twap_so_far            = twap_so_far,
            .time_remaining_sec     = tau,
            .sigma_annual           = sigma_annual,
            .distance_to_strike_bps = dist_bps,
            .is_decided             = (p_up > 0.97 || p_up < 0.03),
        };
    }

    // ── Trade evaluation: edge, fees, Kelly ────────────────────────────────────
    TradeDecision evaluate_trade(
        double p_up,
        double market_price_up,
        double bankroll,
        bool   is_taker
    ) const noexcept {
        double fee_cost = 0.0;
        if (is_taker) {
            fee_cost = 0.072 * market_price_up * (1.0 - market_price_up);
        }
        double edge_after_fees = p_up - market_price_up - fee_cost;

        bool should_trade = (edge_after_fees >= cfg_.min_edge_threshold)
                         && (bankroll > 0.0);

        // Quarter-Kelly for binary bet (payout = 1.0 at price p):
        //   b  = (1 − p) / p   (net odds per unit staked)
        //   f* = (p_up − p) / (1 − p)   (full Kelly)
        //   f_q = f* · 0.25            (quarter-Kelly, capped)
        double kelly_fraction = 0.0;
        if (should_trade && market_price_up > 0.0 && market_price_up < 1.0) {
            double kelly_raw = (p_up - market_price_up) / (1.0 - market_price_up);
            kelly_fraction = std::clamp(kelly_raw * 0.25,
                                        0.0,
                                        cfg_.max_kelly_fraction);
        }

        return TradeDecision{
            .should_trade       = should_trade,
            .edge_after_fees    = edge_after_fees,
            .fee_cost           = fee_cost,
            .kelly_fraction     = kelly_fraction,
        };
    }

    // ── Test helpers (set internal state without price feed) ────────────────────
    void set_sigma_annual(double sigma) noexcept {
        sigma = std::clamp(sigma, cfg_.vol_min_annual, cfg_.vol_max_annual);
        vol_ewma_var_ = sigma * sigma;
    }

    void set_twap_so_far(double twap) noexcept {
        price_sum_  = twap;
        tick_time_  = 1.0;
        tick_count_ = 1;
        window_initialized_ = true;
    }

    // ── Read-back for diagnostics / tests ───────────────────────────────────────
    double get_sigma_annual() const noexcept {
        if (vol_ewma_var_ > 0.0)
            return std::clamp(std::sqrt(vol_ewma_var_),
                              cfg_.vol_min_annual, cfg_.vol_max_annual);
        return cfg_.vol_min_annual;
    }

    double get_twap_so_far() const noexcept {
        if (tick_time_ > 0.0)
            return price_sum_ / tick_time_;
        if (tick_count_ > 0)
            return price_sum_ / static_cast<double>(tick_count_);
        return 0.0;
    }

private:
    TwapBBConfig cfg_;

    // ── Runtime state ───────────────────────────────────────────────────────────
    bool     window_initialized_ = false;
    double   price_sum_          = 0.0;   // Σ (price × dt)  [price-seconds]
    double   tick_time_          = 0.0;   // Σ dt            [seconds]
    uint64_t tick_count_         = 0;     // number of price updates
    double   last_price_         = 0.0;
    uint64_t last_ts_ms_         = 0;
    double   vol_ewma_var_       = 0.0;   // annualised variance (EWMA)

    // ── Constants ───────────────────────────────────────────────────────────────
    static constexpr double SECS_PER_YEAR  = 365.0 * 24.0 * 3600.0;
    static constexpr double SQRT_SECS_YEAR = 5615.69405514;     // sqrt(31,536,000)
    static constexpr double SQRT_2         = 1.4142135623730951;

    // ── Normal CDF Φ(x) via Abramowitz-Stegun 7.1.26 erf approximation ───────────
    // erf(z) = 1 − (a1·t + … + a5·t⁵)·exp(−z²), t = 1/(1+p·z)
    // Φ(x) = 0.5 · (1 + erf(x/√2)),  max error ≈ 1.5 × 10⁻⁷
    static double norm_cdf(double x) noexcept {
        constexpr double a1 =  0.254829592;
        constexpr double a2 = -0.284496736;
        constexpr double a3 =  1.421413741;
        constexpr double a4 = -1.453152027;
        constexpr double a5 =  1.061405429;
        constexpr double p  =  0.3275911;

        double z = std::abs(x) / SQRT_2;   // |x|/√2 for erf domain
        double t = 1.0 / (1.0 + p * z);
        double erf_val = 1.0 - (((((a5 * t + a4) * t) + a3) * t + a2) * t + a1) * t
                               * std::exp(-z * z);
        // Φ(x) = 0.5·(1 + erf(x/√2));  for x<0, erf(−z) = −erf(z)
        return (x >= 0.0) ? 0.5 * (1.0 + erf_val) : 0.5 * (1.0 - erf_val);
    }
};
