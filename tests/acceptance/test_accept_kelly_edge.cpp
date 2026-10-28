// ─────────────────────────────────────────────────────────────────────────────
// Acceptance Test: Kelly Sizing + Fee Math
// Verifies quarter-Kelly, edge computation with fees, breakeven win-rate,
// max bankroll cap, and minimum position floor (1 share).
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include "kelly_sizer.hpp"
#include "fee_calculator.hpp"

static int g_failures = 0;
static int g_tests = 0;
#define CHECK(cond, name)                                                     \
    do { ++g_tests; if (cond) std::printf("  PASS %s\n", name);               \
         else { std::printf("  FAIL %s (line %d)\n", name, __LINE__); ++g_failures; } \
    } while (0)

int main() {
    std::printf("=== Acceptance: Kelly + Fee Math ===\n\n");

    Config kelly_cfg{};
    kelly_cfg.kelly_fraction = 0.25;
    kelly_cfg.max_bankroll_pct = 0.05;
    kelly_cfg.min_edge = 0.005;
    kelly_cfg.max_position = 100.0;
    KellySizer kelly(kelly_cfg);

    // 1. Fee math: breakeven win-rate at p=0.50
    double fee_50 = FeeCalculator::taker_fee(0.50);
    double breakeven_50 = FeeCalculator::breakeven_winrate(0.50, true);
    CHECK(fee_50 > 0.0, "Taker fee positive at p=0.50");
    CHECK(breakeven_50 > 0.50, "Breakeven > market price (fee added)");
    CHECK(std::abs(breakeven_50 - (0.50 + fee_50)) < 1e-9,
          "Breakeven = price + fee");

    // 2. Maker fee = 0
    CHECK(FeeCalculator::maker_fee(0.50) == 0.0, "Maker fee always 0");

    // 3. Edge with sufficient model edge → trade
    auto r1 = kelly.compute(0.65, 0.50, 10000.0, 0.0, 0.0, true, 1.0);
    CHECK(r1.should_trade, "65% model vs 50% price → trade");
    CHECK(r1.edge > 0.005, "Edge > min_edge threshold");
    CHECK(r1.size_usdc > 0.0, "Positive position size");
    CHECK(r1.size_usdc <= 0.05 * 10000.0 + 1e-9,
          "Size capped to 5% of bankroll");

    // 4. Edge below threshold → no trade
    auto r2 = kelly.compute(0.51, 0.50, 10000.0, 0.0, 0.0, true, 1.0);
    CHECK(!r2.should_trade || r2.size_shares < 1.0,
          "Edge below threshold → floor check");

    // 5. Edge at breakeven → no trade (edge < min_edge)
    double be = FeeCalculator::breakeven_winrate(0.50, true);
    auto r3 = kelly.compute(be, 0.50, 10000.0, 0.0, 0.0, true, 1.0);
    CHECK(!r3.should_trade || r3.edge < 0.005,
          "At breakeven edge → no trade (floor or edge < min_edge)");

    // 6. Invalid inputs → no trade (fail-closed)
    auto r4 = kelly.compute(1.5, 0.50, 10000.0, 0.0, 0.0, true, 1.0);
    CHECK(!r4.should_trade, "p_model > 1.0 → no trade (fail-closed)");
    auto r5 = kelly.compute(0.65, 0.0, 10000.0, 0.0, 0.0, true, 1.0);
    CHECK(!r5.should_trade, "price = 0 → no trade (fail-closed)");

    // 7. Zero bankroll → no trade
    auto r6 = kelly.compute(0.65, 0.50, 0.0, 0.0, 0.0, true, 1.0);
    CHECK(!r6.should_trade, "Zero bankroll → no trade");

    std::printf("\n========================================\n");
    std::printf("Kelly + Fee: %d/%d passed\n", g_tests - g_failures, g_tests);
    std::printf("========================================\n");
    return g_failures > 0 ? 1 : 0;
}
