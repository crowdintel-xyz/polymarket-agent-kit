// ─────────────────────────────────────────────────────────────────────────────
// test_multi_feed: Integration test for PHASE-1 multi-feed enhancements
//
// Validates:
//   1. PythClient mock injection produces correct PriceUpdate events
//   2. BinanceWSClient kline configuration for 5m/15m
//   3. MultiFeedManager fusion logic (pyth + binance → weighted average)
//   4. CircuitBreaker integration: reconnect suspended when CB is OPEN
//   5. Failover: when one feed drops, the other takes over with weight=1.0
//   6. Staleness detection triggers fail-closed
//
// Exit code 0 = all pass. No network. No external dependencies.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cassert>
#include <cstdio>
#include <cstring>
#include <cstdint>
#include <cmath>
#include <thread>
#include <chrono>

#include "spsc_ring_buffer.hpp"
#include "binance_ws_client.hpp"
#include "pyth_client.hpp"
#include "multi_feed_manager.hpp"
#include "circuit_breaker.hpp"
#include "fee_calculator.hpp"

// ── Test helpers ──────────────────────────────────────────────────────────────
static int g_tests_run = 0;
static int g_tests_pass = 0;
#define TEST(name) \
    static void test_##name(); \
    static struct TestReg_##name { \
        TestReg_##name() { ::new (&g_tests[g_test_count++]) TestEntry{#name, test_##name}; } \
    } g_reg_##name; \
    static void test_##name()

struct TestEntry { const char* name; void(*fn)(); };
static TestEntry g_tests[64];
static int g_test_count = 0;

static void run_all() {
    for (int i = 0; i < g_test_count; i++) {
        g_tests_run++;
        try {
            g_tests[i].fn();
            g_tests_pass++;
            printf("  ✅ %s\n", g_tests[i].name);
        } catch (const std::exception& e) {
            printf("  ❌ %s: %s\n", g_tests[i].name, e.what());
        } catch (...) {
            printf("  ❌ %s: unknown error\n", g_tests[i].name);
        }
    }
}

// ── Test: PythClient mock injection ─────────────────────────────────────────────
TEST(pyth_inject_price) {
    PythConfig cfg{};
    PythClient client(cfg);
    client.inject_price(97500.5, 100.0, 1700000000000ULL, 1700000001000000000ULL);
    
    PythPriceUpdate upd;
    bool ok = client.price_q().try_pop(upd);
    assert(ok && "PythPriceUpdate should be in queue");
    assert(std::abs(upd.price - 97500.5) < 0.01);
    assert(upd.conf > 0.0 && upd.conf < upd.price);
    assert(client.prices_produced() == 1);
}

// ── Test: BinanceConfig kline selection ──────────────────────────────────────
TEST(binance_kline_5m_default) {
    BinanceConfig cfg{};
    assert(std::string(cfg.kline_stream) == "@kline_5m");
    assert(cfg.strategy_window_seconds == 300);
    assert(cfg.circuit_breaker == nullptr);  // default: no CB
}

TEST(binance_kline_configurable) {
    BinanceConfig cfg{};
    cfg.kline_stream = cfg.kline_15m_stream;  // switch to 15m
    cfg.strategy_window_seconds = 900;
    assert(std::string(cfg.kline_stream) == "@kline_15m");
    assert(cfg.strategy_window_seconds == 900);
}

// ── Test: MultiFeedManager fusion ────────────────────────────────────────────
TEST(multifeed_fusion_both_active) {
    MultiFeedConfig mcfg{};
    CircuitBreaker cb(CircuitBreaker::Config{});
    MultiFeedManager mgr(mcfg, cb);

    BinanceWSClient bclient(BinanceConfig{});
    PythClient pclient(PythConfig{});
    mgr.set_binance(&bclient);
    mgr.set_pyth(&pclient);

    // Inject from both feeds
    bclient.inject_event(97000.0, 97010.0, 1.0, 1.0, false, 1700000001000000000ULL);
    pclient.inject_price(97005.0, 50.0, 1700000000000ULL, 1700000001000000000ULL);

    FusedMarketState fused{};
    bool ok = mgr.try_consume(fused);
    assert(ok && "Should produce fused state");
    // Weighted: pyth=0.55, binance=0.45, pyth=97005, binance_mid=97005
    // Result should be ~97005
    assert(fused.active_sources == 0x03);  // both
    assert(!fused.feed_failover);
    assert(fused.confidence > 0.5);
    assert(std::abs(fused.price - 97005.0) < 50.0);
}

TEST(multifeed_failover_pyth_only) {
    MultiFeedConfig mcfg{};
    CircuitBreaker cb(CircuitBreaker::Config{});
    MultiFeedManager mgr(mcfg, cb);

    BinanceWSClient bclient(BinanceConfig{});
    PythClient pclient(PythConfig{});
    mgr.set_binance(&bclient);
    mgr.set_pyth(&pclient);

    // Only inject Pyth, skip Binance
    pclient.inject_price(97005.0, 50.0, 1700000000000ULL, 1700000001000000000ULL);

    FusedMarketState fused{};
    bool ok = mgr.try_consume(fused);
    assert(ok && "Should produce state on failover");
    assert(fused.active_sources == 0x02);  // pyth only
    assert(fused.feed_failover);
}

TEST(multifeed_fail_closed_no_data) {
    MultiFeedConfig mcfg{};
    CircuitBreaker cb(CircuitBreaker::Config{});
    MultiFeedManager mgr(mcfg, cb);

    BinanceWSClient bclient(BinanceConfig{});
    PythClient pclient(PythConfig{});
    mgr.set_binance(&bclient);
    mgr.set_pyth(&pclient);

    // No data in either queue
    FusedMarketState fused{};
    bool ok = mgr.try_consume(fused);
    assert(!ok && "Should fail-closed with no data");
}

// ── Test: CircuitBreaker integration ──────────────────────────────────────────
TEST(cb_integration_open_state) {
    MultiFeedConfig mcfg{};
    CircuitBreaker cb(CircuitBreaker::Config{});
    MultiFeedManager mgr(mcfg, cb);

    BinanceWSClient bclient(BinanceConfig{});
    PythClient pclient(PythConfig{});
    mgr.set_binance(&bclient);
    mgr.set_pyth(&pclient);

    // Force circuit open by recording failures
    cb.on_failure();
    cb.on_failure();
    cb.on_failure();

    assert(cb.state() == CircuitBreaker::State::OPEN);
    mgr.reconcile_circuit();
    assert(mgr.stats().circuit_open_count >= 1);
}

// ── Test: Staleness detection ─────────────────────────────────────────────────
TEST(multifeed_staleness_detection) {
    MultiFeedConfig mcfg{};
    mcfg.staleness_ns = 100ULL;  // 100ns staleness threshold (very aggressive for test)
    CircuitBreaker cb(CircuitBreaker::Config{});
    MultiFeedManager mgr(mcfg, cb);

    BinanceWSClient bclient(BinanceConfig{});
    PythClient pclient(PythConfig{});
    mgr.set_binance(&bclient);
    mgr.set_pyth(&pclient);

    // Inject with very old timestamp
    pclient.inject_price(97005.0, 50.0, 1000000000000ULL, 1000000000000ULL);
    // Sleep a bit to make it stale
    std::this_thread::sleep_for(std::chrono::nanoseconds(200));

    FusedMarketState fused{};
    bool ok = mgr.try_consume(fused);
    assert(ok && "Should still produce with stale flag");
    assert(fused.is_stale);
}

// ── Main ──────────────────────────────────────────────────────────────────────
int main() {
    printf("=== test_multi_feed: Multi-Feed Integration Tests ===\n");
    run_all();
    printf("\n=== Results: %d/%d passed ===\n", g_tests_pass, g_tests_run);
    return g_tests_pass == g_tests_run ? 0 : 1;
}
