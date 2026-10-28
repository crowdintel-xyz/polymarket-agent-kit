// ─────────────────────────────────────────────────────────────────────────────
// crowdintel_bias.hpp — CrowdIntel Wallet Bias Engine (Fase 4)
//
// CrowdIntel is a WALLET BIAS, not a trade trigger.
// It modifies quote/inventory decisions via apply_bias().
//
// Design:
//   - Reads wallet-level signals (net flow, whale alerts, conviction metrics)
//   - Produces a bounded tilt (bias) on bid/ask prices
//   - Enforces a hard tilt cap to prevent aggressive market orders
//   - Never generates aggressive market orders independently
//
// Integration:
//   - Called BEFORE the Kelly decision in ExecutionEngine
//   - Modifies the effective price level for quote placement
//   - Consumes wallet state from the cold-path Bayesian Engine
//
// Invariants:
//   - Tilt is bounded: |bias| <= tilt_cap (default 0.3%)
//   - Bias only affects PASSIVE quotes, never aggressive orders
//   - All bias calculations are deterministic and reproducible
// ─────────────────────────────────────────────────────────────────────────────

#pragma once
#include <atomic>
#include <cstdint>
#include <cmath>
#include <cstring>

namespace crowdintel {

// ── Bias signal sources (cold-path populated) ─────────────────────────────────
struct BiasSignal {
    // Wallet-level conviction (0.0 to 1.0)
    float conviction;          // Aggregate signal strength from all sources
    
    // Net flow imbalance (positive = buying, negative = selling)
    // Normalized to [-1.0, 1.0]
    float net_flow_imbalance;
    
    // Whale alert (binary: 1 if significant wallet moved, 0 otherwise)
    uint8_t whale_alert;
    
    // Confidence in the signal (0.0 to 1.0)
    float confidence;
    
    // Timestamp of last update
    uint64_t timestamp_ns;
    
    BiasSignal()
        : conviction(0.0f)
        , net_flow_imbalance(0.0f)
        , whale_alert(0)
        , confidence(0.0f)
        , timestamp_ns(0) {}
    
    BiasSignal(float conv, float flow, uint8_t whale, float conf, uint64_t ts)
        : conviction(conv)
        , net_flow_imbalance(flow)
        , whale_alert(whale)
        , confidence(conf)
        , timestamp_ns(ts) {}
};

// ── Bias Engine Configuration ─────────────────────────────────────────────────
struct BiasConfig {
    // Maximum allowed tilt in basis points (0.3% = 30 bps default)
    double tilt_cap_bps = 30.0;
    
    // Minimum confidence to apply any bias
    float min_confidence = 0.75f;
    
    // Conviction weight in bias calculation (0.0 to 1.0)
    float conviction_weight = 0.6f;
    
    // Net flow weight in bias calculation (0.0 to 1.0)
    float flow_weight = 0.4f;
    
    // Whale alert multiplier (when active, boost bias by this factor)
    double whale_boost = 2.0;
    
    // Signal staleness threshold (nanoseconds)
    uint64_t max_age_ns = 30'000'000'000ULL; // 30 seconds
};

// ── Bias Engine ───────────────────────────────────────────────────────────────
// Single producer (cold thread reads wallets) / single consumer (hot thread)
class BiasEngine {
public:
    explicit BiasEngine(const BiasConfig& cfg = {}) : cfg_(cfg) {
        current_signal_.store(0, std::memory_order_relaxed);
        // Pre-initialize signal buffer
        signals_[0] = BiasSignal();
        signals_[1] = BiasSignal();
    }
    
    // ── Cold-path: Update bias signal (called by cold thread) ──────────────────
    // This is the ONLY method that writes to the signal buffer.
    // Must be called when a new CrowdIntel wallet signal is received.
    void update_signal(const BiasSignal& signal) noexcept {
        size_t current_idx = current_signal_.load(std::memory_order_acquire);
        size_t next_idx = 1 - current_idx; // Double-buffering
        
        // Copy new signal to the inactive buffer
        signals_[next_idx] = signal;
        
        // Atomic publish: swap to the new buffer
        current_signal_.store(next_idx, std::memory_order_release);
    }
    
    // ── Hot-path: Read and apply bias (called by ExecutionEngine) ───────────────
    // Returns the bias-adjusted price modifier.
    // Positive = bullish bias (lift bids/asks up)
    // Negative = bearish bias (push bids/asks down)
    double apply_bias(double base_price, uint64_t current_time_ns) const noexcept {
        size_t idx = current_signal_.load(std::memory_order_acquire);
        const BiasSignal& sig = signals_[idx];
        
        // Check signal staleness
        if (current_time_ns - sig.timestamp_ns > cfg_.max_age_ns) {
            return 0.0; // Signal too old, no bias applied
        }
        
        // Check minimum confidence
        if (sig.confidence < cfg_.min_confidence) {
            return 0.0; // Not confident enough
        }
        
        // Calculate raw bias from conviction and flow
        double conviction_component = static_cast<double>(sig.conviction) * cfg_.conviction_weight;
        double flow_component = static_cast<double>(sig.net_flow_imbalance) * cfg_.flow_weight;
        
        // Combine components
        double raw_bias = (conviction_component + flow_component) * 0.5; // Normalize to [-0.5, 0.5]
        
        // Apply whale boost if active
        if (sig.whale_alert > 0) {
            raw_bias *= cfg_.whale_boost;
        }
        
        // Clamp to tilt cap
        double bias_bps = raw_bias * 100.0; // Convert to bps (raw_bias ∈ [-0.5, 0.5] → [-50, 50] bps)
        bias_bps = std::clamp(bias_bps, 
                              -cfg_.tilt_cap_bps, 
                              cfg_.tilt_cap_bps);
        
        // Convert BPS to price modifier
        // bias_bps 30 = 0.30% price adjustment
        double bias_multiplier = 1.0 + (bias_bps / 10000.0);
        
        return base_price * (bias_multiplier - 1.0);
    }
    
    // ── Diagnostic: Get current bias in BPS ───────────────────────────────────
    double current_bias_bps(uint64_t current_time_ns) const noexcept {
        size_t idx = current_signal_.load(std::memory_order_acquire);
        const BiasSignal& sig = signals_[idx];
        
        if (current_time_ns - sig.timestamp_ns > cfg_.max_age_ns) {
            return 0.0;
        }
        
        if (sig.confidence < cfg_.min_confidence) {
            return 0.0;
        }
        
        double conviction_component = static_cast<double>(sig.conviction) * cfg_.conviction_weight;
        double flow_component = static_cast<double>(sig.net_flow_imbalance) * cfg_.flow_weight;
        double raw_bias = (conviction_component + flow_component) * 0.5;
        
        if (sig.whale_alert > 0) {
            raw_bias *= cfg_.whale_boost;
        }
        
        double bias_bps = raw_bias * 100.0;
        return std::clamp(bias_bps, -cfg_.tilt_cap_bps, cfg_.tilt_cap_bps);
    }
    
    // ── Getter for config ──────────────────────────────────────────────────────
    const BiasConfig& config() const noexcept { return cfg_; }

private:
    BiasConfig cfg_;
    
    // Double-buffered signals for lock-free updates
    mutable BiasSignal signals_[2];
    std::atomic<size_t> current_signal_; // Index of active signal buffer
};

// ── Bias Context for ExecutionEngine ──────────────────────────────────────────
// Captures the bias state at decision time, ensuring consistent bias application
struct BiasContext {
    double bias_price;      // Price adjustment in absolute terms (e.g., 0.00150)
    double bias_bps;        // Bias in basis points
    bool   active;          // Whether bias is currently applied
    uint64_t signal_age_ns; // Age of the signal used
};

} // namespace crowdintel
