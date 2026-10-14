// ─────────────────────────────────────────────────────────────────────────────
// kelly_sizer.hpp — Phase 4: Quarter-Kelly Position Sizer
//
// Determines order size based on model confidence, market price,
// bankroll, and inventory constraints.
//
// FÓRMULA:
//   f*      = (p·b - q) / b          // full Kelly
//   f_quarter = f* / 4                // quarter-Kelly (conservative)
//   f_clamped = min(f_quarter, MAX_BANKROLL_PCT)
//
//   donde:
//     p = prob de ganar (CfC + P4 bayesiano)
//     q = 1 - p
//     b = odds netas = (1/price) - 1
//     fees: maker=0%, taker≈2%
//
// INVARIANTS:
//   - O(1) single-thread, < 500ns
//   - Zero heap allocation
//   - Clamped to [0, MAX_BANKROLL_PCT]
//   - Inventory-aware: size *= (1 - |inv|/max_inv)
//   - Volatility-aware: size *= (1 / (1 + vol_z_score))
//
// TODO(P4-T1): Implement full fee-adjusted odds.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <cstdint>
#include <algorithm>

struct KellyConfig {
    float fraction = 0.25f;             // quarter-Kelly
    float max_bankroll_pct = 0.05f;     // max 5% of bankroll per trade
    float min_edge_maker = 0.005f;      // 0.5% min edge (maker, no fee)
    float min_edge_taker = 0.025f;      // 2.5% min edge (taker, ~2% fee)
    float max_inventory_shares = 100.0f; // inventory cap
    float max_vol_zscore = 3.0f;        // cap vol multiplier
};

struct alignas(64) KellyOutput {
    uint64_t order_shares;       // quantized shares to order
    uint64_t order_usd_cents;    // USD cost (cents)
    float fraction_of_bankroll;  // actual fraction used
    bool should_trade;         // false if below min_edge or capped
    float edge_bps;            // informational: model edge in basis points
};

// ── KellySizer ───────────────────────────────────────────────────────────────
class KellySizer {
public:
    explicit KellySizer(const KellyConfig& cfg) : cfg_(cfg) {}

private:
    KellyConfig cfg_;

    // Fee-adjusted effective odds (static: no instance state needed)
    static float effective_b(float b, bool is_maker) noexcept {
        if (is_maker) return b;
        // Taker fee ~2% → effective b = b·(1-0.02)
        return b * 0.98f;
    }

public:
    // ── Inline implementation (hot path, < 500ns) ───────────────────────────
    inline KellyOutput compute(float p,
                               uint64_t price_cents,
                               uint64_t bankroll_cents,
                               float inventory_shares,
                               float vol_zscore,
                               bool is_maker) const noexcept {
        // Clamp probability
        p = p < 0.5f ? 0.5f : (p > 1.0f ? 1.0f : p);
        const float q = 1.0f - p;

        // Odds: b = (1/price) - 1 in price-odds terms
        // For Polymarket binary (price in [0,1] normalized):
        // b = (1.0 - price) / price
        const float price_norm =
            static_cast<float>(price_cents) / 100'000'000.0f; // cents → 0-1
        const float b = (1.0f - price_norm) / price_norm;

        const float b_eff = effective_b(b, is_maker);

        // Full Kelly
        const float f_star = (p * b_eff - q) / b_eff;
        const float min_edge =
            is_maker ? cfg_.min_edge_maker : cfg_.min_edge_taker;

        if (f_star < min_edge) {
            return KellyOutput{
                .order_shares = 0,
                .order_usd_cents = 0,
                .fraction_of_bankroll = 0.0f,
                .should_trade = false,
                .edge_bps = 0.0f,
            };
        }

        // Quarter-Kelly
        float f = f_star * cfg_.fraction;
        f = f < cfg_.max_bankroll_pct ? f : cfg_.max_bankroll_pct;

        // Inventory adjustment
        const float inv_abs = inventory_shares < 0
            ? -inventory_shares : inventory_shares;
        const float inv_factor =
            1.0f - (inv_abs / cfg_.max_inventory_shares);
        f *= (inv_factor > 0.0f ? inv_factor : 0.0f);

        // Volatility adjustment
        float vol_factor =
            1.0f / (1.0f + (vol_zscore > cfg_.max_vol_zscore
                ? cfg_.max_vol_zscore : vol_zscore));
        vol_factor = vol_zscore < 0.0f ? 1.0f : vol_factor;
        f *= vol_factor;

        // Convert to actual order size
        const uint64_t max_order_cents = static_cast<uint64_t>(
            static_cast<double>(bankroll_cents) * f);
        const uint64_t order_shares = max_order_cents / price_cents * 100ULL;
        const uint64_t order_usd_cents = order_shares * price_cents / 100ULL;

        return KellyOutput{
            .order_shares = order_shares,
            .order_usd_cents = order_usd_cents,
            .fraction_of_bankroll = f,
            .should_trade = (order_shares >= 1 && order_usd_cents >= 100),
            .edge_bps = f_star * 10000.0f,
        };
    }
};
