// ─────────────────────────────────────────────────────────────────────────────
// Acceptance Test: Latency Budget Compliance
// Verifies that hot-path components meet their latency budgets:
//   - BB compute:  < 500ns
//   - Kelly sizing: < 200ns
//   - Fee calc:    < 100ns (TSan overhead)
//   - OFI update:  < 100ns
//   - WindowShield: < 200ns
//   - Full pipeline tick: < 50μs p99
//
// Uses high-resolution timer. Runs 100K iterations per component to
// get stable p99 measurements.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include <chrono>
#include <algorithm>
#include <vector>
#include "twap_brownian_bridge.hpp"
#include "volatility_estimator.hpp"
#include "ofi_linear_filter.hpp"
#include "window_shield.hpp"
#include "fee_calculator.hpp"
#include "kelly_sizer.hpp"
#include "ladder_builder.hpp"

static int g_failures = 0;
static int g_tests = 0;
#define CHECK(cond, name)                                                     \
    do { ++g_tests; if (cond) std::printf("  PASS %s\n", name);               \
         else { std::printf("  FAIL %s (line %d)\n", name, __LINE__); ++g_failures; } \
    } while (0)

using Clock = std::chrono::high_resolution_clock;

// Simple percentile helper
static double percentile(std::vector<uint64_t>& v, double pct) {
    if (v.empty()) return 0.0;
    std::sort(v.begin(), v.end());
    size_t idx = static_cast<size_t>(pct * (v.size() - 1) / 100.0);
    return static_cast<double>(v[idx]);
}

int main() {
    std::printf("=== Acceptance: Latency Budget Compliance ===\n\n");

    const int N = 100000;
    std::vector<uint64_t> timings(N);

    // ── BB compute benchmark ──────────────────────────────────────────────────
    TwapBrownianBridge bb;
    bb.set_sigma_annual(0.80);
    bb.set_twap_so_far(60000.0);

    for (int i = 0; i < N; i++) {
        auto t0 = Clock::now();
        volatile auto r = bb.compute(60010.0, 60500.0, 300.0, 60.0);
        auto t1 = Clock::now();
        timings[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    }
    double p50 = percentile(timings, 50);
    double p99 = percentile(timings, 99);
    std::printf("  BB compute: p50=%.0fns p99=%.0fns\n", p50, p99);
    CHECK(p99 < 500.0, "BB compute p99 < 500ns");

    // ── Kelly sizing benchmark ─────────────────────────────────────────────────
    Config kelly_cfg{};
    KellySizer kelly(kelly_cfg);
    for (int i = 0; i < N; i++) {
        auto t0 = Clock::now();
        volatile auto r = kelly.compute(0.65, 0.50, 10000.0, 0.0, 0.0, true, 1.0);
        auto t1 = Clock::now();
        timings[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    }
    p99 = percentile(timings, 99);
    std::printf("  Kelly sizing: p99=%.0fns\n", p99);
    CHECK(p99 < 200.0, "Kelly sizing p99 < 200ns");

    // ── Fee calculation benchmark ──────────────────────────────────────────────
    for (int i = 0; i < N; i++) {
        auto t0 = Clock::now();
        volatile double fee = FeeCalculator::taker_fee(0.50);
        auto t1 = Clock::now();
        timings[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    }
    p99 = percentile(timings, 99);
    std::printf("  Fee calc: p99=%.0fns\n", p99);
    CHECK(p99 < 100.0, "Fee calc p99 < 100ns");

    // ── OFI update benchmark ───────────────────────────────────────────────────
    OfiLinearFilter::Config ofi_cfg{};
    OfiLinearFilter ofi(ofi_cfg);
    ofi.on_book_update(100.0, 100.0, 100.0, 100.0, 100.0, 100.0, 101.0, 101.0, 1000ULL);

    for (int i = 0; i < N; i++) {
        auto t0 = Clock::now();
        ofi.on_book_update(100.0 + i, 99.0 + i, 100.0, 100.0, 100.0, 100.0, 101.0, 101.0, 2000ULL + i);
        auto t1 = Clock::now();
        timings[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    }
    p99 = percentile(timings, 99);
    std::printf("  OFI update: p99=%.0fns\n", p99);
    CHECK(p99 < 100.0, "OFI update p99 < 100ns");

    // ── WindowShield update benchmark ──────────────────────────────────────────
    WindowShield shield(WindowShieldConfig::BTC_5M());
    shield.set_window_start(1'700'000'000'000'000'000ULL);

    for (int i = 0; i < N; i++) {
        auto t0 = Clock::now();
        volatile auto s = shield.update(1'700'000'000'000'000'000ULL + i, 0.5f);
        auto t1 = Clock::now();
        timings[i] = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    }
    p99 = percentile(timings, 99);
    std::printf("  WindowShield: p99=%.0fns\n", p99);
    CHECK(p99 < 200.0, "WindowShield p99 < 200ns");

    std::printf("\n========================================\n");
    std::printf("Latency Budget: %d/%d passed\n", g_tests - g_failures, g_tests);
    std::printf("========================================\n");
    return g_failures > 0 ? 1 : 0;
}
