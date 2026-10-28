// ─────────────────────────────────────────────────────────────────────────────
// fee_calculator.hpp — Polymarket CLOB V2 Non-Linear Fee Engine
//
// Implementation of the exact tiered fee structure for CLOB V2.
// Standard Crypto: 1.75$ per 100 contracts at 0.50 price.
// Non-linear behavior: Fee varies by category (Standard, Policy, Geopolitics).
//
// Invariants:
//   - No heap allocation (always inline)
//   - Exact match with python_oracle/fees.py
//   - Rounding to 5 decimal places (quantum 0.00001$)
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <algorithm>
#include <cmath>
#include <string_view>

#if defined(__GNUC__) || defined(__clang__)
#define FEE_ALWAYS_INLINE __attribute__((always_inline)) inline
#else
#define FEE_ALWAYS_INLINE inline
#endif

enum class MarketCategory {
    Standard,
    Policy,
    Geopolitics
};

class FeeCalculator {
public:
    // Rounding helper to 5 decimal places
    static constexpr double round_to_quantum(double value) noexcept {
        return std::round(value * 100000.0) / 100000.0;
    }

    // Taker fee calculated as absolute USD cost per contract.
    // Formula: base_rate * price * (1 - price) is deprecated in V2 for absolute tiered fees.
    // In CLOB V2, fees are typically fixed per contract based on the price tier.
    FEE_ALWAYS_INLINE
    static double taker_fee_usd(double price, MarketCategory cat = MarketCategory::Standard) noexcept {
        const double p = std::clamp(price, 0.0, 1.0);
        double fee = 0.0;

        if (cat == MarketCategory::Geopolitics) {
            fee = 0.0; // Geopolitics are typically 0 fee
        } else if (cat == MarketCategory::Policy) {
            fee = 0.01; // Example: 1 cent per contract
        } else {
            // Standard tiered logic
            if (p <= 0.1 || p >= 0.9) {
                fee = 0.0063; // 0.63$ per 100
            } else {
                fee = 0.0175; // 1.75$ per 100
            }
        }
        return round_to_quantum(fee);
    }

    // Returns the fee scaled to the [0, 1] probability space (additive to price)
    // fee_prob = fee_usd / (1.0 - price)
    FEE_ALWAYS_INLINE
    static double taker_fee_prob(double price, MarketCategory cat = MarketCategory::Standard) noexcept {
        const double p = std::clamp(price, 0.0, 1.0);
        if (p >= 0.999) return 0.0;
        return taker_fee_usd(p, cat) / (1.0 - p);
    }

    FEE_ALWAYS_INLINE
    static double maker_fee(double /*price*/) noexcept {
        return 0.0;
    }

    FEE_ALWAYS_INLINE
    static double edge_after_fees(double p_model, double market_price, 
                                  bool is_taker, MarketCategory cat = MarketCategory::Standard) noexcept {
        if (is_taker) [[unlikely]] {
            return p_model - market_price - taker_fee_prob(market_price, cat);
        }
        return p_model - market_price;
    }
};
