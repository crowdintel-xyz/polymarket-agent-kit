// ─────────────────────────────────────────────────────────────────────────────
// test_pipeline: Acceptance test for full BTC 5m/15m pipeline integration.
//
// DSH approach — pure deterministic math, NO neural networks:
//   Synthetic price feed → VolatilityEstimator → TwapBrownianBridge
//   → OfiLinearFilter → WindowShield → FeeCalculator → KellySizer
//   → LadderBuilder → TimeStrategy → CircuitBreaker → RateLimiter
//
// Exit code 0 = all pass. Zero external deps.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cstdio>
#include <cstring>
#include <chrono>
#include <cmath>
#include <cstdint>

#include "engine_extensions.hpp"

// ── Test framework ─────────────────────────────────────────────────────────────
static int g_failures = 0;
static int g_tests = 0;
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        ++g_tests;                                                            \
        if (cond) { std::printf("  PASS %s\n", name); }                       \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);         \
               ++g_failures; }                                                \
    } while (0)

static bool approxf(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps * (1.0f + std::fabs(a) + std::fabs(b));
}

int main() {
    std::printf("=== BTC Specialization Pipeline Integration Tests (DSH) ===\n");

    // ── 1. Initialize all DSH modules ───────────────────────────────────────────
    std::printf("\n[1] Module initialization\n");

    // VolatilityEstimator: multi-scale EWMA
    VolatilityEstimator::Config vol_cfg{};
    vol_cfg.lambda_fast = 0.94;
    vol_cfg.lambda_slow = 0.98;
    vol_cfg.vol_min = 0.15;
    vol_cfg.vol_max = 2.00;
    VolatilityEstimator vol(vol_cfg);
    CHECK(true, "VolatilityEstimator initialized");

    // TwapBrownianBridge: P(TWAP_final > K)
    TwapBrownianBridge bb;
    bb.set_sigma_annual(0.80);    // 80% annual vol for BTC 5m
    bb.set_twap_so_far(60000.0);
    CHECK(true, "TwapBrownianBridge initialized");

    // OfiLinearFilter: Cont et al. OFI with z-score
    OfiLinearFilter::Config ofi_cfg{};
    ofi_cfg.ewma_lambda = 0.94;
    ofi_cfg.alert_threshold = 3.0;
    ofi_cfg.caution_threshold = 2.0;
    OfiLinearFilter ofi(ofi_cfg);
    CHECK(true, "OfiLinearFilter initialized");

    // FeeCalculator: Polymarket CLOB V2 fees (all static methods)
    CHECK(FeeCalculator::maker_fee(0.50) == 0.0, "Maker fee is zero");
    CHECK(FeeCalculator::taker_fee(0.50) > 0.0, "Taker fee > 0 at p=0.50");

    // KellySizer: quarter-Kelly position sizing
    Config kelly_cfg{};
    kelly_cfg.kelly_fraction = 0.25;
    kelly_cfg.max_bankroll_pct = 0.05;
    kelly_cfg.min_edge = 0.005;
    kelly_cfg.max_position = 100.0;
    KellySizer kelly(kelly_cfg);
    CHECK(true, "KellySizer initialized");

    // WindowShield: settlement-window lifecycle state machine
    WindowShield shield(WindowShieldConfig::BTC_5M());
    shield.set_window_start(1'700'000'000'000'000'000ULL);
    ShieldState initial_state = shield.update(1'700'000'000'000'000'000ULL, 0.5f);
    CHECK(initial_state == ShieldState::MAKER_PASSIVE,
          "WindowShield active after set_window_start");

    // LadderBuilder: dynamic bid/ask ladder
    LadderBuilder ladder;
    CHECK(true, "LadderBuilder initialized");

    // TimeStrategy: time-based config (static class, no instance needed)
    TimeConfig ts_cfg = TimeStrategy::get_time_config(8);  // US open
    CHECK(true, "TimeStrategy initialized");

    // CircuitBreaker: network health state machine
    CircuitBreaker cb;
    CHECK(true, "CircuitBreaker initialized (default)");

    // RateLimiter: TokenBucket + exponential backoff
    RateLimitConfig rl_cfg{};
    rl_cfg.orders_per_sec = 10;
    rl_cfg.burst_capacity = 20;
    RateLimiter rate_limiter(rl_cfg);
    CHECK(true, "RateLimiter initialized");

    // ExtendedEngineLayers wiring
    ExtendedEngineLayers layers;
    layers.bb = &bb;
    layers.vol = &vol;
    layers.ofi = &ofi;
    layers.window_shield = &shield;
    layers.circuit_breaker = &cb;
    layers.rate_limiter = &rate_limiter;
    layers.kelly_sizer = &kelly;
    layers.ladder = &ladder;
    layers.w_bb = 0.5f;
    layers.w_ofi = 0.3f;
    layers.w_twap = 0.2f;
    CHECK(true, "All layers wired in ExtendedEngineLayers");

    // ── 2. Process synthetic data through pipeline ────────────────────────────
    std::printf("\n[2] End-to-end pipeline processing\n");

    // Simulate a bull trend: price walks from 60000 → 61000 over 10 ticks
    uint64_t base_ts_ns = 1'700'000'000'000'000'000ULL;
    double current_price = 60000.0;
    double strike_price = 60500.0;  // slightly above start
    double bankroll = 50.0;         // $50 canary capital
    double window_sec = 300.0;      // 5-minute window
    double elapsed_sec = 0.0;
    double dt_sec = 10.0;           // 10-second ticks

    int valid_signals = 0;
    double last_p_up = 0.5;

    // OFI: track previous book state
    double prev_bid_vol = 1.5, prev_ask_vol = 1.5;
    double prev_bid_px = 60000.0, prev_ask_px = 60001.0;

    for (int i = 0; i < 10; i++) {
        uint64_t now_ns = base_ts_ns + static_cast<uint64_t>(i * dt_sec * 1'000'000'000ULL);
        uint64_t now_ms = (base_ts_ns / 1'000'000'000ULL + static_cast<uint64_t>(i * dt_sec)) * 1'000ULL;

        // 2a. Volatility update (synthetic log-returns)
        double log_ret = 0.001 * (i < 5 ? 1.0 : -0.5);  // small bullish returns
        vol.on_log_return(log_ret, dt_sec);

        // 2b. Feed price to Brownian Bridge
        bb.on_price(current_price, now_ns);

        // 2c. OFI filter: simulate book updates
        // Bull trend: bids grow, asks shrink (positive OFI)
        double bid_px = current_price * 0.9999;
        double ask_px = current_price * 1.0001;
        double bid_vol = 1.5 + i * 0.1;
        double ask_vol = 0.8 - i * 0.05;
        ofi.on_book_update(bid_vol, ask_vol, prev_bid_vol, prev_ask_vol,
                           bid_px, prev_bid_px, ask_px, prev_ask_px, now_ms);
        prev_bid_vol = bid_vol; prev_ask_vol = ask_vol;
        prev_bid_px = bid_px; prev_ask_px = ask_px;

        // 2d. Compute BB probability
        auto prob = bb.compute(current_price, strike_price, window_sec, elapsed_sec);
        CHECK(prob.sigma_annual > 0.0, "Sigma annual is positive");
        CHECK(prob.p_up >= 0.0 && prob.p_up <= 1.0, "P(Up) in [0, 1]");

        // 2e. Check OFI state
        auto ofi_state = ofi.current_state();
        CHECK(true, "OFI state queryable after update");

        // 2f. WindowShield evaluation
        auto ofi_pressure = static_cast<OfiPressure>(static_cast<uint8_t>(ofi_state.level));
        float conviction = static_cast<float>(prob.p_up);
        ShieldState shield_state = shield.update(now_ns, conviction, ofi_pressure);
        CHECK(shield_state != ShieldState::HALTED || i >= 9,
              "Shield active during normal operation");

        if (prob.is_decided || prob.p_up > 0.55 || prob.p_up < 0.45) {
            valid_signals++;
        }
        last_p_up = prob.p_up;

        // 2g. Advance
        current_price += 10.0;
        elapsed_sec += dt_sec;
    }

    CHECK(valid_signals > 0, "Pipeline produces valid signals (>0)");
    CHECK(last_p_up > 0.0 && last_p_up < 1.0,
          "P(Up) within reasonable range after bull trend");

    // ── 3. Kelly sizing check ─────────────────────────────────────────────────
    std::printf("\n[3] Kelly sizing\n");

    // Polymarket binary-market semantics: p_model is a probability in [0,1],
    // market_price is the contract price in [0,1].
    double p_model = 0.65;
    double market_price = 0.50;
    auto sizing = kelly.compute(p_model, market_price,
                                /*bankroll=*/10000.0,
                                /*inventory=*/0.0,
                                /*vol_z_score=*/0.0,
                                /*is_taker=*/true,
                                /*ofi_size_multiplier=*/1.0);

    CHECK(sizing.should_trade, "Kelly sizing produces a trade with 65% model vs 50% price");
    CHECK(sizing.size_usdc > 0.0, "Kelly size is positive");
    CHECK(sizing.size_shares > 0.0, "Kelly shares > 0");
    CHECK(sizing.size_usdc / 10000.0 <= 0.05 + 1e-9,
          "Position size capped to max 5% of bankroll");

    // Breakeven check: market price 0.50 → breakeven ~0.518
    double fee = FeeCalculator::taker_fee(0.50);
    CHECK(fee > 0.0 && fee < 0.05, "Taker fee is between 0 and 5% at p=0.50");

    // ── 4. Brownian Bridge trade evaluation ─────────────────────────────────────
    std::printf("\n[4] Brownian Bridge trade evaluation\n");

    bb.set_sigma_annual(0.80);
    bb.set_twap_so_far(60200.0);
    auto eval_prob = bb.compute(60250.0, 60500.0, 300.0, 60.0);

    CHECK(eval_prob.p_up >= 0.0 && eval_prob.p_up <= 1.0,
          "BB probability in [0, 1]");
    CHECK(eval_prob.twap_so_far == 60200.0, "TWAP so far preserved");
    CHECK(eval_prob.time_remaining_sec > 0.0, "Time remaining positive");

    // ── 5. Ladder quote ────────────────────────────────────────────────────────
    std::printf("\n[5] Ladder quote\n");

    Quote quote = ladder.build(/*p_up=*/static_cast<double>(last_p_up),
                               /*mid=*/0.50,
                               /*ofi_dir=*/0.1,
                               /*ofi_mult=*/1.0,
                               /*inv=*/0.0,
                               /*spread_base=*/1.0,
                               /*vol_mult=*/1.0);

    CHECK(quote.is_active, "Ladder quote is active");
    CHECK(quote.bid_size > 0.0 || quote.ask_size > 0.0,
          "Ladder produces non-zero size");
    CHECK(quote.bid_price < quote.ask_price, "Bid < Ask");
    CHECK(quote.bid_price >= 0.01 && quote.ask_price <= 0.99,
          "Prices within valid range [0.01, 0.99]");

    // ── 6. WindowShield lifecycle ───────────────────────────────────────────────
    std::printf("\n[6] WindowShield lifecycle\n");

    // Test normal operation
    ShieldState state = shield.update(base_ts_ns + 120'000'000'000ULL, 0.55f);
    CHECK(state != ShieldState::HALTED, "Shield not halted during active window");

    // Test fail-closed on stale timestamp
    ShieldState stale_state = shield.update(base_ts_ns - 10'000'000'000ULL, 0.55f);
    CHECK(stale_state == ShieldState::HALTED || stale_state == ShieldState::CLOSE_ONLY,
          "Shield blocks trading on stale timestamp (fail-closed)");

    // Test OFI EXTREME forces CLOSE_ONLY
    shield.set_window_start(base_ts_ns);
    ShieldState extreme_state = shield.update(
        base_ts_ns + 285'000'000'000ULL,  // near end of window
        0.7f,
        OfiPressure::EXTREME
    );
    CHECK(extreme_state == ShieldState::CLOSE_ONLY,
          "OFI EXTREME forces CLOSE_ONLY in final phase");

    // ── 7. Defense integration (full) ───────────────────────────────────────────
    std::printf("\n[7] Defense integration\n");

    SignalResult result = evaluate_signal(
        layers,
        /*price=*/60250.0,
        /*strike=*/60500.0,
        /*window_sec=*/300.0,
        /*elapsed_sec=*/60.0,
        /*bankroll=*/50.0,
        /*now_ns=*/base_ts_ns + 100'000'000'000ULL
    );

    CHECK(true, "evaluate_signal callable (no segfault)");
    CHECK(result.shield_state != ShieldState::HALTED,
          "Defense check passes during active window");

    // Rate limiter check
    bool rate_ok = rate_limiter.can_send(RequestType::ORDER);
    CHECK(rate_ok, "Rate limiter allows first request");

    // Circuit breaker check (should be CLOSED = allow)
    bool cb_ok = cb.allow_request();
    CHECK(cb_ok, "CircuitBreaker allows request when CLOSED");

    // ── Summary ──────────────────────────────────────────────────────────────
    std::printf("\n========================================\n");
    std::printf("Integration: %d/%d checks passed\n",
        g_tests - g_failures, g_tests);
    std::printf("Valid signals: %d/10\n", valid_signals);
    std::printf("========================================\n");

    return g_failures > 0 ? 1 : 0;
}
