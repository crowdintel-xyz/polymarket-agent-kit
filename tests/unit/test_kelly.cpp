// ─────────────────────────────────────────────────────────────────────────────
// test_kelly: unit tests for KellySizer (Phase 4, Tarea 2).
//
// Verifies Quarter-Kelly sizing with fee-aware edge:
//   edge = p_model − price − fee
//   kelly_raw = (p·b − q) / b,  b = (1/price) − 1
//   quarter → inv/vol/ofi adjustments → bankroll cap → share cap → floor
//
// Exit code 0 = all pass. No external test framework.
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstdio>
#include "../../core/src/kelly_sizer.hpp"

static int g_failures = 0;
static int g_tests = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        ++g_tests;                                                            \
        if (cond) { std::printf("  PASS %s\n", name); }                       \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);          \
               ++g_failures; }                                                \
    } while (0)

static bool approx(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps;
}

// ── KellySizer tests ─────────────────────────────────────────────────────────

static void test_positive_edge_trades() {
    std::printf("positive_edge_trades\n");
    // p=0.60, price=0.50, bankroll=1000, taker → size>0
    KellySizer sizer;  // default Config
    auto r = sizer.compute(/*p_model=*/0.60, /*price=*/0.50,
                           /*bankroll=*/1000.0, /*inventory=*/0.0,
                           /*vol_z=*/0.0, /*is_taker=*/true,
                           /*ofi_mult=*/1.0);
    CHECK(r.should_trade == true, "should_trade = true");
    CHECK(r.size_shares > 0.0, "size_shares > 0");
    CHECK(r.size_usdc > 0.0, "size_usdc > 0");
    // Verify math: edge = 0.60 - 0.50 - 0.018 = 0.082
    CHECK(approx(r.edge, 0.082), "edge = 0.082");
    // kelly_raw = (0.6*1.0 - 0.4)/1.0 = 0.20
    CHECK(approx(r.kelly_raw, 0.20), "kelly_raw = 0.20");
    // fractioned = 0.20 * 0.25 = 0.05, capped at 0.05 → size = 0.05*1000 = 50
    CHECK(approx(r.size_usdc, 50.0), "size_usdc = 50.0");
    // shares = 50/0.50 = 100, capped at 100
    CHECK(approx(r.size_shares, 100.0), "size_shares = 100.0 (capped at max_position)");
}

static void test_edge_below_fee_no_trade() {
    std::printf("edge_below_fee_no_trade\n");
    // p=0.51, price=0.50, taker → edge = 0.51-0.50-0.018 = -0.008 < min_edge → NO TRADE
    KellySizer sizer;
    auto r = sizer.compute(0.51, 0.50, 1000.0, 0.0, 0.0, true, 1.0);
    CHECK(r.should_trade == false, "NO TRADE (edge < min_edge)");
    CHECK(approx(r.edge, -0.008), "edge = -0.008");
}

static void test_maker_trade() {
    std::printf("maker_trade\n");
    // p=0.52, price=0.50, maker → fee=0, edge=0.02 > 0.005 → TRADE
    KellySizer sizer;
    auto r = sizer.compute(0.52, 0.50, 1000.0, 0.0, 0.0, /*is_taker=*/false, 1.0);
    CHECK(r.should_trade == true, "TRADE (fee=0 for maker)");
    CHECK(approx(r.edge, 0.02), "edge = 0.02 (no fee)");
    CHECK(approx(r.fee_cost, 0.0), "fee_cost = 0.0");
    // kelly_raw = (0.52*1.0 - 0.48)/1.0 = 0.04
    CHECK(approx(r.kelly_raw, 0.04), "kelly_raw = 0.04");
    // fractioned = 0.04 * 0.25 = 0.01 → size = 0.01*1000 = 10
    CHECK(approx(r.size_usdc, 10.0), "size_usdc = 10.0");
    CHECK(approx(r.size_shares, 20.0), "size_shares = 20.0");
}

static void test_inventory_reduction() {
    std::printf("inventory_reduction\n");
    // inventory=0.9 → inv_adjust = 1 - 0.9 = 0.1 → size reduced to ~10%
    KellySizer sizer;
    auto r_no_inv = sizer.compute(0.60, 0.50, 1000.0, 0.0, 0.0, true, 1.0);
    auto r_inv = sizer.compute(0.60, 0.50, 1000.0, 0.9, 0.0, true, 1.0);
    CHECK(r_no_inv.should_trade == true, "no inventory: trades");
    CHECK(r_inv.should_trade == true, "with inventory 0.9: still trades (size > 0)");
    // Size should be ~10% of the no-inventory case
    CHECK(r_inv.size_shares < r_no_inv.size_shares * 0.2,
          "size reduced to ~10% (inv_adj=0.1)");
    CHECK(approx(r_inv.size_shares, 10.0), "size_shares = 10.0 (100 * 0.1)");
}

static void test_zero_bankroll_no_crash() {
    std::printf("zero_bankroll_no_crash\n");
    // bankroll=0 → size=0 (no crash)
    KellySizer sizer;
    auto r = sizer.compute(0.60, 0.50, 0.0, 0.0, 0.0, true, 1.0);
    CHECK(r.should_trade == false, "NO TRADE with bankroll=0");
    CHECK(approx(r.size_shares, 0.0), "size_shares = 0.0");
    CHECK(approx(r.size_usdc, 0.0), "size_usdc = 0.0");
    CHECK(r.kelly_raw > 0.0, "kelly_raw still computed (>0)");
}

static void test_small_size_no_trade() {
    std::printf("small_size_no_trade\n");
    // Very small bankroll → size_shares < 1 → NO TRADE
    KellySizer sizer;
    auto r = sizer.compute(0.52, 0.50, /*bankroll=*/1.0, 0.0, 0.0, false, 1.0);
    // fee=0 (maker), edge = 0.52-0.50 = 0.02 > 0.005
    // kelly_raw = 0.04, fractioned = 0.01
    // size_usdc = min(0.01*1, 1*0.05) = 0.01
    // size_shares = 0.01/0.50 = 0.02 < 1 → NO TRADE
    CHECK(r.should_trade == false, "NO TRADE (size < 1 share)");
    CHECK(r.size_shares < 1.0, "size_shares < 1.0");
}

static void test_ofi_multiplier_zero() {
    std::printf("ofi_multiplier_zero\n");
    // ofi_multiplier=0 → kelly_fractioned=0 → size=0 → NO TRADE
    KellySizer sizer;
    auto r = sizer.compute(0.60, 0.50, 1000.0, 0.0, 0.0, true, /*ofi_mult=*/0.0);
    CHECK(r.should_trade == false, "NO TRADE (ofi_multiplier=0)");
    CHECK(approx(r.size_shares, 0.0), "size_shares = 0.0");
    CHECK(approx(r.size_usdc, 0.0), "size_usdc = 0.0");
}

int main() {
    std::printf("== KellySizer tests ==\n");
    test_positive_edge_trades();
    test_edge_below_fee_no_trade();
    test_maker_trade();
    test_inventory_reduction();
    test_zero_bankroll_no_crash();
    test_small_size_no_trade();
    test_ofi_multiplier_zero();
    std::printf("== %s (%d/%d passed) ==\n",
                g_failures ? "FAILED" : "ALL PASS",
                g_tests - g_failures, g_tests);
    return g_failures ? 1 : 0;
}
