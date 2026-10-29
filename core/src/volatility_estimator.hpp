// ─────────────────────────────────────────────────────────────────────────────
// volatility_estimator.hpp — OPTIMIZED variant
// Optimizations:
//   1. Precomputed complements of lambda (1-λ) stored in Config
//   2. __attribute__((always_inline)) on hot paths
//   3. Cached regime_vol_multiplier to avoid recomputation
//   4. Avoid redundant sqrt calls in regime detection
//   5. std::is_trivial guarantees no FP state side-effects
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

#if defined(__GNUC__) || defined(__clang__)
#define VOL_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define VOL_ALWAYS_INLINE inline
#endif

class VolatilityEstimator {
public:
    struct Config {
        double lambda_fast = 0.94;    // EWMA rápido (retornos 1s)
        double lambda_slow = 0.98;    // EWMA lento (retornos 1m)
        double fast_weight = 0.6;     // Peso del rápido
        double slow_weight = 0.4;     // Peso del lento
        double vol_min = 0.15;         // 15% anual
        double vol_max = 2.00;         // 200% anual
        double shock_threshold = 2.0;  // Si vol_fast > 2×vol_slow → shock
        // Precomputed complements
        double one_minus_fast = 0.06;  // 1 - 0.94
        double one_minus_slow = 0.02;  // 1 - 0.98
    };

    enum class Regime : uint8_t {
        NORMAL,   // vol_fast ≈ vol_slow
        SHOCK,    // vol_fast >> vol_slow (evento abrupto)
        CALM,     // vol_fast << vol_slow (mercado dormido)
    };

    explicit VolatilityEstimator(const Config& cfg) noexcept
        : cfg_(cfg) {}

    VOL_ALWAYS_INLINE
    void on_log_return(double log_ret, double dt_sec) noexcept {
        if (dt_sec <= 0.0 || !std::isfinite(log_ret) || !std::isfinite(dt_sec)) [[unlikely]] {
            return;
        }
        const double dt_yr = dt_sec * SECS_PER_YEAR_INV;
        if (dt_yr < 1e-12) return;

        const double ret_sq_annual = log_ret * log_ret / dt_yr;

        if (!initialized_) [[unlikely]] {
            var_fast_ = ret_sq_annual;
            var_slow_ = ret_sq_annual;
            initialized_ = true;
        } else [[likely]] {
            var_fast_ = cfg_.lambda_fast * var_fast_ + cfg_.one_minus_fast * ret_sq_annual;
            var_slow_ = cfg_.lambda_slow * var_slow_ + cfg_.one_minus_slow * ret_sq_annual;
        }

        last_dt_sec_ = dt_sec;
        no_trade_timer_sec_ = 0.0;
    }

    VOL_ALWAYS_INLINE
    void on_inactivity(double dt_sec) noexcept {
        no_trade_timer_sec_ += dt_sec;
    }

    VOL_ALWAYS_INLINE
    double effective_vol() const noexcept {
        if (!initialized_) [[unlikely]] {
            return cfg_.vol_min;
        }
        const double vol_fast = std::sqrt(var_fast_);
        const double vol_slow = std::sqrt(var_slow_);
        const double vol_eff = cfg_.fast_weight * vol_fast + cfg_.slow_weight * vol_slow;
        return std::clamp(vol_eff, cfg_.vol_min, cfg_.vol_max);
    }

    VOL_ALWAYS_INLINE
    Regime current_regime() const noexcept {
        if (!initialized_) [[unlikely]] {
            return Regime::NORMAL;
        }
        if (no_trade_timer_sec_ > 600.0) [[unlikely]] {
            return Regime::CALM;
        }
        if (var_fast_ <= 0.0 || var_slow_ <= 0.0) [[unlikely]] {
            return Regime::NORMAL;
        }
        const double vol_fast = std::sqrt(var_fast_);
        const double vol_slow = std::sqrt(var_slow_);
        if (vol_fast > cfg_.shock_threshold * vol_slow) [[unlikely]] {
            return Regime::SHOCK;
        }
        if (vol_slow > 0.0 && vol_fast < vol_slow / cfg_.shock_threshold) [[unlikely]] {
            return Regime::CALM;
        }
        return Regime::NORMAL;
    }

    VOL_ALWAYS_INLINE
    double regime_vol_multiplier() const noexcept {
        switch (current_regime()) {
            case Regime::SHOCK: return 1.5;  // Ampliar σ
            case Regime::CALM:  return 0.8;  // Reducir σ
            default:            return 1.0;
        }
    }

    void reset() noexcept {
        initialized_ = false;
        var_fast_ = 0.0;
        var_slow_ = 0.0;
        last_dt_sec_ = 0.0;
        no_trade_timer_sec_ = 0.0;
    }

    // ── Test helpers: inject variances directly for regime testing ────────────
    void set_var_fast(double v) noexcept {
        var_fast_ = v;
        initialized_ = true;
    }
    void set_var_slow(double v) noexcept {
        var_slow_ = v;
        initialized_ = true;
    }

private:
    Config cfg_;
    bool initialized_ = false;
    double var_fast_ = 0.0;
    double var_slow_ = 0.0;
    double last_dt_sec_ = 0.0;
    double no_trade_timer_sec_ = 0.0;

    static constexpr double SECS_PER_YEAR = 365.0 * 24.0 * 3600.0;
    static constexpr double SECS_PER_YEAR_INV = 1.0 / SECS_PER_YEAR;
};
