// ─────────────────────────────────────────────────────────────────────────────
// time_strategy.hpp — Phase 4: Time-Based Trading Configuration
//
// Adjusts position sizing, spread, and confidence thresholds by UTC hour
// to match liquidity and volatility profiles of Polymarket settlement windows.
//
// Schedule (UTC hour → config):
//   00–05  Asia lull        size=1.30  spread=1.0  conf=0.55  (aggressive size)
//   06–12  US open → noon   size=1.00  spread=1.0  conf=0.60  (baseline)
//   13–16  US afternoon     size=0.50  spread=1.5  conf=0.70  (cautious, wide)
//   17–19  US close         size=0.80  spread=1.0  conf=0.60  (reduced)
//   20     settlement edge  size=0.30  spread=2.0  conf=0.75  (minimal, wide)
//   21–23  overnight        size=1.10  spread=1.0  conf=0.58  (mild rebound)
//
// INVARIANTS:
//   - O(1) branch lookup, < 50ns
//   - Zero heap allocation
//   - Pure function
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <cstdint>

struct TimeConfig {
    double size_mult;            // multiplier on position size
    double spread_mult;          // multiplier on quote spread
    double confidence_threshold;// minimum model confidence to trade
};

class TimeStrategy {
public:
    // Returns time-based config for the given UTC hour [0, 23].
    static TimeConfig get_time_config(uint32_t hour_utc) noexcept {
        // Clamp to [0, 23]
        if (hour_utc > 23) hour_utc = 23;

        // 00–05: Asia lull — aggressive sizing (thin markets, high edge capture)
        if (hour_utc <= 5) {
            return TimeConfig{1.3, 1.0, 0.55};
        }
        // 06–12: US open → noon — baseline
        if (hour_utc <= 12) {
            return TimeConfig{1.0, 1.0, 0.60};
        }
        // 13–16: US afternoon — cautious (institutional flow, elevated vol)
        if (hour_utc <= 16) {
            return TimeConfig{0.5, 1.5, 0.70};
        }
        // 17–19: US close — reduced activity
        if (hour_utc <= 19) {
            return TimeConfig{0.8, 1.0, 0.60};
        }
        // 20: settlement edge — minimal, wide spread
        if (hour_utc == 20) {
            return TimeConfig{0.3, 2.0, 0.75};
        }
        // 21–23: overnight — mild rebound
        return TimeConfig{1.1, 1.0, 0.58};
    }
};
