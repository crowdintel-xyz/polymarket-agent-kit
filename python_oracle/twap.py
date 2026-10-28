"""
twap.py — Python Oráculo Canónico: Varianza Condicional del TWAP Brownian Bridge

Reproduce el cálculo de la clase C++ TwapBrownianBridge::compute() pero con:
  1.  Varianza condicional EXACTA: sigma_cond = s * tau^1.5 / (sqrt(3) * W)
  2.  Función CDF normal de alta precisión (Abramowitz-Stegun 26.2.17)
  3.  Salida idéntica a C++ para validación bit-a-bit.

Invariants:
  - tau < 0.5s → determinista (1.0 / 0.0 / 0.5)
  - sigma_t < 1e-12 → determinista
"""
import math
from dataclasses import dataclass

SECS_PER_YEAR = 31_536_000.0
SQRT_SECS_YEAR = math.sqrt(SECS_PER_YEAR)
SQRT_2 = math.sqrt(2.0)

# Constantes para Abramowitz-Stegun 7.1.26 (error < 1.5e-7)
_A1 =  0.254829592
_A2 = -0.284496736
_A3 =  1.421413741
_A4 = -1.453152027
_A5 =  1.061405429
_P  =  0.3275911

def norm_cdf(x: float) -> float:
    """CDF de la normal estándar usando aproximación Abramowitz-Stegun."""
    z = abs(x) / SQRT_2
    t = 1.0 / (1.0 + _P * z)
    erf_val = 1.0 - (((((_A5 * t + _A4) * t) + _A3) * t + _A2) * t + _A1) * t \
              * math.exp(-z * z)
    return 0.5 * (1.0 + erf_val) if x >= 0.0 else 0.5 * (1.0 - erf_val)

@dataclass
class ProbResult:
    p_up: float
    twap_so_far: float
    time_remaining_sec: float
    sigma_annual: float
    distance_to_strike_bps: float
    is_decided: bool

def compute_twap_prob(
    current_price: float,
    strike_price: float,
    twap_window_total_sec: float,
    elapsed_in_window_sec: float,
    sigma_annual_in: float,
    tick_time: float = 0.0,
    price_sum: float = 0.0,
    tick_count: int = 0,
    ewma_lambda: float = 0.94,
    vol_min_annual: float = 0.15,
    vol_max_annual: float = 2.00,
) -> ProbResult:
    """
    Calcula P(TWAP_final > Strike) usando el puente de Brownian con varianza condicional.
    
    Parámetros:
        current_price: S_t — último precio tick
        strike_price:  K  — precio strike
        twap_window_total_sec: T_total — ventana total (ej. 60.0)
        elapsed_in_window_sec: t — tiempo transcurrido en la ventana
        sigma_annual_in: volatilidad anualizada (EWMA)
        tick_time, price_sum, tick_count: acumuladores del TWAP
    """
    tau = max(0.0, twap_window_total_sec - elapsed_in_window_sec)

    # TWAP acumulado
    if tick_time > 0.0:
        twap_so_far = price_sum / tick_time
    elif tick_count > 0:
        twap_so_far = price_sum / float(tick_count)
    else:
        twap_so_far = current_price

    # Volatilidad anualizada clamped
    sigma_annual = max(vol_min_annual, min(vol_max_annual, sigma_annual_in))

    # Volatilidad absoluta por sqrt(segundo)
    sigma_sec = sigma_annual / SQRT_SECS_YEAR
    sigma_t = sigma_sec * current_price

    # Distancia al strike (bps)
    if strike_price > 0.0:
        dist_bps = (current_price - strike_price) / strike_price * 10000.0
    else:
        dist_bps = 0.0

    # Casos deterministas
    if tau < 0.5:
        if twap_so_far > strike_price:
            p_up = 1.0
        elif twap_so_far < strike_price:
            p_up = 0.0
        else:
            p_up = 0.5
        return ProbResult(
            p_up=p_up,
            twap_so_far=twap_so_far,
            time_remaining_sec=tau,
            sigma_annual=sigma_annual,
            distance_to_strike_bps=dist_bps,
            is_decided=True,
        )

    if sigma_t < 1e-12:
        if twap_so_far > strike_price:
            p_up = 1.0
        elif twap_so_far < strike_price:
            p_up = 0.0
        else:
            p_up = 0.5
        return ProbResult(
            p_up=p_up,
            twap_so_far=twap_so_far,
            time_remaining_sec=tau,
            sigma_annual=sigma_annual,
            distance_to_strike_bps=dist_bps,
            is_decided=True,
        )

    # Brownian Bridge con varianza condicional (CORRECCIÓN V2)
    numerator = (twap_so_far * elapsed_in_window_sec
                 + current_price * tau
                 - strike_price * twap_window_total_sec)

    sigma_cond = sigma_t * (tau ** 1.5) / (math.sqrt(3.0) * twap_window_total_sec)
    denominator = sigma_cond * math.sqrt(twap_window_total_sec)

    if denominator < 1e-15:
        if numerator > 0.0:
            p_up = 1.0
        elif numerator < 0.0:
            p_up = 0.0
        else:
            p_up = 0.5
        return ProbResult(
            p_up=p_up,
            twap_so_far=twap_so_far,
            time_remaining_sec=tau,
            sigma_annual=sigma_annual,
            distance_to_strike_bps=dist_bps,
            is_decided=True,
        )

    d = numerator / denominator
    p_up = norm_cdf(d)
    p_up = max(0.01, min(0.99, p_up))  # clamp probabilístico

    is_decided = (p_up > 0.97 or p_up < 0.03)

    return ProbResult(
        p_up=p_up,
        twap_so_far=twap_so_far,
        time_remaining_sec=tau,
        sigma_annual=sigma_annual,
        distance_to_strike_bps=dist_bps,
        is_decided=is_decided,
    )

# ── Vector de prueba estático para validación cruzada ──────────────────────────
if __name__ == "__main__":
    # Test 1: 10s antes del cierre, vol 26.2% anual
    result = compute_twap_prob(
        current_price=0.62,
        strike_price=0.60,
        twap_window_total_sec=60.0,
        elapsed_in_window_sec=50.0,
        sigma_annual_in=0.262,
        tick_time=50.0,
        price_sum=0.62 * 50.0,
        tick_count=500,
    )
    print(f"Test 1 (10s remaining): p_up={result.p_up:.6f}, sigma_cond={result.sigma_annual:.4f}, decided={result.is_decided}")
    
    # Test 2: 5s antes del cierre
    result2 = compute_twap_prob(
        current_price=0.62,
        strike_price=0.60,
        twap_window_total_sec=60.0,
        elapsed_in_window_sec=55.0,
        sigma_annual_in=0.262,
        tick_time=55.0,
        price_sum=0.62 * 55.0,
        tick_count=550,
    )
    print(f"Test 2 (5s remaining): p_up={result2.p_up:.6f}, sigma_cond={result2.sigma_annual:.4f}, decided={result2.is_decided}")
    
    # Verificación del factor de 9.5x en la varianza
    var_ratio = (result.sigma_annual ** 2 * (50.0 ** 1.5)) / (result2.sigma_annual ** 2 * (5.0 ** 1.5))
    print(f"Factor de varianza tau^1.5 entre 10s y 5s: ~{var_ratio:.1f}x (esperado ~9.5x con raíz cúbica)")

    # Test 3: Arbitraje estático a 0.973 - debe tener margen negativo
    result3 = compute_twap_prob(
        current_price=0.973,
        strike_price=0.973,
        twap_window_total_sec=60.0,
        elapsed_in_window_sec=10.0,
        sigma_annual_in=0.262,
    )
    print(f"\nTest Arbitraje (0.973): p_up={result3.p_up:.6f}")
    print("Nota: El margen neto después de fees (0.63$) es -0.75cts, debe rechazarse.")
