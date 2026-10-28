// ─────────────────────────────────────────────────────────────────────────────
// test_vol_estimator: Tests for VolatilityEstimator (Phase 1).
//
// Tests:
//   1. Constant volatility → converges to known value
//   2. SHOCK regime (vol_fast > 2×vol_slow) via direct setter
//   3. CALM regime (vol_fast < vol_slow/2) via direct setter
//   4. Clamp bounds [0.15, 2.00]
//
// Compile: g++ -std=c++20 -O2 -Wall -Wextra -Werror -I core/src
//          tests/unit/test_vol_estimator.cpp -o /tmp/test_vol_estimator
// ─────────────────────────────────────────────────────────────────────────────

#include <chrono>
#include <cmath>
#include <cstdio>
#include "volatility_estimator.hpp"

static int g_fail = 0;
static int g_pass = 0;
static int g_total = 0;

#define CHECK(cond, name) do { \
    ++g_total; \
    if (cond) { ++g_pass; std::printf("  PASS %s\n", name); } \
    else { ++g_fail; std::printf("  FAIL %s (line %d)\n", name, __LINE__); } \
} while(0)

// ── Constants ──────────────────────────────────────────────────────────────────
static constexpr double SECS_PER_YEAR = 365.0 * 24.0 * 3600.0;
static constexpr double VOL_TARGET    = 0.20;  // 20 % annualised

// ── Test 1: Constant volatility converges ─────────────────────────────────────
// Feeds alternating ±r where r gives exactly vol=0.20.
// After EWMA convergence, effective_vol() should return ≈ 0.20.
static void test_converge() {
    std::printf("test_converge\n");
    VolatilityEstimator ve(VolatilityEstimator::Config{});

    // Compute log_return magnitude that yields annualised var = 0.20² = 0.04
    // ret_sq_annual = log_ret² × SECS_PER_YEAR / dt
    // => log_ret = vol / sqrt(SECS_PER_YEAR)  when dt=1
    double log_ret = VOL_TARGET / std::sqrt(SECS_PER_YEAR);
    std::printf("  log_ret = %.10f\n", log_ret);

    // Feed 300 alternating returns (both EWMAs fully converged)
    for (int i = 0; i < 300; i++) {
        double r = (i % 2 == 0) ? log_ret : -log_ret;
        ve.on_log_return(r, 1.0);
    }

    double vol = ve.effective_vol();
    std::printf("  vol = %.6f (expect ≈ %.2f)\n", vol, VOL_TARGET);

    CHECK(std::fabs(vol - VOL_TARGET) < 0.01, "volatility converges to 0.20");
    CHECK(ve.current_regime() == VolatilityEstimator::Regime::NORMAL,
          "NORMAL regime when vol_fast ≈ vol_slow");
    CHECK(ve.regime_vol_multiplier() == 1.0, "NORMAL → 1.0x multiplier");
}

// ── Test 2: SHOCK regime ───────────────────────────────────────────────────────
// With λ_fast=0.94, λ_slow=0.98, max vol_fast/vol_slow ratio from returns
// is sqrt(3)≈1.73 < 2.0.  Use setters to inject directly.
static void test_shock_regime() {
    std::printf("test_shock_regime\n");
    VolatilityEstimator ve(VolatilityEstimator::Config{});

    // var_fast = 5.0 → vol_fast = 2.236
    // var_slow = 1.0 → vol_slow = 1.0
    // ratio  = 2.236 / 1.0 = 2.236 > 2.0 → SHOCK
    ve.set_var_fast(5.0);
    ve.set_var_slow(1.0);

    auto regime = ve.current_regime();
    std::printf("  regime = %d (expect SHOCK=%d)\n",
                static_cast<int>(regime),
                static_cast<int>(VolatilityEstimator::Regime::SHOCK));

    CHECK(regime == VolatilityEstimator::Regime::SHOCK,
          "vol_fast > 2×vol_slow → SHOCK regime");
    CHECK(ve.regime_vol_multiplier() == 1.5, "SHOCK → 1.5× multiplier");
}

// ── Test 3: CALM regime ───────────────────────────────────────────────────────
// Inject vol_fast << vol_slow to trigger CALM.
static void test_calm_regime() {
    std::printf("test_calm_regime\n");
    VolatilityEstimator ve(VolatilityEstimator::Config{});

    // var_fast = 0.16 → vol_fast = 0.4
    // var_slow = 1.0  → vol_slow = 1.0
    // ratio  = 0.4 / 1.0 = 0.4 < 0.5 → CALM
    ve.set_var_fast(0.16);
    ve.set_var_slow(1.0);

    auto regime = ve.current_regime();
    std::printf("  regime = %d (expect CALM=%d)\n",
                static_cast<int>(regime),
                static_cast<int>(VolatilityEstimator::Regime::CALM));

    CHECK(regime == VolatilityEstimator::Regime::CALM,
          "vol_fast < vol_slow/2 → CALM regime");
    CHECK(ve.regime_vol_multiplier() == 0.8, "CALM → 0.8× multiplier");
}

// ── Test 4: Clamp bounds ───────────────────────────────────────────────────────
static void test_clamp_bounds() {
    std::printf("test_clamp_bounds\n");
    VolatilityEstimator::Config cfg;
    cfg.vol_min = 0.15;
    cfg.vol_max = 2.00;
    VolatilityEstimator ve(cfg);

    // Upper clamp: huge returns push vol → ∞, should be capped at 2.00
    for (int i = 0; i < 50; i++) {
        double r = (i % 2 == 0) ? 5.0 : -5.0;  // 500% log return
        ve.on_log_return(r, 1.0);
    }
    double vol_high = ve.effective_vol();
    std::printf("  vol (high) = %.6f (expect 2.00)\n", vol_high);
    CHECK(vol_high <= 2.00 + 1e-9, "volatility capped at 2.00");

    // Lower clamp: seed with normal vol, then decay with zero returns
    ve.reset();
    double log_ret = 0.30 / std::sqrt(SECS_PER_YEAR);  // ~30% annualised
    for (int i = 0; i < 300; i++) {
        double r = (i % 2 == 0) ? log_ret : -log_ret;
        ve.on_log_return(r, 1.0);
    }
    double vol_before = ve.effective_vol();
    std::printf("  vol (before decay) = %.6f\n", vol_before);
    CHECK(vol_before > 0.15, "vol before decay > floor");

    // Now feed zero returns — variance decays exponentially
    for (int i = 0; i < 2000; i++) {
        ve.on_log_return(0.0, 1.0);
    }
    double vol_low = ve.effective_vol();
    std::printf("  vol (after decay)  = %.6f (expect 0.15)\n", vol_low);
    CHECK(vol_low >= 0.15 - 1e-9, "volatility floored at 0.15");
}

// ── Latency benchmark ──────────────────────────────────────────────────────────
// Skipped under ASan/UBSan where instrumentation inflates timings.
static void test_latency() {
    std::printf("test_latency\n");
#if defined(__SANITIZE_ADDRESS__) || defined(__SANITIZE_UNDEFINED__)
    std::printf("  benchmark skipped (sanitizer build — overhead inflates timings)\n");
    CHECK(true, "latency benchmark skipped under sanitizers");
#else
    VolatilityEstimator ve(VolatilityEstimator::Config{});
    double log_ret = 0.20 / std::sqrt(SECS_PER_YEAR);

    auto start = std::chrono::high_resolution_clock::now();
    volatile double sink = 0.0;
    for (int i = 0; i < 10000; i++) {
        double r = (i % 2 == 0) ? log_ret : -log_ret;
        ve.on_log_return(r + static_cast<double>(i % 3) * 1e-12, 1.0);
        sink = ve.effective_vol();
    }
    auto end = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
    double ns_per_call = static_cast<double>(ns) / 10000.0;
    std::printf("  10k updates: %.1f ns/call (sink=%.6f)\n", ns_per_call, (double)sink);
    CHECK(ns_per_call < 100.0, "on_log_return() < 100 ns per call");
#endif
}

int main() {
    std::printf("=== VolatilityEstimator Tests (4 tests) ===\n");
    test_converge();          // Test 1
    test_shock_regime();      // Test 2
    test_calm_regime();       // Test 3
    test_clamp_bounds();      // Test 4
    test_latency();           // Benchmark
    std::printf("\n=== %s (%d/%d passed) ===\n",
                g_fail ? "FAILED" : "ALL PASS", g_pass, g_total);
    return g_fail ? 1 : 0;
}
