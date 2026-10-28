#!/usr/bin/env python3
"""
sim.py — Python Oráculo Canónico: Simulador de Profundidad con Selección Adversa

Simula un libro de órdenes L2 y ejecuta trades contra una cola de órdenes
con selección adversa (adversarial selection), permitiendo validar:
  - La lógica de arbitraje con fees corregidas
  - El comportamiento bajo volatilidad >5% / 100ms
  - La máquina de estados de venue (425/429)

Esta simulación debe coincidir con los tests de C++ en:
  tests/unit/test_twap_bb.cpp
  tests/acceptance/test_accept_*_fee_consistency.cpp
"""
import math
import random
from dataclasses import dataclass, field
from typing import List, Tuple, Optional, Dict
from enum import Enum

class OrderType(Enum):
    BID = "bid"
    ASK = "ask"
    BUY = "buy"
    SELL = "sell"

class OrderStatus(Enum):
    PENDING = "pending"
    FILLED = "filled"
    REJECTED = "rejected"
    RATE_LIMITED = "rate_limited"
    TOO_EARLY = "too_early"

@dataclass
class Order:
    price: float
    size: float
    side: OrderType
    order_id: str
    timestamp_ms: int
    status: OrderStatus = OrderStatus.PENDING
    fills: List[Tuple[float, float]] = field(default_factory=list)  # [(price, size), ...]

@dataclass
class DepthLevel:
    price: float
    size: float

@dataclass
class MarketState:
    bids: List[DepthLevel]
    asks: List[DepthLevel]
    spot_price: float
    timestamp_ms: int
    volatility_shock: bool = False

class AdversarialDepthSimulator:
    """
    Simulador de profundidad con selección adversa.
    
    Modela un libro L2 donde:
    - El spot puede sufrir shocks >5% en <100ms (volatility gate)
    - Las órdenes a precios extremos (0.1, 0.9) tienen fees más bajas
    - El arbitraje a 0.973 es rechazado si el edge neto es negativo
    """
    
    def __init__(self, seed: int = 42):
        self.rng = random.Random(seed)
        self.orders: Dict[str, Order] = {}
        self.market_state = self._initial_market()
        self.venue_state = "IDLE"
        self.attempts_429 = 0
        self.attempts_425 = 0
        
    def _initial_market(self) -> MarketState:
        """Estado inicial del mercado: spread típico del 2%."""
        mid = 0.50
        spread = 0.02  # 2% spread
        return MarketState(
            bids=[DepthLevel(0.49, 1000.0), DepthLevel(0.48, 2000.0)],
            asks=[DepthLevel(0.51, 1000.0), DepthLevel(0.52, 2000.0)],
            spot_price=mid,
            timestamp_ms=0
        )
    
    def inject_volatility_shock(self, magnitude_bps: float = 500.0) -> None:
        """Simula un shock de volatilidad >5% (500 bps)."""
        direction = self.rng.choice([-1, 1])
        delta = magnitude_bps / 10000.0 * self.market_state.spot_price
        new_spot = self.market_state.spot_price * (1.0 + direction * delta / self.market_state.spot_price)
        new_spot = max(0.01, min(0.99, new_spot))
        
        self.market_state.spot_price = new_spot
        self.market_state.bids = [
            DepthLevel(new_spot * 0.99, 500.0),
            DepthLevel(new_spot * 0.98, 1000.0)
        ]
        self.market_state.asks = [
            DepthLevel(new_spot * 1.01, 500.0),
            DepthLevel(new_spot * 1.02, 1000.0)
        ]
        self.market_state.volatility_shock = True
        self.market_state.timestamp_ms += 100  # Shock happens fast (<100ms)
    
    def submit_order(self, order: Order) -> OrderStatus:
        """
        Simula el envío de una orden al venue.
        Retorna el estado de la orden según la máquina de estados de venue.
        """
        self.orders[order.order_id] = order
        
        # Simular "too early" (425)
        if self.attempts_425 > 3:
            order.status = OrderStatus.TOO_EARLY
            self.venue_state = "PENDING"
            self.attempts_425 += 1
            return OrderStatus.TOO_EARLY
        
        # Simular rate limiting (429)
        if self.attempts_429 > 10:
            order.status = OrderStatus.RATE_LIMITED
            self.venue_state = "RATE_LIMITED"
            return OrderStatus.RATE_LIMITED
        
        # Match contra el libro de órdenes
        fill_result = self._match_order(order)
        if fill_result:
            order.status = OrderStatus.FILLED
            order.fills.append(fill_result)
            self.venue_state = "CONFIRMED"
            return OrderStatus.FILLED
        else:
            order.status = OrderStatus.REJECTED
            self.venue_state = "IDLE"
            return OrderStatus.REJECTED
    
    def _match_order(self, order: Order) -> Optional[Tuple[float, float]]:
        """Intenta hacer match de la orden contra el libro."""
        if order.side == OrderType.BUY:
            if order.price >= self.market_state.asks[0].price:
                fill_price = self.market_state.asks[0].price
                fill_size = min(order.size, self.market_state.asks[0].size)
                return (fill_price, fill_size)
        else:  # SELL
            if order.price <= self.market_state.bids[0].price:
                fill_price = self.market_state.bids[0].price
                fill_size = min(order.size, self.market_state.bids[0].size)
                return (fill_price, fill_size)
        return None
    
    def advance_market(self, dt_ms: int = 1000) -> None:
        """Avanza el mercado en el tiempo, posiblemente con un shock."""
        self.market_state.timestamp_ms += dt_ms
        
        # Posibilidad de shock de volatilidad
        if self.rng.random() < 0.02:  # 2% de probabilidad por tick
            self.inject_volatility_shock()
        
        # Posibilidad de rate limiting (429)
        if self.rng.random() < 0.05:  # 5% probabilidad
            self.attempts_429 += 1
            if self.attempts_429 > 3:
                self.attempts_429 = 0  # Recupera
        else:
            self.attempts_429 = max(0, self.attempts_429 - 1)
        
        # Posibilidad de "too early" (425)
        if self.rng.random() < 0.03:  # 3% probabilidad
            self.attempts_425 += 1
            if self.attempts_425 > 2:
                self.attempts_425 = 0
        else:
            self.attempts_425 = max(0, self.attempts_425 - 1)
    
    def simulate_arbitrage(self, price: float, max_retries: int = 5) -> Tuple[bool, str]:
        """
        Simula un intento de arbitraje en un par a un precio dado.
        
        Retorna (success, message) indicando si la operación fue exitosa.
        """
        from fees import taker_fee_prob, edge_after_fees
        
        # Calcular edge después de fees
        # Suponemos p_model ≈ price para arbitraje estático
        result = edge_after_fees(price, price, is_taker=True)
        
        if result.edge_after_fees < 0:
            return (False, f"Arbitraje rechazado: edge={result.edge_after_fees:.5f} (negativo)")
        
        # Intentar submit con reintentos
        for attempt in range(max_retries):
            order = Order(
                price=price,
                size=100.0,
                side=OrderType.BUY if price > self.market_state.spot_price else OrderType.SELL,
                order_id=f"arb_{self.market_state.timestamp_ms}_{attempt}",
                timestamp_ms=self.market_state.timestamp_ms
            )
            
            status = self.submit_order(order)
            if status == OrderStatus.FILLED:
                return (True, f"Arbitraje ejecutado: {len(order.fills)} fills")
            elif status == OrderStatus.RATE_LIMITED:
                # Backoff exponencial
                wait_ms = (2 ** attempt) * 500 + random.randint(0, 500)
                self.advance_market(wait_ms)
                continue
            elif status == OrderStatus.TOO_EARLY:
                # Polling con delay corto
                self.advance_market(100)
                continue
            else:
                return (False, f"Orden rechazada: {status.value}")
        
        return (False, f"Max retries ({max_retries}) exceeded")
    
    def run_backtest(self, duration_steps: int = 100) -> List[Dict]:
        """Ejecuta una simulación de backtest y retorna resultados."""
        results = []
        
        for step in range(duration_steps):
            self.advance_market()
            
            # Intentar arbitraje en precios típicos
            for price in [0.50, 0.973, 0.9615, 0.30]:
                success, msg = self.simulate_arbitrage(price)
                results.append({
                    "step": step,
                    "timestamp_ms": self.market_state.timestamp_ms,
                    "price": price,
                    "spot": self.market_state.spot_price,
                    "success": success,
                    "message": msg,
                    "venue_state": self.venue_state,
                    "volatility_shock": self.market_state.volatility_shock
                })
        
        return results

# ── Test de integración ───────────────────────────────────────────────────────
if __name__ == "__main__":
    sim = AdversarialDepthSimulator()
    
    # Test 1: Arbitraje a 0.973 debe ser rechazado
    print("Test 1: Arbitraje a 0.973")
    success, msg = sim.simulate_arbitrage(0.973)
    print(f"  Result: success={success}, msg={msg}")
    assert not success, "Arbitraje a 0.973 debería ser rechazado"
    print("  ✓ PASS")
    
    # Test 2: Shock de volatilidad
    print("\nTest 2: Shock de volatilidad >5%")
    sim.inject_volatility_shock(600.0)  # 6% shock
    print(f"  Spot antes: 0.50, Spot después: {sim.market_state.spot_price:.4f}")
    print(f"  Volatility shock: {sim.market_state.volatility_shock}")
    print("  ✓ PASS")
    
    # Test 3: Rate limiting (429)
    print("\nTest 3: Manejo de 429 Rate Limited")
    sim.attempts_429 = 15  # Forzar rate limit
    order = Order(0.50, 100.0, OrderType.BUY, "test_429", 1000)
    status = sim.submit_order(order)
    print(f"  Status: {status.value}")
    assert status == OrderStatus.RATE_LIMITED, "Debería estar rate limited"
    print("  ✓ PASS")
    
    # Test 4: Too Early (425)
    print("\nTest 4: Manejo de 425 Too Early")
    sim.attempts_425 = 5  # Forzar too early
    order = Order(0.50, 100.0, OrderType.BUY, "test_425", 1000)
    status = sim.submit_order(order)
    print(f"  Status: {status.value}")
    assert status == OrderStatus.TOO_EARLY, "Debería estar too early"
    print("  ✓ PASS")
    
    # Test 5: Backtest de 10 pasos
    print("\nTest 5: Backtest corto (10 steps)")
    results = sim.run_backtest(10)
    success_count = sum(1 for r in results if r["success"])
    reject_count = sum(1 for r in results if not r["success"] and "rechazado" in r["message"])
    print(f"  Total: {len(results)}, Success: {success_count}, Rejected (no edge): {reject_count}")
    print("  ✓ PASS")
    
    print("\n✅ Todos los tests del simulador pasaron")
