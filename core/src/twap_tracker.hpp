// ─────────────────────────────────────────────────────────────────────────────
// twap_tracker.hpp — Phase 4: TWAP Convergence Tracker
//
// Tracks time-weighted average price incrementally. Used for:
// 1. Convergence trigger (TWAP deviation = trend signal)
// 2. Anti-manipulation (sudden deviation = warning)
// 3. Execution benchmark (actual fill vs TWAP)
//
// FÓRMULA INCREMENTAL:
//   cumulative_price_time += price * Δt
//   cumulative_time       += Δt
//   twap = cumulative_price_time / cumulative_time
//
// INVARIANTS:
//   - O(1) per tick, < 1μs p50
//   - Zero heap (only accumulators)
//   - Clock: CLOCK_MONOTONIC_RAW
//
// TODO(P4-T2): Implement convergence trigger + anti-manipulation guard.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <cstdint>

struct TWAPConfig {
    static constexpr double CONVERGENCE_THRESHOLD = 0.001;  // 10 bps deviation
    static constexpr double MANIPULATION_THRESHOLD = 0.003; // 30 bps deviation
    static constexpr double MAX_WINDOW_FRACTION = 0.20;     // trigger at 80% elapsed
    static constexpr uint64_t MIN_UPDATE_INTERVAL_NS = 100'000'000ULL;  // 100ms
};

struct alignas(64) TWAPState {
    double cumulative_price_time = 0.0;
    double cumulative_time = 0.0;
    double window_start_price = 0.0;
    double last_price = 0.0;
    uint64_t window_start_ns = 0;
    uint64_t last_update_ns = 0;
    bool initialized = false;
};

struct TWAPSignal {
    double twap;
    double deviation;        // (twap - window_start_price) / window_start_price
    bool convergence_trigger;  // TWAP deviated enough near window end
    bool manipulation_alert;   // sudden large deviation
    double time_remaining_frac; // fraction of window remaining
};

// ── TWAPTracker ──────────────────────────────────────────────────────────────
class TWAPTracker {
public:
    TWAPTracker() = default;

    // Feed new price tick (hot path, < 1μs p50)
    void update(double price, uint64_t now_ns) noexcept {
        if (!state_.initialized) {
            reset(price, now_ns);
            return;
        }
        const double dt = (now_ns - state_.last_update_ns) * 1e-9;
        if (dt < 0.001) return;  // min 1ms between updates
        state_.cumulative_price_time += price * dt;
        state_.cumulative_time += dt;
        state_.last_price = price;
        state_.last_update_ns = now_ns;
    }

    // Check for convergence trigger + manipulation alert
    TWAPSignal check(uint64_t now_ns, uint32_t window_duration_sec) const noexcept {
        const double elapsed = (now_ns - state_.window_start_ns) * 1e-9;
        const double remaining = static_cast<double>(window_duration_sec) - elapsed;
        const double remaining_frac =
            remaining > 0.0 ? remaining / window_duration_sec : 0.0;
        const double twap = current_twap();
        const double dev = deviation();
        return TWAPSignal{
            .twap = twap,
            .deviation = dev,
            .convergence_trigger =
                (remaining_frac < TWAPConfig::MAX_WINDOW_FRACTION &&
                 (dev > TWAPConfig::CONVERGENCE_THRESHOLD ||
                  dev < -TWAPConfig::CONVERGENCE_THRESHOLD)),
            .manipulation_alert =
                (dev > TWAPConfig::MANIPULATION_THRESHOLD ||
                 dev < -TWAPConfig::MANIPULATION_THRESHOLD),
            .time_remaining_frac = remaining_frac,
        };
    }

    // Reset at window start
    void reset(double initial_price, uint64_t window_start_ns) noexcept {
        state_.cumulative_price_time = 0.0;
        state_.cumulative_time = 0.0;
        state_.window_start_price = initial_price;
        state_.last_price = initial_price;
        state_.window_start_ns = window_start_ns;
        state_.last_update_ns = window_start_ns;
        state_.initialized = true;
    }

    // Current TWAP
    double current_twap() const noexcept {
        if (!state_.initialized || state_.cumulative_time < 0.001) {
            return state_.window_start_price;
        }
        return state_.cumulative_price_time / state_.cumulative_time;
    }

    // Current deviation from window-open price
    double deviation() const noexcept {
        if (!state_.initialized || state_.window_start_price == 0.0) {
            return 0.0;
        }
        const double twap = current_twap();
        return (twap - state_.window_start_price) / state_.window_start_price;
    }

private:
    TWAPState state_;
};
