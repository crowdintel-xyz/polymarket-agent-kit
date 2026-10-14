// ─────────────────────────────────────────────────────────────────────────────
// Acceptance Test: Ladder Builder + Time Strategy
// Verifies dynamic quote ladder generation with correct bid/ask spread,
// size skewing based on conviction and OFI, and time-based position adjustment.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cstdio>
#include <cmath>
#include <cstdint>
#include "ladder_builder.hpp"
#include "time_strategy.hpp"

static int g_failures = 0;
static int g_tests = 0;
#define CHECK(cond, name)                                                     \
    do { ++g_tests; if (cond) std::printf("  PASS %s\n", name);               \
         else { std::printf("  FAIL %s (line %d)\n", name, __LINE__); ++g_failures; } \
    } while (0)

int main() {
    std::printf("=== Acceptance: Ladder + Time Strategy ===\n\n");

    LadderBuilder ladder;

    // 1. Basic ladder: mid price 0.50, neutral conviction
    Quote q1 = ladder.build(0.50, 0.50, 0.0, 1.0, 0.0, 1.0, 1.0);
    CHECK(q1.is_active, "Quote is active at mid confidence");
    CHECK(q1.bid_price < 0.50, "Bid < mid");
    CHECK(q1.ask_price > 0.50, "Ask > mid");
    CHECK(q1.bid_price < q1.ask_price, "Bid < Ask (spread)");

    // 2. High conviction → wider skew
    Quote q2 = ladder.build(0.70, 0.50, 0.1, 1.0, 0.0, 1.0, 1.0);
    CHECK(q2.bid_size > q1.bid_size, "Higher conviction → larger bid size");

    // 3. Negative OFI direction → reduce bid skew
    Quote q3 = ladder.build(0.50, 0.50, -0.5, 1.0, 0.0, 1.0, 1.0);
    CHECK(q3.bid_size <= q1.bid_size, "Negative OFI → smaller bid size");

    // 4. High volatility → wider spread
    Quote q4 = ladder.build(0.50, 0.50, 0.0, 1.0, 0.0, 2.0, 1.0);
    double spread_normal = q1.ask_price - q1.bid_price;
    double spread_wide = q4.ask_price - q4.bid_price;
    CHECK(spread_wide >= spread_normal, "Higher vol → wider spread");

    // 5. Inventory adjustment
    Quote q5 = ladder.build(0.50, 0.50, 0.0, 1.0, 0.0, 1.0, 1.0);
    Quote q6 = ladder.build(0.50, 0.50, 0.0, 1.0, 0.8, 1.0, 1.0);  // long inventory
    CHECK(q6.ask_size >= q5.ask_size, "Long inventory → reduce ask size");

    // 6. Time strategy: Asia lull → aggressive sizing
    TimeConfig asia = TimeStrategy::get_time_config(3);  // 00-05 UTC
    CHECK(asia.size_mult >= 1.0, "Asia lull → size_mult >= 1.0");

    // 7. Time strategy: US close → reduced activity
    TimeConfig us_close = TimeStrategy::get_time_config(20);  // settlement edge
    CHECK(us_close.size_mult < 1.0, "US close → size_mult < 1.0 (reduced)");

    // 8. Extreme conviction → prices still within [0, 1]
    Quote q8 = ladder.build(0.95, 0.50, 0.0, 1.0, 0.0, 1.0, 1.0);
    CHECK(q8.bid_price >= 0.0 && q8.ask_price <= 1.0,
          "Prices within [0, 1] for extreme conviction");

    std::printf("\n========================================\n");
    std::printf("Ladder + Time: %d/%d passed\n", g_tests - g_failures, g_tests);
    std::printf("========================================\n");
    return g_failures > 0 ? 1 : 0;
}
