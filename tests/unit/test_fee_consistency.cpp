// ─────────────────────────────────────────────────────────────────────────────
// test_fee_consistency.cpp — T1.2: Fee model consistency between
//   FeeCalculator::TAKER_FEE_RATE (0.072) and MarketConfig::taker_fee_rate
//
// BUG: fee_calculator.hpp uses 0.072 (7.2%) but market_config.hpp defaults
//      to 0.07 (7.0%). ExecutionEngine uses MarketConfig's value for actual
//      fee calculations, while FeeCalculator is used for modeling — causing
//      a 0.2% discrepancy in sizing and edge estimation.
//
// Test: Verify MarketConfig::taker_fee_rate matches FeeCalculator::TAKER_FEE_RATE
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <cmath>
#include <cstdint>

#include "../../core/src/fee_calculator.hpp"
#include "../../core/src/market_config.hpp"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  \u274C FAIL: %s\n", msg); g_failures++; } \
    else { printf("  \u2705 PASS: %s\n", msg); } \
} while(0)

int main() {
    printf("=== T1.2: Fee rate consistency ===\n");

    // Test 1: FeeCalculator and MarketConfig use the same taker fee rate
    MarketConfig cfg{};
    CHECK(FeeCalculator::TAKER_FEE_RATE == cfg.taker_fee_rate,
          "FeeCalculator rate matches MarketConfig default");

    // Test 2: Fee calculation is consistent
    double price = 0.60;
    double model_fee = FeeCalculator::taker_fee(price);
    double config_fee = cfg.taker_fee_rate * price * (1.0 - price);
    CHECK(std::abs(model_fee - config_fee) < 1e-9,
          "Fee calculation matches between model and config");

    // Test 3: FeeCalculator can construct a MarketConfig with matching rate
    MarketConfig cfg2{};
    cfg2.taker_fee_rate = FeeCalculator::TAKER_FEE_RATE;
    CHECK(cfg2.taker_fee_rate == FeeCalculator::TAKER_FEE_RATE,
          "MarketConfig can be configured to match FeeCalculator");

    printf("\n  T1.2 test complete: %d failures\n", g_failures);
    return g_failures > 0 ? 1 : 0;
}
