// ─────────────────────────────────────────────────────────────────────────────
// test_ladder: unit tests for LadderBuilder (Phase 4, Tarea 3).
//
// Verifies adaptive spread, skew, inventory flattening, and active gate.
//
// Exit code 0 = all pass. No external test framework.
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstdio>
#include "../../core/src/ladder_builder.hpp"

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

// ── LadderBuilder tests ──────────────────────────────────────────────────────

static void test_symmetric_quote() {
    std::printf("symmetric_quote\n");
    // p=0.50, OFI=0, inv=0 → symmetric
    LadderBuilder builder;
    Quote q = builder.build(/*p_up=*/0.50, /*mid=*/0.50,
                             /*ofi_dir=*/0.0, /*ofi_mult=*/1.0,
                             /*inv=*/0.0, /*spread_base=*/1.0,
                             /*vol_mult=*/1.0);

    CHECK(q.is_active == true, "is_active = true (ofi_multiplier > 0)");
    // skew = 0 → bid/ask symmetric around mid
    CHECK(approx(q.bid_size, q.ask_size), "bid_size == ask_size (symmetric)");
    CHECK(approx(q.bid_size, 10.0), "bid_size = 10.0 (base * mult * 1)");
    CHECK(approx(q.ask_size, 10.0), "ask_size = 10.0 (base * mult * 1)");
    // Prices symmetric around 0.50
    const double mid_actual = (q.bid_price + q.ask_price) / 2.0;
    CHECK(approx(mid_actual, 0.50, 1e-6), "mid == market_mid (0.50)");
    const double half = (q.ask_price - q.bid_price) / 2.0;
    CHECK(approx(half, 0.005), "half-spread = 0.005 (1 cent / 200)");
}

static void test_positive_skew() {
    std::printf("positive_skew\n");
    // p=0.70, OFI=+1 → positive skew
    LadderBuilder builder;
    Quote q = builder.build(/*p_up=*/0.70, /*mid=*/0.50,
                             /*ofi_dir=*/1.0, /*ofi_mult=*/1.0,
                             /*inv=*/0.0, /*spread_base=*/1.0,
                             /*vol_mult=*/1.0);

    CHECK(q.is_active == true, "is_active = true");

    // bb_skew = (0.70-0.5)*2 = 0.4
    // ofi_skew = 1.0*0.3 = 0.3
    // inv_skew = 0
    // skew = 0.4*0.5 + 0.3*0.2 + 0 = 0.2 + 0.06 = 0.26
    CHECK(q.bid_size > q.ask_size, "bid_size > ask_size (positive skew → more bid)");
    CHECK(approx(q.bid_size, 10.0 * (1.0 + 0.26)), "bid_size = 10*(1+0.26) = 12.6");
    CHECK(approx(q.ask_size, 10.0 * (1.0 - 0.26)), "ask_size = 10*(1-0.26) = 7.4");

    // Prices: bid closer to mid, ask farther (bullish skew widens ask side)
    // bid = mid - half*(1-skew), ask = mid + half*(1+skew)
    // With skew=0.26: bid offset = 0.005*(1-0.26)=0.0037, ask offset = 0.005*(1+0.26)=0.0063
    CHECK(q.bid_price > 0.49, "bid_price pulled toward mid by bullish skew");
    CHECK(q.ask_price > 0.505, "ask_price pushed away by bullish skew");
}

static void test_inventory_flattening_ask() {
    std::printf("inventory_flattening_ask\n");
    // inventory=0.9 → only ask (inv>0)
    LadderBuilder builder;
    Quote q = builder.build(/*p_up=*/0.50, /*mid=*/0.50,
                             /*ofi_dir=*/0.0, /*ofi_mult=*/1.0,
                             /*inv=*/0.9, /*spread_base=*/1.0,
                             /*vol_mult=*/1.0);

    CHECK(q.is_active == true, "is_active = true");
    // |inventory| = 0.9 > 0.8 and inventory > 0 → only ask
    CHECK(approx(q.bid_size, 0.0), "bid_size = 0 (only ask when inv=+0.9)");
    CHECK(q.ask_size > 0.0, "ask_size > 0 (active ask to reduce long)");
}

static void test_ofi_zero_inactive() {
    std::printf("ofi_zero_inactive\n");
    // ofi_multiplier=0 → is_active=false
    LadderBuilder builder;
    Quote q = builder.build(/*p_up=*/0.50, /*mid=*/0.50,
                             /*ofi_dir=*/0.0, /*ofi_mult=*/0.0,
                             /*inv=*/0.0, /*spread_base=*/1.0,
                             /*vol_mult=*/1.0);

    CHECK(q.is_active == false, "is_active = false (ofi_multiplier=0)");
    CHECK(approx(q.bid_size, 0.0), "bid_size = 0 (inactive)");
    CHECK(approx(q.ask_size, 0.0), "ask_size = 0 (inactive)");
}

int main() {
    std::printf("== LadderBuilder tests ==\n");
    test_symmetric_quote();
    test_positive_skew();
    test_inventory_flattening_ask();
    test_ofi_zero_inactive();
    std::printf("== %s (%d/%d passed) ==\n",
                g_failures ? "FAILED" : "ALL PASS",
                g_tests - g_failures, g_tests);
    return g_failures ? 1 : 0;
}
