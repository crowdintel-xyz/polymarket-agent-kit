// ─────────────────────────────────────────────────────────────────────────────
// test_ofi_filter: Tests for OfiLinearFilter
// ─────────────────────────────────────────────────────────────────────────────
#include <cassert>
#include <chrono>
#include <cmath>
#include <cstdio>
#include "ofi_linear_filter.hpp"

static int g_tests = 0;
static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name) do { \
    ++g_tests; \
    if (cond) { ++g_pass; std::printf("  PASS %s\n", name); } \
    else { ++g_fail; std::printf("  FAIL %s (line %d)\n", name, __LINE__); } \
} while(0)

int main() {
    std::printf("=== OfiLinearFilter Tests (8 tests) ===\n");

    // TEST 1: Normal flow — size 1.0x
    {
        OfiLinearFilter ofi(OfiLinearFilter::Config{});
        ofi.reset();
        // Symmetric book update, no imbalance
        ofi.on_book_update(
            100.0, 100.0,  // bid_vol, ask_vol
            100.0, 100.0,  // bid_vol_prev, ask_vol_prev
            50000.0, 50000.0,  // best_bid, best_bid_prev
            50001.0, 50001.0,  // best_ask, best_ask_prev
            1700000001ULL
        );
        auto state = ofi.current_state();
        std::printf("  TEST 1: level=%d, size=%.1f (expect NORMAL, 1.0x)\n",
                    static_cast<int>(state.level), state.size_multiplier);
        CHECK(state.level == OfiLinearFilter::PressureLevel::NORMAL, "Symmetric flow → NORMAL");
        CHECK(state.size_multiplier == 1.0, "Size multiplier 1.0x");
    }

    // TEST 2: Buy imbalance → positive z-score
    {
        OfiLinearFilter ofi(OfiLinearFilter::Config{});
        ofi.reset();
        // Simulate buy-side imbalance (bid grows faster than ask)
        for (int i = 0; i < 10; i++) {
            ofi.on_book_update(
                200.0, 50.0,   // bid_vol grows, ask stays low
                100.0, 100.0,  // prev
                50000.0, 50000.0,
                50001.0, 50001.0,
                1700000000ULL + static_cast<uint64_t>(i) * 1000ULL
            );
        }
        auto state = ofi.current_state();
        std::printf("  TEST 2: z=%.2f, level=%d\n", state.ofi_zscore, static_cast<int>(state.level));
        CHECK(state.ofi_zscore > 1.0, "Buy imbalance → positive z-score");
        CHECK(ofi.ofi_direction() == 1.0, "of_i_direction() = +1 (buy pressure)");
    }

    // TEST 3: Sell pressure — ask grows at higher prices → negative z-score
    {
        OfiLinearFilter ofi(OfiLinearFilter::Config{});
        ofi.reset();
        for (int i = 0; i < 10; i++) {
            ofi.on_book_update(
                100.0, 200.0,   // bids stay, asks grow
                100.0, 100.0,
                50000.0 - i * 0.1, 50000.0,   // bid drops slightly
                50002.0 + i * 0.1, 50001.0,   // ask rises → e_a = -100 (negative)
                1700000000ULL + static_cast<uint64_t>(i) * 1000ULL
            );
        }
        auto state = ofi.current_state();
        std::printf("  TEST 3: z=%.2f, level=%d\n", state.ofi_zscore, static_cast<int>(state.level));
        CHECK(state.ofi_zscore < -1.0, "Ask grows at higher prices → negative z-score");
        CHECK(ofi.ofi_direction() == -1.0, "ofi_direction() = -1 (sell pressure)");
    }

    // TEST 4: Extreme z-score → freeze
    {
        OfiLinearFilter::Config cfg;
        cfg.alert_threshold = 3.0;
        OfiLinearFilter ofi(cfg);
        ofi.reset();
        // Feed extreme buy pressure
        for (int i = 0; i < 20; i++) {
            ofi.on_book_update(
                1000.0, 1.0,
                10.0, 10.0,
                50000.0, 50000.0,
                50001.0, 50001.0,
                1700000000ULL + static_cast<uint64_t>(i) * 1000ULL
            );
        }
        auto state = ofi.current_state();
        std::printf("  TEST 4: z=%.2f, frozen=%d (expect EXTREME→freeze)\n", state.ofi_zscore, state.should_freeze);
        CHECK(state.should_freeze || state.level == OfiLinearFilter::PressureLevel::EXTREME,
              "Extreme z-score → freeze/EXTREME");
    }

    // TEST 5: Caution zone — moderate positive pressure with noise
    {
        OfiLinearFilter::Config cfg;
        cfg.caution_threshold = 8.0;
        cfg.alert_threshold = 50.0;
        OfiLinearFilter ofi(cfg);
        ofi.reset();
        // Alternating strong buy / small sell signals → moderate z-score
        for (int i = 0; i < 20; i++) {
            if (i % 3 == 0) {
                // Strong buy: bid grows, ask shrinks
                ofi.on_book_update(200.0, 50.0, 100.0, 100.0,
                                   50000.5, 50000.0, 50001.0, 50001.0,
                                   1700000000ULL + static_cast<uint64_t>(i) * 1000ULL);
            } else {
                // Neutral: no volume change
                ofi.on_book_update(100.0, 100.0, 100.0, 100.0,
                                   50000.0, 50000.0, 50001.0, 50001.0,
                                   1700000000ULL + static_cast<uint64_t>(i) * 1000ULL);
            }
        }
        auto state = ofi.current_state();
        std::printf("  TEST 5: level=%d, z=%.2f, size=%.1f\n",
                    static_cast<int>(state.level), state.ofi_zscore, state.size_multiplier);
        CHECK(!state.should_freeze, "Not frozen in caution");
        CHECK(state.size_multiplier > 0.0, "Caution zone → some size retained");
    }

    // TEST 6: Reset clears state
    {
        OfiLinearFilter ofi(OfiLinearFilter::Config{});
        ofi.on_book_update(100, 100, 100, 100, 50000, 50000, 50001, 50001, 1700000000ULL);
        ofi.reset();
        auto state = ofi.current_state();
        std::printf("  TEST 6: z=%.2f, size=%.1f (expect reset)\n", state.ofi_zscore, state.size_multiplier);
        CHECK(state.ofi_zscore == 0.0, "Reset → z-score = 0");
        CHECK(state.size_multiplier == 1.0, "Reset → size 1.0x");
    }

    // TEST 7: Out-of-order timestamp (ignored)
    {
        OfiLinearFilter ofi(OfiLinearFilter::Config{});
        ofi.reset();
        uint64_t base = 1700000000ULL;
        ofi.on_book_update(100, 100, 100, 100, 50000, 50000, 50001, 50001, base + 1000);
        ofi.on_book_update(150, 150, 100, 100, 50000, 50000, 50001, 50001, base + 500);  // out of order
        auto state = ofi.current_state();
        std::printf("  TEST 7: z=%.2f (expect 0, ignored)\n", state.ofi_zscore);
        CHECK(state.ofi_zscore == 0.0, "Out-of-order timestamp ignored");
    }

    // TEST 8: Latency
    {
        OfiLinearFilter ofi(OfiLinearFilter::Config{});
        ofi.reset();
        uint64_t base = 1700000000ULL;
        auto start = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 10000; i++) {
            ofi.on_book_update(100, 100, 100, 100, 50000, 50000, 50001, 50001,
                               base + static_cast<uint64_t>(i) * 100ULL);
        }
        auto end = std::chrono::high_resolution_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(end - start).count();
        double ns_per_call = static_cast<double>(ns) / 10000.0;
        std::printf("  TEST 8: %.1fns per on_book_update\n", ns_per_call);
        CHECK(ns_per_call < 100.0, "on_book_update < 100ns");
    }

    std::printf("\n=== Results: %d/%d passed, %d failed ===\n", g_pass, g_tests, g_fail);
    return g_fail > 0 ? 1 : 0;
}
