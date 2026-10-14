// ─────────────────────────────────────────────────────────────────────────────
// circuit_breaker.hpp — Phase 3: Circuit breaker for Polymarket 429/5xx defense
//
// State machine:
//   CLOSED  → (3 failures: 429/502/503/timeout)  → OPEN
//   OPEN    → (5 000 ms TTL)                     → HALF_OPEN
//   HALF_OPEN → (2 successes)                    → CLOSED
//   HALF_OPEN → (1 failure)                       → OPEN
//
// When OPEN:
//   • allow_request() returns false for ALL request types.
//   • Strategy relies on TTL expiry (no new orders, no cancels).
//   • trading_mode() returns CLOSE_ONLY → WindowShield forces close-only.
//
// Invariants:
//   • No heap allocation — stack-only state machine.
//   • Deterministic via injectable ClockFn.
//   • All operations O(1), noexcept.
//
// TODO(P3-T2): wire into order_gateway for automatic fallback.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef CIRCUIT_BREAKER_HPP
#define CIRCUIT_BREAKER_HPP

#include <cstdint>

#include "../include/time_utils.hpp"

// ── Clock type (shared with rate_limiter.hpp) ────────────────────────────────
// MEDIUM-1 FIX (NEMESIS): Use shared ClockFn from clock_types.hpp
#include "clock_types.hpp"

// ── Trading mode (drives WindowShield / strategy layer) ──────────────────────
enum class TradingMode : uint8_t {
    NORMAL,      // Full operation: new entries, closes, queries
    CLOSE_ONLY,  // No new positions; only close existing inventory
    HALTED,      // Emergency — no trading at all
};

// ── Circuit breaker ──────────────────────────────────────────────────────────
class CircuitBreaker {
public:
    enum class State : uint8_t {
        CLOSED    = 0,
        OPEN      = 1,
        HALF_OPEN = 2,
    };

    struct Config {
        uint32_t failure_threshold  = 3;     // consecutive failures → OPEN
        uint64_t open_duration_ms   = 5000;  // OPEN → HALF_OPEN after this
        uint32_t half_open_max      = 1;     // max trial requests in HALF_OPEN
        uint32_t success_threshold  = 2;     // successes → CLOSED
    };

    CircuitBreaker() noexcept;
    explicit CircuitBreaker(const Config& cfg,
                            ClockFn now_fn = crowdintel::mono_ms) noexcept;

    // Gate: should we allow a new request?
    bool allow_request() noexcept;

    // Call after a successful response.
    void on_success() noexcept;

    // Call after a failure (429, 502, 503, timeout).
    void on_failure() noexcept;

    // Current state.
    State state() const noexcept;

    // Milliseconds until the circuit may retry (0 when CLOSED / HALF_OPEN-ready).
    uint64_t ms_until_retry() const noexcept;

    // Trading mode derived from circuit state (for WindowShield integration).
    TradingMode trading_mode() const noexcept;

    // Reset to initial CLOSED state (for testing or manual recovery).
    void reset() noexcept;

private:
    Config         config_;
    ClockFn        now_fn_;

    State         state_;
    uint32_t      failure_count_;
    uint32_t      half_open_successes_;
    uint32_t      half_open_allowed_;   // requests permitted in current HALF_OPEN window
    uint64_t      opened_at_ms_;

    // Optional log callback (defaults to stderr).
    using LogFn = void(*)(const char*);
    LogFn       log_fn_;
};

// ── Inline implementations ───────────────────────────────────────────────────

inline CircuitBreaker::CircuitBreaker() noexcept
    : config_()
    , now_fn_(crowdintel::mono_ms)
    , state_(State::CLOSED)
    , failure_count_(0)
    , half_open_successes_(0)
    , half_open_allowed_(0)
    , opened_at_ms_(0)
    , log_fn_(nullptr)
{
}

inline CircuitBreaker::CircuitBreaker(const Config& cfg, ClockFn now_fn) noexcept
    : config_(cfg)
    , now_fn_(now_fn)
    , state_(State::CLOSED)
    , failure_count_(0)
    , half_open_successes_(0)
    , half_open_allowed_(0)
    , opened_at_ms_(0)
    , log_fn_(nullptr)
{
}

inline bool CircuitBreaker::allow_request() noexcept {
    uint64_t now = now_fn_();

    switch (state_) {
        case State::CLOSED:
            return true;

        case State::OPEN:
            // Check if the TTL has expired → transition to HALF_OPEN
            if (now - opened_at_ms_ >= config_.open_duration_ms) {
                state_ = State::HALF_OPEN;
                half_open_successes_ = 0;
                half_open_allowed_ = 0;
                if (log_fn_) {
                    log_fn_("CB_HALF_OPEN: probing with single trial request");
                }
            }
            if (state_ == State::OPEN) {
                // Still OPEN — block all requests
                if (log_fn_) {
                    log_fn_("CB_OPEN: relying on TTL expiry");
                }
                return false;
            }
            // Fell through to HALF_OPEN — allow one probe request
            ++half_open_allowed_;
            return true;

        case State::HALF_OPEN:
            // Allow up to half_open_max trial requests
            if (half_open_allowed_ >= config_.half_open_max) {
                return false;
            }
            ++half_open_allowed_;
            return true;
    }

    // Should never reach here (all enum values covered above).
    return false;
}

inline void CircuitBreaker::on_success() noexcept {
    switch (state_) {
        case State::CLOSED:
            failure_count_ = 0;
            break;

        case State::HALF_OPEN:
            ++half_open_successes_;
            if (half_open_successes_ >= config_.success_threshold) {
                state_ = State::CLOSED;
                failure_count_ = 0;
                if (log_fn_) {
                    log_fn_("CB_CLOSED: circuit recovered");
                }
            }
            break;

        case State::OPEN:
            // A success while OPEN should not happen (requests are blocked),
            // but handle gracefully by resetting failure count.
            failure_count_ = 0;
            break;
    }
}

inline void CircuitBreaker::on_failure() noexcept {
    uint64_t now = now_fn_();

    switch (state_) {
        case State::CLOSED:
            ++failure_count_;
            if (failure_count_ >= config_.failure_threshold) {
                state_ = State::OPEN;
                opened_at_ms_ = now;
                failure_count_ = 0;
                half_open_successes_ = 0;
                half_open_allowed_ = 0;
                if (log_fn_) {
                    log_fn_("CB_OPEN: relying on TTL expiry");
                }
            }
            break;

        case State::HALF_OPEN:
            // A failure during probing → back to OPEN
            state_ = State::OPEN;
            opened_at_ms_ = now;
            half_open_successes_ = 0;
            half_open_allowed_ = 0;
            if (log_fn_) {
                log_fn_("CB_OPEN: relying on TTL expiry");
            }
            break;

        case State::OPEN:
            // Failures while OPEN are not tracked (requests are blocked).
            break;
    }
}

inline CircuitBreaker::State CircuitBreaker::state() const noexcept {
    return state_;
}

inline uint64_t CircuitBreaker::ms_until_retry() const noexcept {
    if (state_ != State::OPEN) return 0;

    uint64_t now = now_fn_();
    uint64_t elapsed = now - opened_at_ms_;
    if (elapsed >= config_.open_duration_ms) return 0;

    return config_.open_duration_ms - elapsed;
}

inline TradingMode CircuitBreaker::trading_mode() const noexcept {
    switch (state_) {
        case State::CLOSED:
            return TradingMode::NORMAL;
        case State::OPEN:
        case State::HALF_OPEN:
            return TradingMode::CLOSE_ONLY;
        default:
            return TradingMode::HALTED;
    }
}

inline void CircuitBreaker::reset() noexcept {
    state_ = State::CLOSED;
    failure_count_ = 0;
    half_open_successes_ = 0;
    half_open_allowed_ = 0;
    opened_at_ms_ = 0;
}

#endif // CIRCUIT_BREAKER_HPP
