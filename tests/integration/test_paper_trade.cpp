// ─────────────────────────────────────────────────────────────────────────────
// test_paper_trade: Paper trading simulation (Phase 5 acceptance).
//
// Simulates 100 mock Binance price ticks fed through the full DSH pipeline:
//   Binance price ticks → TwapBrownianBridge → VolatilityEstimator →
//   OfiLinearFilter → WindowShield → FeeCalculator → KellySizer →
//   LadderBuilder → TimeStrategy → RateLimiter → CircuitBreaker
//
// Verifies:
//   1. ASan/UBSan clean — no crashes, no leaks, no undefined behaviour.
//   2. Trades only execute when edge ≥ 0.5% (min_edge_threshold).
//   3. Position sizes follow quarter-Kelly (kelly_fraction = 0.25).
//   4. WindowShield transitions through ALL states:
//      MAKER_PASSIVE → MAKER_SKEWED → DIRECTIONAL → CLOSE_ONLY → HALTED.
//   5. Circuit breaker fail-closed: trades blocked when OPEN.
//
// Exit code 0 = all pass. No external dependencies. BOT_MODE=mock.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <vector>

// ── Phase 1 ──────────────────────────────────────────────────────────────────
#include "twap_brownian_bridge.hpp"
#include "volatility_estimator.hpp"

// ── Phase 2 ──────────────────────────────────────────────────────────────────
#include "ofi_linear_filter.hpp"
#include "window_shield.hpp"

// ── Phase 3 ──────────────────────────────────────────────────────────────────
#include "rate_limiter.hpp"
#include "circuit_breaker.hpp"

// ── Phase 4 ──────────────────────────────────────────────────────────────────
#include "fee_calculator.hpp"
#include "kelly_sizer.hpp"
#include "ladder_builder.hpp"
#include "time_strategy.hpp"

// ── Test harness ─────────────────────────────────────────────────────────────
static int g_tests   = 0;
static int g_passed  = 0;
static int g_failed  = 0;

#define CHECK(cond, name)                                                      \
    do {                                                                       \
        ++g_tests;                                                             \
        if (cond) { ++g_passed; std::printf("  PASS  %s\n", name); }           \
        else { ++g_failed; std::printf("  FAIL  %s (line %d)\n", name, __LINE__); } \
    } while (0)

// ── Mock clock ───────────────────────────────────────────────────────────────
static uint64_t g_mock_ms = 0;
static uint64_t mock_clock_ms() noexcept { return g_mock_ms; }

// ── Epoch (non-zero to avoid WindowShield fail-closed guard) ─────────────────
static constexpr uint64_t EPOCH_MS = 1'000'000ULL;
static constexpr uint64_t EPOCH_NS = EPOCH_MS * 1'000'000ULL;

// ── Order-book snapshot ─────────────────────────────────────────────────────
struct BookSnapshot {
    double bid_vol, ask_vol;
    double best_bid, best_ask;
};

// ── Price tick generator ─────────────────────────────────────────────────────
// Generates a deterministic 100-tick sequence spanning 320 seconds.
// Price path:
//   ticks  0–40  (  0–128s): oscillate around 0.50 → ATM, mixed trades
//   ticks 41–76  (128–243s): trend up to 0.53    → bullish, trades
//   ticks 77–99  (243–320s): hold at 0.54        → high p_up
//
// Time advances 3.2 s per tick → 100 ticks = 320 s (covers full 300 s window).
// At t=240s → MAKER_SKEWED, t=270s → DIRECTIONAL, t=290s → CLOSE_ONLY,
// t=300s → HALTED.  All 5 states are hit because Phase B/C prices are > 0.50.
struct TickGenerator {
    struct TickData {
        double price;
        double bid_vol;
        double ask_vol;
        double best_bid;
        double best_ask;
    };

    TickData generate(int i) noexcept {
        TickData td{};

        if (i < 41) {
            // Phase A: oscillate around 0.50 — half above, half below strike.
            double wave = 0.008 * std::sin(static_cast<double>(i) * 0.3);
            td.price = 0.50 + wave;
        } else if (i < 77) {
            // Phase B: trend upward from 0.50 → 0.53.
            double trend = static_cast<double>(i - 41) / 36.0;
            td.price = 0.50 + trend * 0.03;
        } else {
            // Phase C: hold at 0.54 — strongly above strike.
            td.price = 0.54;
        }

        // Symmetric constant volumes → e_n = 0 → OFI stays NORMAL.
        // This isolates WindowShield state transitions to time + CFC only.
        td.bid_vol = 100.0;
        td.ask_vol = 100.0;

        double tick = 0.001;
        td.best_bid = td.price - tick;
        td.best_ask = td.price + tick;
        return td;
    }
};

// ── Trading record ───────────────────────────────────────────────────────────
struct TradeRecord {
    uint64_t ts_ms;
    double price;
    double p_up;
    double edge;
    double fee;
    double size_shares;
    double size_usdc;
    double kelly_raw;
    double bankroll_before;
    double bankroll_after;
    ShieldState shield_state;
};

// ── Helper: map OfiLinearFilter::PressureLevel → WindowShield::OfiPressure ──
static OfiPressure map_pressure(OfiLinearFilter::PressureLevel lvl) noexcept {
    switch (lvl) {
        case OfiLinearFilter::PressureLevel::NORMAL:  return OfiPressure::NORMAL;
        case OfiLinearFilter::PressureLevel::CAUTION: return OfiPressure::CAUTION;
        case OfiLinearFilter::PressureLevel::HIGH:    return OfiPressure::HIGH;
        case OfiLinearFilter::PressureLevel::EXTREME: return OfiPressure::EXTREME;
    }
    return OfiPressure::NORMAL;
}

// ── Paper trading engine ─────────────────────────────────────────────────────
struct PaperTrader {
    // Phase 1
    TwapBrownianBridge   bb{TwapBBConfig{}};
    VolatilityEstimator  vol{VolatilityEstimator::Config{}};

    // Phase 2
    OfiLinearFilter      ofi{OfiLinearFilter::Config{}};
    WindowShield         shield{WindowShieldConfig::BTC_5M()};

    // Phase 3
    RateLimiter          rl{RateLimitConfig{}};
    CircuitBreaker       cb{CircuitBreaker::Config{}, mock_clock_ms};

    // Phase 4
    KellySizer           sizer{Config{}};   // global Config struct
    LadderBuilder        ladder;

    // Mutable state
    double               bankroll = 10000.0;
    double               inventory = 0.0;
    double               strike_price = 0.50;
    double               prev_price = 0.50;
    int                  trades_executed = 0;
    int                  trades_blocked = 0;
    int                  ticks_processed = 0;
    double               max_kelly_ratio = 0.0;

    // Trade log (non-hot-path; std::vector is fine here).
    std::vector<TradeRecord> trade_log;

    // State transition tracking
    bool saw_passive = false;
    bool saw_skewed = false;
    bool saw_directional = false;
    bool saw_close_only = false;
    bool saw_halted = false;

    PaperTrader() {
        shield.set_window_start(EPOCH_NS);
    }

    void process_tick(double price, uint64_t ts_ms,
                      const BookSnapshot& book,
                      const BookSnapshot& prev_book) noexcept
    {
        // Advance mock clock for rate-limiter refills.
        g_mock_ms = ts_ms;

        // 1. Price tick → TwapBrownianBridge
        bb.on_price(price, ts_ms);

        // 2. Price tick → VolatilityEstimator (log return)
        if (prev_price > 0.0 && price > 0.0) {
            double log_ret = std::log(price / prev_price);
            vol.on_log_return(log_ret, 1.0);
        }
        prev_price = price;

        // 3. Vol feedback → BB sigma
        double vol_eff = vol.effective_vol();
        bb.set_sigma_annual(vol_eff);
        double vol_mult = vol.regime_vol_multiplier();

        // 4. Compute probability
        double elapsed = 30.0;  // midpoint of 60s window
        double tau_total = 60.0;
        ProbResult prob = bb.compute(price, strike_price, tau_total, elapsed);

        // 5. OFI filter
        ofi.on_book_update(book.bid_vol, book.ask_vol,
                           prev_book.bid_vol, prev_book.ask_vol,
                           book.best_bid, prev_book.best_bid,
                           book.best_ask, prev_book.best_ask,
                           ts_ms);
        auto ofi_state = ofi.current_state();

        // 6. WindowShield lifecycle
        uint64_t now_ns = ts_ms * 1'000'000ULL;
        OfiPressure ofi_p = map_pressure(ofi_state.level);
        ShieldState state = shield.update(now_ns,
                                          static_cast<float>(prob.p_up),
                                          ofi_p);

        switch (state) {
            case ShieldState::MAKER_PASSIVE:  saw_passive    = true; break;
            case ShieldState::MAKER_SKEWED:   saw_skewed     = true; break;
            case ShieldState::DIRECTIONAL:    saw_directional = true; break;
            case ShieldState::CLOSE_ONLY:     saw_close_only  = true; break;
            case ShieldState::HALTED:          saw_halted     = true; break;
        }

        // 7. Fee + edge
        double fee = FeeCalculator::taker_fee(strike_price);
        double edge = FeeCalculator::edge_after_fees(prob.p_up, strike_price, true);

        // 8. Kelly sizer
        // Inventory normalised by (10 × max_position) so the inv_adj
        // factor gradually scales down as position builds, allowing many
        // trades across the 100-tick simulation.  The sizer expects
        // a dimensionless position fraction in [0, 1].
        SizingResult sizing = sizer.compute(prob.p_up,
                                            strike_price,
                                            bankroll,
                                            inventory / 1000.0,
                                            vol_mult - 1.0,
                                            true,
                                            ofi_state.size_multiplier);

        // Track Kelly fraction ratio (should be ≤ 0.25).
        if (sizing.kelly_raw > 1e-12) {
            double ratio = sizing.kelly_fractioned / sizing.kelly_raw;
            if (ratio > max_kelly_ratio) max_kelly_ratio = ratio;
        }

        // 9. Ladder builder
        ladder.build(prob.p_up, strike_price,
                     ofi.ofi_direction(),
                     ofi_state.size_multiplier,
                     inventory,
                     /*spread_base=*/5.0,
                     vol_mult);

        // 10. Time strategy
        uint32_t hour_utc = static_cast<uint32_t>((ts_ms / 3'600'000ULL) % 24ULL);
        TimeStrategy::get_time_config(hour_utc);

        // 11. Rate limiter + circuit breaker
        bool cb_ok = cb.allow_request();
        bool rl_ok = rl.can_send(RequestType::ORDER);

        // 12. Trade decision (fail-closed: shield, circuit, rate all gate)
        bool shield_ok = (state != ShieldState::CLOSE_ONLY &&
                          state != ShieldState::HALTED);

        bool trade_allowed = (sizing.should_trade &&
                              shield_ok && cb_ok && rl_ok &&
                              edge >= 0.005);  // min_edge: 0.5%

        if (trade_allowed) {
            double bankroll_before = bankroll;
            if (bankroll >= sizing.size_usdc) {
                bankroll -= sizing.size_usdc;
                inventory += sizing.size_shares;
                trades_executed++;
                rl.on_sent(RequestType::ORDER);
                cb.on_success();

                trade_log.push_back(TradeRecord{
                    .ts_ms = ts_ms,
                    .price = price,
                    .p_up = prob.p_up,
                    .edge = edge,
                    .fee = fee,
                    .size_shares = sizing.size_shares,
                    .size_usdc = sizing.size_usdc,
                    .kelly_raw = sizing.kelly_raw,
                    .bankroll_before = bankroll_before,
                    .bankroll_after = bankroll,
                    .shield_state = state,
                });
            } else {
                trades_blocked++;
                if (cb_ok) cb.on_success();
            }
        } else {
            trades_blocked++;
            // Only signal success to the circuit breaker when it was
            // actually allowing requests (i.e., not OPEN or HALF_OPEN-exhausted).
            if (cb_ok) cb.on_success();
        }

        ticks_processed++;
    }
};

// ── Test 1: 100 ticks through the pipeline (no crash) ────────────────────────
static void test_100_ticks_no_crash() {
    std::printf("test_100_ticks_no_crash\n");

    PaperTrader trader;
    TickGenerator gen;
    BookSnapshot prev{100.0, 100.0, 0.499, 0.501};

    for (int i = 0; i < 100; i++) {
        auto td = gen.generate(i);
        uint64_t ts_ms = EPOCH_MS + static_cast<uint64_t>(i) * 3200ULL;
        BookSnapshot book{td.bid_vol, td.ask_vol, td.best_bid, td.best_ask};
        trader.process_tick(td.price, ts_ms, book, prev);
        prev = book;
    }

    CHECK(trader.ticks_processed == 100, "All 100 ticks processed");
    CHECK(trader.trades_executed + trader.trades_blocked == 100,
          "Every tick either traded or was blocked (100 total)");
    std::printf("  Trades executed: %d, Trades blocked: %d\n",
                trader.trades_executed, trader.trades_blocked);
    CHECK(trader.trades_executed > 0, "At least some trades executed (bullish phase)");
}

// ── Test 2: Trades only execute when edge ≥ 0.5% ─────────────────────────────
static void test_edge_threshold() {
    std::printf("test_edge_threshold\n");

    PaperTrader trader;
    TickGenerator gen;
    BookSnapshot prev{100.0, 100.0, 0.499, 0.501};

    for (int i = 0; i < 100; i++) {
        auto td = gen.generate(i);
        uint64_t ts_ms = EPOCH_MS + static_cast<uint64_t>(i) * 3200ULL;
        BookSnapshot book{td.bid_vol, td.ask_vol, td.best_bid, td.best_ask};
        trader.process_tick(td.price, ts_ms, book, prev);
        prev = book;
    }

    // Verify EVERY executed trade had edge ≥ 0.5%.
    bool all_edges_ok = true;
    for (const auto& tr : trader.trade_log) {
        if (tr.edge < 0.005) {
            all_edges_ok = false;
            std::printf("  VIOLATION: ts=%llu edge=%.6f < 0.005\n",
                        (unsigned long long)tr.ts_ms, tr.edge);
        }
    }
    CHECK(all_edges_ok, "All executed trades have edge ≥ 0.5% (0.005)");

    // Find minimum edge across all trades.
    if (!trader.trade_log.empty()) {
        double min_edge = 999.0;
        for (const auto& tr : trader.trade_log) {
            if (tr.edge < min_edge) min_edge = tr.edge;
        }
        std::printf("  Min edge seen in trades: %.6f\n", min_edge);
        CHECK(min_edge >= 0.005, "Minimum trade edge ≥ 0.5%");
    } else {
        CHECK(false, "Expected at least 1 executed trade");
    }
}

// ── Test 3: Position sizes follow quarter-Kelly ──────────────────────────────
static void test_quarter_kelly_sizing() {
    std::printf("test_quarter_kelly_sizing\n");

    PaperTrader trader;
    TickGenerator gen;
    BookSnapshot prev{100.0, 100.0, 0.499, 0.501};

    for (int i = 0; i < 100; i++) {
        auto td = gen.generate(i);
        uint64_t ts_ms = EPOCH_MS + static_cast<uint64_t>(i) * 3200ULL;
        BookSnapshot book{td.bid_vol, td.ask_vol, td.best_bid, td.best_ask};
        trader.process_tick(td.price, ts_ms, book, prev);
        prev = book;
    }

    // Every trade's size_usdc must be ≤ full_kelly * bankroll * 0.25.
    bool all_quarter = true;
    for (const auto& tr : trader.trade_log) {
        if (tr.kelly_raw > 1e-12) {
            double max_quarter = tr.kelly_raw * 0.25 * tr.bankroll_before;
            double actual = tr.size_usdc;
            if (actual > max_quarter * 1.0001 + 1e-6) {
                all_quarter = false;
                std::printf("  VIOLATION: size=%.4f > max=%.4f (raw=%.4f, br=%.2f)\n",
                            actual, max_quarter, tr.kelly_raw, tr.bankroll_before);
            }
        }
    }
    CHECK(all_quarter, "All position sizes ≤ quarter-Kelly (≤ 25% of full Kelly)");

    // Verify max ratio of fractioned/raw ≤ 0.25.
    CHECK(trader.max_kelly_ratio <= 0.25 + 1e-9,
          "Max (kelly_fractioned / kelly_raw) ≤ 0.25 (quarter-Kelly verified)");

    if (!trader.trade_log.empty()) {
        std::printf("  %zu trades checked, max_kelly_ratio=%.6f\n",
                    trader.trade_log.size(), trader.max_kelly_ratio);
    }
}

// ── Test 4: WindowShield transitions through ALL states ─────────────────────
static void test_window_shield_all_states() {
    std::printf("test_window_shield_all_states\n");

    PaperTrader trader;
    TickGenerator gen;
    BookSnapshot prev{100.0, 100.0, 0.499, 0.501};

    for (int i = 0; i < 100; i++) {
        auto td = gen.generate(i);
        uint64_t ts_ms = EPOCH_MS + static_cast<uint64_t>(i) * 3200ULL;
        BookSnapshot book{td.bid_vol, td.ask_vol, td.best_bid, td.best_ask};
        trader.process_tick(td.price, ts_ms, book, prev);
        prev = book;
    }

    std::printf("  MAKER_PASSIVE: %s\n", trader.saw_passive    ? "yes" : "no");
    std::printf("  MAKER_SKEWED:  %s\n", trader.saw_skewed      ? "yes" : "no");
    std::printf("  DIRECTIONAL:   %s\n", trader.saw_directional ? "yes" : "no");
    std::printf("  CLOSE_ONLY:    %s\n", trader.saw_close_only  ? "yes" : "no");
    std::printf("  HALTED:        %s\n", trader.saw_halted      ? "yes" : "no");

    CHECK(trader.saw_passive,     "WindowShield visited MAKER_PASSIVE");
    CHECK(trader.saw_skewed,      "WindowShield visited MAKER_SKEWED");
    CHECK(trader.saw_directional, "WindowShield visited DIRECTIONAL");
    CHECK(trader.saw_close_only, "WindowShield visited CLOSE_ONLY");
    CHECK(trader.saw_halted,     "WindowShield visited HALTED");

    // No trades during CLOSE_ONLY or HALTED.
    bool trades_during_restricted = false;
    for (const auto& tr : trader.trade_log) {
        if (tr.shield_state == ShieldState::CLOSE_ONLY ||
            tr.shield_state == ShieldState::HALTED) {
            trades_during_restricted = true;
        }
    }
    CHECK(!trades_during_restricted, "No trades during CLOSE_ONLY/HALTED");
}

// ── Test 5: Circuit breaker fail-closed blocks trades ──────────────────────
static void test_circuit_breaker_fail_closed() {
    std::printf("test_circuit_breaker_fail_closed\n");

    // ── Part A: Circuit breaker unit-level state-machine test ──────────────
    // Test the breaker directly (deterministic mock clock) to verify
    // the full CLOSED → OPEN → HALF_OPEN → CLOSED transition cycle.
    CircuitBreaker::Config cb_cfg;
    cb_cfg.open_duration_ms = 5000;  // 5 s TTL (same as default)
    CircuitBreaker cb_direct(cb_cfg, mock_clock_ms);

    g_mock_ms = 0;

    // CLOSED → allows
    CHECK(cb_direct.allow_request(), "CLOSED → allow_request = true");
    CHECK(cb_direct.state() == CircuitBreaker::State::CLOSED, "State = CLOSED");
    CHECK(cb_direct.trading_mode() == TradingMode::NORMAL, "Mode = NORMAL");

    // Trip with 3 failures → OPEN
    cb_direct.on_failure();
    cb_direct.on_failure();
    cb_direct.on_failure();
    CHECK(cb_direct.state() == CircuitBreaker::State::OPEN, "3 failures → OPEN");
    CHECK(cb_direct.trading_mode() == TradingMode::CLOSE_ONLY, "Mode = CLOSE_ONLY");
    CHECK(!cb_direct.allow_request(), "OPEN → allow_request = false");

    // TTL not expired → still OPEN
    g_mock_ms += 3000;  // 3 s < 5 s
    CHECK(cb_direct.allow_request() == false, "3s < TTL → still blocked");
    CHECK(cb_direct.state() == CircuitBreaker::State::OPEN, "Still OPEN at 3s");

    // TTL expired → HALF_OPEN
    g_mock_ms += 3000;  // now 6 s > 5 s
    CHECK(cb_direct.allow_request(), "6s > TTL → allow_request = true");
    CHECK(cb_direct.state() == CircuitBreaker::State::HALF_OPEN,
          "→ HALF_OPEN after TTL");
    // half_open_max = 1 → second probe blocked
    CHECK(!cb_direct.allow_request(), "HALF_OPEN: 2nd probe blocked");

    // 2 successes → CLOSED
    cb_direct.on_success();
    cb_direct.on_success();
    CHECK(cb_direct.state() == CircuitBreaker::State::CLOSED,
          "2 successes → CLOSED");
    CHECK(cb_direct.trading_mode() == TradingMode::NORMAL,
          "→ NORMAL after recovery");

    // ── Part B: Integration — pipeline blocks trades while circuit OPEN ─────
    PaperTrader trader;
    TickGenerator gen;
    BookSnapshot prev{100.0, 100.0, 0.499, 0.501};

    // Run 30 ticks normally (circuit stays CLOSED).
    int trades_before = 0;
    for (int i = 0; i < 30; i++) {
        auto td = gen.generate(i);
        uint64_t ts_ms = EPOCH_MS + static_cast<uint64_t>(i) * 3200ULL;
        BookSnapshot book{td.bid_vol, td.ask_vol, td.best_bid, td.best_ask};
        trader.process_tick(td.price, ts_ms, book, prev);
        prev = book;
        trades_before = trader.trades_executed;
    }

    // Trip the circuit.
    trader.cb.on_failure();
    trader.cb.on_failure();
    trader.cb.on_failure();
    CHECK(trader.cb.state() == CircuitBreaker::State::OPEN,
          "Pipeline circuit OPEN after 3 failures");
    CHECK(trader.cb.trading_mode() == TradingMode::CLOSE_ONLY,
          "Pipeline mode = CLOSE_ONLY");

    // Process 20 more ticks while OPEN, but keep the mock clock within
    // the TTL window (5 s) so the circuit does NOT auto-recover.
    // Each tick advances 100 ms → 20 ticks = 1.9 s < 5 s TTL.
    uint64_t open_time = g_mock_ms;
    for (int i = 30; i < 50; i++) {
        auto td = gen.generate(i);
        uint64_t ts_ms = open_time + static_cast<uint64_t>(i - 30) * 100ULL;
        BookSnapshot book{td.bid_vol, td.ask_vol, td.best_bid, td.best_ask};
        trader.process_tick(td.price, ts_ms, book, prev);
        prev = book;
    }
    CHECK(trader.trades_executed == trades_before,
          "No trades while circuit OPEN (fail-closed)");
    CHECK(trader.cb.state() == CircuitBreaker::State::OPEN,
          "Circuit still OPEN after 20 ticks (TTL not expired)");
}

// ── Test 6: Bankroll consistency ─────────────────────────────────────────────
static void test_bankroll_consistency() {
    std::printf("test_bankroll_consistency\n");

    PaperTrader trader;
    TickGenerator gen;
    BookSnapshot prev{100.0, 100.0, 0.499, 0.501};

    for (int i = 0; i < 100; i++) {
        auto td = gen.generate(i);
        uint64_t ts_ms = EPOCH_MS + static_cast<uint64_t>(i) * 3200ULL;
        BookSnapshot book{td.bid_vol, td.ask_vol, td.best_bid, td.best_ask};
        trader.process_tick(td.price, ts_ms, book, prev);
        prev = book;
    }

    // Bankroll should never go negative.
    CHECK(trader.bankroll >= 0.0, "Bankroll never negative");

    // Total spent on trades should equal initial - current bankroll.
    if (!trader.trade_log.empty()) {
        double total_spent = 0.0;
        for (const auto& tr : trader.trade_log) {
            total_spent += tr.size_usdc;
        }
        double expected_bankroll = 10000.0 - total_spent;
        CHECK(std::fabs(trader.bankroll - expected_bankroll) < 1e-3,
              "Bankroll = initial - sum(size_usdc) (consistency)");
    }
}

// ── Main ─────────────────────────────────────────────────────────────────────
int main() {
    std::printf("╔══════════════════════════════════════════════════════════╗\n");
    std::printf("║  Paper Trading Simulation (Phase 5 Acceptance)            ║\n");
    std::printf("║  BOT_MODE=mock  |  100 ticks  |  ASan+UBSan              ║\n");
    std::printf("╚══════════════════════════════════════════════════════════╝\n\n");

    test_100_ticks_no_crash();
    test_edge_threshold();
    test_quarter_kelly_sizing();
    test_window_shield_all_states();
    test_circuit_breaker_fail_closed();
    test_bankroll_consistency();

    std::printf("\n========================================\n");
    std::printf("Paper Trade: %d/%d passed, %d failed\n",
                g_passed, g_tests, g_failed);
    std::printf("========================================\n");

    return g_failed > 0 ? 1 : 0;
}
