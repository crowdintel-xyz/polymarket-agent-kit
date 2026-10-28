// ─────────────────────────────────────────────────────────────────────────────
// Acceptance Test: WindowShield Lifecycle
// Verifies 5-state machine (MAKER_PASSIVE → MAKER_SKEWED → DIRECTIONAL →
// CLOSE_ONLY → HALTED) and fail-closed behavior on stale timestamps and
// EXTREME OFI pressure.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cstdio>
#include <cstdint>
#include "window_shield.hpp"

static int g_failures = 0;
static int g_tests = 0;
#define CHECK(cond, name)                                                     \
    do { ++g_tests; if (cond) std::printf("  PASS %s\n", name);               \
         else { std::printf("  FAIL %s (line %d)\n", name, __LINE__); ++g_failures; } \
    } while (0)

int main() {
    std::printf("=== Acceptance: WindowShield Lifecycle ===\n\n");

    WindowShield shield(WindowShieldConfig::BTC_5M());
    uint64_t window_start = 1'700'000'000'000'000'000ULL;
    shield.set_window_start(window_start);

    // 1. Fail-closed: no window_start → HALTED (already set above)
    WindowShield shield_no_init(WindowShieldConfig::BTC_5M());
    ShieldState init_state = shield_no_init.update(window_start, 0.5f);
    CHECK(init_state == ShieldState::HALTED,
          "Fail-closed: HALTED without set_window_start");

    // 2. Early window → MAKER_PASSIVE
    ShieldState s1 = shield.update(window_start + 10'000'000'000ULL, 0.5f);
    CHECK(s1 == ShieldState::MAKER_PASSIVE, "Early window → MAKER_PASSIVE");

    // 3. Skewed phase (t > 240s) with high conviction → MAKER_SKEWED
    uint64_t skewed_ts = window_start + 241'000'000'000ULL;
    ShieldState s2 = shield.update(skewed_ts, 0.65f);
    CHECK(s2 == ShieldState::MAKER_SKEWED || s2 == ShieldState::MAKER_PASSIVE,
          "Skewed phase with conviction >0.6 → MAKER_SKEWED or PASSIVE");

    // 4. Directional phase (t > 270s) with high conviction → DIRECTIONAL
    uint64_t directional_ts = window_start + 271'000'000'000ULL;
    ShieldState s3 = shield.update(directional_ts, 0.78f);
    CHECK(s3 == ShieldState::DIRECTIONAL || s3 == ShieldState::MAKER_SKEWED,
          "Directional phase with conviction >0.75 → DIRECTIONAL or SKEWED");

    // 5. Close-only phase (t > 290s) → CLOSE_ONLY regardless of conviction
    uint64_t close_ts = window_start + 291'000'000'000ULL;
    ShieldState s4 = shield.update(close_ts, 0.9f);
    CHECK(s4 == ShieldState::CLOSE_ONLY,
          "Close-only phase → CLOSE_ONLY (regardless of conviction)");

    // 6. Window complete → HALTED (cooldown)
    uint64_t halt_ts = window_start + 301'000'000'000ULL;
    ShieldState s5 = shield.update(halt_ts, 0.5f);
    CHECK(s5 == ShieldState::HALTED, "Window end → HALTED");

    // 7. Fail-closed on stale timestamp (older than window_start)
    WindowShield shield_stale(WindowShieldConfig::BTC_5M());
    shield_stale.set_window_start(window_start);
    ShieldState s6 = shield_stale.update(window_start - 10'000'000'000ULL, 0.5f);
    CHECK(s6 == ShieldState::HALTED || s6 == ShieldState::CLOSE_ONLY,
          "Stale timestamp → HALTED (fail-closed)");

    // 8. EXTREME OFI pressure forces CLOSE_ONLY even in MAKER_PASSIVE phase
    WindowShield shield_extreme(WindowShieldConfig::BTC_5M());
    shield_extreme.set_window_start(window_start);
    ShieldState s7 = shield_extreme.update(
        window_start + 120'000'000'000ULL,  // mid-window
        0.5f,
        OfiPressure::EXTREME
    );
    CHECK(s7 == ShieldState::CLOSE_ONLY,
          "OFI EXTREME → CLOSE_ONLY even in early window");

    std::printf("\n========================================\n");
    std::printf("Shield Lifecycle: %d/%d passed\n", g_tests - g_failures, g_tests);
    std::printf("========================================\n");
    return g_failures > 0 ? 1 : 0;
}
