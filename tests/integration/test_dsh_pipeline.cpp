// ─────────────────────────────────────────────────────────────────────────────
// test_dsh_pipeline: Full DSH pipeline integration test (Phase 5).
//
// Exercises the complete deterministic signal pipeline:
//
//   Binance price ticks
//     → TwapBrownianBridge::on_price() + compute()   (P(TWAP>K))
//     → VolatilityEstimator::on_log_return()         (σ EWMA, regime)
//     → volatility feedback into TwapBrownianBridge     (σ adjustment)
//     → OfiLinearFilter::on_book_update()             (OFI z-score, pressure)
//     → WindowShield::update()                        (settlement lifecycle)
//     → FeeCalculator::edge_after_fees()              (fee-aware edge)
//     → KellySizer::compute()                         (quarter-Kelly sizing)
//     → LadderBuilder::build()                        (adaptive quote ladder)
//     → TimeStrategy::get_time_config()               (time-based config)
//     → RateLimiter::can_send()                       (egress throttle)
//     → CircuitBreaker::allow_request()               (fail-closed gate)
//
// All prices are in the Polymarket binary-market domain [0.01, 0.99].
// All timestamps use a shared mock epoch so WindowShield's fail-closed
// (window_start_ns == 0 → HALTED) guard is never triggered.
//
// All components are header-only C++20 with zero heap allocation on hot path.
// Compiled and run under ASan + UBSan — must be leak-free and crash-free.
//
// Exit code 0 = all pass. No external dependencies.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>

// ── Phase 1: Core Mathematics ───────────────────────────────────────────────
#include "twap_brownian_bridge.hpp"
#include "volatility_estimator.hpp"

// ── Phase 2: Defense ─────────────────────────────────────────────────────────
#include "ofi_linear_filter.hpp"
#include "window_shield.hpp"

// ── Phase 3: Network-layer primitives ───────────────────────────────────────
#include "rate_limiter.hpp"
#include "circuit_breaker.hpp"

// ── Phase 4: Strategy ───────────────────────────────────────────────────────
#include "fee_calculator.hpp"
#include "kelly_sizer.hpp"
#include "ladder_builder.hpp"
#include "time_strategy.hpp"

// ── Test harness ─────────────────────────────────────────────────────────────
static int g_tests  = 0;
static int g_passed = 0;
static int g_failed = 0;

#define CHECK(cond, name)                                                      \
    do {                                                                       \
        ++g_tests;                                                             \
        if (cond) { ++g_passed; std::printf("  PASS  %s\n", name); }           \
        else { ++g_failed; std::printf("  FAIL  %s (line %d)\n", name, __LINE__); } \
    } while (0)

#define CHECK_EQ(actual, expected, name)                                       \
    do {                                                                       \
        ++g_tests;                                                             \
        if ((actual) == (expected)) { ++g_passed; std::printf("  PASS  %s\n", name); } \
        else { ++g_failed; std::printf("  FAIL  %s: got %llu, expected %llu (line %d)\n", \
               name,                                                          \
               static_cast<unsigned long long>(actual),                        \
               static_cast<unsigned long long>(expected),                      \
               __LINE__); }                                                    \
    } while (0)

// ── Mock clock (deterministic, no real sleeps) ──────────────────────────────
static uint64_t g_mock_ms = 0;
static uint64_t mock_clock_ms() noexcept { return g_mock_ms; }

// ── Shared epoch: WindowShield treats start_ns==0 as HALTED (fail-closed).
//    Use a non-zero base so the guard never fires.
//    All tick timestamps are relative to this base.
static constexpr uint64_t EPOCH_MS = 1'000'000ULL;       // 1 000 s in ms
static constexpr uint64_t EPOCH_NS = EPOCH_MS * 1'000'000ULL;

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

// ── Order-book snapshot (for OFI filter) ────────────────────────────────────
struct BookSnapshot {
    double bid_vol, ask_vol;     // current volumes at best level
    double best_bid, best_ask;   // current best prices
};

// ── Pipeline container: wires all 11 components together ────────────────────
struct DshPipeline {
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
    TimeStrategy         ts;                 // static methods only

    // ── Mutable state ─────────────────────────────────────────────────────
    double               bankroll = 10000.0;   // $10k mock
    double               inventory = 0.0;      // position in shares
    double               strike_price = 0.50;  // Polymarket binary at 50¢
    double               prev_price_ = 0.50;   // running previous price for vol
    double               total_fees_paid = 0.0;
    int                  trades_executed = 0;
    int                  trades_blocked = 0;
    int                  orders_sent = 0;

    // ── Process a single combined tick ────────────────────────────────────
    struct TickResult {
        ProbResult        prob;
        SizingResult      sizing;
        Quote             quote;
        TimeConfig        time_cfg;
        ShieldState       shield_state;
        TradingMode       trading_mode;
        bool              rate_limited;
        bool              circuit_open;
        bool              trade_allowed;
        bool              trade_executed;
        double            ofi_direction;
        double            edge;
        double            fee;
        OfiLinearFilter::PressureLevel ofi_pressure;
    };

    TickResult process_tick(double price, uint64_t ts_ms,
                            const BookSnapshot& book,
                            const BookSnapshot& prev_book) noexcept
    {
        TickResult tr{};

        // ── 1. Feed price tick into TwapBrownianBridge ───────────────────
        bb.on_price(price, ts_ms);

        // ── 2. Compute log return for VolatilityEstimator ─────────────────
        if (prev_price_ > 0.0 && price > 0.0) {
            double log_ret = std::log(price / prev_price_);
            double dt_sec = 1.0;  // 1-second tick interval in mock
            vol.on_log_return(log_ret, dt_sec);
        }
        prev_price_ = price;

        // ── 3. Volatility feedback → adjust BB sigma ─────────────────────
        double vol_eff = vol.effective_vol();
        bb.set_sigma_annual(vol_eff);
        double vol_mult = vol.regime_vol_multiplier();

        // ── 4. Compute Brownian Bridge probability ───────────────────────
        // 60s TWAP window, halfway through (30s elapsed).
        double elapsed = 30.0;
        double tau_total = 60.0;
        tr.prob = bb.compute(price, strike_price, tau_total, elapsed);

        // ── 5. OFI linear filter ─────────────────────────────────────────
        ofi.on_book_update(book.bid_vol, book.ask_vol,
                           prev_book.bid_vol, prev_book.ask_vol,
                           book.best_bid, prev_book.best_bid,
                           book.best_ask, prev_book.best_ask,
                           ts_ms);
        auto ofi_state = ofi.current_state();
        tr.ofi_direction = ofi.ofi_direction();
        tr.ofi_pressure = ofi_state.level;

        // ── 6. WindowShield lifecycle ──────────────────────────────────────
        // now_ns is in the same ns domain as EPOCH_NS.
        uint64_t now_ns = ts_ms * 1'000'000ULL;
        OfiPressure ofi_p = map_pressure(tr.ofi_pressure);
        tr.shield_state = shield.update(now_ns,
                                        static_cast<float>(tr.prob.p_up),
                                        ofi_p);
        tr.trading_mode = cb.trading_mode();

        // ── 7. Fee calculator ────────────────────────────────────────────
        tr.fee = FeeCalculator::taker_fee(strike_price);
        tr.edge = FeeCalculator::edge_after_fees(tr.prob.p_up, strike_price, true);

        // ── 8. Kelly sizer ──────────────────────────────────────────────
        tr.sizing = sizer.compute(tr.prob.p_up,        // p_model
                                  strike_price,         // market price
                                  bankroll,
                                  inventory,
                                  vol_mult - 1.0,     // vol_z_score proxy
                                  true,                // is_taker
                                  ofi_state.size_multiplier);  // ofi_size_mult

        // ── 9. Ladder builder ────────────────────────────────────────────
        tr.quote = ladder.build(tr.prob.p_up, strike_price,
                                tr.ofi_direction,
                                ofi_state.size_multiplier,
                                inventory,
                                /*spread_base=*/5.0,
                                vol_mult);

        // ── 10. Time strategy ───────────────────────────────────────────
        uint32_t hour_utc = static_cast<uint32_t>((ts_ms / 3'600'000ULL) % 24ULL);
        tr.time_cfg = TimeStrategy::get_time_config(hour_utc);

        // ── 11. Rate limiter + Circuit breaker ───────────────────────────
        bool cb_ok = cb.allow_request();
        tr.circuit_open = (cb.trading_mode() != TradingMode::NORMAL);

        bool rl_ok = rl.can_send(RequestType::ORDER);
        tr.rate_limited = !rl_ok;

        // ── 12. Trade decision ───────────────────────────────────────────
        bool shield_ok = (tr.shield_state != ShieldState::CLOSE_ONLY &&
                          tr.shield_state != ShieldState::HALTED);

        tr.trade_allowed = (tr.sizing.should_trade &&
                            shield_ok && cb_ok && rl_ok &&
                            tr.edge >= 0.005);  // min_edge: 0.5%

        if (tr.trade_allowed) {
            double trade_cost = tr.sizing.size_usdc * tr.fee / strike_price;
            if (bankroll >= tr.sizing.size_usdc) {
                bankroll -= tr.sizing.size_usdc;
                inventory += tr.sizing.size_shares;
                total_fees_paid += trade_cost;
                trades_executed++;
                orders_sent++;
                rl.on_sent(RequestType::ORDER);
                cb.on_success();
                tr.trade_executed = true;
            } else {
                trades_blocked++;
                cb.on_success();
            }
        } else {
            trades_blocked++;
            cb.on_success();
        }

        return tr;
    }

    // Initialise WindowShield with the shared epoch.
    void init_shield() noexcept {
        shield.set_window_start(EPOCH_NS);
    }
};

// ── Test 1: Pipeline construction & wiring ──────────────────────────────────
static void test_pipeline_construction() {
    std::printf("test_pipeline_construction\n");
    DshPipeline p;

    CHECK(p.shield.current_state() == ShieldState::MAKER_PASSIVE,
          "WindowShield initial state = MAKER_PASSIVE");
    CHECK(p.cb.state() == CircuitBreaker::State::CLOSED,
          "CircuitBreaker initial state = CLOSED");
    CHECK(p.cb.trading_mode() == TradingMode::NORMAL,
          "CircuitBreaker initial trading_mode = NORMAL");
    CHECK(p.ofi.current_state().level == OfiLinearFilter::PressureLevel::NORMAL,
          "OfiLinearFilter initial pressure = NORMAL");
    CHECK(p.vol.current_regime() == VolatilityEstimator::Regime::NORMAL,
          "VolatilityEstimator initial regime = NORMAL");

    TwapBBConfig cfg;
    CHECK(cfg.min_edge_threshold == 0.005,
          "TwapBBConfig min_edge_threshold = 0.005 (0.5%)");
    CHECK(cfg.max_kelly_fraction == 0.05,
          "TwapBBConfig max_kelly_fraction = 0.05");
}

// ── Test 2: Single tick through full pipeline ────────────────────────────────
// Uses TWAP above strike → high p_up → positive edge → trade viable.
static void test_single_tick() {
    std::printf("test_single_tick\n");
    DshPipeline p;
    p.init_shield();

    // Tick at t=1s into the window (EPOCH_MS + 1000).
    uint64_t ts_ms = EPOCH_MS + 1000;

    // Price = 0.51 (above strike), symmetric book.
    BookSnapshot book{100.0, 100.0, 0.509, 0.511};
    BookSnapshot prev{100.0, 100.0, 0.499, 0.501};

    // Seed TWAP above strike so p_up is high.
    p.bb.set_twap_so_far(0.52);
    p.bb.set_sigma_annual(0.40);

    auto tr = p.process_tick(0.51, ts_ms, book, prev);

    // With TWAP=0.52 > strike=0.50, p_up should be high (>0.75).
    CHECK(tr.prob.p_up > 0.75,
          "TWAP above strike → p_up > 0.75");

    // OFI: symmetric update → NORMAL pressure.
    CHECK(tr.ofi_pressure == OfiLinearFilter::PressureLevel::NORMAL,
          "Symmetric book → OFI NORMAL pressure");

    // Shield: 1s into window → MAKER_PASSIVE.
    CHECK(tr.shield_state == ShieldState::MAKER_PASSIVE,
          "Early window → MAKER_PASSIVE");

    // Edge should be ≥ 0.5% when p_up is high.
    CHECK(tr.edge >= 0.005 || !tr.sizing.should_trade,
          "Edge ≥ 0.5% only when trade deemed viable");

    // Quote: symmetric when OFI is neutral.
    CHECK(tr.quote.is_active, "Quote is active (ofi_multiplier > 0)");
    CHECK(tr.quote.bid_price < tr.quote.ask_price, "Bid < Ask");
    CHECK(tr.quote.bid_price >= 0.01 && tr.quote.ask_price <= 0.99,
          "Quote prices in [0.01, 0.99]");

    // Time config: should be valid.
    CHECK(tr.time_cfg.size_mult > 0.0, "Time config size_mult > 0");
    CHECK(tr.time_cfg.confidence_threshold > 0.0, "Time config threshold > 0");

    // Rate + circuit: both should allow initially.
    CHECK(!tr.rate_limited, "Rate limiter allows (fresh bucket)");
    CHECK(!tr.circuit_open, "Circuit breaker allows (CLOSED)");
}

// ── Test 3: Volatility feeds back into Brownian Bridge ───────────────────────
static void test_volatility_feedback() {
    std::printf("test_volatility_feedback\n");
    DshPipeline p;
    p.init_shield();

    uint64_t ts_ms = EPOCH_MS + 1;  // start at 1ms past epoch
    BookSnapshot prev{100.0, 100.0, 0.499, 0.501};

    for (int i = 0; i < 300; i++) {
        // Alternate price above/below strike to generate volatility.
        double price = 0.50 + (i % 2 == 0 ? 0.02 : -0.02);
        BookSnapshot book{
            100.0 + (i % 5) * 10.0,
            100.0 + (i % 5) * 10.0,
            price - 0.001,
            price + 0.001
        };
        p.process_tick(price, ts_ms + static_cast<uint64_t>(i), book, prev);
        prev = book;
    }

    double vol = p.vol.effective_vol();
    CHECK(vol >= 0.15, "VolatilityEstimator effective_vol ≥ 0.15 (floor)");
    CHECK(vol <= 2.00, "VolatilityEstimator effective_vol ≤ 2.00 (cap)");

    double sigma = p.bb.get_sigma_annual();
    CHECK(sigma >= 0.15 && sigma <= 2.00,
          "TwapBrownianBridge sigma reflects vol estimator (clamped [0.15, 2.0])");

    double mult = p.vol.regime_vol_multiplier();
    CHECK(mult > 0.0, "Regime vol multiplier > 0 (active)");
}

// ── Test 4: OFI pressure propagates to WindowShield ──────────────────────────
// Buy pressure: bid volume grows continuously, ask volume shrinks,
// prices rise.  This creates a strongly positive z-score.
static void test_ofi_pressure_propagation() {
    std::printf("test_ofi_pressure_propagation\n");
    DshPipeline p;
    p.init_shield();

    uint64_t ts_ms = EPOCH_MS + 1;  // non-zero base
    // Start with large ask volume, small bid volume.
    BookSnapshot prev{10.0, 1000.0, 0.500, 0.501};

    for (int i = 0; i < 30; i++) {
        // Continuously growing bid vol, shrinking ask vol, rising prices.
        double bid_vol = 10.0 + (i + 1) * 50.0;
        double ask_vol = 1000.0 - (i + 1) * 30.0;
        double best_bid = 0.500 + (i + 1) * 0.001;
        double best_ask = 0.501 + (i + 1) * 0.001;
        BookSnapshot book{bid_vol, ask_vol, best_bid, best_ask};

        p.process_tick(0.50, ts_ms + static_cast<uint64_t>(i) * 1000ULL,
                       book, prev);
        prev = book;
    }

    auto ofi_state = p.ofi.current_state();
    std::printf("  z=%.2f, level=%d\n", ofi_state.ofi_zscore,
                static_cast<int>(ofi_state.level));
    CHECK(ofi_state.ofi_zscore > 1.0,
          "OFI detects buy pressure (z > 1.0)");
    CHECK(ofi_state.level == OfiLinearFilter::PressureLevel::HIGH ||
          ofi_state.level == OfiLinearFilter::PressureLevel::EXTREME ||
          ofi_state.level == OfiLinearFilter::PressureLevel::CAUTION,
          "OFI pressure elevated (>= CAUTION)");
}

// ── Test 5: Rate limiter + circuit breaker gating ───────────────────────────
static void test_defense_gating() {
    std::printf("test_defense_gating\n");
    DshPipeline p;
    p.init_shield();

    CHECK(p.cb.allow_request(), "Circuit breaker allows (CLOSED)");
    CHECK(p.cb.trading_mode() == TradingMode::NORMAL,
          "Trading mode = NORMAL (CLOSED)");

    // Trip the circuit with 3 failures.
    p.cb.on_failure();
    p.cb.on_failure();
    p.cb.on_failure();
    CHECK(p.cb.state() == CircuitBreaker::State::OPEN,
          "Circuit opens after 3 failures");
    CHECK(p.cb.trading_mode() == TradingMode::CLOSE_ONLY,
          "Trading mode = CLOSE_ONLY (OPEN)");
    CHECK(!p.cb.allow_request(), "Circuit blocks requests when OPEN");

    // Rate limiter: exhaust burst tokens.
    RateLimitConfig cfg;
    cfg.orders_per_sec = 10;
    cfg.burst_capacity = 10;
    RateLimiter rl(cfg, mock_clock_ms);
    int allowed = 0;
    for (int i = 0; i < 10; i++) {
        if (rl.can_send(RequestType::ORDER)) allowed++;
    }
    CHECK_EQ(allowed, 10, "10 orders allowed (burst)");
    CHECK(!rl.can_send(RequestType::ORDER), "11th order blocked (burst exhausted)");

    // After advancing clock + success, circuit recovers.
    g_mock_ms += 6000;  // past TTL
    p.cb.allow_request();  // triggers HALF_OPEN
    CHECK(p.cb.state() == CircuitBreaker::State::HALF_OPEN,
          "Circuit → HALF_OPEN after TTL");
    p.cb.on_success();
    p.cb.on_success();
    CHECK(p.cb.state() == CircuitBreaker::State::CLOSED,
          "Circuit → CLOSED after 2 successes");
    CHECK(p.cb.trading_mode() == TradingMode::NORMAL,
          "Trading mode → NORMAL after recovery");
}

// ── Test 6: Kelly sizing respects quarter-Kelly + bankroll cap ─────────────
static void test_kelly_quarter_fraction() {
    std::printf("test_kelly_quarter_fraction\n");
    Config kc;
    kc.kelly_fraction = 0.25;      // quarter-Kelly
    kc.max_bankroll_pct = 0.05;    // 5% max of bankroll
    KellySizer sizer(kc);

    // p=0.60, price=0.50 → b = (1/0.50) - 1 = 1.0, q = 0.40
    // f* = (0.60*1.0 - 0.40) / 1.0 = 0.20 (full Kelly)
    // quarter → 0.20 * 0.25 = 0.05
    auto r = sizer.compute(0.60, 0.50, 10000.0, 0.0, 0.0, true, 1.0);
    CHECK(r.should_trade, "Trade with p=0.60, price=0.50");
    CHECK(r.kelly_raw > 0.0, "kelly_raw > 0");
    CHECK(r.kelly_fractioned < r.kelly_raw,
          "Quarter Kelly < full Kelly (fractioned < raw)");
    CHECK(r.kelly_fractioned <= 0.05 + 1e-9,
          "Kelly capped at max_bankroll_pct (5%)");
    CHECK(r.size_shares <= 100.0 + 1e-9,
          "Shares capped at max_position (100)");

    // Verify exact quarter-Kelly: fractioned = kelly_raw * 0.25
    double expected_qk = r.kelly_raw * 0.25;
    CHECK(std::fabs(r.kelly_fractioned - expected_qk) < 1e-9,
          "Quarter-Kelly factor exactly 0.25");

    // Edge below threshold → no trade.
    // p=0.51, price=0.50, fee=0.018 → edge = -0.008 < 0.005
    auto r2 = sizer.compute(0.51, 0.50, 10000.0, 0.0, 0.0, true, 1.0);
    CHECK(!r2.should_trade, "No trade when edge < 0.5% threshold");
    CHECK(r2.edge < 0.005, "Edge < min_edge_threshold (0.5%)");
}

// ── Test 7: WindowShield transitions through all states ─────────────────────
static void test_window_shield_transitions() {
    std::printf("test_window_shield_transitions\n");
    auto cfg = WindowShieldConfig::BTC_5M();
    WindowShield ws(cfg);

    // Non-zero start to avoid the fail-closed (start_ns==0 → HALTED) guard.
    uint64_t start_ns = 1'000'000'000'000ULL;
    ws.set_window_start(start_ns);

    CHECK(ws.update(start_ns + 1ULL * 1'000'000'000ULL, 0.5f)
          == ShieldState::MAKER_PASSIVE,
          "t=0 → MAKER_PASSIVE");

    CHECK(ws.update(start_ns + 100ULL * 1'000'000'000ULL, 0.5f)
          == ShieldState::MAKER_PASSIVE,
          "t=100s → MAKER_PASSIVE");

    CHECK(ws.update(start_ns + 250ULL * 1'000'000'000ULL, 0.7f)
          == ShieldState::MAKER_SKEWED,
          "t=250s, CFC=0.7 → MAKER_SKEWED");

    CHECK(ws.update(start_ns + 280ULL * 1'000'000'000ULL, 0.8f)
          == ShieldState::DIRECTIONAL,
          "t=280s, CFC=0.8 → DIRECTIONAL");

    CHECK(ws.update(start_ns + 295ULL * 1'000'000'000ULL, 0.95f)
          == ShieldState::CLOSE_ONLY,
          "t=295s → CLOSE_ONLY");

    CHECK(ws.update(start_ns + 301ULL * 1'000'000'000ULL, 0.95f)
          == ShieldState::HALTED,
          "t=301s → HALTED");
}

// ── Test 8: OFI EXTREME pressure overrides WindowShield → CLOSE_ONLY ─────────
static void test_ofi_extreme_override() {
    std::printf("test_ofi_extreme_override\n");
    auto cfg = WindowShieldConfig::BTC_5M();
    WindowShield ws(cfg);

    uint64_t start_ns = 1'000'000'000'000ULL;
    ws.set_window_start(start_ns);

    CHECK(ws.update(start_ns + 120ULL * 1'000'000'000ULL, 0.5f,
                    OfiPressure::NORMAL) == ShieldState::MAKER_PASSIVE,
          "t=120s, NORMAL → MAKER_PASSIVE");

    CHECK(ws.update(start_ns + 120ULL * 1'000'000'000ULL, 0.5f,
                    OfiPressure::EXTREME) == ShieldState::CLOSE_ONLY,
          "EXTREME → CLOSE_ONLY override");

    CHECK(ws.update(start_ns + 120ULL * 1'000'000'000ULL, 0.5f,
                    OfiPressure::NORMAL) == ShieldState::MAKER_PASSIVE,
          "EXTREME clears → back to PASSIVE");
}

// ── Test 9: Latency measurement of full pipeline tick ─────────────────────────
static void test_pipeline_latency() {
    std::printf("test_pipeline_latency\n");
    DshPipeline p;
    p.init_shield();

    uint64_t ts_ms = EPOCH_MS + 1;
    BookSnapshot prev{100.0, 100.0, 0.499, 0.501};

    // Warm up
    BookSnapshot book{100.0, 100.0, 0.499, 0.501};
    p.process_tick(0.50, ts_ms, book, prev);
    prev = book;

    // Measure 50k ticks
    auto t0 = std::chrono::high_resolution_clock::now();
    volatile int sink = 0;
    for (int i = 0; i < 50'000; i++) {
        double price = 0.50 + static_cast<double>(i % 7 - 3) * 0.001;
        BookSnapshot bk{
            100.0 + (i % 5) * 10.0,
            100.0 + (i % 5) * 10.0,
            price - 0.001,
            price + 0.001
        };
        auto tr = p.process_tick(price, ts_ms + static_cast<uint64_t>(i), bk, prev);
        sink += (tr.trade_executed ? 1 : 0);
        prev = bk;
    }
    auto t1 = std::chrono::high_resolution_clock::now();
    auto ns = std::chrono::duration_cast<std::chrono::nanoseconds>(t1 - t0).count();
    double ns_per_tick = static_cast<double>(ns) / 50'000.0;
    std::printf("  50k ticks: %.1f ns/tick (sink=%d)\n", ns_per_tick, (int)sink);
    CHECK(ns_per_tick < 50000.0, "Full pipeline < 50 μs per tick (p50 under ASan)");
}

// ── Main ─────────────────────────────────────────────────────────────────────
int main() {
    std::printf("╔══════════════════════════════════════════════════════════╗\n");
    std::printf("║  DSH Pipeline Integration Tests (Phase 5)                 ║\n");
    std::printf("╚══════════════════════════════════════════════════════════╝\n\n");

    test_pipeline_construction();
    test_single_tick();
    test_volatility_feedback();
    test_ofi_pressure_propagation();
    test_defense_gating();
    test_kelly_quarter_fraction();
    test_window_shield_transitions();
    test_ofi_extreme_override();
    test_pipeline_latency();

    std::printf("\n========================================\n");
    std::printf("Integration: %d/%d passed, %d failed\n",
                g_passed, g_tests, g_failed);
    std::printf("========================================\n");

    return g_failed > 0 ? 1 : 0;
}
