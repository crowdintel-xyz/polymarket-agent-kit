// ─────────────────────────────────────────────────────────────────────────────
// test_rate_limiter — Phase 3 unit tests for TokenBucket + RateLimiter
//
// Covered scenarios:
//   1. 10 orders in 1 s → all allowed (rate + burst)
//   2. 11th order        → rejected (burst exhausted)
//   3. 30 cancels in 1 s → all allowed
//   4. 429 backoff       → 200 → 400 → 800 ms (exponential)
//   5. Burst 20 instant  → all allowed
//   6. Post-burst rate   → only 10 tokens/s refill (not 20)
//
// Deterministic: uses a mock clock — no real sleeps.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <cstdint>

#include "../../core/src/rate_limiter.hpp"

// ── Mock clock ───────────────────────────────────────────────────────────────
static uint64_t g_mock_time_ms = 0;
static uint64_t mock_clock_ms() noexcept { return g_mock_time_ms; }

// ── Tiny test harness ────────────────────────────────────────────────────────
static int g_failures = 0;
#define CHECK(cond, name)                                                    \
    do {                                                                     \
        if (cond) { std::printf("  PASS %s\n", name); }                      \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);        \
               ++g_failures; }                                               \
    } while (0)

#define CHECK_EQ(actual, expected, name)                                     \
    do {                                                                     \
        if ((actual) == (expected)) {                                        \
            std::printf("  PASS %s\n", name);                                \
        } else {                                                             \
            std::printf("  FAIL %s: got %llu, expected %llu (line %d)\n",    \
                        name,                                                \
                        static_cast<unsigned long long>(actual),             \
                        static_cast<unsigned long long>(expected),          \
                        __LINE__);                                           \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// ── Test 1: 10 orders in 1 second → all allowed ─────────────────────────────
static void test_10_orders_per_second() {
    std::printf("test_10_orders_per_second\n");
    g_mock_time_ms = 0;

    RateLimitConfig cfg;
    cfg.orders_per_sec   = 10;
    cfg.burst_capacity   = 20;
    RateLimiter rl(cfg, mock_clock_ms);

    // Send 10 orders, one every 100 ms (exactly the rate).
    int allowed = 0;
    for (int i = 0; i < 10; ++i) {
        if (rl.can_send(RequestType::ORDER)) {
            ++allowed;
            rl.on_sent(RequestType::ORDER);
        }
        g_mock_time_ms += 100;  // advance 100 ms → 1 token refills
    }

    CHECK_EQ(allowed, 10, "10 orders in 1s all allowed");
}

// ── Test 2: 11th order rejected ───────────────────────────────────────────────
static void test_11th_order_rejected() {
    std::printf("test_11th_order_rejected\n");
    g_mock_time_ms = 0;

    RateLimitConfig cfg;
    cfg.orders_per_sec   = 10;
    cfg.burst_capacity   = 10;  // burst == rate so 10 instant orders drain the bucket
    RateLimiter rl(cfg, mock_clock_ms);

    // 10 instant orders consume all tokens.
    int allowed = 0;
    for (int i = 0; i < 10; ++i) {
        if (rl.can_send(RequestType::ORDER)) ++allowed;
    }
    CHECK_EQ(allowed, 10, "first 10 orders allowed");

    // 11th instant order — no time for refill → rejected.
    CHECK(!rl.can_send(RequestType::ORDER),
          "11th order rejected (burst exhausted)");
    CHECK_EQ(rl.wait_time_ms(RequestType::ORDER), 100,
             "wait time equals 1 token refill interval (100 ms)");
}

// ── Test 3: 30 cancels → all allowed ─────────────────────────────────────────
static void test_30_cancels_allowed() {
    std::printf("test_30_cancels_allowed\n");
    g_mock_time_ms = 0;

    RateLimitConfig cfg;
    cfg.cancels_per_sec   = 30;
    cfg.burst_capacity    = 30;  // enough burst for 30 instant cancels
    RateLimiter rl(cfg, mock_clock_ms);

    int allowed = 0;
    for (int i = 0; i < 30; ++i) {
        if (rl.can_send(RequestType::CANCEL)) {
            ++allowed;
            rl.on_sent(RequestType::CANCEL);
        }
    }
    CHECK_EQ(allowed, 30, "30 cancels all allowed");
}

// ── Test 4: 429 → exponential backoff 200 → 400 → 800 ms ──────────────────────
static void test_exponential_backoff() {
    std::printf("test_exponential_backoff\n");
    g_mock_time_ms = 0;

    RateLimitConfig cfg;  // backoff_base_ms=200, multiplier=2.0
    RateLimiter rl(cfg, mock_clock_ms);

    // First 429 → 200 ms
    rl.on_rate_limited(RequestType::ORDER, 0);
    CHECK_EQ(rl.backoff_ms(RequestType::ORDER), 200,
             "first 429 → backoff 200 ms");

    // During backoff: can_send must block.
    CHECK(!rl.can_send(RequestType::ORDER),
          "can_send blocked during backoff");

    // Second 429 → 400 ms
    rl.on_rate_limited(RequestType::ORDER, 0);
    CHECK_EQ(rl.backoff_ms(RequestType::ORDER), 400,
             "second 429 → backoff 400 ms");

    // Third 429 → 800 ms
    rl.on_rate_limited(RequestType::ORDER, 0);
    CHECK_EQ(rl.backoff_ms(RequestType::ORDER), 800,
             "third 429 → backoff 800 ms");

    // Backoff capped at max
    cfg.backoff_max_ms = 500;
    RateLimiter rl2(cfg, mock_clock_ms);
    for (int i = 0; i < 10; ++i) {
        rl2.on_rate_limited(RequestType::ORDER, 0);
    }
    CHECK_EQ(rl2.backoff_ms(RequestType::ORDER), 500,
             "backoff capped at max_ms");

    // After backoff expires + on_sent resets, can_send works again.
    g_mock_time_ms += 1000;  // advance past all backoff
    rl.on_rate_limited(RequestType::ORDER, 0);  // one more to be sure backoff is set
    g_mock_time_ms += rl.backoff_ms(RequestType::ORDER) + 1;
    rl.on_sent(RequestType::ORDER);  // success resets backoff
    CHECK_EQ(rl.backoff_ms(RequestType::ORDER), 0,
             "backoff resets after successful send");
}

// ── Test 5: Burst 20 instant orders → all allowed ────────────────────────────
static void test_burst_20_instant() {
    std::printf("test_burst_20_instant\n");
    g_mock_time_ms = 0;

    RateLimitConfig cfg;
    cfg.orders_per_sec  = 10;
    cfg.burst_capacity  = 20;
    RateLimiter rl(cfg, mock_clock_ms);

    int allowed = 0;
    for (int i = 0; i < 20; ++i) {
        if (rl.can_send(RequestType::ORDER)) {
            ++allowed;
            rl.on_sent(RequestType::ORDER);
        }
    }
    CHECK_EQ(allowed, 20, "20 instant orders allowed (burst)");

    // 21st instant order — no time for refill → rejected.
    CHECK(!rl.can_send(RequestType::ORDER),
          "21st instant order rejected (burst exhausted)");
}

// ── Test 6: Post-burst — rate reverts to 10 tokens/s ──────────────────────────
static void test_post_burst_rate() {
    std::printf("test_post_burst_rate\n");
    g_mock_time_ms = 0;

    RateLimitConfig cfg;
    cfg.orders_per_sec  = 10;
    cfg.burst_capacity  = 20;
    RateLimiter rl(cfg, mock_clock_ms);

    // Drain burst: 20 instant orders.
    int allowed = 0;
    for (int i = 0; i < 20; ++i) {
        if (rl.can_send(RequestType::ORDER)) ++allowed;
    }
    CHECK_EQ(allowed, 20, "burst drained with 20 orders");

    // After 1 second, only 10 tokens refill (rate = 10/s, NOT 20/s).
    g_mock_time_ms += 1000;  // 1 second

    int refilled_allowed = 0;
    for (int i = 0; i < 20; ++i) {
        if (rl.can_send(RequestType::ORDER)) {
            ++refilled_allowed;
            rl.on_sent(RequestType::ORDER);
        }
    }
    // 10 tokens refilled in 1s → only 10 more orders allowed.
    CHECK_EQ(refilled_allowed, 10,
             "post-burst: only 10 tokens/s refill (not 20)");

    // Verify status
    RateStatus st = rl.status();
    CHECK_EQ(st.tokens_remaining, 0,
             "tokens remaining is 0 after draining");
    CHECK_EQ(st.backoff_ms, 0,
             "no backoff after burst");
}

// ── Test 7: Clock backward jump does not wrap to UINT64_MAX ───────────────────
// SECURITY FIX (NEMESIS): backward jump → refill() must not overflow
void test_backward_clock_jump() {
    std::printf("[7] Backward clock jump — no overflow\n");

    RateLimitConfig cfg{};
    cfg.orders_per_sec = 10;
    cfg.burst_capacity = 20;
    RateLimiter rl(cfg, mock_clock_ms);

    // Set clock forward 10s (bucket should be full again after draining)
    g_mock_time_ms = 10000;
    // Drain all tokens
    for (int i = 0; i < 20; i++) {
        CHECK(rl.can_send(RequestType::ORDER), "can send during burst");
        rl.on_sent(RequestType::ORDER);
    }
    CHECK(!rl.can_send(RequestType::ORDER), "burst exhausted");

    // Simulate clock jumping backward by 5s
    g_mock_time_ms = 5000;
    // SECURITY: backward jump must NOT refill bucket (would bypass rate limit)
    CHECK(!rl.can_send(RequestType::ORDER),
          "Bucket NOT refilled after backward jump (no overflow bypass)");

    // After backward jump, move forward again — bucket should refill normally
    g_mock_time_ms = 5001;
    CHECK(!rl.can_send(RequestType::ORDER),
          "Still empty after backward+1ms (rate-limited by 10/s refill)");

    // Move forward by 1 second — should have 10 new tokens
    g_mock_time_ms = 11001;
    CHECK(rl.can_send(RequestType::ORDER),
          "10 tokens refill after clock passes original last_refill");

    std::printf("  PASS No overflow on backward jump\n\n");
}

// ── Test 8: Unbounded retry_ms is capped ──────────────────────────────────────
void test_retry_after_capped() {
    std::printf("[8] Server retry_after capped at backoff_max\n");

    RateLimitConfig cfg{};
    cfg.orders_per_sec = 10;
    cfg.burst_capacity = 5;
    cfg.backoff_base_ms = 200;
    cfg.backoff_max_ms = 10000;
    RateLimiter rl(cfg, mock_clock_ms);

    rl.can_send(RequestType::ORDER);
    // Server sends absurd Retry-After
    rl.on_rate_limited(RequestType::ORDER, UINT64_MAX);
    uint64_t bo = rl.backoff_ms(RequestType::ORDER);
    CHECK(bo <= 10000, "Retry-After capped at backoff_max_ms (10000)");

    std::printf("  PASS Retry-After capped\n\n");
}

// ── Main ─────────────────────────────────────────────────────────────────────
int main() {
    std::printf("⚡ RateLimiter — Unit Tests\n");
    std::printf("=========================================\n\n");

    test_10_orders_per_second();
    test_11th_order_rejected();
    test_30_cancels_allowed();
    test_exponential_backoff();
    test_burst_20_instant();
    test_post_burst_rate();
    test_backward_clock_jump();
    test_retry_after_capped();

    std::printf("\n=========================================\n");
    std::printf("📊 Results: %d check(s) failed\n", g_failures);
    return g_failures > 0 ? 1 : 0;
}
