// ─────────────────────────────────────────────────────────────────────────────
// test_window_shield: Tests for WindowShield with OFI integration
// ─────────────────────────────────────────────────────────────────────────────
#include <cassert>
#include <chrono>
#include <cstdio>
#include <cstdint>
#include "window_shield.hpp"
#include "ofi_linear_filter.hpp"

static int g_tests = 0;
static int g_pass = 0;
static int g_fail = 0;

#define CHECK(cond, name) do { \
    ++g_tests; \
    if (cond) { ++g_pass; std::printf("  PASS %s\n", name); } \
    else { ++g_fail; std::printf("  FAIL %s (line %d)\n", name, __LINE__); } \
} while(0)

// Mock clock
static uint64_t g_mock_ns = 0;
[[maybe_unused]]
static uint64_t mock_clock_ns() { return g_mock_ns; }
[[maybe_unused]] static uint64_t mock_clock_ns_unused = 0;

int main() {
    std::printf("=== WindowShield Tests (6 tests) ===\n");

    auto cfg = WindowShieldConfig::BTC_5M();

    // TEST 1: Initial state = MAKER_PASSIVE
    {
        WindowShield ws(cfg);
        uint64_t start = 1'700'000'000'000ULL;  // 1.7 trillion ns
        ws.set_window_start(start);
        auto info = ws.snapshot(start + 5'000'000'000ULL);  // 5s elapsed
        std::printf("  TEST 1: state=%d (expect 0=MAKER_PASSIVE)\n", static_cast<int>(info.state));
        CHECK(info.state == ShieldState::MAKER_PASSIVE, "Initial state = MAKER_PASSIVE");
        CHECK(info.remaining_sec == 295, "Remaining: 295s (5s elapsed of 300)");
    }

    // TEST 2: Transition to MAKER_SKEWED
    {
        WindowShield ws(cfg);
        uint64_t start = 1'700'000'000'000ULL;
        ws.set_window_start(start);
        // 250s = 5s before skew
        uint64_t now = start + 250ULL * 1'000'000'000ULL;
        auto state = ws.update(now, 0.5f);  // CfC < 0.6
        std::printf("  TEST 2a: state=%d (expect 0=PASSIVE, CfC too low)\n", static_cast<int>(state));
        CHECK(state == ShieldState::MAKER_PASSIVE, "250s, CfC<0.6 → still PASSIVE");

        state = ws.update(now, 0.7f);  // CfC ≥ 0.6
        std::printf("  TEST 2b: state=%d (expect 1=MAKER_SKEWED)\n", static_cast<int>(state));
        CHECK(state == ShieldState::MAKER_SKEWED, "250s, CfC≥0.6 → MAKER_SKEWED");
    }

    // TEST 3: Transition to DIRECTIONAL
    {
        WindowShield ws(cfg);
        uint64_t start = 1'700'000'000'000ULL;
        ws.set_window_start(start);
        // 275s = in directional zone
        uint64_t now = start + 275ULL * 1'000'000'000ULL;
        auto state = ws.update(now, 0.6f);  // CfC < 0.75
        std::printf("  TEST 3a: state=%d (expect 1=SKEWED, CfC too low)\n", static_cast<int>(state));
        CHECK(state == ShieldState::MAKER_SKEWED, "275s, CfC<0.75 → still SKEWED");

        state = ws.update(now, 0.8f);
        std::printf("  TEST 3b: state=%d (expect 2=DIRECTIONAL)\n", static_cast<int>(state));
        CHECK(state == ShieldState::DIRECTIONAL, "275s, CfC≥0.75 → DIRECTIONAL");
    }

    // TEST 4: Late window → CLOSE_ONLY
    {
        WindowShield ws(cfg);
        uint64_t start = 1'700'000'000'000ULL;
        ws.set_window_start(start);
        // 295s = in close-only zone
        uint64_t now = start + 295ULL * 1'000'000'000ULL;
        auto state = ws.update(now, 0.95f);  // CfC high
        std::printf("  TEST 4: state=%d (expect 3=CLOSE_ONLY)\n", static_cast<int>(state));
        CHECK(state == ShieldState::CLOSE_ONLY, "295s → CLOSE_ONLY regardless of CfC");
    }

    // TEST 5: After window close → HALTED
    {
        WindowShield ws(cfg);
        uint64_t start = 1'700'000'000'000ULL;
        ws.set_window_start(start);
        // 301s = past window end
        uint64_t now = start + 301ULL * 1'000'000'000ULL;
        auto state = ws.update(now, 0.0f);
        std::printf("  TEST 5: state=%d (expect 4=HALTED)\n", static_cast<int>(state));
        CHECK(state == ShieldState::HALTED, "301s → HALTED");
    }

    // TEST 6: OFI EXTREME pressure → forces CLOSE_ONLY
    {
        WindowShield ws(cfg);
        uint64_t start = 1'700'000'000'000ULL;
        ws.set_window_start(start);
        uint64_t now = start + 60ULL * 1'000'000'000ULL;  // 60s elapsed
        auto state = ws.update(now, 0.3f, OfiPressure::EXTREME);
        std::printf("  TEST 6: state=%d, ofi=%d (expect CLOSE_ONLY, EXTREME)\n",
                    static_cast<int>(state), static_cast<int>(ws.snapshot(now).ofi_pressure));
        CHECK(state == ShieldState::CLOSE_ONLY, "OFI EXTREME → CLOSE_ONLY override");
        CHECK(ws.snapshot(now).ofi_pressure == OfiPressure::EXTREME, "ofi_pressure stored in snapshot");
    }

    // TEST 7: Stale timestamp → HALTED (fail-closed)
    {
        WindowShield ws(cfg);
        // No set_window_start() called → window_start_ns_ = 0
        auto state = ws.update(1'700'000'001'000'000'000ULL, 0.5f);
        std::printf("  TEST 7: state=%d (expect 4=HALTED)\n", static_cast<int>(state));
        CHECK(state == ShieldState::HALTED, "No window start → HALTED (fail-closed)");
    }

    // TEST 8: Latency
    {
        WindowShield ws(cfg);
        uint64_t start = 1'700'000'000'000ULL;
        ws.set_window_start(start);
        auto t0 = std::chrono::high_resolution_clock::now();
        for (int i = 0; i < 10000; i++) {
            ws.update(start + 100ULL * 1'000'000ULL + static_cast<uint64_t>(i), 0.5f);
        }
        auto t1 = std::chrono::high_resolution_clock::now();
        auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
        double ns_per = static_cast<double>(ns) / 10000.0;
        std::printf("  TEST 8: %.1fns per update\n", ns_per);
        CHECK(ns_per < 200.0, "update() < 200ns");
    }

    std::printf("\n=== Results: %d/%d passed, %d failed ===\n", g_pass, g_tests, g_fail);
    return g_fail > 0 ? 1 : 0;
}
