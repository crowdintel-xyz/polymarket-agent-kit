"""
fees.py — Python Oráculo Canónico: Motor de Comisiones CLOB V2 No Lineal

Reproduce exactamente el motor FeeCalculator de C++ con:
  1.  Estructura por categorías: Standard, Policy, Geopolitics
  2.  Tramado de precios para tarifas absolutas
  3.  Redondeo a 5 decimales (cuantum 0.00001$)
  4.  Conversión a espacio de probabilidad aditivo

Referencia: python_oracle/fees.py → core/src/fee_calculator.hpp
"""
import math
from enum import Enum
from dataclasses import dataclass

class MarketCategory(Enum):
    Standard = "Standard"
    Policy = "Policy"
    Geopolitics = "Geopolitics"

def round_to_quantum(value: float) -> float:
    """Redondeo canónico a 5 decimales."""
    return round(value * 100000.0) / 100000.0

@dataclass
class FeeResult:
    fee_usd: float          # Coste absoluto en USD por contrato
    fee_prob: float         # Coste expresado como desplazamiento en probabilidad
    edge_after_fees: float  # P_model - market_price - fee_prob

def taker_fee_usd(price: float, cat: MarketCategory = MarketCategory.Standard) -> float:
    """
    Coste absoluto en USD por contrato para un taker.
    
    Standard Crypto (0.50): 1.75$ por cada 100 contratos → 0.0175 $/contrato
    Para precios < 0.1 o > 0.9: 0.63$ por cada 100 → 0.0063 $/contrato
    Policy: 0.01 $/contrato
    Geopolitics: 0.00 $/contrato
    """
    p = max(0.0, min(1.0, price))
    fee = 0.0

    if cat == MarketCategory.Geopolitics:
        fee = 0.0
    elif cat == MarketCategory.Policy:
        fee = 0.01
    else:  # Standard
        if p <= 0.1 or p >= 0.9:
            fee = 0.0063
        else:
            fee = 0.0175
    
    return round_to_quantum(fee)

def taker_fee_prob(price: float, cat: MarketCategory = MarketCategory.Standard) -> float:
    """
    Convierte el coste absoluto a espacio de probabilidad.
    fee_prob = fee_usd / (1.0 - price)
    """
    p = max(0.0, min(1.0, price))
    if p >= 0.999:
        return 0.0
    return round_to_quantum(taker_fee_usd(p, cat) / (1.0 - p))

def maker_fee(price: float) -> float:
    """Maker fee: siempre cero (liquidez proporcionada)."""
    return 0.0

def edge_after_fees(
    p_model: float,
    market_price: float,
    is_taker: bool,
    cat: MarketCategory = MarketCategory.Standard
) -> FeeResult:
    """
    Calcula el edge neto después de comisiones.
    
    Para arbitraje estático a 0.973:
    - fee_usd = 0.0063 (precio >= 0.9)
    - fee_prob = 0.0063 / (1 - 0.973) = 0.0063 / 0.027 ≈ 0.0447
    - edge = p_model - 0.973 - 0.0447
    
    Si p_model ≈ 0.973 (sin edge real), margin = -0.0447 ≈ -4.47cts (pérdida)
    """
    fee_usd = taker_fee_usd(market_price, cat) if is_taker else maker_fee(market_price)
    fee_prob = taker_fee_prob(market_price, cat) if is_taker else 0.0
    
    if is_taker:
        edge = p_model - market_price - fee_prob
    else:
        edge = p_model - market_price
    
    return FeeResult(
        fee_usd=round_to_quantum(fee_usd),
        fee_prob=round_to_quantum(fee_prob),
        edge_after_fees=round_to_quantum(edge)
    )

# ── Tests de paridad con C++ ────────────────────────────────────────────────────
if __name__ == "__main__":
    # Test 1: Arbitraje estático a 0.973
    result = edge_after_fees(0.973, 0.973, is_taker=True, cat=MarketCategory.Standard)
    print(f"Arbitraje 0.973 → fee_usd={result.fee_usd:.5f}, fee_prob={result.fee_prob:.5f}, edge={result.edge_after_fees:.5f}")
    assert result.fee_usd == 0.00630, f"Expected 0.00630, got {result.fee_usd}"
    assert result.edge_after_fees < 0, "Edge should be negative (unprofitable)"
    print("  ✓ Rechazado automáticamente (edge negativo)")

    # Test 2: Arbitraje a precio 0.9615 (umbral de disparo)
    result2 = edge_after_fees(0.975, 0.9615, is_taker=True, cat=MarketCategory.Standard)
    fee_usd = taker_fee_usd(0.9615, MarketCategory.Standard)
    fee_prob = taker_fee_prob(0.9615, MarketCategory.Standard)
    print(f"\nPrecio 0.9615 → fee_usd={fee_usd:.5f}, fee_prob={fee_prob:.5f}, edge={result2.edge_after_fees:.5f}")
    # El umbral exige >= 0.004 de margen neto
    if result2.edge_after_fees >= 0.004:
        print("  ✓ Cumple umbral mínimo (+0.4¢ netos)")
    else:
        print(f"  ✗ No cumple umbral (edge={result2.edge_after_fees:.4f})")

    # Test 3: Paridad de categorías
    print("\nCategorías de comisiones:")
    for price in [0.5, 0.1, 0.9, 0.973]:
        for cat in MarketCategory:
            fee = taker_fee_usd(price, cat)
            print(f"  {cat.name:12s} p={price:.3f} → fee_usd={fee:.5f}")
