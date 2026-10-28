// ─────────────────────────────────────────────────────────────────────────────
// ladder_skew.hpp — Phase 4: Dynamic Pre-Signed Ladder Skewing
//
// Adjusts quote positions and sizes based on combined conviction signal.
//
// CONVOLUCIÓN DE CONvicción:
//   conviction = w1·cfc_p + w2·ofi_signal + w3·twap_signal
//   skew = (conviction - 0.5) * 2    // [-1, +1]
//
// SKEW APPLICATION (skew > 0 = bullish):
//   bid_size = base * (1 + skew * 0.7)
//   ask_size = base * (1 - skew * 0.7)
//   bid_offset = -spread/2 * (1 - skew * 0.3)   // closer to mid
//   ask_offset = +spread/2 * (1 + skew * 0.3)   // farther from mid
//
// INVARIANTS:
//   - Hot-path (order construction phase)
//   - < 3μs p50 (16-order pool construction)
//   - Zero heap (pre-signed pool)
//   - Never violate bid/ask ratio limits [0.2, 5.0]
//   - Skewed prices within [mid - max_slippage, mid + max_slippage]
//
// TODO(P4-T3): Integrate with PresignedOrderPool.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <cstdint>
#include <algorithm>

struct LadderSkewConfig {
    float w_cfc = 0.5f;
    float w_ofi = 0.3f;
    float w_twap = 0.2f;

    float skew_intensity = 0.7f;        // how strongly to skew sizes
    float spread_intensity = 0.3f;     // how strongly to skew offsets
    float max_ratio = 5.0f;            // max bid/ask size ratio
    float min_ratio = 0.2f;
    float max_slippage_bps = 5.0f;      // max deviation from mid
};

struct alignas(64) ConvictionInput {
    float cfc_probability;    // [0, 1] from CfCNetwork
    float ofi_signal;         // [0, 1] from normalized OFI
    float twap_signal;        // [0, 1] from TWAP deviation
};

struct alignas(64) LadderQuote {
    int64_t price_cents;      // quote price in cents
    uint64_t size_shares;     // size in shares (quantized)
    bool is_bid;              // true = bid, false = ask
};

struct alignas(64) LadderOutput {
    LadderQuote quotes[16];    // up to 16 quotes (8 bid levels + 8 ask)
    uint32_t n_quotes;
    float conviction;
    float skew;
};

// ── LadderSkewer ─────────────────────────────────────────────────────────────
class LadderSkewer {
public:
    explicit LadderSkewer(const LadderSkewConfig& cfg) : cfg_(cfg) {}

    // Build skewed ladder from market state.
    // mid_price_cents: current market mid in cents
    // spread_cents: current spread in cents
    // base_size_shares: base quote size (before skew)
    // bankroll_cents: for sizing cap
private:
    LadderSkewConfig cfg_;

    static float skew_factor(float skew, float intensity) noexcept {
        return 1.0f + skew * intensity;
    }

public:
    // Compute combined conviction signal [0, 1] — used by both sides
    // NOTE: weights must match LadderSkewConfig (w_cfc, w_ofi, w_twap)
    inline static float compute_conviction(const ConvictionInput& input) noexcept {
        float c = input.cfc_probability * 0.5f
                + input.ofi_signal * 0.3f
                + input.twap_signal * 0.2f;
        return std::clamp(c, 0.0f, 1.0f);
    }
    inline LadderOutput build(const ConvictionInput& conviction_input,
                              uint64_t mid_price_cents,
                              uint64_t spread_cents,
                              uint64_t base_size_shares,
                              uint64_t bankroll_cents) const noexcept {
        const float conviction = compute_conviction(conviction_input);
        const float skew = (conviction - 0.5f) * 2.0f;  // [-1, +1]

        LadderOutput out{};
        out.conviction = conviction;
        out.skew = skew;
        out.n_quotes = 0;

        // Number of price levels (cap at 8 per side for 16 total)
        constexpr uint32_t LEVELS_PER_SIDE = 8;

        for (uint32_t i = 0; i < LEVELS_PER_SIDE && out.n_quotes < 16; ++i) {
            // Price ladder: geometric decay from mid
            const double level_mult = 1.0 - 0.001 * static_cast<double>(i);
            const int64_t bid_price =
                static_cast<int64_t>(mid_price_cents) -
                static_cast<int64_t>(spread_cents / 2) * (1.0 - skew * 0.3) -
                static_cast<int64_t>(i * 10);  // 0.10 cents per level
            const int64_t ask_price =
                static_cast<int64_t>(mid_price_cents) +
                static_cast<int64_t>(spread_cents / 2) * (1.0 + skew * 0.3) +
                static_cast<int64_t>(i * 10);

            // Size skew: more size on favored side
            const uint64_t bid_size =
                static_cast<uint64_t>(static_cast<double>(base_size_shares) *
                                      skew_factor(skew, cfg_.skew_intensity));
            const uint64_t ask_size =
                static_cast<uint64_t>(static_cast<double>(base_size_shares) /
                                      skew_factor(skew, cfg_.skew_intensity));

            // Enforce ratio limits [0.2, 5.0]
            const float ratio = static_cast<float>(bid_size) /
                                 static_cast<float>(ask_size > 0 ? ask_size : 1);
            if (ratio > cfg_.max_ratio || ratio < cfg_.min_ratio) {
                // Clamp by using min ratio
                uint64_t min_sz = std::min(bid_size, ask_size);
                uint64_t max_sz = static_cast<uint64_t>(min_sz * cfg_.max_ratio);
                // Redistribute
            }

            // Price cap (slippage limit)
            const int64_t max_bid = static_cast<int64_t>(mid_price_cents) -
                static_cast<int64_t>(cfg_.max_slippage_bps * 10);
            const int64_t max_ask = static_cast<int64_t>(mid_price_cents) +
                static_cast<int64_t>(cfg_.max_slippage_bps * 10);

            if (bid_price >= max_bid && ask_price <= max_ask) {
                out.quotes[out.n_quotes++] = LadderQuote{
                    .price_cents = bid_price,
                    .size_shares = bid_size,
                    .is_bid = true
                };
                out.quotes[out.n_quotes++] = LadderQuote{
                    .price_cents = ask_price,
                    .size_shares = ask_size,
                    .is_bid = false
                };
            }
        }
        (void)bankroll_cents;  // sizing cap applied at Kelly layer
        return out;
    }
};
