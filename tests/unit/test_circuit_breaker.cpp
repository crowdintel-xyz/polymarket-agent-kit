// ─────────────────────────────────────────────────────────────────────────────
// test_circuit_breaker — Phase 3 unit tests for the circuit breaker state machine
//
// Covered scenarios:
//   1. 3 failures → OPEN
//   2. OPEN → allow_request() returns false
//   3. 5 s → HALF_OPEN → 1 trial request allowed
//   4. HALF_OPEN → 2 successes → CLOSED
//   5. CB_OPEN → trading_mode() == CLOSE_ONLY (WindowShield integration)
//
// Deterministic: uses a mock clock — no real sleeps.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <cstdint>

#include "../../core/src/circuit_breaker.hpp"

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
            std::printf("  FAIL %s: got %u, expected %u (line %d)\n",        \
                        name,                                                \
                        static_cast<unsigned>(actual),                       \
                        static_cast<unsigned>(expected),                     \
                        __LINE__);                                           \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// ── Test 1: 3 failures → OPEN ────────────────────────────────────────────────
static void test_three_failures_open() {
    std::printf("test_three_failures_open\n");
    g_mock_time_ms = 0;

    CircuitBreaker::Config cfg;
    cfg.failure_threshold  = 3;
    cfg.open_duration_ms   = 5000;
    cfg.half_open_max      = 1;
    cfg.success_threshold  = 2;

    CircuitBreaker cb(cfg, mock_clock_ms);

    CHECK(cb.state() == CircuitBreaker::State::CLOSED,
          "starts CLOSED");

    // Two failures — still CLOSED.
    cb.on_failure();
    CHECK(cb.state() == CircuitBreaker::State::CLOSED,
          "after 1 failure still CLOSED");
    cb.on_failure();
    CHECK(cb.state() == CircuitBreaker::State::CLOSED,
          "after 2 failures still CLOSED");

    // Third failure → OPEN.
    cb.on_failure();
    CHECK(cb.state() == CircuitBreaker::State::OPEN,
          "after 3 failures → OPEN");
}

// ── Test 2: OPEN → allow_request() returns false ─────────────────────────────
static void test_open_blocks_requests() {
    std::printf("test_open_blocks_requests\n");
    g_mock_time_ms = 0;

    CircuitBreaker::Config cfg;  // default: threshold=3, open=5s
    CircuitBreaker cb(cfg, mock_clock_ms);

    // Trip the circuit.
    cb.on_failure();
    cb.on_failure();
    cb.on_failure();
    CHECK(cb.state() == CircuitBreaker::State::OPEN,
          "circuit is OPEN");

    // In OPEN state, all requests must be blocked.
    CHECK(!cb.allow_request(),
          "allow_request() = false when OPEN");
    CHECK(!cb.allow_request(),
          "second allow_request() = false when OPEN");

    // ms_until_retry should be > 0.
    CHECK(cb.ms_until_retry() > 0,
          "ms_until_retry > 0 when OPEN");
}

// ── Test 3: 5 s → HALF_OPEN → 1 trial request allowed ────────────────────────
static void test_half_open_allows_one() {
    std::printf("test_half_open_allows_one\n");
    g_mock_time_ms = 0;

    CircuitBreaker::Config cfg;
    cfg.open_duration_ms = 5000;
    cfg.half_open_max    = 1;
    CircuitBreaker cb(cfg, mock_clock_ms);

    // Trip the circuit.
    cb.on_failure();
    cb.on_failure();
    cb.on_failure();
    CHECK(cb.state() == CircuitBreaker::State::OPEN, "OPEN after 3 failures");

    // Advance past the TTL.
    g_mock_time_ms += 5000;

    // allow_request triggers the OPEN → HALF_OPEN transition and permits 1 request.
    CHECK(cb.allow_request(),
          "allow_request() = true after TTL (HALF_OPEN)");
    CHECK(cb.state() == CircuitBreaker::State::HALF_OPEN,
          "state is HALF_OPEN after TTL");

    // Second request immediately after — HALF_OPEN allows only 1.
    CHECK(!cb.allow_request(),
          "second allow_request() = false in HALF_OPEN (max=1)");
}

// ── Test 4: HALF_OPEN → 2 successes → CLOSED ─────────────────────────────────
static void test_half_open_two_successes_closed() {
    std::printf("test_half_open_two_successes_closed\n");
    g_mock_time_ms = 0;

    CircuitBreaker::Config cfg;
    cfg.open_duration_ms  = 5000;
    cfg.half_open_max     = 2;     // allow 2 trial requests
    cfg.success_threshold = 2;
    CircuitBreaker cb(cfg, mock_clock_ms);

    // Trip → OPEN.
    cb.on_failure();
    cb.on_failure();
    cb.on_failure();
    CHECK(cb.state() == CircuitBreaker::State::OPEN, "OPEN");

    // Advance TTL → allow first trial request.
    g_mock_time_ms += 5000;
    CHECK(cb.allow_request(), "first half-open request allowed");
    CHECK(cb.state() == CircuitBreaker::State::HALF_OPEN, "HALF_OPEN");

    // First success → still HALF_OPEN (need 2).
    cb.on_success();
    CHECK(cb.state() == CircuitBreaker::State::HALF_OPEN,
          "still HALF_OPEN after 1 success");

    // Second trial request + success → CLOSED.
    CHECK(cb.allow_request(), "second half-open request allowed");
    cb.on_success();
    CHECK(cb.state() == CircuitBreaker::State::CLOSED,
          "CLOSED after 2 successes");
}

// ── Test 5: CB_OPEN → trading_mode() == CLOSE_ONLY ───────────────────────────
// When the circuit is OPEN, the trading mode must be CLOSE_ONLY so the
// WindowShield forces close-only behaviour (no new positions).
static void test_open_forces_close_only() {
    std::printf("test_open_forces_close_only\n");
    g_mock_time_ms = 0;

    CircuitBreaker::Config cfg;
    CircuitBreaker cb(cfg, mock_clock_ms);

    // Closed → normal trading.
    CHECK(cb.trading_mode() == TradingMode::NORMAL,
          "CLOSED → NORMAL trading mode");

    // Trip circuit.
    cb.on_failure();
    cb.on_failure();
    cb.on_failure();
    CHECK(cb.state() == CircuitBreaker::State::OPEN, "OPEN after 3 failures");

    // OPEN → CLOSE_ONLY (WindowShield integration point).
    CHECK(cb.trading_mode() == TradingMode::CLOSE_ONLY,
          "OPEN → CLOSE_ONLY (no new orders, no cancels)");

    // HALF_OPEN → also CLOSE_ONLY (probing only).
    g_mock_time_ms += 5000;
    cb.allow_request();
    CHECK(cb.state() == CircuitBreaker::State::HALF_OPEN, "HALF_OPEN after TTL");
    CHECK(cb.trading_mode() == TradingMode::CLOSE_ONLY,
          "HALF_OPEN → CLOSE_ONLY (probing)");

    // Recovery → NORMAL.
    cb.on_success();
    cb.on_success();
    CHECK(cb.trading_mode() == TradingMode::NORMAL,
          "CLOSED → NORMAL after recovery");
}

// ── Main ─────────────────────────────────────────────────────────────────────
int main() {
    std::printf("🛡️ CircuitBreaker — Unit Tests\n");
    std::printf("=========================================\n\n");

    test_three_failures_open();
    test_open_blocks_requests();
    test_half_open_allows_one();
    test_half_open_two_successes_closed();
    test_open_forces_close_only();

    std::printf("\n=========================================\n");
    std::printf("📊 Results: %d check(s) failed\n", g_failures);
    return g_failures > 0 ? 1 : 0;
}
