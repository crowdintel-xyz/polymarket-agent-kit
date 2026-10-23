// ─────────────────────────────────────────────────────────────────────────────
// test_fees: unit tests for FeeCalculator (Phase 4, Tarea 1).
//
// Verifies deterministic fee math:
//   taker_fee = 0.072 · p · (1 − p)
//   maker_fee = 0
//   breakeven_winrate = price + fee
//   edge_after_fees = p_model − market_price − fee
//
// Exit code 0 = all pass. No external test framework.
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstdio>
#include "../../core/src/fee_calculator.hpp"

static int g_failures = 0;
static int g_tests = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        ++g_tests;                                                            \
        if (cond) { std::printf("  PASS %s\n", name); }                       \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);          \
               ++g_failures; }                                                \
    } while (0)

static bool approx(double a, double b, double eps = 1e-9) {
    return std::fabs(a - b) <= eps;
}

// ── FeeCalculator tests ────────────────────────────────────────────────────────

static void test_taker_fee_center() {
    std::printf("taker_fee_center\n");
    CHECK(approx(FeeCalculator::taker_fee(0.50), 0.018),
          "taker_fee(0.50) = 0.072*0.5*0.5 = 0.018");
}

static void test_taker_fee_low() {
    std::printf("taker_fee_low\n");
    // 0.072 * 0.10 * 0.90 = 0.00648
    CHECK(approx(FeeCalculator::taker_fee(0.10), 0.00648),
          "taker_fee(0.10) = 0.072*0.1*0.9 = 0.00648");
}

static void test_taker_fee_high() {
    std::printf("taker_fee_high\n");
    // 0.072 * 0.90 * 0.10 = 0.00648 (symmetric around 0.5)
    CHECK(approx(FeeCalculator::taker_fee(0.90), 0.00648),
          "taker_fee(0.90) = 0.072*0.9*0.1 = 0.00648");
}

static void test_maker_fee_zero() {
    std::printf("maker_fee_zero\n");
    CHECK(FeeCalculator::maker_fee(0.50) == 0.0, "maker_fee(0.50) = 0.0");
    CHECK(FeeCalculator::maker_fee(0.10) == 0.0, "maker_fee(0.10) = 0.0");
    CHECK(FeeCalculator::maker_fee(0.90) == 0.0, "maker_fee(0.90) = 0.0");
    CHECK(FeeCalculator::maker_fee(0.01) == 0.0, "maker_fee(0.01) = 0.0");
    CHECK(FeeCalculator::maker_fee(0.99) == 0.0, "maker_fee(0.99) = 0.0");
}

static void test_edge_after_fees_taker() {
    std::printf("edge_after_fees_taker\n");
    // p_model=0.55, price=0.50, taker: fee=0.018
    // edge = 0.55 - 0.50 - 0.018 = 0.032
    CHECK(approx(FeeCalculator::edge_after_fees(0.55, 0.50, true), 0.032),
          "edge_after_fees(0.55, 0.50, taker) = 0.032");
}

static void test_edge_after_fees_no_trade() {
    std::printf("edge_after_fees_no_trade\n");
    // p_model=0.51, price=0.50, taker: fee=0.018
    // edge = 0.51 - 0.50 - 0.018 = -0.008 (negative → no trade)
    CHECK(approx(FeeCalculator::edge_after_fees(0.51, 0.50, true), -0.008),
          "edge_after_fees(0.51, 0.50, taker) = -0.008 → NO TRADE");
}

int main() {
    std::printf("== FeeCalculator tests ==\n");
    test_taker_fee_center();
    test_taker_fee_low();
    test_taker_fee_high();
    test_maker_fee_zero();
    test_edge_after_fees_taker();
    test_edge_after_fees_no_trade();
    std::printf("== %s (%d/%d passed) ==\n",
                g_failures ? "FAILED" : "ALL PASS",
                g_tests - g_failures, g_tests);
    return g_failures ? 1 : 0;
}
