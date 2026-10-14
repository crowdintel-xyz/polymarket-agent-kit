// ─────────────────────────────────────────────────────────────────────────────
// window_shield.hpp — Phase 3: Window Lifecycle State Machine
//
// Defiende contra manipulación de liquidación en los últimos segundos.
// Transiciones de estado en función del tiempo restante de la ventana.
//
// ESTADOS (MERCADO 5m = 300s):
//   t=0-240s:  MAKER_PASSIVE    (spread normal, quote simétrico)
//   t=240-270s: MAKER_SKEWED     (si CfC > 0.6, skew quotes)
//   t=270-290s: DIRECTIONAL     (si CfC > 0.75, solo posición direccional)
//   t=290-300s: CLOSE_ONLY      (cancel all, solo cerrar inventario)
//   t=300s+:   HALTED            (5s cooldown, no órdenes)
//
// INVARIANTS:
//   - HOT path: state transition check < 200ns (timer arithmetic + lookup)
//   - FAIL CLOSED: stale timestamp → HALTED
//   - All timers use CLOCK_MONOTONIC from window_start_timestamp
//
// TODO(P3-T1): Implement state machine + transitions.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include "ofi_linear_filter.hpp"  // OfiPressure (Phase 2 defense)
#include <cstdint>
#include <atomic>

// ── Configuration ────────────────────────────────────────────────────────────
struct WindowShieldConfig {
    // Time thresholds (seconds into the window)
    uint32_t window_seconds;
    uint32_t skewed_start_sec;
    uint32_t directional_start_sec;
    uint32_t close_only_start_sec;
    uint32_t halt_duration_sec;

    // Conviction thresholds (CfC probability)
    float skewed_threshold;       // 0.6
    float directional_threshold;  // 0.75

    // Constructor for 5-minute windows
    static constexpr WindowShieldConfig BTC_5M() {
        return WindowShieldConfig{
            .window_seconds = 300,
            .skewed_start_sec = 240,
            .directional_start_sec = 270,
            .close_only_start_sec = 290,
            .halt_duration_sec = 5,
            .skewed_threshold = 0.6f,
            .directional_threshold = 0.75f
        };
    }

    // Constructor for 15-minute windows
    static constexpr WindowShieldConfig BTC_15M() {
        return WindowShieldConfig{
            .window_seconds = 900,
            .skewed_start_sec = 810,
            .directional_start_sec = 870,
            .close_only_start_sec = 890,
            .halt_duration_sec = 3,
            .skewed_threshold = 0.6f,
            .directional_threshold = 0.75f
        };
    }
};

// ── State Enum ───────────────────────────────────────────────────────────────
enum class ShieldState : uint8_t {
    MAKER_PASSIVE,     // Normal quoting, symmetric
    MAKER_SKEWED,      // Quote skewed toward conviction
    DIRECTIONAL,       // Directional only, no passive quotes
    CLOSE_ONLY,        // Cancel all, only close inventory
    HALTED             // Emergency, no orders, cooldown period
};

// ── OFI Pressure Level (Phase 2 defense integration) ──────────────────────────
enum class OfiPressure : uint8_t {
    NORMAL,    // size 1.0x
    CAUTION,   // size 0.5x
    HIGH,      // close-only
    EXTREME,   // freeze → force CLOSE_ONLY
};

// ── State Query Result ───────────────────────────────────────────────────────
struct ShieldStateInfo {
    ShieldState state;
    uint32_t remaining_sec;
    uint32_t elapsed_sec;
    bool is_settlement_time;    // true during HALTED after window close
    OfiPressure ofi_pressure;
    const char* name() const noexcept;
};

// ── WindowShield ─────────────────────────────────────────────────────────────
// Single-market state machine. Hot-path safe (all O(1)).
class WindowShield {
public:
    explicit WindowShield(const WindowShieldConfig& cfg) : cfg_(cfg) {}

    // Set window start timestamp (called once at window open, from hot path)
    void set_window_start(uint64_t start_ns_monotonic) noexcept {
        window_start_ns_ = start_ns_monotonic;
        current_state_.store(ShieldState::MAKER_PASSIVE);
    }

    // Update: call every tick with current monotonic time.
    // cfc_confidence: [0, 1] from CfCNetwork.
    // Returns the active state and whether a transition occurred.
    ShieldState update(uint64_t now_ns, float cfc_confidence, OfiPressure ofi_pressure = OfiPressure::NORMAL) noexcept;

    // Get current state (hot-path read, atomic load)
    ShieldState current_state() const noexcept {
        return current_state_.load(std::memory_order_acquire);
    }

    // Get full state info for strategy layer
    ShieldStateInfo snapshot(uint64_t now_ns) const noexcept;

    // Force reset (window closed, starting new window)
    void reset(uint64_t new_window_start_ns) noexcept {
        window_start_ns_ = new_window_start_ns;
        current_state_.store(ShieldState::MAKER_PASSIVE);
    }

    // Check if this is a new window (for CfC reset signal)
    bool should_reset_cfc(uint64_t now_ns) const noexcept;

    const char* state_name(ShieldState s) const noexcept;

private:
    WindowShieldConfig cfg_;
    uint64_t window_start_ns_ = 0;
    std::atomic<ShieldState> current_state_{ShieldState::MAKER_PASSIVE};

    ShieldState compute_state(uint32_t elapsed_sec,
                              float cfc_confidence) const noexcept;

    // Last time state changed (for cooldown logic and CfC reset)
    uint64_t last_state_change_ns_ = 0;
    OfiPressure ofi_pressure_ = OfiPressure::NORMAL;
    ShieldState prev_state_ = ShieldState::MAKER_PASSIVE;
};

// ── Inline implementations (hot path, O(1)) ─────────────────────────────────

inline const char* ShieldStateInfo::name() const noexcept {
    switch (state) {
        case ShieldState::MAKER_PASSIVE:  return "MAKER_PASSIVE";
        case ShieldState::MAKER_SKEWED:   return "MAKER_SKEWED";
        case ShieldState::DIRECTIONAL:    return "DIRECTIONAL";
        case ShieldState::CLOSE_ONLY:     return "CLOSE_ONLY";
        case ShieldState::HALTED:         return "HALTED";
    }
    return "UNKNOWN";
}

inline const char* WindowShield::state_name(ShieldState s) const noexcept {
    switch (s) {
        case ShieldState::MAKER_PASSIVE:  return "MAKER_PASSIVE";
        case ShieldState::MAKER_SKEWED:   return "MAKER_SKEWED";
        case ShieldState::DIRECTIONAL:    return "DIRECTIONAL";
        case ShieldState::CLOSE_ONLY:     return "CLOSE_ONLY";
        case ShieldState::HALTED:         return "HALTED";
    }
    return "UNKNOWN";
}

inline ShieldState WindowShield::compute_state(uint32_t elapsed_sec,
                                               float cfc_confidence) const noexcept {
    // FAIL-CLOSED: if elapsed exceeds window + halt, go HALTED
    if (elapsed_sec >= cfg_.window_seconds + cfg_.halt_duration_sec) {
        return ShieldState::HALTED;
    }

    // Post-settlement cooling period
    if (elapsed_sec >= cfg_.window_seconds) {
        return ShieldState::HALTED;
    }

    // Close-only window (last 10s of 300s window)
    if (elapsed_sec >= cfg_.close_only_start_sec) {
        return ShieldState::CLOSE_ONLY;
    }

    // Directional-only zone (if CfC confidence high enough)
    if (elapsed_sec >= cfg_.directional_start_sec &&
        cfc_confidence >= cfg_.directional_threshold) {
        return ShieldState::DIRECTIONAL;
    }

    // Skewed quoting zone
    if (elapsed_sec >= cfg_.skewed_start_sec &&
        cfc_confidence >= cfg_.skewed_threshold) {
        return ShieldState::MAKER_SKEWED;
    }

    // Default: passive maker
    return ShieldState::MAKER_PASSIVE;
}

inline ShieldState WindowShield::update(uint64_t now_ns,
                                        float cfc_confidence, OfiPressure ofi_pressure) noexcept {
    // FAIL-CLOSED: stale timestamp → HALTED
    if (window_start_ns_ == 0) {
        return ShieldState::HALTED;
    }

    const uint64_t elapsed_ns = now_ns - window_start_ns_;
    const uint32_t elapsed_sec = static_cast<uint32_t>(elapsed_ns / 1'000'000'000ULL);

    // OFI EXTREME pressure: force CLOSE_ONLY (Phase 2 defense)
    if (ofi_pressure == OfiPressure::EXTREME) {
        ofi_pressure_ = ofi_pressure;
        current_state_.store(ShieldState::CLOSE_ONLY);
        return ShieldState::CLOSE_ONLY;
    }
    ofi_pressure_ = ofi_pressure;

    const ShieldState new_state = compute_state(elapsed_sec, cfc_confidence);

    ShieldState expected = current_state_.load(std::memory_order_acquire);
    if (new_state != expected) {
        current_state_.store(new_state, std::memory_order_release);
        if (new_state != prev_state_) {
            last_state_change_ns_ = now_ns;
        }
        prev_state_ = new_state;
    }

    return new_state;
}

inline ShieldStateInfo WindowShield::snapshot(uint64_t now_ns) const noexcept {
    const uint64_t elapsed_ns = now_ns - window_start_ns_;
    const uint32_t elapsed_sec = static_cast<uint32_t>(elapsed_ns / 1'000'000'000ULL);
    const uint32_t remaining_sec =
        (elapsed_sec < cfg_.window_seconds)
            ? (cfg_.window_seconds - elapsed_sec)
            : 0;

    return ShieldStateInfo{
        .state = current_state_.load(std::memory_order_acquire),
        .remaining_sec = remaining_sec,
        .elapsed_sec = elapsed_sec,
        .is_settlement_time =
            (elapsed_sec >= cfg_.window_seconds) &&
            (elapsed_sec < cfg_.window_seconds + cfg_.halt_duration_sec),
        .ofi_pressure = ofi_pressure_,
    };
}

inline bool WindowShield::should_reset_cfc(uint64_t now_ns) const noexcept {
    // Signal CfC reset on window boundary
    if (window_start_ns_ == 0) return false;
    const uint64_t elapsed_ns = now_ns - window_start_ns_;
    const uint32_t elapsed_sec = static_cast<uint32_t>(elapsed_ns / 1'000'000'000ULL);
    return elapsed_sec >= cfg_.window_seconds;
}
