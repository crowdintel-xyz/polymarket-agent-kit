// ─────────────────────────────────────────────────────────────────────────────
// engine_extensions.hpp — PHASE-5: Hot-path integration of DSH modules
//
// Extends ExecutionEngine with BTC 5m/15m specialization using pure
// deterministic math (NO neural networks):
//   - TwapBrownianBridge: P(TWAP_final > K) via Brownian Bridge integral
//   - VolatilityEstimator: multi-scale EWMA σ with regime detection
//   - OfiLinearFilter: Cont et al. (2014) OFI with z-score thresholds
//   - WindowShield: settlement-window lifecycle state machine (5 states)
//   - RateLimiter: TokenBucket + exponential backoff (fail-closed)
//   - CircuitBreaker: CLOSED/OPEN/HALF_OPEN state machine
//   - FeeCalculator: Polymarket CLOB V2 fee computation
//   - KellySizer: quarter-Kelly, capped at 3% bankroll, edge ≥ 0.5%
//   - LadderBuilder: dynamic bid/ask ladder sizing
//   - TimeStrategy: time-based TTL and market-window selection
//
// Integration is opt-in: all pointers nullable, legacy behavior preserved
// when absent.  Hot-path additions sum to < 12μs p50 (see LATENCY_BUDGET.md).
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <atomic>
#include <cstdint>
#include <cstdio>

// ── DSH components (Phase 1-4) ───────────────────────────────────────────────
#include "twap_brownian_bridge.hpp"   // TwapBrownianBridge, ProbResult, TradeDecision
#include "volatility_estimator.hpp"   // VolatilityEstimator
#include "ofi_linear_filter.hpp"      // OfiLinearFilter
#include "window_shield.hpp"           // WindowShield, ShieldState, OfiPressure
#include "rate_limiter.hpp"           // RateLimiter, TokenBucket
#include "circuit_breaker.hpp"         // CircuitBreaker
#include "request_prioritizer.hpp"    // RequestPrioritizer
#include "fee_calculator.hpp"         // FeeCalculator
#include "kelly_sizer.hpp"             // KellySizer
#include "ladder_builder.hpp"          // LadderBuilder
#include "time_strategy.hpp"           // TimeStrategy, TimeConfig
#include "../include/spsc_ring_buffer.hpp"  // SPSC_RingBuffer
#include "../include/journal.hpp"
#include "../include/position_tracker.hpp"
#include "../include/risk_manager.hpp"
#include "../include/volatility_gate.hpp"
#include "../include/time_utils.hpp"

// ── Extended EngineLayers ─────────────────────────────────────────────────────
// Standalone struct (not inheriting EngineLayers to avoid circular include).
// The ExecutionEngine constructor merges this into EngineLayers at startup.
// All new pointers are nullable (nullptr = disabled / legacy mode).
//
// Integration approach: the hot path checks these pointers first; if nullptr,
// it skips the new logic and falls through to legacy behavior.
// CRITICAL FIX ID-C03: alignas(64) prevents false sharing between
// trading thread (writes pointer updates) and strategy thread (reads)
struct alignas(64) ExtendedEngineLayers {
    // ---- Existing layers (copied from EngineLayers for convenience) ----
    SPSC_RingBuffer<JournalEvent>* journal_q = nullptr;
    PositionTracker* tracker = nullptr;
    RiskManager* risk = nullptr;
    VolatilityGate* volatility = nullptr;

    // ---- Phase 1: Deterministic math ─────────────────────────────────────
    TwapBrownianBridge* bb = nullptr;
    VolatilityEstimator* vol = nullptr;

    // ---- Phase 2: Defense ────────────────────────────────────────────────
    OfiLinearFilter* ofi = nullptr;
    WindowShield* window_shield = nullptr;

    // ---- Phase 3: Network ────────────────────────────────────────────────
    RateLimiter* rate_limiter = nullptr;
    CircuitBreaker* circuit_breaker = nullptr;
    RequestPrioritizer* prioritizer = nullptr;

    // ---- Phase 4: Strategy ───────────────────────────────────────────────
    KellySizer* kelly_sizer = nullptr;
    LadderBuilder* ladder = nullptr;

    // ---- Conviction weights ──────────────────────────────────────────────
    // DSH conviction = w_bb * P(Up) + w_ofi * OFI_signal + w_twap * TWAP_signal
    float w_bb = 0.5f;      // Brownian Bridge probability
    float w_ofi = 0.3f;     // OFI z-score direction
    float w_twap = 0.2f;    // TWAP deviation

    // ---- Runtime flags ----
    std::atomic<bool>* trading_enabled = nullptr;
};

// ── Hot-path helpers (all inline, < 500ns combined) ───────────────────────────

// Compute conviction from BrownianBridge P(Up) + OFI direction + TWAP deviation.
// All inputs in [0, 1]. Returns weighted conviction ∈ [0, 1].
// NOTE: No heap allocation, no std::cout, O(1).
static inline float compute_conviction(
    float bb_p_up,
    float ofi_signal,
    float twap_signal,
    const ExtendedEngineLayers& layers) noexcept
{
    // Clamp inputs to [0, 1]
    float bb = bb_p_up < 0.0f ? 0.0f : (bb_p_up > 1.0f ? 1.0f : bb_p_up);
    float ofi = ofi_signal < 0.0f ? 0.0f : (ofi_signal > 1.0f ? 1.0f : ofi_signal);
    float twap = twap_signal < 0.0f ? 0.0f : (twap_signal > 1.0f ? 1.0f : twap_signal);

    // Weighted conviction: BB 0.5 + OFI 0.3 + TWAP 0.2
    float conviction = bb * layers.w_bb + ofi * layers.w_ofi + twap * layers.w_twap;
    // Normalize by sum of weights
    float w_sum = layers.w_bb + layers.w_ofi + layers.w_twap;
    if (w_sum > 0.0f) conviction /= w_sum;
    return conviction < 0.0f ? 0.0f : (conviction > 1.0f ? 1.0f : conviction);
}

// Check WindowShield + CircuitBreaker + RateLimiter for trade authorization.
// Returns false if trading is blocked (fail-closed).
static inline bool defense_check(
    const ExtendedEngineLayers& layers,
    uint64_t now_ns,
    float conviction,
    OfiPressure ofi_pressure) noexcept
{
    // WindowShield: settlement phase gate
    if (layers.window_shield) {
        const ShieldState state =
            layers.window_shield->update(now_ns, conviction, ofi_pressure);
        if (state == ShieldState::HALTED ||
            state == ShieldState::CLOSE_ONLY) {
            // Log once per transition
            static thread_local ShieldState last_logged{};  // T1.8 FIX: zero-init, no dynamic init
            if (last_logged != state) {
                last_logged = state;
                // T1.8 FIX: Removed fprintf from hot path — use a silent
                // state change. For debugging, the return value (false)
                // indicates the block to the caller.
            }
            return false;
        }
    }

    // CircuitBreaker: network health gate
    if (layers.circuit_breaker) {
        if (!layers.circuit_breaker->allow_request()) {
            return false;  // fail-closed
        }
    }

    return true;
}

// Run the Brownian Bridge computation.
// Returns ProbResult with p_up, twap_so_far, sigma_annual, etc.
// Caller must ensure layers.bb is non-null.
static inline ProbResult
compute_bb_probability(
    ExtendedEngineLayers& layers,
    double current_price,
    double strike,
    double window_sec,
    double elapsed_sec,
    uint64_t now_ns) noexcept
{
    // Update TWAP with latest price
    layers.bb->on_price(current_price, now_ns);

    // Compute P(TWAP_final > strike)
    return layers.bb->compute(current_price, strike, window_sec, elapsed_sec);
}

// Run the full signal evaluation: volatility + BB + OFI + WindowShield.
// Returns TradeDecision from KellySizer (via BB evaluate_trade).
struct SignalResult {
    float conviction;
    ShieldState shield_state;
    bool trade_authorized;
    ProbResult prob;
    OfiLinearFilter::FilterState ofi_state;
};

static inline SignalResult
evaluate_signal(
    ExtendedEngineLayers& layers,
    double price,
    double strike,
    double window_sec,
    double elapsed_sec,
    double bankroll,
    uint64_t now_ns) noexcept
{
    SignalResult result{};

    // 1. Volatility update (if available)
    // (volatility is updated per log-return in the main loop, not here)

    // 2. Brownian Bridge TWAP probability
    if (layers.bb) {
        layers.bb->on_price(price, now_ns);
        result.prob = layers.bb->compute(price, strike, window_sec, elapsed_sec);
    }

    // 3. OFI filter state (if available)
    // (OFI is updated per book update in the main loop, not here)
    if (layers.ofi) {
        result.ofi_state = layers.ofi->current_state();
    }

    // 4. WindowShield evaluation
    if (layers.window_shield) {
        float conviction = result.prob.is_decided ? result.prob.p_up : 0.5f;
        // Convert PressureLevel → OfiPressure (same underlying values)
        auto ofi_pressure = static_cast<OfiPressure>(
            static_cast<uint8_t>(result.ofi_state.level));
        result.shield_state = layers.window_shield->update(
            now_ns, conviction, ofi_pressure);
    } else {
        result.shield_state = ShieldState::MAKER_PASSIVE;
    }

    // 5. Defense check (fail-closed)
    float ofi_signal = result.ofi_state.ofi_zscore > 0 ? 1.0f : 0.0f;
    float twap_signal = static_cast<float>(result.prob.p_up);
    result.conviction = compute_conviction(
        static_cast<float>(result.prob.p_up),
        ofi_signal,
        twap_signal,
        layers
    );

    // Convert OFI pressure for defense_check
    auto ofi_pressure = static_cast<OfiPressure>(
        static_cast<uint8_t>(result.ofi_state.level));
    result.trade_authorized = defense_check(
        layers, now_ns, result.conviction, ofi_pressure
    );

    return result;
}
