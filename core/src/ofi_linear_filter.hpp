// ─────────────────────────────────────────────────────────────────────────────
// ofi_linear_filter.hpp — PHASE-2: Deterministic OFI linear filter
//
// Order Flow Imbalance (Cont et al., 2014) with EWMA + z-score thresholds.
// Linear relationship: OFI ~ ΔP. No ML. All decisions auditable with one line.
//
// FORMULA:
//   e_n = ΔB·I(P_b ≥ P_b_prev) − ΔB·I(P_b < P_b_prev)
//       + ΔA·I(P_a ≤ P_a_prev) − ΔA·I(P_a > P_a_prev)
//   OFI_t = λ·OFI_{t-1} + (1-λ)·e_n    (EWMA, λ=0.95)
//   σ_OFI = EWMA of std dev of OFI increments
//   z_OFI = OFI_t / σ_OFI
//
// PRESSURE LEVELS:
//   |z| < 1σ  → NORMAL:    size 1.0x
//   1σ ≤ |z| < 2σ → CAUTION: size 0.5x
//   2σ ≤ |z| < 3σ → HIGH: only close
//   |z| ≥ 3σ → EXTREME: freeze 2s
//
// Invariants:
//   - Zero heap allocation
//   - <20ns per update
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <algorithm>
#include <cmath>
#include <cstdint>

class OfiLinearFilter {
public:
    struct Config {
        double ewma_lambda = 0.95;
        double alert_threshold = 3.0;    // σ
        double caution_threshold = 2.0;  // σ
        uint64_t cooldown_ms = 2000;
    };

    enum class PressureLevel : uint8_t {
        NORMAL,   // size 1.0x
        CAUTION,  // size 0.5x
        HIGH,     // close-only
        EXTREME,  // freeze
    };

    struct FilterState {
        double ofi_value;
        double ofi_sigma;
        double ofi_zscore;
        PressureLevel level;
        bool should_freeze;
        bool should_close_only;
        double size_multiplier;
    };

    explicit OfiLinearFilter(const Config& cfg) : cfg_(cfg) {}

    void on_book_update(
        double bid_vol, double ask_vol,
        double bid_vol_prev, double ask_vol_prev,
        double best_bid, double best_bid_prev,
        double best_ask, double best_ask_prev,
        uint64_t ts_ms
    ) noexcept {
        // Skip out-of-order or stale timestamps
        if (ts_ms <= last_ts_ms_) return;
        last_ts_ms_ = ts_ms;
        // Compute delta volumes
        double delta_b = bid_vol - bid_vol_prev;
        double delta_a = ask_vol - ask_vol_prev;

        // Cont et al. event sign
        double e_b = 0.0;
        if (best_bid >= best_bid_prev) e_b = delta_b;
        else e_b = -delta_b;

        double e_a = 0.0;
        if (best_ask <= best_ask_prev) e_a = delta_a;
        else e_a = -delta_a;

        double e_n = e_b + e_a;

        // Check cooldown for freeze
        if (frozen_) {
            if (ts_ms - freeze_start_ms_ >= cfg_.cooldown_ms) {
                frozen_ = false;
            } else {
                // Stay frozen
                return;
            }
        }

        // EWMA update
        if (!initialized_) {
            ofi_ = (1.0 - cfg_.ewma_lambda) * e_n;
            var_ = 0.0;  // start with zero variance
            initialized_ = true;
        } else {
            double prev_ofi = ofi_;
            ofi_ = cfg_.ewma_lambda * ofi_ + (1.0 - cfg_.ewma_lambda) * e_n;
            double delta = ofi_ - prev_ofi;
            // EWMA of variance
            var_ = cfg_.ewma_lambda * var_ + (1.0 - cfg_.ewma_lambda) * delta * delta;
        }

        // Update frozen state based on z-score
        double sigma = std::sqrt(var_);
        double z = 0.0;
        if (sigma > 1e-12) {
            z = ofi_ / sigma;
        }
        last_zscore_ = z;

        if (std::abs(z) >= cfg_.alert_threshold) {
            frozen_ = true;
            freeze_start_ms_ = ts_ms;
        }
    }

    FilterState current_state() const noexcept {
        double sigma = std::sqrt(var_);
        double z = 0.0;
        if (sigma > 1e-12) {
            z = ofi_ / sigma;
        }
        double abs_z = std::abs(z);

        PressureLevel level;
        bool should_freeze = frozen_;
        bool should_close_only = false;
        double size_multiplier = 1.0;

        if (should_freeze) {
            level = PressureLevel::EXTREME;
            size_multiplier = 0.0;
            should_close_only = true;
        } else if (abs_z >= cfg_.caution_threshold * 1.5) {
            // HIGH: 2σ to 3σ
            level = PressureLevel::HIGH;
            size_multiplier = 0.0;
            should_close_only = true;
        } else if (abs_z >= cfg_.caution_threshold) {
            // CAUTION: 1σ to 2σ
            level = PressureLevel::CAUTION;
            size_multiplier = 0.5;
        } else {
            // NORMAL: < 1σ
            level = PressureLevel::NORMAL;
            size_multiplier = 1.0;
        }

        return FilterState{
            .ofi_value = ofi_,
            .ofi_sigma = sigma,
            .ofi_zscore = z,
            .level = level,
            .should_freeze = should_freeze,
            .should_close_only = should_close_only,
            .size_multiplier = size_multiplier,
        };
    }

    void reset() noexcept {
        ofi_ = 0.0;
        var_ = 0.0;
        frozen_ = false;
        initialized_ = false;
        last_zscore_ = 0.0;
        freeze_start_ms_ = 0;
        last_ts_ms_ = 0;
    }

    double ofi_direction() const noexcept {
        return last_zscore_ > 1e-9 ? 1.0 : (last_zscore_ < -1e-9 ? -1.0 : 0.0);
    }

private:
    Config cfg_;
    double ofi_ = 0.0;
    double var_ = 0.0;
    bool initialized_ = false;
    bool frozen_ = false;
    double last_zscore_ = 0.0;
    uint64_t freeze_start_ms_ = 0;
    uint64_t last_ts_ms_ = 0;
};
