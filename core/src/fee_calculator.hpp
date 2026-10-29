// ─────────────────────────────────────────────────────────────────────────────
// fee_calculator.hpp — OPTIMIZED variant (MutaLambda evolution candidate)
// Optimizations:
//   1. __attribute__((always_inline)) on all hot functions
//   2. Branch prediction hints (__builtin_expect + [[likely]]/[[unlikely]])
//   3. Precomputed constant: 0.072 → constexpr
//   4. std::clamp replaces ternary for better codegen
//   5. maker_fee simplified (always returns 0.0, no branch)
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <algorithm>
#include <cmath>

#if defined(__GNUC__) || defined(__clang__)
#define FEE_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define FEE_ALWAYS_INLINE inline
#endif

class FeeCalculator {
public:
    // T1.2 FIX: Make fee rate public so MarketConfig can verify consistency
    static constexpr double TAKER_FEE_RATE = 0.072;

    // Taker fee: 7.2% of price × (1 − price) (the binary payout at this price).
    FEE_ALWAYS_INLINE
    static double taker_fee(double price) noexcept {
        // Clamp price to [0, 1] using std::clamp (branchless on most compilers)
        const double p = std::clamp(price, 0.0, 1.0);
        return TAKER_FEE_RATE * p * (1.0 - p);
    }

    // Maker fee: always zero (liquidity provision).
    FEE_ALWAYS_INLINE
    static double maker_fee(double /*price*/) noexcept {
        return 0.0;
    }

    // Breakeven win-rate: the model probability at which edge_after_fees == 0.
    // For taker: price + taker_fee(price).  For maker: price.
    FEE_ALWAYS_INLINE
    static double breakeven_winrate(double price, bool is_taker) noexcept {
        if (is_taker) [[unlikely]] {
            return price + taker_fee(price);
        }
        return price;
    }

    // Edge after fees: p_model − market_price − fee.
    // Positive edge → trade has positive expected value.
    FEE_ALWAYS_INLINE
    static double edge_after_fees(double p_model, double market_price,
                                   bool is_taker) noexcept {
        if (is_taker) [[unlikely]] {
            const double fee = taker_fee(market_price);
            return p_model - market_price - fee;
        }
        return p_model - market_price;
    }

private:
};
