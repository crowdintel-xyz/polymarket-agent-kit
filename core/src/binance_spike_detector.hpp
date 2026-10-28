// ─────────────────────────────────────────────────────────────────────────────
// binance_spike_detector.hpp — Phase 1/3: Binance Price Spike Detection
//
// Detects rapid price movements (>threshold bps in <window) using a
// micro ring buffer of recent mid-prices. Used for anti-sniping defense.
//
// INVARIANTS:
//   - O(1) per tick (ring buffer compare + atomic flag set)
//   - < 1μs p50, < 2μs p99
//   - Thread-safe: written from hot-path, read from hot-path
//   - Fail-closed: stale data → spike detected (conservative)
//
// TODO(P1-T3, P3-T2): Wire into BinanceWSClient stream and hot-path.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <cstdint>
#include <atomic>
#include <array>

struct SpikeDetectorConfig {
    static constexpr uint32_t HISTORY_DEPTH = 64;         // micro ring
    static constexpr double TRIGGER_THRESHOLD_BPS = 15.0;  // 15 bps
    static constexpr uint64_t TRIGGER_WINDOW_NS = 50'000'000ULL;  // 50ms
    static constexpr uint64_t COOLDOWN_NS = 200'000'000ULL;       // 200ms
    static constexpr double CONFIRM_THRESHOLD_BPS = 40.0;  // 500ms confirm
    static constexpr uint64_t CONFIRM_WINDOW_NS = 500'000'000ULL;  // 500ms
};

// ── Output ───────────────────────────────────────────────────────────────────
struct SpikeSignal {
    bool detected;
    double return_bps;       // magnitude of spike
    bool direction_up;       // true = upward spike
    uint64_t timestamp_ns;   // when detected
    bool confirmed;          // also passed the 500ms confirmation
};

// ── SpikeDetector ─────────────────────────────────────────────────────────────
// Hot-path safe. Call update() on every Binance tick.
class alignas(64) BinanceSpikeDetector {
public:
    BinanceSpikeDetector() = default;

    // Feed a new mid-price (hot path). Returns spike signal if detected.
    SpikeSignal update(double mid_price, uint64_t now_ns) noexcept;

    // Check and clear the spike flag (hot path, called once per tick).
    bool has_spike() const noexcept {
        return spike_flag_.load(std::memory_order_acquire);
    }

    // Clear the flag after processing (call from ExecutionEngine after cancel)
    void clear_spike() noexcept {
        spike_flag_.store(false, std::memory_order_release);
    }

    // Cooldown active?
    bool in_cooldown(uint64_t now_ns) const noexcept {
        return (now_ns - last_spike_ns_) < SpikeDetectorConfig::COOLDOWN_NS;
    }

private:
    // Micro ring buffer (does NOT allocate — fixed array)
    std::array<double, SpikeDetectorConfig::HISTORY_DEPTH> prices_{};
    std::array<uint64_t, SpikeDetectorConfig::HISTORY_DEPTH> timestamps_{};
    uint32_t head_ = 0;

    std::atomic<bool> spike_flag_{false};
    uint64_t last_spike_ns_ = 0;
    double last_price_ = 0.0;
    uint64_t last_price_ns_ = 0;
};
