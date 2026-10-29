// ─────────────────────────────────────────────────────────────────────────────
// test_twap_bb: Known-answer tests for TwapBrownianBridge (Phase 1).
//
// Verifies the Brownian Bridge probability formula:
//   d = (A_t·t + S_t·τ − K·T_total) / (σ · √(τ³/3))
//   P(TWAP_final > K) = Φ(d)
//
// Plus trade evaluation logic (edge, fees, Kelly).
//
// Compile: g++ -std=c++20 -O2 -Wall -Wextra -Werror -I core/src
//          tests/unit/test_twap_bb.cpp -o /tmp/test_twap_bb
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstdio>
#include <cstdint>
#include <chrono>
#include "twap_brownian_bridge.hpp"

static int g_failures = 0;
static int g_tests    = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        ++g_tests;                                                            \
        if (cond) { std::printf("  PASS %s\n", name); }                       \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);          \
               ++g_failures; }                                                \
    } while (0)

#define CHECK_APPROX(a, b, eps, name)                                         \
    do {                                                                      \
        ++g_tests;                                                            \
        if (std::fabs((a) - (b)) <= (eps)) {                                   \
            std::printf("  PASS %s (%.6f)\n", name, (double)(a));             \
        } else {                                                              \
            std::printf("  FAIL %s: %f != %f (±%f) line %d\n",                 \
                        name, (double)(a), (double)(b), (double)(eps),        \
                        __LINE__);                                            \
            ++g_failures;                                                     \
        }                                                                     \
    } while (0)

// ── Test 1: S_t=K, τ=30s, σ=40% → P≈0.50 ± 0.01 ─────────────────────────────
// At-the-money with symmetric random walk: numerator of d is zero → P=Φ(0)=0.50
static void test_symmetric_atm() {
    std::printf("test_symmetric_atm\n");
    TwapBrownianBridge bb;
    bb.reset();
    bb.set_twap_so_far(67500.0);     // TWAP = strike
    bb.set_sigma_annual(0.40);       // 40 % annual vol

    // T_total=60, elapsed=30 → τ = 30s
    auto r = bb.compute(/*current_price=*/67500.0,
                        /*strike_price=*/67500.0,
                        /*window_total=*/60.0,
                        /*elapsed=*/30.0);

    std::printf("  P=%.6f (expect ≈0.50)\n", r.p_up);
    CHECK(std::fabs(r.p_up - 0.50) < 0.01, "P ≈ 0.50 when S_t = K");
    CHECK_APPROX(r.twap_so_far, 67500.0, 0.01, "twap_so_far");
    CHECK_APPROX(r.time_remaining_sec, 30.0, 0.01, "τ = 30s");
    CHECK(!r.is_decided, "not decided (P ≈ 0.5)");
}

// ── Test 2: S_t=67600, K=67500, τ=5s, σ=40% → P > 0.95 ───────────────────────
// Price 100 bps above strike with 5s left: d → +∞, P → 1.0 (clamped 0.99)
static void test_price_above_short_tau() {
    std::printf("test_price_above_short_tau\n");
    TwapBrownianBridge bb;
    bb.reset();
    bb.set_twap_so_far(67600.0);
    bb.set_sigma_annual(0.40);

    auto r = bb.compute(67600.0, 67500.0, 60.0, 55.0);  // τ = 5s

    std::printf("  P=%.6f (expect >0.95)\n", r.p_up);
    CHECK(r.p_up > 0.95, "P > 0.95 (price above strike, τ=5s)");
    CHECK(r.is_decided, "is_decided = true");
}

// ── Test 3: S_t=67400, K=67600, τ=50s, σ=30% → P < 0.15 ──────────────────────
// Price 300 bps below strike with 50s remaining and lower vol → P → 0
static void test_price_below_medium_tau() {
    std::printf("test_price_below_medium_tau\n");
    TwapBrownianBridge bb;
    bb.reset();
    bb.set_twap_so_far(67400.0);
    bb.set_sigma_annual(0.30);

    auto r = bb.compute(67400.0, 67600.0, 60.0, 10.0);  // τ = 50s

    std::printf("  P=%.6f (expect <0.15)\n", r.p_up);
    CHECK(r.p_up < 0.15, "P < 0.15 (price below strike, τ=50s)");
    CHECK(r.is_decided, "is_decided = true");
}

// ── Test 4: τ=0 → P=1.0 when TWAP > K (deterministic) ───────────────────────
static void test_deterministic_zero_tau() {
    std::printf("test_deterministic_zero_tau\n");
    TwapBrownianBridge bb;
    bb.reset();
    bb.set_twap_so_far(67600.0);     // TWAP > K
    bb.set_sigma_annual(0.40);

    auto r = bb.compute(67600.0, 67500.0, 60.0, 60.0);  // τ = 0

    std::printf("  P=%.6f (expect 1.0)\n", r.p_up);
    CHECK_APPROX(r.p_up, 1.0, 1e-12, "P = 1.0 (τ=0, TWAP > K)");
    CHECK(r.is_decided, "is_decided = true");
    CHECK_APPROX(r.time_remaining_sec, 0.0, 1e-12, "τ = 0");

    // Also verify TWAP < K → P = 0.0
    bb.set_twap_so_far(67400.0);   // lower TWAP below strike
    auto r2 = bb.compute(67400.0, 67500.0, 60.0, 60.0);
    CHECK_APPROX(r2.p_up, 0.0, 1e-12, "P = 0.0 (τ=0, TWAP < K)");
}

// ── Test 5: σ=200% (clamped max), τ=55s → 0.15 < P < 0.85 ─────────────────────
// With maximum vol and S_t = K, uncertainty keeps P near 0.5
static void test_high_vol_bounded() {
    std::printf("test_high_vol_bounded\n");
    TwapBrownianBridge bb;
    bb.reset();
    bb.set_twap_so_far(67500.0);
    bb.set_sigma_annual(2.00);       // 200 % → clamped at max

    auto r = bb.compute(67500.0, 67500.0, 60.0, 5.0);  // τ = 55s

    std::printf("  P=%.6f, σ=%.4f (expect 0.15<P<0.85)\n", r.p_up, r.sigma_annual);
    CHECK(r.p_up > 0.15 && r.p_up < 0.85,
          "P in [0.15, 0.85] (high vol, S_t=K)");
    CHECK_APPROX(r.sigma_annual, 2.00, 1e-12, "sigma clamped to 2.00");
}

// ── Test 6: p_up=0.55, market=0.50, taker → should_trade=true, edge=0.032
// fee = 0.072 × 0.50 × 0.50 = 0.018
// edge = 0.55 − 0.50 − 0.018 = 0.032
static void test_trade_eval_positive_taker() {
    std::printf("test_trade_eval_positive_taker\n");
    TwapBrownianBridge bb;
    bb.reset();

    auto td = bb.evaluate_trade(/*p_up=*/0.55,
                                /*market_price_up=*/0.50,
                                /*bankroll=*/1000.0,
                                /*is_taker=*/true);

    std::printf("  edge=%.4f, fee=%.4f, trade=%d (expect edge=0.032)\n",
                td.edge_after_fees, td.fee_cost, td.should_trade);
    CHECK(td.should_trade == true, "should_trade = true");
    CHECK_APPROX(td.edge_after_fees, 0.032, 1e-9, "edge = 0.032");
    CHECK_APPROX(td.fee_cost, 0.018, 1e-9, "fee = 0.018");
    CHECK(td.kelly_fraction > 0.0, "kelly > 0");
}

// ── Test 7: p_up=0.51, market=0.50, taker → should_trade=false
// fee = 0.018, edge = 0.51 − 0.50 − 0.018 = −0.008 (below threshold)
static void test_trade_eval_negative_taker() {
    std::printf("test_trade_eval_negative_taker\n");
    TwapBrownianBridge bb;
    bb.reset();

    auto td = bb.evaluate_trade(0.51, 0.50, 1000.0, true);

    std::printf("  edge=%.4f, trade=%d (expect no trade)\n",
                td.edge_after_fees, td.should_trade);
    CHECK(td.should_trade == false, "should_trade = false (edge < threshold)");
    CHECK_APPROX(td.edge_after_fees, -0.008, 1e-9, "edge = -0.008");
}

// ── Test 8: p_up=0.52, market=0.50, maker → should_trade=true
// fee = 0, edge = 0.52 − 0.50 = 0.02 (above threshold)
static void test_trade_eval_maker() {
    std::printf("test_trade_eval_maker\n");
    TwapBrownianBridge bb;
    bb.reset();

    auto td = bb.evaluate_trade(0.52, 0.50, 1000.0, false);

    std::printf("  edge=%.4f, fee=%.4f, trade=%d (expect trade)\n",
                td.edge_after_fees, td.fee_cost, td.should_trade);
    CHECK(td.should_trade == true, "should_trade = true (maker, no fee)");
    CHECK_APPROX(td.edge_after_fees, 0.02, 1e-9, "edge = 0.02");
    CHECK_APPROX(td.fee_cost, 0.0, 1e-12, "fee = 0 (maker)");
}

// ── Latency benchmark ──────────────────────────────────────────────────────────
static void test_latency() {
    std::printf("latency_benchmark\n");
    TwapBrownianBridge bb;
    bb.reset();
    bb.set_twap_so_far(67500.0);
    bb.set_sigma_annual(0.40);

    volatile double sink = 0.0;
    auto start = std::chrono::high_resolution_clock::now();
    for (int i = 0; i < 10000; i++) {
        // Vary input slightly so compiler cannot hoist the computation
        double price = 67500.0 + static_cast<double>(i % 3) * 0.01;
        sink = bb.compute(price, 67500.0, 60.0, 30.0).p_up;
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto ns_total = std::chrono::duration_cast<std::chrono::nanoseconds>(
                        end - start).count();
    double ns_per_call = static_cast<double>(ns_total) / 10000.0;
    std::printf("  10k compute() calls: %.1f ns/call (P=%.6f)\n",
                ns_per_call, (double)sink);
    CHECK(ns_per_call < 500.0, "compute() < 500 ns per call");
}

int main() {
    std::printf("=== TwapBrownianBridge KAT (10 tests) ===\n");
    test_symmetric_atm();                // Test 1
    test_price_above_short_tau();        // Test 2
    test_price_below_medium_tau();       // Test 3
    test_deterministic_zero_tau();       // Test 4
    test_high_vol_bounded();             // Test 5
    test_trade_eval_positive_taker();    // Test 6
    test_trade_eval_negative_taker();    // Test 7
    test_trade_eval_maker();             // Test 8
    test_latency();                      // Benchmark
    std::printf("\n=== %s (%d/%d passed) ===\n",
                g_failures ? "FAILED" : "ALL PASS",
                g_tests - g_failures, g_tests);
    return g_failures ? 1 : 0;
}
