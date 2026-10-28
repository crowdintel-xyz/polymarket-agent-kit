// ─────────────────────────────────────────────────────────────────────────────
// ladder_builder.hpp — Phase 4: Adaptive Quote Ladder Builder
// (REPLACES ladder_skew.hpp — old LadderSkewer archived to core/src/archive/)
//
// Builds a single bid/ask quote pair from conviction signals:
//   - Bayesian probability (p_up)
//   - OFI direction + multiplier (order-flow signal)
//   - Inventory position (skew-aware inventory management)
//   - Volatility (spread expansion)
//
// SKEW MODEL:
//   bb_skew   = (p_up − 0.5) × 2     ∈ [−1, +1]
//   ofi_skew  = ofi_direction × 0.3   ∈ [−0.3, +0.3]
//   inv_skew  = −inventory × 0.5      ∈ [−0.5, +0.5]
//   skew      = clamp(0.5·bb + 0.2·ofi + 0.3·inv, −0.7, 0.7)
//
// PRICING (spread in cents, prices in [0, 1]):
//   spread  = clamp(spread_base × vol_multiplier, 1.0, 10.0)  [cents]
//   half    = spread / 200.0                                    [as price delta]
//   bid     = clamp(mid − half × (1 − skew), 0.01, 0.99)
//   ask     = clamp(mid + half × (1 + skew), 0.01, 0.99)
//
// SIZING:
//   base_size = 10
//   bid_size   = base × ofi_multiplier × (1 + skew)
//   ask_size   = base × ofi_multiplier × (1 − skew)
//
// INVENTORY FLATTENING (|inventory| > 0.8):
//   inv > +0.8  → only ask (reduce long)
//   inv < −0.8  → only bid (reduce short)
//
// is_active = (ofi_multiplier > 0)
//
// INVARIANTS:
//   - O(1), single-thread, < 2μs
//   - Zero heap allocation
//   - Prices always within [0.01, 0.99]
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <algorithm>
#include <cmath>

struct Quote {
    double bid_price;
    double ask_price;
    double bid_size;
    double ask_size;
    bool is_active;
};

class LadderBuilder {
public:
    Quote build(double p_up,
                double market_mid,
                double ofi_direction,
                double ofi_multiplier,
                double inventory,
                double spread_base,
                double vol_multiplier) const noexcept
    {
        Quote q{};

        // ── Active gate ──────────────────────────────────────────────────────
        q.is_active = (ofi_multiplier > 0.0);

        // ── Adaptive spread (cents) ──────────────────────────────────────────
        double spread = spread_base * vol_multiplier;
        // Clamp to [1.0, 10.0] cents
        if (spread < 1.0) spread = 1.0;
        if (spread > 10.0) spread = 10.0;

        // Half-spread in price delta terms (cents → price = cents/100)
        const double half = spread / 200.0;

        // ── Skew components ─────────────────────────────────────────────────
        const double bb_skew  = (p_up - 0.5) * 2.0;     // [−1, +1]
        const double ofi_skew = ofi_direction * 0.3;    // [−0.3, +0.3]
        const double inv_skew = -inventory * 0.5;      // [−0.5, +0.5]

        double skew = bb_skew * 0.5 + ofi_skew * 0.2 + inv_skew * 0.3;
        // Clamp skew to [−0.7, +0.7]
        if (skew < -0.7) skew = -0.7;
        if (skew > 0.7) skew = 0.7;

        // ── Prices ───────────────────────────────────────────────────────────
        double bid_price = market_mid - half * (1.0 - skew);
        double ask_price = market_mid + half * (1.0 + skew);
        // Clamp to valid binary-market range [0.01, 0.99]
        if (bid_price < 0.01) bid_price = 0.01;
        if (bid_price > 0.99) bid_price = 0.99;
        if (ask_price < 0.01) ask_price = 0.01;
        if (ask_price > 0.99) ask_price = 0.99;

        q.bid_price = bid_price;
        q.ask_price = ask_price;

        // ── Sizes ────────────────────────────────────────────────────────────
        const double base_size = 10.0;
        double bid_size = base_size * ofi_multiplier * (1.0 + skew);
        double ask_size = base_size * ofi_multiplier * (1.0 - skew);
        // Clamp sizes to non-negative
        if (bid_size < 0.0) bid_size = 0.0;
        if (ask_size < 0.0) ask_size = 0.0;

        // ── Inventory flattening ─────────────────────────────────────────────
        const double inv_abs = std::abs(inventory);
        if (inv_abs > 0.8) {
            if (inventory > 0.0) {
                // Long → only ask to reduce position
                bid_size = 0.0;
            } else {
                // Short → only bid to reduce position
                ask_size = 0.0;
            }
        }

        q.bid_size = bid_size;
        q.ask_size = ask_size;

        return q;
    }
};
