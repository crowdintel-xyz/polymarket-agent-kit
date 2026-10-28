// ─────────────────────────────────────────────────────────────────────────────
// rate_limiter.hpp — Phase 3: Token-bucket rate limiting + exponential backoff
//
// Defends against Cloudflare HTTP 429 under burst traffic by:
//   • Multi-bucket token buckets (orders / cancels / queries) with shared
//     burst capacity, fixed-point milli-token accounting (zero FP drift).
//   • Per-request-type exponential backoff (200→400→800…) that gates
//     can_send() independently of the token bucket.
//   • An injectable ClockFn so unit tests run fully deterministic without
//     sleeping.
//
// Invariants:
//   • No heap allocation — every member is stack/static.
//   • All integer arithmetic (uint64_t milli-tokens), deterministic.
//   • Hot-path try_consume(): 1× atomic-load + 1× CAS + 2 branches.
//
// TODO(P3-T1): integrate with order_gateway for 429 handling.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef RATE_LIMITER_HPP
#define RATE_LIMITER_HPP

#include <atomic>
#include <cstdint>
#include <algorithm>

#include "../include/time_utils.hpp"

// ── Clock abstraction ────────────────────────────────────────────────────────
// MEDIUM-1 FIX (NEMESIS): Use shared ClockFn from clock_types.hpp
#include "clock_types.hpp"

// ── Request types ────────────────────────────────────────────────────────────
enum class RequestType : uint8_t {
    ORDER  = 0,
    CANCEL = 1,
    QUERY  = 2,
};

inline constexpr uint8_t RequestTypeCount = 3;

// ── Configuration ────────────────────────────────────────────────────────────
struct RateLimitConfig {
    uint32_t orders_per_sec   = 10;
    uint32_t cancels_per_sec  = 30;
    uint32_t queries_per_sec  = 20;
    uint32_t burst_capacity   = 20;

    // Exponential backoff for 429 responses
    uint64_t backoff_base_ms      = 200;
    uint64_t backoff_max_ms       = 10000;
    double   backoff_multiplier   = 2.0;
};

// ── Fixed-point token bucket ────────────────────────────────────────────────
// Tokens are stored in milli-tokens (1 token = 1000 units).
// This gives sub-millisecond refill precision at any integer rate ≥ 1 token/s
// using purely integer arithmetic (no floating-point drift).
class TokenBucket {
public:
    static constexpr uint64_t TOKEN_SCALE = 1000ULL;  // milli-tokens per token

    // Default constructor for array compatibility
    TokenBucket() noexcept
        : capacity_(0), rate_per_sec_(0), now_fn_(nullptr),
          tokens_(0), last_refill_ms_(0) {}

    // Initialize bucket parameters
    void init(uint32_t capacity, uint32_t rate_per_sec,
              ClockFn now_fn = crowdintel::mono_ms) noexcept {
        capacity_ = capacity;
        rate_per_sec_ = rate_per_sec;
        now_fn_ = now_fn;
        tokens_.store(static_cast<uint64_t>(capacity) * TOKEN_SCALE,
                      std::memory_order_release);
        last_refill_ms_.store(now_fn_(), std::memory_order_release);
    }

    // capacity  — max tokens (integer)
    // rate      — refill rate in tokens/sec
    // now_fn    — clock source (defaults to system monotonic clock)
    TokenBucket(uint32_t capacity, uint32_t rate_per_sec,
                ClockFn now_fn = crowdintel::mono_ms) noexcept;

    // Try to consume one token. Returns true if a token was available
    // (and was consumed); false if the bucket is empty.
    bool try_consume() noexcept;

    // Milliseconds until at least one token is available.
    // Returns 0 if a token is immediately available.
    uint64_t ms_until_available() const noexcept;

    // Trigger a lazy refill based on elapsed wall-clock time.
    void refill() const noexcept;

    // Current whole-token count (for telemetry).
    uint32_t tokens_remaining() const noexcept;

    // Full bucket capacity in whole tokens.
    uint32_t capacity() const noexcept { return capacity_; }

    // Reset bucket to full capacity.
    void reset() noexcept;

private:
    uint32_t capacity_;             // max tokens
    uint32_t rate_per_sec_;         // refill rate (tokens/sec)
    ClockFn  now_fn_;               // time source

    // Mutable because refill() is conceptually a const lazy-update.
    mutable std::atomic<uint64_t> tokens_;        // milli-tokens
    mutable std::atomic<uint64_t> last_refill_ms_; // last refill timestamp
};

// ── Rate status result ───────────────────────────────────────────────────────
struct RateStatus {
    bool        allowed;             // can_send would return true right now
    uint64_t    wait_ms;             // max(backoff_remaining, token_wait)
    uint64_t    backoff_ms;          // current exponential backoff
    uint32_t    tokens_remaining;    // whole tokens in the bucket
};

// ── Multi-bucket rate limiter ────────────────────────────────────────────────
class RateLimiter {
public:
    explicit RateLimiter(const RateLimitConfig& config,
                         ClockFn now_fn = crowdintel::mono_ms) noexcept;

    // Check + consume: returns true if the request is allowed (backoff
    // inactive AND token available). Consumes one token on success.
    bool can_send(RequestType type) noexcept;

    // Notify that a request was successfully sent (respawns backoff reset).
    void on_sent(RequestType type) noexcept;

    // Notify that a 429 / rate-limited response was received.
    // Doubles the exponential backoff for this request type.
    void on_rate_limited(RequestType type, uint64_t retry_ms) noexcept;

    // Milliseconds until the next request of this type may be sent.
    uint64_t wait_time_ms(RequestType type) const noexcept;

    // Current backoff value for a request type (for tests).
    uint64_t backoff_ms(RequestType type) const noexcept;

    // Snapshot of the order-bucket status.
    RateStatus status() const noexcept;

private:
    TokenBucket& bucket(RequestType type) noexcept;
    const TokenBucket& bucket(RequestType type) const noexcept;

    struct BackoffState {
        uint64_t backoff_ms      = 0;
        uint64_t backoff_until_ms = 0;
    };

    BackoffState& backoff_state(RequestType type) noexcept;
    const BackoffState& backoff_state(RequestType type) const noexcept;

    RateLimitConfig config_;
    ClockFn         now_fn_;
    TokenBucket     buckets_[RequestTypeCount];
    BackoffState    backoff_[RequestTypeCount];
};

// ── Inline implementations ───────────────────────────────────────────────────

inline TokenBucket::TokenBucket(uint32_t capacity, uint32_t rate_per_sec,
                                ClockFn now_fn) noexcept
    : capacity_(capacity)
    , rate_per_sec_(rate_per_sec)
    , now_fn_(now_fn)
    , tokens_(static_cast<uint64_t>(capacity) * TOKEN_SCALE)
    , last_refill_ms_(now_fn_())
{
}

inline void TokenBucket::refill() const noexcept {
    uint64_t now = now_fn_();
    uint64_t last = last_refill_ms_.load(std::memory_order_relaxed);
    // Guard against clock backward jump: cap elapsed to 0 if now < last
    // so a backward jump does not wrap to UINT64_MAX (which would either
    // starve the bucket or refill it completely — a rate-limit bypass).
    uint64_t elapsed_ms = (now > last) ? (now - last) : 0;
    if (elapsed_ms == 0) return;

    // milli-tokens to add = elapsed_ms * rate_per_sec
    uint64_t refill_milli =
        static_cast<uint64_t>(elapsed_ms) * static_cast<uint64_t>(rate_per_sec_);
    if (refill_milli == 0) return;

    // CAS: claim the time-window so concurrent callers don't double-refill.
    if (last_refill_ms_.compare_exchange_strong(
            last, now,
            std::memory_order_acq_rel,
            std::memory_order_relaxed)) {
        uint64_t cur = tokens_.load(std::memory_order_relaxed);
        uint64_t refilled = cur + refill_milli;
        uint64_t cap = static_cast<uint64_t>(capacity_) * TOKEN_SCALE;
        if (refilled > cap) refilled = cap;
        tokens_.store(refilled, std::memory_order_release);
    }
}

inline bool TokenBucket::try_consume() noexcept {
    refill();
    uint64_t cur = tokens_.load(std::memory_order_relaxed);
    while (cur >= TOKEN_SCALE) {
        if (tokens_.compare_exchange_weak(
                cur, cur - TOKEN_SCALE,
                std::memory_order_acquire,
                std::memory_order_relaxed)) {
            return true;
        }
        // CAS failed — cur was updated by another thread, retry.
    }
    return false;
}

inline uint64_t TokenBucket::ms_until_available() const noexcept {
    refill();
    uint64_t cur = tokens_.load(std::memory_order_relaxed);
    if (cur >= TOKEN_SCALE) return 0;

    // deficit in milli-tokens
    uint64_t deficit = TOKEN_SCALE - cur;
    if (rate_per_sec_ == 0) return UINT64_MAX;

    // ceiling division: (deficit + rate - 1) / rate
    uint64_t denom = static_cast<uint64_t>(rate_per_sec_);
    return (deficit + denom - 1ULL) / denom;
}

inline uint32_t TokenBucket::tokens_remaining() const noexcept {
    refill();
    return static_cast<uint32_t>(
        tokens_.load(std::memory_order_relaxed) / TOKEN_SCALE);
}

inline void TokenBucket::reset() noexcept {
    uint64_t now = now_fn_();
    tokens_.store(static_cast<uint64_t>(capacity_) * TOKEN_SCALE,
                  std::memory_order_release);
    last_refill_ms_.store(now, std::memory_order_release);
}

// ── RateLimiter ──────────────────────────────────────────────────────────────

inline RateLimiter::RateLimiter(const RateLimitConfig& config,
                                ClockFn now_fn) noexcept
    : config_(config)
    , now_fn_(now_fn)
{
    buckets_[0].init(config.burst_capacity, config.orders_per_sec,  now_fn_);
    buckets_[1].init(config.burst_capacity, config.cancels_per_sec, now_fn_);
    buckets_[2].init(config.burst_capacity, config.queries_per_sec, now_fn_);
}

inline TokenBucket& RateLimiter::bucket(RequestType type) noexcept {
    return buckets_[static_cast<uint8_t>(type)];
}

inline const TokenBucket& RateLimiter::bucket(RequestType type) const noexcept {
    return buckets_[static_cast<uint8_t>(type)];
}

inline RateLimiter::BackoffState& RateLimiter::backoff_state(RequestType type) noexcept {
    return backoff_[static_cast<uint8_t>(type)];
}

inline const RateLimiter::BackoffState&
RateLimiter::backoff_state(RequestType type) const noexcept {
    return backoff_[static_cast<uint8_t>(type)];
}

inline bool RateLimiter::can_send(RequestType type) noexcept {
    auto& bs = backoff_state(type);
    uint64_t now = now_fn_();

    // If exponential backoff is active, reject immediately.
    if (bs.backoff_ms > 0 && bs.backoff_until_ms > now) {
        return false;
    }
    // Backoff expired — clear it so the next 429 starts fresh from base.
    if (bs.backoff_ms > 0) {
        bs.backoff_ms = 0;
        bs.backoff_until_ms = 0;
    }

    // Try to consume a token from the bucket.
    return bucket(type).try_consume();
}

inline void RateLimiter::on_sent(RequestType type) noexcept {
    // A successful send resets the exponential backoff chain.
    auto& bs = backoff_state(type);
    bs.backoff_ms = 0;
    bs.backoff_until_ms = 0;
}

inline void RateLimiter::on_rate_limited(RequestType type,
                                          uint64_t retry_ms) noexcept {
    auto& bs = backoff_state(type);
    uint64_t new_backoff;

    if (bs.backoff_ms == 0) {
        new_backoff = config_.backoff_base_ms;
    } else {
        new_backoff = static_cast<uint64_t>(
            static_cast<double>(bs.backoff_ms) * config_.backoff_multiplier);
    }

    if (new_backoff < config_.backoff_base_ms) {
        new_backoff = config_.backoff_base_ms;
    }
    if (new_backoff > config_.backoff_max_ms) {
        new_backoff = config_.backoff_max_ms;
    }
    // Respect server's Retry-After hint, but cap to prevent malicious/DoS value.
    if (retry_ms > new_backoff && retry_ms <= config_.backoff_max_ms) {
        new_backoff = retry_ms;
    }

    bs.backoff_ms = new_backoff;
    bs.backoff_until_ms = now_fn_() + new_backoff;
}

inline uint64_t RateLimiter::wait_time_ms(RequestType type) const noexcept {
    const auto& b = bucket(type);
    uint64_t token_wait = b.ms_until_available();

    const auto& bs = backoff_state(type);
    uint64_t now = now_fn_();
    uint64_t backoff_wait = 0;
    if (bs.backoff_ms > 0 && bs.backoff_until_ms > now) {
        backoff_wait = bs.backoff_until_ms - now;
    }

    return std::max(token_wait, backoff_wait);
}

inline uint64_t RateLimiter::backoff_ms(RequestType type) const noexcept {
    return backoff_state(type).backoff_ms;
}

inline RateStatus RateLimiter::status() const noexcept {
    const auto& b = bucket(RequestType::ORDER);
    uint64_t token_wait = b.ms_until_available();

    const auto& bs = backoff_state(RequestType::ORDER);
    uint64_t now = now_fn_();
    uint64_t backoff_wait = 0;
    if (bs.backoff_ms > 0 && bs.backoff_until_ms > now) {
        backoff_wait = bs.backoff_until_ms - now;
    }

    return RateStatus{
        .allowed          = (backoff_wait == 0) && (token_wait == 0),
        .wait_ms          = std::max(token_wait, backoff_wait),
        .backoff_ms       = bs.backoff_ms,
        .tokens_remaining = b.tokens_remaining(),
    };
}

#endif // RATE_LIMITER_HPP
