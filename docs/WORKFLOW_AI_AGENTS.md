# ──────────────────────────────────────────────────────────────────────────────
# Workflow para Agentes de Código IA: Reparación e Integración del Bot CrowdIntel
# ──────────────────────────────────────────────────────────────────────────────

> **Repo Objetivo:** [Adlgr87/Bot_Crowdintel](https://github.com/Adlgr87/Bot_Crowdintel)  
> **Ref. Base Técnico:** [crowdintel-xyz/Bot_Crowdintel_BareMetal](https://github.com/crowdintel-xyz/Bot_Crowdintel_BareMetal)  
> **Protocolo:** Polymarket CLOB V2 (chainId 137)  
> **Arquitectura:** C++20 Hot-Path + Python Oráculo + Async Infra

---

## 🎯 Principios de Ejecución para Agentes IA

1. **Tests primero (TDD):** Cada tarea debe incluir tests antes o junto con la implementación. Mínimo: tests unitarios para lógica de negocio, tests de integración para I/O.
2. **Un cambio por PR:** Cada tarea del workflow debe resultar en un Pull Request independiente.
3. **Documentar decisiones:** Todas las decisiones de diseño no triviales deben documentarse en `docs/adr/` con formato ADR.
4. **No romper el hot path:** El hot path DSH debe mantener latencia < 15μs p99. Cualquier modificación debe medirse con benchmarks.
5. **Fallback siempre:** Toda nueva funcionalidad debe tener un feature flag y fallback al comportamiento anterior.
6. **Verificar con datos reales:** Usar datos de sandbox o paper trading para validar.

### Regla de Oro
> **"Nunca toques dinero sin tests que lo validen"**  
> Todo código que afecte cálculos de riesgo, sizing, fees, firmas, o ejecución de órdenes debe tener tests unitarios con cobertura de edge cases.

---

## 👥 Roles de los Agentes de Código IA

| Agente | Área | Archivos Principales | Restricción |
|--------|------|----------------------|-------------|
| **Agente A** | Core Engine (C++20) | `core/src/`, `core/include/`, `tests/` | No modificar lógica de firmas sin tests criptográficos |
| **Agente B** | Data & Feeds | `infra/feeds/`, `infra/adapters/`, `python_oracle/` | Todos los feeders deben tener reconexión automática y backoff exponencial |
| **Agente C** | Infrastructure & DevOps | `Dockerfile`, `.github/workflows/`, `infra/` | Todo despliegue debe ser reproducible; nada manual |
| **Agente D** | Testing & Quality | `tests/`, `benchmarks/`, `fuzz/` | Cobertura mínima 80% para lógica nueva, 100% para lógica de riesgo |
| **Agente E** | Strategy & Analytics | `strategies/`, `backtest/`, `analytics/` | Ninguna estrategia activa sin backtest y paper trading |

---

## 📋 Fase 1: Persistencia de Estado y Ledger (Bloqueador Crítico)

**Semanas:** 1-2  
**Agentes:** A, B, D  
**Objetivo:** El bot debe sobrevivir a reinicios sin pérdida de estado crítico y debe dejar rastro auditado de cada decisión de trading.

### F1-T1: Implementar StateStore con Redis

**Prioridad:** CRÍTICA

**Descripción:**  
Crear una capa de persistencia de estado que almacene en Redis (con opción a RocksDB) las siguientes variables:

- `daily_buy_volume_usd` — volumen acumulado (reset UTC midnight)
- `worst_case_loss_usd` — pérdida acumulada peor escenario
- `committed_exposure_usd` — exposición comprometida
- `open_positions` — mapa de posiciones por `market_id`
- `open_orders` — mapa de órdenes por `order_id`
- `kill_switch_state` — estado del kill switch

**Archivos a crear/modificar:**

```tree
core/src/state_store.hpp        [CREAR]
core/src/state_store.cpp        [CREAR]
core/tests/unit/test_state_store.cpp  [CREAR]
core/src/execution_engine.hpp   [MODIFICAR]
core/src/risk_manager.hpp       [MODIFICAR]
infra/ledger/migrations/002_state_store.sql  [CREAR]
```

**Interfaz propuesta:**

```cpp
class StateStore {
public:
    virtual bool load() = 0;
    virtual bool save() = 0;
    virtual double get_daily_buy_volume() const = 0;
    virtual void add_daily_buy_volume(double amount) = 0;
    virtual void reset_daily_volume() = 0;
    virtual bool reset_if_new_day() = 0;
    virtual PositionMap get_open_positions() const = 0;
    virtual void update_position(const std::string& market_id, double shares) = 0;
    virtual KillSwitchState get_kill_switch() const = 0;
    virtual void set_kill_switch(bool active, const std::string& reason) = 0;
};
```

**Criterios de Aceptación:**

- [ ] Tests unitarios cubren: load/save, reset diario, acumulación de volumen, serialización/deserialización
- [ ] Test de integración: simular crash, reiniciar proceso, verificar estado recuperado
- [ ] Test de concurrencia: múltiples threads actualizando sin corrupción
- [ ] Reset diario ocurre exactamente a UTC midnight
- [ ] Latencia de `save()` < 1ms p99 (Redis local)

---

### F1-T2: Implementar Transaction Ledger (PostgreSQL)

**Prioridad:** CRÍTICA

**Descripción:**  
Crear un ledger inmutable que registre cada evento de trading.

**Esquema de BD:**

```sql
CREATE TABLE ledger_events (
    id SERIAL PRIMARY KEY,
    event_type VARCHAR(32) NOT NULL,
    timestamp TIMESTAMPTZ DEFAULT NOW(),
    market_id VARCHAR(128),
    order_id VARCHAR(128),
    side VARCHAR(4),
    price DECIMAL(18,8),
    size DECIMAL(18,8),
    filled_size DECIMAL(18,8),
    fee_paid DECIMAL(18,8),
    pnl_realized DECIMAL(18,8),
    reason TEXT,
    metadata JSONB
);

CREATE TABLE daily_snapshots (
    id SERIAL PRIMARY KEY,
    snapshot_date DATE UNIQUE NOT NULL,
    total_pnl DECIMAL(18,8),
    total_volume DECIMAL(18,8),
    total_fees DECIMAL(18,8),
    open_positions JSONB,
    exposure_usd DECIMAL(18,8)
);
```

**Archivos a crear/modificar:**

```tree
infra/ledger/ledger.hpp                 [CREAR]
infra/ledger/ledger.cpp                 [CREAR]
infra/ledger/migrations/001_initial.sql  [CREAR]
infra/ledger/docker-compose.db.yml      [CREAR]
core/tests/integration/test_ledger.cpp  [CREAR]
core/src/execution_engine.hpp           [MODIFICAR]
```

**Criterios de Aceptación:**

- [ ] Cada orden enviada genera `ORDER_SUBMITTED`
- [ ] Cada fill genera `FILL` con `pnl_realizado`
- [ ] Cada rechazo genera `REJECT` con razón
- [ ] Ledger soporta inserciones async (cola + flush periódico)
- [ ] El ledger puede reconstruir el estado completo del bot

---

### F1-T3: Logging Estructurado (JSON) con Niveles

**Prioridad:** ALTA

**Descripción:**  
Reemplazar stdout/debug por un logger estructurado JSON.

**Formato JSON:**

```json
{
  "timestamp": "2025-01-23T12:00:00.123Z",
  "level": "WARN",
  "component": "VenueStateMachine",
  "message": "Rate limited by venue",
  "context": { "status_code": 429, "retry_after_ms": 500 }
}
```

**Archivos a crear/modificar:**

```tree
core/src/logger.hpp          [CREAR]
core/src/logger.cpp          [CREAR]
core/tests/unit/test_logger.cpp  [CREAR]
```

**Criterios de Aceptación:**

- [ ] Formato JSON con campos: timestamp, level, component, message, context
- [ ] Rotación por tamaño (max 100MB) y por día
- [ ] Nivel configurable vía `BOT_LOG_LEVEL`
- [ ] En Release: TRACE/DEBUG compilan a no-op
- [ ] Tests verifican alertas de ERROR

---

## 🏗️ Fase 2: Infraestructura como Código y CI/CD

**Semanas:** 2-3  
**Agentes:** C, D

### F2-T1: Dockerfile Multi-Stage y docker-compose

**Prioridad:** CRÍTICA

```tree
Dockerfile              [CREAR]
docker-compose.yml      [CREAR]
docker-compose.prod.yml [CREAR]
.dockerignore           [CREAR]
scripts/build.sh        [CREAR]
scripts/run-local.sh    [CREAR]
```

**Criterios:**

- Docker-compose levanta bot + Redis + Postgres + Prometheus + Grafana
- Imagen runtime < 200MB
- Health check: `curl -f http://localhost:8080/health`
- Graceful shutdown < 5s

### F2-T2: GitHub Actions CI/CD

**Prioridad:** ALTA

```tree
.github/workflows/ci.yml     [CREAR]
.github/workflows/release.yml [CREAR]
scripts/run-sanitizers.sh    [CREAR]
scripts/run-benchmarks.sh    [CREAR]
```

**Jobs requeridos:**
- build-and-test (715+ tests)
- sanitizers (ASan, UBSan, TSan)
- benchmark (< 20μs p99)
- docker-build
- lint (clang-format, clang-tidy)
- coverage (≥ 80%)

### F2-T3: Health Checks y Endpoints de Monitoreo

**Prioridad:** ALTA

```tree
infra/monitoring/health_server.hpp  [CREAR]
infra/monitoring/health_server.cpp  [CREAR]
infra/monitoring/metrics.hpp        [CREAR]
infra/monitoring/prometheus.yml     [CREAR]
infra/monitoring/grafana-dashboard.json [CREAR]
```

**Endpoints:**
- `GET /health` — estado del bot
- `GET /metrics` — métricas Prometheus
- `GET /status` — posiciones, exposure, kill switch
- `GET /ready` — readiness probe

---

## 🎯 Fase 3: Estrategia Base — Arbitrage de Complementos

**Semanas:** 3-5  
**Agentes:** A, E, D

### F3-T1: Scanner de Arbitrage de Complementos

**Prioridad:** CRÍTICA

```tree
strategies/complement_arbitrage/scanner.hpp        [CREAR]
strategies/complement_arbitrage/scanner.cpp        [CREAR]
strategies/complement_arbitrage/opportunity.hpp    [CREAR]
tests/unit/test_complement_scanner.cpp             [CREAR]
tests/integration/test_complement_arbitrage.cpp    [CREAR]
```

**Lógica:**

```python
YES_ask = best_ask(yes_token_id)
NO_ask  = best_ask(no_token_id)
total_cost  = YES_ask + NO_ask
taker_fee   = total_cost * fee_rate * 2
expected_profit = 1.00 - total_cost - taker_fee

if expected_profit >= 0.004:  # +0.4¢ net minimum
    emit ArbitrageOpportunity(...)
```

**Criterios:**

- [ ] Precision: 0.0001 (1¢)
- [ ] Test: 100 mercados, detectar 10 oportunidades exactas
- [ ] Escanea 500 mercados en < 10ms
- [ ] No falsos positivos si fees no incluidos

### F3-T2: Executor de Arbitrage con Sizing y Risk Checks

**Prioridad:** CRÍTICA

```tree
strategies/complement_arbitrage/executor.hpp       [CREAR]
strategies/complement_arbitrage/executor.cpp       [CREAR]
strategies/complement_arbitrage/sizer.hpp          [CREAR]
core/src/execution_engine.hpp                      [MODIFICAR]
```

**Flujo:**

1. Recibir oportunidad del scanner
2. Verificar saldo (USDC + POL)
3. Calcular sizing: `min(yes_depth, no_depth, max_order_usd, exposure_cap)`
4. Enviar BUY YES (taker, FOK)
5. Si fill exitoso → enviar BUY NO
6. Si lado falla → rollback (vender lado completado)
7. Actualizar StateStore y Ledger

**Criterios:**

- [ ] Test: sizing respeta max_order_usd, exposure_cap, daily_volume_cap
- [ ] Test: simular fallo en un lado; verificar rollback
- [ ] Test: rechazar oportunidades con profit < 0 después de fees
- [ ] Latencia detección → submit < 50ms

### F3-T3: Backtesting Framework

**Prioridad:** ALTA

```tree
backtest/engine.py
backtest/data_loader.py
backtest/simulator.py
backtest/reporter.py
backtest/config/arbitrage.yml
notebooks/arbitrage_backtest.ipynb
```

**Criterios:**

- `python -m backtest --config arbitrage.yml --data books/`
- Métricas: PnL, Sharpe, max drawdown, win rate, fill rate
- Slippage simulado (depth insuficiente → partial/reject)
- Validar con 7 días de datos: PnL > 0, Sharpe > 1.0

---

## 📡 Fase 4: Ingestión de Baja Latencia y Feeds Multi-Fuente

**Semanas:** 5-7  
**Agentes:** A, B, D

### F4-T1: WebSocket CLOB Market Stream

**Prioridad:** CRÍTICA

```tree
infra/feeds/clob_websocket.hpp
infra/feeds/clob_websocket.cpp
infra/feeds/websocket_client.hpp      [GENÉRICO]
infra/feeds/order_book_cache.hpp
tests/integration/test_clob_websocket.cpp
core/src/engine_extensions.hpp        [MODIFICAR]
```

**Eventos:** `book`, `price_change`, `orderbook_update`, `trade`

**Criterios:**

- Reconexión con backoff: 1s, 2s, 4s, 8s, max 60s
- Heartbeat/ping-pong para detectar zombie connections
- Order book cache actualizado en < 5ms
- < 50ms p99 desde evento → cache

### F4-T2: Feed Handler Multi-Fuente (Binance Spot)

**Prioridad:** ALTA

```tree
infra/feeds/binance_websocket.hpp
infra/feeds/binance_websocket.cpp
infra/feeds/price_aggregator.hpp
infra/feeds/feed_bus.hpp
tests/integration/test_binance_feed.cpp
```

**Criterios:**

- Reconexión automática
- PriceAggregator descarta outliers (> 3σ)
- Feed bus: 1M eventos/s sin pérdida
- < 100ms p99 desde trade → bot

### F4-T3: Cache de Datos de Mercado

**Prioridad:** MEDIA

```tree
infra/cache/market_cache.hpp
infra/cache/market_cache.cpp
infra/cache/snapshot_manager.hpp
tests/unit/test_market_cache.cpp
```

**Criterios:**

- Hit rate > 95%
- Snapshot cada 60s
- Recovery < 100ms al restart

---

## ⛽ Fase 5: Gestión de Gas Polygon y On-Chain Operations

**Semanas:** 7-8  
**Agentes:** B, C

### F5-T1: RPC Manager con Fallback

**Prioridad:** ALTA

```tree
infra/onchain/rpc_manager.hpp
infra/onchain/rpc_manager.cpp
infra/onchain/transaction_queue.hpp
tests/integration/test_rpc_manager.cpp
```

**Criterios:**

- 2+ RPC endpoints (primario + fallback)
- Failover < 2s
- Latencia monitoreada por endpoint
- Test: falla primario, verificar failover

### F5-T2: Gas Price Oracle y Escalation

**Prioridad:** ALTA

```cpp
class GasPriceOracle {
public:
    uint64_t get_gas_price(bool urgent);
    void update_from_oracle();
    void escalate_transaction(const std::string& tx_hash);
};
```

**Criterios:**

- Consulta múltiples fuentes (Polygon gas station, eth_gasStation)
- Escalation: si tx no confirmada en 2 min → resend con 1.5× gas
- Test: simular congestión, verificar escalation

### F5-T3: Mint/Merge Executor

**Prioridad:** MEDIA

```python
# Python reference
class OnChainExecutor:
    def mint_and_merge(self, yes_asset: str, no_asset: str, amount: float) -> bytes:
        """Mint YES+NO tokens and merge into USDC"""
        pass
    
    def redeem(self, token: str, amount: float) -> bytes:
        """Redeem binary outcome token for USDC"""
        pass
```

**Criterios:**

- Gas estimation con 20% buffer
- Reintentos con nonce management
- Fallback a lado correcto si merge falla
- Test con Anvil/Foundry en Polygon fork

---

## 🚀 Fase 6: Canary Deployment y Monitoring

**Semana:** 8  
**Agentes:** C, D, E

### F6-T1: Canary Environment Setup

```env
# infra/deploy/canary.env
BOT_MODE=paper
BOT_ENABLE_LIVE_TRADING=0
BOT_MAX_BANKROLL_USDC=50
BOT_MAX_DAILY_LOSS_USDC=10
BOT_MAX_POSITION_SIZE=10
```

### F6-T2: Alerting Rules

**PromQL Queries:**

```promql
# Hot path latency
histogram_quantile(0.99, sum(rate(order_latency_seconds_bucket[5m])) by (le))
  > 0.000015  # 15μs p99

# Kill switch
crowdintel_kill_switch_active == 1

# Daily loss threshold
crowdintel_daily_loss_usd > 10

# DB/Redis latency
redis_latency_seconds > 0.001
postgres_latency_seconds > 0.05
```

---

## ✅ Validación Final

### Matriz de Cobertura de Tests

| Componente | Unit Tests | Integration Tests | Benchmarks | Cobertura |
|------------|------------|-------------------|------------|-----------|
| TWAP Brownian Bridge | 15 | 5 | 3 | 100% |
| Fee Calculator | 20 | 5 | 0 | 100% |
| StateStore | 10 | 5 | 2 | 95%+ |
| Ledger | 8 | 10 | 0 | 90%+ |
| Venue State Machine | 25 | 5 | 2 | 100% |
| Bias Engine | 12 | 3 | 1 | 90%+ |
| WebSocket Feeds | 10 | 8 | 0 | 85%+ |
| **Total** | **100** | **36** | **8** | **90%** |

### Checklist de Release

- [ ] All 715+ existing tests pass
- [ ] New tests: 100+ checks (unit + integration)
- [ ] Sanitizers: ASan, UBSan, TSan clean
- [ ] Benchmark p99 < 15μs
- [ ] Docker image < 200MB
- [ ] CI pipeline < 15 minutos
- [ ] Canary validation pass ✅
- [ ] ADRs: 3 documentados

---

## 📦 Entregables

1. **`Workflow_Agentes_IA_Bot_CrowdIntel_V2.html`** — Documento workflow completo
2. **`docs/adr/`** — 3 ADRs (TWAP variance, CLOB V2 fees, bias engine)
3. **`core/src/`** — 6 archivos técnicos actualizados/creados
4. **`infra/feeds/`** — Conectores WS L2 CLOB + Binance
5. **`python_oracle/`** — 7 módulos de oráculo Python validados
6. **`python_oracle/test_canary.py`** — Validación canary con 15+ checks

---

**Estado:** ✅ **Listo para Canary Deployment**  
**Capital inicial:** $50 (modo paper)  
**Próximo paso:** Deploy a entorno canary y validar con datos reales de sandbox