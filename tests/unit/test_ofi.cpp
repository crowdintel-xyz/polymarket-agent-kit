// ─────────────────────────────────────────────────────────────────────────────
// test_ofi: unit tests for Phase 1 data-pipeline components.
//
//   OFICalculator (known-answer + throughput)
//   BinanceWSClient (mock feed, SPSC ring, metrics)
//   BinanceOFIAdapter (OFI → LR conversion, rate limiting, hot-reload)
//
// Exit code 0 = all pass.  No external test framework (zero deps).
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../../core/include/spsc_ring_buffer.hpp"
#include "../../core/include/source_reliability.hpp"
#include "../../core/include/evidence.hpp"
#include "../../core/include/time_utils.hpp"
#include "../../core/src/ofi_calculator.hpp"
#include "../../core/src/binance_ws_client.hpp"
#include "../../core/src/evidence_ingress.hpp"

// ── Test framework ─────────────────────────────────────────────────────────────
static int g_failures = 0;
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) { std::printf("  PASS %s\n", name); }                       \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);         \
               ++g_failures; }                                                \
    } while (0)

static bool approx(double a, double b, double eps = 1e-6) {
    return std::fabs(a - b) <= eps * (1.0 + std::fabs(a) + std::fabs(b));
}

// ── OFI Calculator: known-answer tests ─────────────────────────────────────────

// Test 1: Empty calculator returns no state.
static void test_ofi_empty_state() {
    std::printf("ofi_empty_state\n");
    OFICalculator calc{OFIConfig{}};
    MarketState state{};
    CHECK(!calc.current_state(state), "no state before events");
    CHECK(approx(state.ofi_normalized, 0.0), "default ofi_normalized is zero");
}

// Test 2: Single depth event — OFI should be zero (no delta from 0 to volume).
static void test_ofi_single_depth() {
    std::printf("ofi_single_depth\n");
    OFICalculator calc{OFIConfig{}};
    const uint64_t t0 = 1'000'000'000ULL;
    // First depth: establishes baseline, no prior volume → delta is zero
    calc.on_event(26500.0, 26510.0, 10.0, 8.0, false, t0);
    MarketState state{};
    CHECK(calc.current_state(state), "state exists after event");
    // Initial event: ofi_running = 0.95*0 + 0 = 0 (no delta from baseline)
    CHECK(approx(state.ofi_normalized, 0.0, 1e-10),
          "first OFI is zero (no prior depth to compare)");
    CHECK(approx(state.spread_bps, 10000.0 * 10.0 / 26505.0, 1e-3),
          "spread_bps computed correctly");
    CHECK(approx(state.depth_imbalance, (10.0 - 8.0) / 18.0, 1e-6),
          "depth_imbalance computed correctly");
    CHECK(approx(state.microprice, (10.0 * 26510.0 + 8.0 * 26500.0) / 18.0, 1e-3),
          "microprice is liquidity-weighted");
}

// Test 3: Two depth events — verify EWMA accumulation.
static void test_ofi_two_depth_events() {
    std::printf("ofi_two_depth_events\n");
    OFICalculator calc{OFIConfig{}};
    const uint64_t t0 = 1'000'000'000ULL;
    const uint64_t t1 = t0 + 100'000'000ULL;  // +100ms

    // Establish baseline
    calc.on_event(26500.0, 26510.0, 10.0, 8.0, false, t0);
    // Second event: bid vol increases by 5, ask vol unchanged
    // event_delta = (15 - 10) - (8 - 8) = 5
    // ofi_running = 0.95 * 0 + 5 = 5
    // ofi_normalized = 5 / (15 + 8) = 5/23
    calc.on_event(26500.0, 26510.0, 15.0, 8.0, false, t1);

    MarketState state{};
    CHECK(calc.current_state(state), "state exists");
    const double expected_ofi = 5.0 / 23.0;
    CHECK(approx(state.ofi_normalized, expected_ofi, 1e-6),
          "OFI_EWMA = 0.95*0 + 5, normalized = 5/23");
}

// Test 4: Trade events — buyer-initiated increases OFI.
static void test_ofi_trade_events() {
    std::printf("ofi_trade_events\n");
    OFICalculator calc{OFIConfig{}};
    const uint64_t t0 = 1'000'000'000ULL;
    const uint64_t t1 = t0 + 100'000'000ULL;

    // Establish depth baseline
    calc.on_event(26500.0, 26510.0, 10.0, 8.0, false, t0);

    // Buyer-initiated trade: positive bid_vol signal
    // event_delta = -12.0 (we pass signed trade vol as bid_vol for trades,
    // which is the "event flow").  Actually for trades, the convention in
    // the BinanceWSClient is: bid_vol = signed trade size.
    // Positive = buyer-initiated, which should increase OFI.
    // ofi_running = 0.95 * 0 + 12 = 12
    // ofi_norm = 12 / (10 + 8) = 12/18
    calc.on_event(26500.0, 26510.0, 12.0, 8.0, true, t1);
    MarketState state{};
    CHECK(calc.current_state(state), "state after trade");
    CHECK(state.trade_intensity > 0.0, "trade_intensity set for trade event");
    CHECK(state.ofi_normalized > 0.0, "positive OFI for buyer-initiated trade");
}

// Test 5: EWMA decay — multiple events converge.
static void test_ofi_ewma_decay() {
    std::printf("ofi_ewma_decay\n");
    OFICalculator calc{OFIConfig{}};
    const uint64_t t0 = 1'000'000'000ULL;

    // Establish baseline (first event → zero delta)
    calc.on_event(26500.0, 26510.0, 10.0, 10.0, false, t0);

    // Apply 10 events with constant bid delta of 10 each:
    //   bid_vol = 20, 30, 40, 50, ... (delta = 10 each time)
    //   ask_vol stays at 10 (delta = 0)
    //   event_delta = 10 - 0 = 10 each time
    const double dt = 100'000'000ULL;  // 100ms
    double expected_ofi = 0.0;
    for (int i = 0; i < 10; ++i) {
        expected_ofi = OFIConfig::DECAY_LAMBDA * expected_ofi + 10.0;
        const double bid_vol = 20.0 + static_cast<double>(i) * 10.0;
        calc.on_event(26500.0, 26510.0, bid_vol, 10.0, false,
                      t0 + static_cast<uint64_t>(dt * (i + 1)));
    }
    MarketState state{};
    CHECK(calc.current_state(state), "state after decay sequence");
    // Last event: bid_vol = 20 + 9*10 = 110, ask_vol = 10
    // vol_sum = 120, expected_norm = expected_ofi / 120
    const double expected_norm = expected_ofi / 120.0;
    CHECK(approx(state.ofi_normalized, expected_norm, 1e-4),
          "EWMA decay follows λ*prev + delta");
}

// Test 6: Reset clears all state.
static void test_ofi_reset() {
    std::printf("ofi_reset\n");
    OFICalculator calc{OFIConfig{}};
    const uint64_t t0 = 1'000'000'000ULL;
    calc.on_event(26500.0, 26510.0, 10.0, 8.0, false, t0);
    calc.reset();
    MarketState state{};
    CHECK(!calc.current_state(state), "no state after reset");
}

// Test 7: Mid velocity computation.
static void test_ofi_mid_velocity() {
    std::printf("ofi_mid_velocity\n");
    OFICalculator calc{OFIConfig{}};
    const uint64_t t0 = 1'000'000'000ULL;
    const uint64_t t1 = t0 + 500'000'000ULL;  // +500ms

    calc.on_event(26500.0, 26510.0, 10.0, 10.0, false, t0);  // mid = 26505
    calc.on_event(26505.0, 26515.0, 10.0, 10.0, false, t1);  // mid = 26510, Δ=5

    MarketState state{};
    CHECK(calc.current_state(state), "state exists");
    // mid_velocity = (Δmid / prev_mid) * 10000 / dt_sec
    // = (5.0 / 26505.0) * 10000 / 0.5
    const double expected_vel = (5.0 / 26505.0) * 10000.0 / 0.5;
    CHECK(approx(state.mid_velocity, expected_vel, 1e-2),
          "mid_velocity = Δmid/mid * 1e4 / dt");
}

// ── OFI Throughput Benchmark ───────────────────────────────────────────────────
static void test_ofi_throughput() {
    std::printf("ofi_throughput\n");
    OFICalculator calc{OFIConfig{}};
    const uint64_t t0 = 1'000'000'000ULL;

    // Establish baseline
    calc.on_event(26500.0, 26510.0, 10.0, 8.0, false, t0);

    constexpr uint64_t N = 1'000'000;
    const auto start = std::chrono::steady_clock::now();
    double sink = 0.0;
    for (uint64_t i = 0; i < N; ++i) {
        calc.on_event(26500.0, 26510.0, 10.0 + static_cast<double>(i & 7), 8.0, false,
                      t0 + (i + 1) * 100'000ULL);
        // Accumulate to prevent dead-code elimination
        MarketState state{};
        calc.current_state(state);
        sink += state.ofi_normalized;
    }
    const auto end = std::chrono::steady_clock::now();
    const double elapsed_ms = std::chrono::duration<double, std::milli>(
        end - start).count();
    const double ns_per_event = (elapsed_ms * 1e6) / static_cast<double>(N);
    std::printf("  INFO: %llu events in %.2f ms (%.1f ns/event, sink=%.6f)\n",
                static_cast<unsigned long long>(N),
                elapsed_ms, ns_per_event, sink);
    // Prevent sink from being optimized away
    if (sink == 0.0) std::printf("  WARN: sink was zero\n");
    CHECK(ns_per_event < 200.0, "OFI < 200ns/event (hot-path budget: 2μs)");
}

// ── OFI Clamp test ─────────────────────────────────────────────────────────────
static void test_ofi_clamp() {
    std::printf("ofi_clamp\n");
    OFICalculator calc{OFIConfig{}};
    const uint64_t t0 = 1'000'000'000ULL;
    calc.on_event(26500.0, 26510.0, 1.0, 1.0, false, t0);
    // Large bid delta with tiny ask → normalized OFI should clamp to [-1, 1]
    calc.on_event(26500.0, 26510.0, 1'000'000.0, 0.001, false, t0 + 100'000'000ULL);
    MarketState state{};
    CHECK(calc.current_state(state), "state after large delta");
    CHECK(state.ofi_normalized >= -1.0 && state.ofi_normalized <= 1.0,
          "OFI normalized clamped to [-1, 1]");
}

// ── OFI zero-division guard ────────────────────────────────────────────────────
static void test_ofi_zero_volume() {
    std::printf("ofi_zero_volume\n");
    OFICalculator calc{OFIConfig{}};
    const uint64_t t0 = 1'000'000'000ULL;
    calc.on_event(26500.0, 26510.0, 0.0, 0.0, false, t0);
    MarketState state{};
    CHECK(calc.current_state(state), "state with zero volume");
    CHECK(approx(state.ofi_normalized, 0.0), "zero volume → zero OFI (no NaN)");
    CHECK(approx(state.microprice, 26505.0), "zero volume → mid as microprice");
}

// ── BinanceWSClient: mock feed ─────────────────────────────────────────────────
static void test_binance_client_mock() {
    std::printf("binance_client_mock\n");
    BinanceConfig cfg{};
    BinanceWSClient client{cfg};
    CHECK(!client.is_connected(), "not connected before start");
    CHECK(client.events_produced() == 0, "zero events before start");

    client.start();
    // Let the mock loop run briefly
    std::this_thread::sleep_for(std::chrono::milliseconds(500));
    client.stop();

    CHECK(client.events_produced() > 0, "mock feed produced events");
    CHECK(client.reconnect_count() == 0, "no reconnects in mock mode");

    // Drain the ring buffer and verify we get valid MarketStates
    MarketState state{};
    uint64_t drained = 0;
    bool valid_state_found = false;
    while (client.market_q().try_pop(state)) {
        ++drained;
        if (state.ofi_normalized >= -1.0 && state.ofi_normalized <= 1.0 &&
            !std::isnan(state.spread_bps)) {
            valid_state_found = true;
        }
    }
    std::printf("  INFO: drained %llu MarketStates, valid ones: %s\n",
                static_cast<unsigned long long>(drained),
                valid_state_found ? "yes" : "no");
    CHECK(valid_state_found, "at least one valid MarketState in ring");
}

// ── BinanceWSClient: inject_event API ──────────────────────────────────────────
static void test_binance_client_inject() {
    std::printf("binance_client_inject\n");
    BinanceConfig cfg{};
    BinanceWSClient client{cfg};

    const uint64_t t0 = 1'000'000'000ULL;
    client.inject_event(26500.0, 26510.0, 10.0, 8.0, false, t0);
    client.inject_event(26500.0, 26510.0, 12.0, 8.0, true, t0 + 100'000'000ULL);

    CHECK(client.events_produced() == 2, "two events injected");

    // Drain and verify
    MarketState state{};
    uint64_t count = 0;
    while (client.market_q().try_pop(state)) ++count;
    CHECK(count == 2, "two MarketStates in ring");
}

// ── Evidence Adapter: OFI → LR conversion ──────────────────────────────────────
static void test_ofi_to_lr() {
    std::printf("ofi_to_lr\n");
    // Known answers for ofi_to_lr_x1e6:
    // atanh(0) = 0 → lr_x1e6 = 0
    CHECK(binance_ofi::ofi_to_lr_x1e6(0.0) == 0, "zero OFI → zero LR");

    // atanh(0.5) = 0.5493061443... → lr_x1e6 ≈ 549306
    const int32_t lr_pos = binance_ofi::ofi_to_lr_x1e6(0.5);
    std::printf("  INFO: ofi_to_lr(0.5) = %d\n", lr_pos);
    CHECK(std::abs(lr_pos - 549306) < 10, "atanh(0.5) ≈ 549306");

    // atanh(-0.5) = -0.5493061443... → lr_x1e6 ≈ -549306
    const int32_t lr_neg = binance_ofi::ofi_to_lr_x1e6(-0.5);
    CHECK(std::abs(lr_neg + 549306) < 10, "atanh(-0.5) ≈ -549306");

    // Clamping test
    CHECK(binance_ofi::ofi_to_lr_x1e6(1.0) > 0, "clamped atanh(1.0) is finite");
    CHECK(binance_ofi::ofi_to_lr_x1e6(-1.0) < 0, "clamped atanh(-1.0) is finite");
}

// ── Evidence Adapter: source registration ─────────────────────────────────────
static void test_source_registration() {
    std::printf("source_registration\n");
    SourceReliability sources{};
    // Default: all sources have weight 0
    CHECK(sources.weight_x1e6(0x02) == 0, "unregistered source has weight 0");

    // Register with default weight
    sources.set_weight(binance_ofi::SOURCE_BINANCE_OFI,
                       binance_ofi::DEFAULT_BINANCE_WEIGHT);
    CHECK(sources.weight_x1e6(0x02) == 850000, "Binance OFI registered with 0.85");

    // Hot-reload: parse JSON weights
    const char* json1 = R"({"sources":{"1":0.90,"2":0.85,"3":0.50}})";
    SourceReliability recal{};
    const size_t applied = binance_ofi::parse_source_reliability_json(
        json1, std::strlen(json1), recal);
    CHECK(applied == 3, "parsed 3 source weights from JSON");
    CHECK(recal.weight_x1e6(1) == 900000, "source 1 weight = 0.90");
    CHECK(recal.weight_x1e6(2) == 850000, "source 2 weight = 0.85");
    CHECK(recal.weight_x1e6(3) == 500000, "source 3 weight = 0.50");
}

// ── Evidence Adapter: rate limiting ───────────────────────────────────────────
static void test_rate_limiting() {
    std::printf("rate_limiting\n");
    CHECK(binance_ofi::RATE_LIMIT_NS == 10'000'000ULL,
          "10ms rate limit");
    CHECK(1'000'000'000ULL / binance_ofi::RATE_LIMIT_NS == 100,
          "100 Hz max evidence cadence");
}

// ── Evidence Adapter: full integration ─────────────────────────────────────────
static void test_adapter_integration() {
    std::printf("adapter_integration\n");
    using MarketStateQ = SPSC_RingBuffer<MarketState, BinanceConfig::RING_CAPACITY>;
    using EvidenceQ = SPSC_RingBuffer<EvidenceEvent>;

    MarketStateQ market_q{};
    EvidenceQ evidence_q{};
    SourceReliability sources{};

    const uint64_t t0 = crowdintel::realtime_ns();
    binance_ofi::BinanceOFIAdapter<MarketStateQ> adapter(
        market_q, evidence_q, sources,
        /* recal_path */ "", t0);

    adapter.start();
    std::this_thread::sleep_for(std::chrono::milliseconds(10));

    // Inject events
    const uint64_t base = crowdintel::realtime_ns();
    for (uint64_t i = 0; i < 100; ++i) {
        const double d = static_cast<double>(i) * 0.01;
        market_q.try_push(MarketState{
            /*ofi_normalized=*/0.3, /*trade_intensity=*/50.0,
            /*spread_bps=*/5.0, /*depth_imbalance=*/0.2,
            /*microprice=*/26505.0 + d, /*mid_velocity=*/10.0,
            /*timestamp_ns=*/base + i * 10'000'000ULL,
            /*delta_t_sec=*/0.01,
            /*realized_vol_1h=*/0.0, /*funding_rate=*/0.0,
            /*volume_zscore=*/0.0
        });
    }

    // Wait for drain
    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    adapter.stop();

    const uint64_t drained = adapter.drained();
    const uint64_t emitted = adapter.emitted();
    const uint64_t rate_limited = adapter.rate_limited();
    std::printf("  INFO: drained=%llu, emitted=%llu, rate_limited=%llu\n",
                static_cast<unsigned long long>(drained),
                static_cast<unsigned long long>(emitted),
                static_cast<unsigned long long>(rate_limited));

    CHECK(drained == 100, "all 100 MarketStates drained");
    // With 10ms rate limit and 10ms spacing between events, all should emit
    CHECK(emitted > 0, "at least some evidence emitted");
    CHECK(sources.weight_x1e6(0x02) == 850000,
          "Binance weight registered after adapter construction");

    // Verify evidence event
    EvidenceEvent ev{};
    uint64_t ev_count = 0;
    while (evidence_q.try_pop(ev)) {
        ++ev_count;
        CHECK(ev.source_id == 0x02, "evidence from source 2");
        CHECK(ev.kind == EvidenceEvent::Kind::LR, "evidence is LR kind");
        CHECK(ev.lr_x1e6 != 0, "non-zero LR for non-zero OFI");
    }
    CHECK(ev_count > 0, "non-zero evidence events emitted");
}

// ── SPSC integration with MarketState ──────────────────────────────────────────
static void test_spsc_market_state() {
    std::printf("spsc_market_state\n");
    SPSC_RingBuffer<MarketState, 16> q;
    MarketState state{};
    state.ofi_normalized = 0.5;
    state.spread_bps = 2.0;
    state.timestamp_ns = 12345;

    CHECK(!q.try_pop(state), "empty pop fails on empty ring");

    size_t pushed = 0;
    while (q.try_push(state)) ++pushed;
    CHECK(pushed == 15, "capacity-1 slots usable");

    MarketState out{};
    CHECK(q.try_pop(out) && out.ofi_normalized == 0.5,
          "FIFO MarketState with correct data");
    CHECK(q.try_push(state), "push after pop succeeds");
}

// ── Main ───────────────────────────────────────────────────────────────────────
int main() {
    std::printf("== Phase 1: OFI + Binance WS + Evidence Adapter ==\n");

    // OFI Calculator
    test_ofi_empty_state();
    test_ofi_single_depth();
    test_ofi_two_depth_events();
    test_ofi_trade_events();
    test_ofi_ewma_decay();
    test_ofi_reset();
    test_ofi_mid_velocity();
    test_ofi_throughput();
    test_ofi_clamp();
    test_ofi_zero_volume();

    // BinanceWSClient
    test_binance_client_mock();
    test_binance_client_inject();

    // Evidence Adapter
    test_ofi_to_lr();
    test_source_registration();
    test_rate_limiting();
    test_adapter_integration();

    // SPSC
    test_spsc_market_state();

    std::printf("== %s (%d failures) ==\n",
                g_failures ? "FAILED" : "ALL PASS", g_failures);
    return g_failures ? 1 : 0;
}
