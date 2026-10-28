# ──────────────────────────────────────────────────────────────────────────────
# ADR-003: CrowdIntel Wallet Bias Engine
# ──────────────────────────────────────────────────────────────────────────────

**Status:** ✅ Accepted  
**Date:** 2025-01-23  
**Authors:** OMNISCIENT Engineering Team  
**Tags:** bias, crowdintel, risk, hot-path  

## Context

Originally, there was no formal model for how CrowdIntel wallet signals should influence trading decisions. This led to two risks:

1. **Risk of over-aggressive trading:** Raw wallet conviction scores might trigger aggressive market orders, increasing drawdown.
2. **Risk of ignored signals:** Without a bounded bias mechanism, valuable wallet signal data (whale alerts, net flows) would be underutilized.

## Decision

Implement `crowdintel_bias.hpp` — a **wallet bias engine** that modifies passive quote prices based on wallet activity, with strict safety bounds:

### Architecture

- **Signal sources:** Conviction (0-1), net flow imbalance (-1 to 1), whale alert (binary), confidence (0-1)
- **Bias calculation:** `raw_bias = (conviction × 0.6 + flow × 0.4) × 0.5`
- **Whale boost:** 2× multiplier when whale_alert is active
- **Tilt cap:** Bias clamped to ±30 basis points (0.30%) by default
- **Confidence gate:** Bias only applied when `confidence ≥ 0.75`

### Double-Buffering for Lock-Free Updates

The cold thread (data ingestion) writes to the inactive buffer; the hot thread (trading) reads from the active buffer. Atomic publication ensures zero-copy, lock-free consumption:

```cpp
size_t idx = current_signal_.load(std::memory_order_acquire);
const BiasSignal& sig = signals_[idx];
double bias_price = base_price * (bias_multiplier - 1.0);
```

### Key Properties

1. **Passive-only:** Bias only modifies passive quote placement, never generates aggressive market orders.
2. **Bounded:** Maximum tilt is 30bps regardless of signal strength.
3. **Stale-safe:** Signals older than 30 seconds are ignored.
4. **Zero-allocation:** Lock-free double-buffering ensures no heap allocations in the hot path.

## Impact

- **Positive:** Wallet signals now contribute to more informed passive quoting without increasing tail risk.
- **Risk reduction:** The 30bps tilt cap prevents excessive price deviation from market mid.
- **Latency:** Bias calculation adds < 5ns to the hot path (simple arithmetic + atomic load).

## Consequences

- The bias engine state must be checkpointed to the `StateStore` for recovery after restarts.
- Dashboard must display current `bias_bps` as a separate gauge from market price.
- Future whale alert sources (Twitter, on-chain) can be added without architecture changes.