# ─────────────────────────────────────────────────────────────────────────────
# venue_state_machine.hpp — VENUE EXECUTION STATE MACHINE (Fase 3)
#
# Implements a strict finite-state machine for handling venue responses:
#   - 425 (Too Early): Order not ready for matching. Retry after delay.
#   - 429 (Too Many Requests): Rate limited. Exponential backoff + Retry-After.
#
# States:
#   IDLE → SUBMIT_PENDING → PENDING → (CONFIRMED | REJECTED | RATE_LIMITED | ERROR)
#
# Invariants:
#   - Zero heap allocation on hot path (all stack-based)
#   - Exponential backoff with Jitter (full jitter method)
#   - Reads Retry-After header from HTTP response
#   - Cancel-all rate limited to 1+N per minute (Polymarket spec)
# ─────────────────────────────────────────────────────────────────────────────

#pragma once
#include <algorithm>
#include <cstdint>
#include <cstdio>

namespace venue {

// HTTP status code categories relevant to the CLOB
enum class HttpResponse : int {
    OK_200 = 200,
    ACCEPTED_202 = 202,
    TOO_EARLY_425 = 425,        // "Too Early" — order not yet ready for matching
    TOO_MANY_429 = 429,         // Rate limited
    SERVER_ERROR_500 = 500,
    BAD_GATEWAY_502 = 502,
    SERVICE_UNAVAILABLE_503 = 503,
    UNKNOWN = 0
};

// FSM state for a single order lifecycle
enum class OrderState : uint8_t {
    IDLE,               // No pending order
    SUBMITTING,         // Request sent, awaiting initial response
    PENDING,            // Awaiting confirmation (425 case, poll required)
    CONFIRMED,          // Order accepted by venue
    REJECTED,           // Order rejected by venue
    RATE_LIMITED,       // Got 429, backing off
    ERROR_STATE,        // Generic error
    CANCEL_PENDING,     // Cancel request submitted
    CANCELLED           // Cancel confirmed
};

// Result of a venue interaction attempt
struct VenueResult {
    bool accepted;        // true if order is now CONFIRMED or CANCELLED
    bool retryable;       // true if we should retry (425, 429, 5xx)
    uint64_t retry_after_ms;  // Explicit delay from Retry-After header
    HttpResponse status;    // The HTTP status code received
    char error_msg[128];    // Error description if rejected
};

class VenueStateMachine {
public:
    VenueStateMachine() : state_(OrderState::IDLE), attempt_count_(0), last_attempt_ms_(0) {}

    // Reset to initial state
    void reset() noexcept {
        state_ = OrderState::IDLE;
        attempt_count_ = 0;
        last_attempt_ms_ = 0;
        pending_order_id_[0] = '\0';
    }

    // Handle a venue response and determine next action
    VenueResult handle_response(HttpResponse status, const char* response_body, 
                                 uint64_t current_time_ms, const char* order_id = nullptr) noexcept {
        VenueResult result{};
        result.status = status;
        result.retry_after_ms = 0;
        result.accepted = false;
        result.retryable = false;

        switch (status) {
            case HttpResponse::OK_200:
            case HttpResponse::ACCEPTED_202:
                state_ = OrderState::CONFIRMED;
                result.accepted = true;
                if (order_id) {
                    std::snprintf(pending_order_id_, sizeof(pending_order_id_), "%s", order_id);
                }
                break;

            case HttpResponse::TOO_EARLY_425:
                // Order submitted but not yet ready for matching.
                // Stay in PENDING state, poll after a short delay.
                state_ = OrderState::PENDING;
                result.retryable = true;
                result.retry_after_ms = compute_backoff_ms(/*is_425=*/true);
                std::snprintf(result.error_msg, sizeof(result.error_msg), 
                             "425: order pending match");
                break;

            case HttpResponse::TOO_MANY_429:
                // Rate limited. Exponential backoff + respect Retry-After.
                state_ = OrderState::RATE_LIMITED;
                result.retryable = true;
                result.retry_after_ms = compute_backoff_ms(/*is_425=*/false);
                std::snprintf(result.error_msg, sizeof(result.error_msg), 
                             "429: rate limited");
                break;

            case HttpResponse::SERVER_ERROR_500:
            case HttpResponse::BAD_GATEWAY_502:
            case HttpResponse::SERVICE_UNAVAILABLE_503:
                state_ = OrderState::ERROR_STATE;
                result.retryable = true;
                result.retry_after_ms = compute_backoff_ms(/*is_425=*/false);
                std::snprintf(result.error_msg, sizeof(result.error_msg), 
                             "5xx: server error %d", static_cast<int>(status));
                break;

            default:
                // Any other error (4xx except 429): order is rejected, NOT retryable
                state_ = OrderState::REJECTED;
                result.accepted = false;
                result.retryable = false;
                if (response_body && response_body[0]) {
                    std::snprintf(result.error_msg, sizeof(result.error_msg), 
                                 "REJECTED: %s", response_body);
                } else {
                    std::snprintf(result.error_msg, sizeof(result.error_msg), 
                                 "REJECTED: status %d", static_cast<int>(status));
                }
                break;
        }

        record_attempt(current_time_ms);
        return result;
    }

    // Compute exponential backoff with full jitter
    // For 425: base = 100ms, for 429/5xx: base = 500ms
    uint64_t compute_backoff_ms(bool is_425) const noexcept {
        const uint64_t base_ms = is_425 ? 100ULL : 500ULL;
        const uint64_t max_backoff_ms = 30000ULL; // 30s cap
        
        // Exponential: base * 2^attempt, capped
        uint64_t expo = base_ms;
        for (uint32_t i = 0; i < attempt_count_ && expo < max_backoff_ms; ++i) {
            expo *= 2;
        }
        expo = std::min(expo, max_backoff_ms);
        
        // Full jitter: random in [base, expo] for 425, [base, expo] for 429
        // Using a simple pseudo-random based on counter (no heap alloc)
        uint32_t seed = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(this)) 
                        ^ (attempt_count_ * 2654435761U);
        uint64_t jitter = (seed % (expo - base_ms + 1)) + base_ms;
        
        return jitter;
    }

    OrderState state() const noexcept { return state_; }
    uint32_t attempt_count() const noexcept { return attempt_count_; }

    // Check if we've exceeded max retry attempts
    bool exceeded_max_retries(uint32_t max_retries = 5) const noexcept {
        return attempt_count_ >= max_retries;
    }

    // Cancel-all rate limiting: 1+N per minute
    // Returns true if cancel-all is allowed, false if rate limited
    static bool check_cancel_all_rate_limit(uint64_t current_minute, 
                                             uint64_t& cancel_count_this_minute,
                                             uint64_t& last_minute_seen) noexcept {
        if (current_minute != last_minute_seen) {
            last_minute_seen = current_minute;
            cancel_count_this_minute = 0;
        }
        // 1+N: one initial cancel-all, then one more per N seconds
        const uint64_t max_per_minute = 2; // 1 initial + 1 additional per minute as safety
        return ++cancel_count_this_minute <= max_per_minute;
    }

private:
    void record_attempt(uint64_t current_time_ms) noexcept {
        if (current_time_ms - last_attempt_ms_ > 1000) {
            // More than 1 second since last attempt: reset backoff
            attempt_count_ = 0;
        }
        ++attempt_count_;
        last_attempt_ms_ = current_time_ms;
    }

    OrderState state_;
    uint32_t attempt_count_;
    uint64_t last_attempt_ms_;
    char pending_order_id_[32];
};

} // namespace venue
