"""
crowdintel_bias.py — Python Oráculo Canónico: CrowdIntel Bias Engine

Reproduce el motor C++ crowdintel_bias.hpp como referencia de validación.
"""
import time
from dataclasses import dataclass
from typing import Optional

@dataclass
class BiasSignal:
    conviction: float = 0.0
    net_flow_imbalance: float = 0.0
    whale_alert: int = 0
    confidence: float = 0.0
    timestamp_ns: int = 0

@dataclass
class BiasConfig:
    tilt_cap_bps: float = 30.0  # 0.3% max tilt
    min_confidence: float = 0.75
    conviction_weight: float = 0.6
    flow_weight: float = 0.4
    whale_boost: float = 2.0
    max_age_ns: int = 30_000_000_000  # 30 seconds

class BiasEngine:
    def __init__(self, config: BiasConfig = None):
        self.cfg = config or BiasConfig()
        self.signals = [BiasSignal(), BiasSignal()]
        self.current_idx = 0

    def update_signal(self, signal: BiasSignal):
        next_idx = 1 - self.current_idx
        self.signals[next_idx] = signal
        self.current_idx = next_idx

    def apply_bias(self, base_price: float, current_time_ns: int) -> float:
        sig = self.signals[self.current_idx]
        
        if current_time_ns - sig.timestamp_ns > self.cfg.max_age_ns:
            return 0.0
        if sig.confidence < self.cfg.min_confidence:
            return 0.0
        
        conviction_component = sig.conviction * self.cfg.conviction_weight
        flow_component = sig.net_flow_imbalance * self.cfg.flow_weight
        raw_bias = (conviction_component + flow_component) * 0.5
        
        if sig.whale_alert > 0:
            raw_bias *= self.cfg.whale_boost
        
        bias_bps = raw_bias * 100.0
        bias_bps = max(-self.cfg.tilt_cap_bps, min(self.cfg.tilt_cap_bps, bias_bps))
        
        bias_multiplier = 1.0 + (bias_bps / 10000.0)
        return base_price * (bias_multiplier - 1.0)

    def current_bias_bps(self, current_time_ns: int) -> float:
        sig = self.signals[self.current_idx]
        
        if current_time_ns - sig.timestamp_ns > self.cfg.max_age_ns:
            return 0.0
        if sig.confidence < self.cfg.min_confidence:
            return 0.0
        
        conviction_component = sig.conviction * self.cfg.conviction_weight
        flow_component = sig.net_flow_imbalance * self.cfg.flow_weight
        raw_bias = (conviction_component + flow_component) * 0.5
        
        if sig.whale_alert > 0:
            raw_bias *= self.cfg.whale_boost
        
        bias_bps = raw_bias * 100.0
        return max(-self.cfg.tilt_cap_bps, min(self.cfg.tilt_cap_bps, bias_bps))

if __name__ == "__main__":
    print("Test: Bias acotado con whale alert extremo")
    engine = BiasEngine()
    
    extreme = BiasSignal(
        conviction=1.0,
        net_flow_imbalance=1.0,
        whale_alert=1,
        confidence=0.95,
        timestamp_ns=time.time_ns()
    )
    engine.update_signal(extreme)
    
    bias = engine.apply_bias(0.50, time.time_ns())
    bbox = engine.current_bias_bps(time.time_ns())
    
    print(f"  bias_price={bias:.6f}, bias_bps={bbox:.2f}")
    print(f"  tilt_cap_bps=30.0")
    
    if abs(bbox) <= 30.0:
        print("  ✓ PASS: Bias acotado dentro del tilt cap")
    else:
        print(f"  ✗ FAIL: Bias {bbox:.2f} excede el cap de 30.0")
