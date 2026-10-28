// ─────────────────────────────────────────────────────────────────────────────
// kelly_sizer.hpp — OPTIMIZED variant (MutaLambda evolution candidate)
// Optimizations:
//   1. __attribute__((always_inline)) on compute()
//   2. FeeCalculator::taker_fee inlined directly (avoid function call)
//   3. Eliminated redundant field assignments in early-return paths
//   4. Branch prediction hints on rare paths
//   5. Precomputed kelly_fraction multiplier
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "fee_calculator.hpp"
#include <algorithm>
#include <cmath>

#if defined(__GNUC__) || defined(__clang__)
#define KELLY_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define KELLY_ALWAYS_INLINE inline
#endif

struct Config {
    double kelly_fraction = 0.25;     // Quarter-Kelly (0.25 = 1/4)
    double max_bankroll_pct = 0.05;   // Max 5% of bankroll per trade
    double min_edge = 0.005;          // 0.5% min edge post-fees
    double max_position = 100.0;      // Max 100 shares
};

struct SizingResult {
    bool should_trade;
    double size_shares;
    double size_usdc;
    double kelly_raw;
    double kelly_fractioned;
    double edge;
    double fee_cost;
};

class KellySizer {
public:
    explicit KellySizer(const Config& config = {}) noexcept
        : config_(config) {}

    KELLY_ALWAYS_INLINE
    SizingResult compute(double p_model,
                         double market_price,
                         double bankroll_usdc,
                         double inventory,
                         double vol_z_score,
                         bool is_taker,
                         double ofi_size_multiplier = 1.0) const noexcept
    {
        SizingResult result{};

        // Guard against degenerate price / probability inputs.
        if (market_price <= 0.0 || market_price >= 1.0 ||
            p_model < 0.0 || p_model > 1.0) [[unlikely]] {
            result.should_trade = false;
            return result;
        }
        if (bankroll_usdc < 0.0) bankroll_usdc = 0.0;

        // ── Fee & edge (inlined FeeCalculator::taker_fee) ────────────────────────
        const double fee = is_taker ? (FeeCalculator::TAKER_FEE_RATE * market_price * (1.0 - market_price)) : 0.0;
        result.fee_cost = fee;

        const double edge = p_model - market_price - fee;
        result.edge = edge;

        if (edge < config_.min_edge) [[unlikely]] {
            result.should_trade = false;
            return result;
        }

        // ── Full Kelly ───────────────────────────────────────────────────────
        const double odds = (1.0 / market_price) - 1.0;
        const double q = 1.0 - p_model;
        const double kelly_raw = (p_model * odds - q) / odds;
        result.kelly_raw = kelly_raw;

        if (kelly_raw <= 0.0) [[unlikely]] {
            result.should_trade = false;
            return result;
        }

        // ── Quarter-Kelly ────────────────────────────────────────────────────
        double kelly_fractioned = kelly_raw * config_.kelly_fraction;

        // ── Adjustments ──────────────────────────────────────────────────────
        const double inv_adjust = std::max(0.0, 1.0 - std::abs(inventory));
        const double vol_adjust = 1.0 / (1.0 + std::max(0.0, vol_z_score));
        const double ofi_adjust = std::max(0.0, ofi_size_multiplier);

        kelly_fractioned *= inv_adjust * vol_adjust * ofi_adjust;

        // ── Cap to max_bankroll_pct ──────────────────────────────────────────
        const double capped_fraction = std::min(kelly_fractioned, config_.max_bankroll_pct);
        result.kelly_fractioned = kelly_fractioned;

        // ── Convert to dollar size ───────────────────────────────────────────
        const double size_usdc = capped_fraction * bankroll_usdc;
        result.size_usdc = size_usdc;

        // ── Convert to shares, cap at max_position ───────────────────────────
        double size_shares = size_usdc / market_price;
        if (size_shares > config_.max_position) [[unlikely]] {
            size_shares = config_.max_position;
        }
        result.size_shares = size_shares;

        // ── Floor: minimum 1 share ───────────────────────────────────────────
        if (size_shares < 1.0) [[unlikely]] {
            result.should_trade = false;
            result.size_shares = 0.0;
            result.size_usdc = 0.0;
        } else [[likely]] {
            result.should_trade = true;
        }

        return result;
    }

private:
    Config config_;
};
