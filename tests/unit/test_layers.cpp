// ─────────────────────────────────────────────────────────────────────────────
// test_layers: unit/integration tests for the second-generation layers.
//
//   P1 eyes:    PositionTracker accounting, dedup, VWAP, seqlock snapshots,
//               user-channel message parser, engine/tracker integration
//               (no double exposure on partial fills).
//   P2 brakes:  RiskManager caps, stop-loss, kill switch, hedging.
//   P3 adverse: VolatilityGate regimes, pool slippage/TTL policy.
//   P4 brain:   BayesianEngine posterior math, source reliability gating.
//
// Exit code 0 = all pass.  No external framework (matches test_core).
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../../core/include/account_events.hpp"
#include "../../core/include/bayesian_engine.hpp"
#include "../../core/include/evidence.hpp"
#include "../../core/include/journal.hpp"
#include "../../core/include/source_reliability.hpp"
#include "../../core/include/order_book.hpp"
#include "../../core/include/position_tracker.hpp"
#include "../../core/include/risk_manager.hpp"
#include "../../core/include/spsc_ring_buffer.hpp"
#include "../../core/include/time_utils.hpp"
#include "../../core/include/volatility_gate.hpp"
#include <cmath>
#include "../../core/src/execution_engine.hpp"
#include "../../core/src/market_config.hpp"
#include "../../core/src/mock_client.hpp"
#include "../../core/src/presigned_pool.hpp"
#include "../../core/src/user_event_parser.hpp"
#include "../../core/crypto/eip712_signer.hpp"
#include "../../core/crypto/secure_zero.hpp"
#include "alpha_parser.hpp"

// Phase headers are included as their phases land (P3 adverse-selection,
// P4 brain):
//   #include "../../core/include/volatility_gate.hpp"
//   #include "bayesian_engine.hpp" / "evidence.hpp"

static int g_failures = 0;
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) { std::printf("  PASS %s\n", name); }                       \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);         \
               ++g_failures; }                                                \
    } while (0)

static AccountEvent make_fill(uint8_t side, double price, double shares,
                              uint64_t order_hash, uint64_t event_id,
                              uint8_t asset = 0) {
    AccountEvent ev{};
    ev.type = AccountEvent::Type::FILL;
    ev.side = side;
    ev.asset = asset;
    ev.price = static_cast<uint64_t>(price * 1000000.0);
    ev.size = static_cast<uint64_t>(shares * 1000000.0);
    ev.order_hash = order_hash;
    ev.event_id = event_id;
    ev.market_hash = 7;
    ev.timestamp_ns = crowdintel::realtime_ns();
    return ev;
}

// ── P1: PositionTracker ──────────────────────────────────────────────────────
static void test_tracker_partial_fill() {
    std::printf("tracker_partial_fill\n");
    PositionTracker tracker;
    const uint64_t price = 550000;
    tracker.reserve_buy(10000000000ULL, price);  // 10,000 shares
    CHECK(tracker.open_buy() == 10000000000ULL, "reservation visible pre-fill");

    const uint64_t t0 = crowdintel::mono_ns();
    AccountEvent partial =
        make_fill(0, 0.55, 3000.0, /*order=*/11, /*event=*/101);
    partial.remaining = 7000000000ULL;
    tracker.apply(partial);
    const uint64_t applied_ns = crowdintel::mono_ns() - t0;

    CHECK(tracker.net_yes() == 3000000000ULL, "3,000 of 10,000 tracked as inventory");
    CHECK(tracker.open_buy() == 7000000000ULL, "7,000 remain pending");
    CHECK(applied_ns < 1000000ULL, "fill applied in <1 ms");

    AccountEvent rest = make_fill(0, 0.55, 7000.0, /*order=*/11, /*event=*/102);
    tracker.apply(rest);
    CHECK(tracker.net_yes() == 10000000000ULL && tracker.open_buy() == 0,
          "second fill completes the order without residue");
    CHECK(tracker.yes_avg() == 550000, "single-price VWAP is exact");

    // Worst-cost exposure must never double count reservation + inventory.
    PositionTracker t2;
    t2.reserve_buy(4000000000ULL, 500000);  // $2,000 worst cost
    CHECK(t2.exposure_worst_cost() == 2000000000ULL, "worst cost reserved");
    AccountEvent fill = make_fill(0, 0.50, 4000.0, 12, 103);
    t2.apply(fill);
    CHECK(t2.open_buy() == 0 && t2.open_buy_cost() == 0 &&
              t2.exposure_worst_cost() == 2000000000ULL,
          "fill moves reservation into inventory without double counting");
}

static void test_tracker_vwap_pnl_dedup() {
    std::printf("tracker_vwap_pnl_dedup\n");
    PositionTracker tracker;
    tracker.apply(make_fill(0, 0.40, 1000.0, 1, 201));
    tracker.apply(make_fill(0, 0.60, 1000.0, 2, 202));
    CHECK(tracker.net_yes() == 2000000000ULL, "two buys accumulate");
    CHECK(tracker.yes_avg() == 500000, "VWAP of 0.40/0.60 equals 0.50");

    tracker.apply(make_fill(1, 0.80, 500.0, 3, 203));  // sell 500 @ 0.80
    CHECK(tracker.net_yes() == 1500000000ULL, "sell reduces inventory");
    CHECK(tracker.realized_pnl() == 150000000LL,  // (0.80-0.50)*500 = $150
          "realized P&L uses tracked VWAP");

    // Duplicate delivery of trade 203 (e.g. MATCHED then re-sent) is dropped.
    tracker.apply(make_fill(1, 0.80, 500.0, 3, 203));
    CHECK(tracker.fills() == 3 && tracker.realized_pnl() == 150000000LL &&
              tracker.anomalies() == 1,
          "duplicate trade id cannot double-count a fill");

    // Oversold inventory saturates and is flagged for reconciliation.
    tracker.apply(make_fill(1, 0.10, 99999.0, 4, 204));
    CHECK(tracker.net_yes() == 0 && tracker.anomalies() >= 2,
          "sell beyond inventory clamps net position and flags anomaly");
}

static void test_tracker_reservations_snapshot() {
    std::printf("tracker_reservations_snapshot\n");
    PositionTracker tracker(2000000000ULL, 450000);  // 2,000 YES @ 0.45
    CHECK(tracker.sellable() == 2000000000ULL, "initial inventory sellable");
    tracker.reserve_sell(500000000ULL);
    CHECK(tracker.sellable() == 1500000000ULL,
          "sell reservation cannot be spent twice");
    tracker.release_sell(500000000ULL);
    CHECK(tracker.sellable() == 2000000000ULL, "release restores inventory");

    std::atomic<bool> stop{false};
    std::atomic<uint64_t> bad{0};
    std::thread writer([&] {
        uint64_t id = 1000;
        while (!stop.load(std::memory_order_acquire)) {
            tracker.apply(make_fill(0, 0.50, 10.0, id, id));
            ++id;
        }
    });
    std::thread reader([&] {
        PositionTracker::Snapshot view{};
        while (!stop.load(std::memory_order_acquire)) {
            if (tracker.snapshot(view)) {
                const bool coherent =
                    view.open_buy == 0 || view.open_buy_cost != 0;
                if (!coherent) bad.fetch_add(1, std::memory_order_relaxed);
            }
        }
    });
    std::this_thread::sleep_for(std::chrono::milliseconds(120));
    stop.store(true, std::memory_order_release);
    writer.join();
    reader.join();
    CHECK(bad.load() == 0, "cold snapshots stay coherent under hot writes");
}

// ── P1: user-channel parser ─────────────────────────────────────────────────
static void test_user_event_parser() {
    std::printf("user_event_parser\n");
    const char* token = "71321045679252212594626395510336467040167069592778062791519851593659551227755";
    UserEventParser parser(token, nullptr, 42);

    const char* trade =
        "{\"event_type\":\"trade\",\"type\":\"MATCHED\","
        "\"id\":\"0xtrade1\",\"taker_order_id\":\"0xorder9\","
        "\"asset_id\":\"71321045679252212594626395510336467040167069592778062791519851593659551227755\","
        "\"market\":\"0xbd31dc69a81dc0b5e6b5b9bb6d2e2ebf\",\"side\":\"BUY\","
        "\"size\":\"3000\",\"price\":\"0.55\",\"fee_rate_bps\":\"0\","
        "\"status\":\"MATCHED\",\"matchtime\":\"1727000000000\","
        "\"maker_orders\":[],\"trader_side\":\"TAKER\"}";
    AccountEvent ev{};
    CHECK(parser.parse(trade, std::strlen(trade), ev),
          "MATCHED trade parses");
    CHECK(ev.type == AccountEvent::Type::FILL && ev.side == 0 &&
              ev.size == 3000000000ULL && ev.price == 550000 &&
              ev.event_id != 0 && ev.order_hash != 0,
          "partial fill normalized with fixed-point size/price and hashed ids");

    const char* mined =
        "{\"event_type\":\"trade\",\"type\":\"MINED\","
        "\"id\":\"0xtrade1\",\"taker_order_id\":\"0xorder9\","
        "\"asset_id\":\"71321045679252212594626395510336467040167069592778062791519851593659551227755\","
        "\"side\":\"BUY\",\"size\":\"3000\",\"price\":\"0.55\","
        "\"status\":\"MINED\"}";
    AccountEvent dup{};
    CHECK(parser.parse(mined, std::strlen(mined), dup) &&
              dup.type == AccountEvent::Type::FILL_MINED &&
              dup.event_id == ev.event_id,
          "MINED restatement keeps the same trade id for dedup");

    const char* placement =
        "{\"event_type\":\"order\",\"type\":\"PLACEMENT\","
        "\"id\":\"0xorder9\",\"asset_id\":\"71321045679252212594626395510336467040167069592778062791519851593659551227755\","
        "\"side\":\"BUY\",\"original_size\":\"10000\",\"size_matched\":\"3000\","
        "\"price\":\"0.55\",\"outcome\":\"YES\",\"status\":\"LIVE\"}";
    AccountEvent open{};
    CHECK(parser.parse(placement, std::strlen(placement), open) &&
              open.type == AccountEvent::Type::OPEN &&
              open.remaining == 7000000000ULL,
          "PLACEMENT normalizes remaining 7,000 of 10,000");

    const char* cancel =
        "{\"event_type\":\"order\",\"type\":\"CANCELLATION\","
        "\"id\":\"0xorder9\",\"asset_id\":\"71321045679252212594626395510336467040167069592778062791519851593659551227755\","
        "\"side\":\"BUY\",\"original_size\":\"10000\",\"size_matched\":\"3000\","
        "\"price\":\"0.55\"}";
    AccountEvent canc{};
    CHECK(parser.parse(cancel, std::strlen(cancel), canc) &&
              canc.type == AccountEvent::Type::CANCEL &&
              canc.order_hash == open.order_hash,
          "CANCELLATION maps to a reservation release for the same order");

    const char* other_asset =
        "{\"event_type\":\"trade\",\"type\":\"MATCHED\",\"id\":\"0xt2\","
        "\"asset_id\":\"42\",\"side\":\"SELL\",\"size\":\"5\","
        "\"price\":\"0.5\",\"status\":\"MATCHED\"}";
    AccountEvent ignored{};
    CHECK(!parser.parse(other_asset, std::strlen(other_asset), ignored),
          "events for other assets are filtered");

    const char* dup_key =
        "{\"event_type\":\"trade\",\"event_type\":\"trade\",\"type\":\"MATCHED\","
        "\"id\":\"0xt3\",\"asset_id\":\"71321045679252212594626395510336467040167069592778062791519851593659551227755\","
        "\"side\":\"BUY\",\"size\":\"5\",\"price\":\"0.5\"}";
    AccountEvent hostile{};
    CHECK(!parser.parse(dup_key, std::strlen(dup_key), hostile),
          "duplicate semantic keys fail closed");

    const char* failed =
        "{\"event_type\":\"trade\",\"type\":\"FAILED\","
        "\"id\":\"0xt4\",\"taker_order_id\":\"0xorder9\","
        "\"asset_id\":\"71321045679252212594626395510336467040167069592778062791519851593659551227755\","
        "\"side\":\"BUY\",\"size\":\"7000\",\"price\":\"0.55\","
        "\"status\":\"FAILED\"}";
    AccountEvent fail{};
    CHECK(parser.parse(failed, std::strlen(failed), fail) &&
              fail.type == AccountEvent::Type::FAILED,
          "FAILED trade normalizes as a release, never as a fill");
}

// ── P1: engine + tracker integration (no double exposure) ───────────────────
static void init_fixture(MarketConfig& cfg, EIP712Signer& signer) {
    const char* key_text =
        "23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b";
    uint8_t key[32];
    if (!parse_hex_bytes(key_text, 64, key, sizeof(key))) std::abort();
    if (!signer.init(key, false)) std::abort();
    secure_zero(key, sizeof(key));
    const char* token =
        "71321045679252212594626395510336467040167069592778062791519851593659551227755";
    std::snprintf(cfg.token_id_dec, sizeof(cfg.token_id_dec), "%s", token);
    if (!parse_uint256_dec(token, std::strlen(token), cfg.token_id_be))
        std::abort();
    if (cfg.finalize_identity(signer.signer_address()) != nullptr) std::abort();
    std::snprintf(cfg.owner_api_key, sizeof(cfg.owner_api_key), "test-owner");
    std::snprintf(cfg.market_slug, sizeof(cfg.market_slug), "layers-market");
    cfg.market_hash = alpha_hash_bytes(cfg.market_slug,
                                       std::strlen(cfg.market_slug));
}

static AlphaSignal make_buy_signal(const MarketConfig& cfg, double p_win,
                                   uint64_t id) {
    AlphaSignal signal{};
    signal.direction_hint = K_SIDE_BUY;
    signal.p_win = p_win;
    signal.confidence = 0.95;
    signal.q_value = 0.01;
    signal.timestamp_ns = AlphaParser::realtime_ns();
    signal.market_hash = cfg.market_hash;
    signal.signal_id = id;
    return signal;
}

static void test_engine_tracker_integration() {
    std::printf("engine_tracker_integration\n");
    MarketConfig cfg;
    EIP712Signer signer;
    init_fixture(cfg, signer);
    cfg.max_order_usd = 200.0;
    cfg.max_exposure_usd = 300.0;
    cfg.max_daily_loss_usd = 500.0;
    cfg.bankroll_usd = 10000.0;
    cfg.min_size_shares = 5000000;

    OrderBookL2 book;
    book.set_tick_size(10000);
    const Level2Entry bids[] = {{470000, 5000000000ULL}};
    const Level2Entry asks[] = {{530000, 5000000000ULL}};
    book.set_book(bids, 1, asks, 1);

    SPSC_RingBuffer<AlphaSignal> signals;
    SPSC_RingBuffer<AccountEvent> account_q;
    SPSC_RingBuffer<JournalEvent> journal_q;
    PositionTracker tracker;
    EngineLayers layers{};
    layers.account_q = &account_q;
    layers.tracker = &tracker;
    layers.journal_q = &journal_q;

    PresignedOrderPool pool(cfg, signer, cfg.presign_ttl_ms);
    pool.rebuild(470000, 530000, 400000000ULL, 10000);
    MockCLOBClient client(cfg);
    ExecutionEngine<MockCLOBClient> engine(
        cfg, book, signals, signer, pool, client, nullptr, &layers);

    // Signal 1: BUY ~54,716 @ 0.53 → reserved (mock accepts, no fill yet).
    signals.try_push(make_buy_signal(cfg, 0.75, 1));
    CHECK(engine.run_tick() == TickResult::SUBMITTED,
          "first buy signal submits");
    CHECK(tracker.open_buy() > 0 && tracker.net_yes() == 0,
          "accepted order is a reservation, not inventory");
    const uint64_t reserved = tracker.open_buy();

    // Signal 2: an identical buy is budget-capped; the tracker must account
    // for both reservations before the partial fill below is applied.
    signals.try_push(make_buy_signal(cfg, 0.75, 2));
    const TickResult second = engine.run_tick();
    CHECK(second == TickResult::SUBMITTED || second == TickResult::TOO_SMALL ||
              second == TickResult::RISK_REJECTED,
          "second buy respects budget caps");
    const uint64_t reserved_total = tracker.open_buy();
    CHECK(reserved_total >= reserved,
          "both accepted buys accumulate reservations (no overwrite)");

    // Partial fill arrives through the user channel: 40% of the reservation.
    const uint64_t fill_qty = reserved_total * 2 / 5;
    AccountEvent fill = make_fill(0, 0.53, 0.0, 5, 9001);
    fill.size = fill_qty;
    account_q.try_push(fill);
    CHECK(engine.run_tick() == TickResult::ACCOUNT_APPLIED,
          "account event is processed in housekeeping");
    CHECK(tracker.net_yes() == fill_qty &&
              tracker.open_buy() == reserved_total - fill_qty,
          "partial fill splits reservation/inventory exactly");

    // SELL signal with inventory now available: must succeed, reserving at
    // most the reconciled (filled) inventory.
    AlphaSignal sell = make_buy_signal(cfg, 0.20, 3);
    sell.direction_hint = K_SIDE_SELL;
    signals.try_push(sell);
    CHECK(engine.run_tick() == TickResult::SUBMITTED,
          "sell uses reconciled inventory, not bootstrap guesses");
    const uint64_t first_sell_reserved = tracker.open_sell();
    CHECK(first_sell_reserved > 0 && first_sell_reserved <= fill_qty,
          "sell reservation never exceeds filled inventory");

    // A second SELL can only take the remainder; cumulative reservations must
    // never exceed the filled inventory (no duplicated exposure).
    AlphaSignal sell2 = make_buy_signal(cfg, 0.20, 4);
    sell2.direction_hint = K_SIDE_SELL;
    signals.try_push(sell2);
    const TickResult sell2_result = engine.run_tick();
    CHECK(sell2_result == TickResult::SUBMITTED ||
              sell2_result == TickResult::NO_INVENTORY,
          "second sell either takes the exact remainder or finds nothing");
    CHECK(tracker.open_sell() <= tracker.net_yes(),
          "cumulative sell reservations never duplicate inventory");
    CHECK(tracker.sellable() == 0,
          "inventory is fully reserved after both sells");

    // A third SELL must find nothing sellable at all.
    AlphaSignal sell3 = make_buy_signal(cfg, 0.20, 5);
    sell3.direction_hint = K_SIDE_SELL;
    signals.try_push(sell3);
    CHECK(engine.run_tick() == TickResult::NO_INVENTORY,
          "third sell cannot duplicate exposure of the same inventory");

    // Journal captured fill + orders.
    JournalEvent jrn{};
    unsigned seen = 0;
    while (journal_q.try_pop(jrn)) ++seen;
    CHECK(seen >= 3, "journal recorded fills and order outcomes");
}

// ── P2: RiskManager units ────────────────────────────────────────────────────
static void test_risk_units() {
    std::printf("risk_manager_units\n");
    RiskLimits limits{};
    limits.stop_loss_pct = 0.20;
    limits.max_daily_loss_usd = 50.0;
    limits.max_market_exposure_usd = 100.0;
    limits.max_portfolio_exposure_usd = 200.0;
    RiskManager risk(limits);
    CHECK(!risk.killed(), "kill starts unlatched");
    CHECK(risk.authorize(0, 50.0, 40.0, 60.0), "order within caps authorized");
    CHECK(!risk.authorize(0, 70.0, 40.0, 60.0),
          "market exposure cap denies order");
    CHECK(!risk.authorize(0, 50.0, 40.0, 160.0),
          "portfolio exposure cap denies order");
    CHECK(RiskManager::unrealized_pnl(100000000, 700000, 400000) ==
              -30000000LL,
          "long 100 @0.70 marked at bid 0.40 is -30 USD floating");
    CHECK(RiskManager::unrealized_pnl(0, 700000, 400000) == 0,
          "flat book has no floating P&L");

    CHECK(risk.maintain_day_anchor(-10000000LL), "first day anchors base");
    CHECK(!risk.maintain_day_anchor(-5000000LL), "same day keeps its base");
    CHECK(risk.day_realized(-5000000LL) == 5000000LL,
          "day realized is measured from the anchored base");

    risk.latch_kill();
    CHECK(risk.killed() && !risk.authorize(0, 1.0, 0.0, 0.0),
          "latched kill denies every new order");
}

static void test_risk_evaluate_ladder() {
    std::printf("risk_evaluate_ladder\n");
    RiskLimits limits{};
    limits.stop_loss_pct = 0.30;
    limits.hedge_trigger_pct = 0.10;
    limits.max_daily_loss_usd = 1000.0;
    limits.max_market_exposure_usd = 100000.0;
    limits.max_portfolio_exposure_usd = 100000.0;
    RiskManager risk(limits);
    risk.maintain_day_anchor(0);

    PositionTracker tracker(100000000ULL, 700000);  // 100 YES @ 0.70
    OrderBookL2::Top top{};
    top.bid = {660000, 1000000};   // drop 5.7% — no action
    top.ask = {680000, 1000000};
    top.updated_ns = crowdintel::mono_ns();
    RiskDecision decision = risk.evaluate(top, tracker, 300000, 320000);
    CHECK(decision.action == RiskAction::NONE, "small dip triggers nothing");

    top.bid = {620000, 1000000};   // drop 11.4% — hedge triggers first
    decision = risk.evaluate(top, tracker, 300000, 320000);
    CHECK(decision.action == RiskAction::HEDGE &&
              decision.shares == 100000000ULL,
          "hedge trigger fires before the stop-loss");

    top.bid = {450000, 1000000};   // drop 35.7% — stop-loss takes priority
    decision = risk.evaluate(top, tracker, 300000, 320000);
    CHECK(decision.action == RiskAction::CLOSE &&
              decision.shares == 100000000ULL &&
              decision.projected_loss == -25000000LL,
          "stop-loss closes the whole position and projects the loss");

    // Daily-loss kill: realized −80 against a 1000 budget does nothing…
    PositionTracker burnt(100000000ULL, 900000);
    burnt.apply(make_fill(1, 0.10, 100.0, 1, 77));  // realize −80 USD
    RiskLimits tight = limits;
    tight.max_daily_loss_usd = 50.0;
    risk.configure(tight);
    decision = risk.evaluate(top, burnt, 0, 0);
    CHECK(decision.action == RiskAction::KILL && risk.killed(),
          "day loss beyond budget latches the kill switch");
}

// ── P2: engine stop-loss integration (acceptance: 0.70 → 0.10 crash) ────────
static void test_engine_stop_loss_on_crash() {
    std::printf("engine_stop_loss_on_crash\n");
    MarketConfig cfg;
    EIP712Signer signer;
    init_fixture(cfg, signer);
    cfg.stop_loss_pct = 0.30;
    cfg.max_daily_loss_usd = 500.0;
    cfg.max_exposure_usd = 10000.0;
    cfg.max_portfolio_exposure_usd = 10000.0;
    cfg.initial_position_shares = 100000000ULL;  // 100 YES
    cfg.initial_position_avg_price = 0.70;

    OrderBookL2 book;
    book.set_tick_size(10000);
    Level2Entry bids[1] = {{690000, 100000000000ULL}};
    Level2Entry asks[1] = {{710000, 100000000000ULL}};
    book.set_book(bids, 1, asks, 1);

    SPSC_RingBuffer<AlphaSignal> signals;
    SPSC_RingBuffer<AccountEvent> account_q;
    SPSC_RingBuffer<JournalEvent> journal_q;
    PositionTracker tracker(cfg.initial_position_shares, 700000);
    RiskLimits limits{};
    limits.stop_loss_pct = cfg.stop_loss_pct;
    limits.max_daily_loss_usd = cfg.max_daily_loss_usd;
    limits.max_market_exposure_usd = cfg.max_exposure_usd;
    limits.max_portfolio_exposure_usd = cfg.max_portfolio_exposure_usd;
    RiskManager risk(limits);
    EngineLayers layers{};
    layers.account_q = &account_q;
    layers.tracker = &tracker;
    layers.journal_q = &journal_q;
    layers.risk = &risk;

    std::atomic<bool> trading_enabled{true};
    PresignedOrderPool pool(cfg, signer, cfg.presign_ttl_ms);
    MockCLOBClient client(cfg);
    ExecutionEngine<MockCLOBClient> engine(
        cfg, book, signals, signer, pool, client, &trading_enabled, &layers);

    CHECK(engine.run_tick() == TickResult::NO_SIGNAL,
          "calm book at entry price triggers no protective action");

    // Crash 0.70 → 0.10 across fast steps (the 2-second window of the
    // acceptance criterion, compressed: marks only move through set_book).
    bool stop_seen = false;
    const uint64_t marks[] = {610000, 500000, 390000, 240000, 100000};
    for (const uint64_t mark : marks) {
        bids[0] = {mark, 100000000000ULL};
        asks[0] = {mark + 10000, 100000000000ULL};
        book.set_book(bids, 1, asks, 1);
        const TickResult result = engine.run_tick();
        if (result == TickResult::RISK_STOP_LOSS) {
            stop_seen = true;
            break;
        }
    }
    CHECK(stop_seen, "stop-loss fires on the way down without human input");
    CHECK(client.submissions() == 1,
          "exactly one protective close order was emitted");
    CHECK(tracker.open_sell() == 100000000ULL,
          "the close reserves the entire sellable inventory once");

    // Next ticks do not duplicate the close: reservation exhausts sellable.
    const TickResult again = engine.run_tick();
    CHECK(again != TickResult::RISK_STOP_LOSS && client.submissions() == 1,
          "protective close is not duplicated while in flight");

    // The venue fill lands through the user channel; P&L is realized.
    account_q.try_push(make_fill(1, 0.10, 100.0, 91, 9101));
    engine.run_tick();
    CHECK(tracker.net_yes() == 0 &&
              tracker.realized_pnl() == -60000000LL,
          "stop-loss exit realizes the loss: P&L recorded (-60 USD)");

    // Journal proves the trigger and the fill.
    JournalEvent last{};
    bool stop_event = false, fill_event = false;
    while (journal_q.try_pop(last)) {
        if (last.type == JournalEvent::Type::STOP_LOSS_TRIGGERED) {
            stop_event = true;
            CHECK(last.pnl < 0, "stop-loss journal carries projected loss");
        }
        if (last.type == JournalEvent::Type::ACCOUNT_FILL) fill_event = true;
    }
    CHECK(stop_event && fill_event, "journal contains trigger and exit fill");
}

static void test_engine_kill_freezes_orders() {
    std::printf("engine_kill_freezes_orders\n");
    MarketConfig cfg;
    EIP712Signer signer;
    init_fixture(cfg, signer);
    cfg.max_daily_loss_usd = 50.0;
    cfg.max_exposure_usd = 10000.0;
    cfg.max_portfolio_exposure_usd = 10000.0;

    OrderBookL2 book;
    book.set_tick_size(10000);
    const Level2Entry bids[] = {{470000, 1000000000ULL}};
    const Level2Entry asks[] = {{530000, 1000000000ULL}};
    book.set_book(bids, 1, asks, 1);

    SPSC_RingBuffer<AlphaSignal> signals;
    SPSC_RingBuffer<AccountEvent> account_q;
    SPSC_RingBuffer<JournalEvent> journal_q;
    PositionTracker tracker;  // starts flat; losses happen during this session
    RiskLimits limits{};
    limits.max_daily_loss_usd = cfg.max_daily_loss_usd;
    limits.max_market_exposure_usd = cfg.max_exposure_usd;
    limits.max_portfolio_exposure_usd = cfg.max_portfolio_exposure_usd;
    RiskManager risk(limits);
    EngineLayers layers{};
    layers.account_q = &account_q;
    layers.tracker = &tracker;
    layers.journal_q = &journal_q;
    layers.risk = &risk;

    std::atomic<bool> trading_enabled{true};
    PresignedOrderPool pool(cfg, signer, cfg.presign_ttl_ms);
    MockCLOBClient client(cfg);
    ExecutionEngine<MockCLOBClient> engine(
        cfg, book, signals, signer, pool, client, &trading_enabled, &layers);

    // Tick 1 anchors the day at zero realized P&L (no position yet).
    (void)engine.run_tick();
    // Realize an −$80 loss inside the session: buy 100 @0.90, sell 100 @0.10.
    account_q.try_push(make_fill(0, 0.90, 100.0, 1, 81));
    account_q.try_push(make_fill(1, 0.10, 100.0, 2, 82));
    const TickResult killed = engine.run_tick();
    CHECK(killed == TickResult::RISK_KILL_SWITCH && !trading_enabled.load(),
          "day-loss kill latches and freezes the trading flag");
    CHECK(risk.killed(), "kill state persists in the manager");

    signals.try_push(make_buy_signal(cfg, 0.90, 9));
    CHECK(engine.run_tick() == TickResult::RISK_REJECTED &&
              client.submissions() == 0,
          "post-kill alpha signals are denied before signing");
}

static void test_engine_hedge_on_dip() {
    std::printf("engine_hedge_on_dip\n");
    MarketConfig cfg;
    EIP712Signer signer;
    init_fixture(cfg, signer);
    cfg.hedge_trigger_pct = 0.10;
    cfg.stop_loss_pct = 0.30;
    cfg.max_daily_loss_usd = 500.0;
    cfg.max_exposure_usd = 10000.0;
    cfg.max_portfolio_exposure_usd = 10000.0;
    std::strcpy(cfg.hedge_token_id_dec, "98765432109876543210");
    std::memset(cfg.hedge_token_id_be, 0x07, sizeof(cfg.hedge_token_id_be));
    cfg.initial_position_shares = 100000000ULL;  // 100 YES
    cfg.initial_position_avg_price = 0.70;

    OrderBookL2 book, hedge_book;
    book.set_tick_size(10000);
    hedge_book.set_tick_size(10000);
    Level2Entry bids[1] = {{600000, 100000000000ULL}};   // drop 14.3% vs 0.70
    Level2Entry asks[1] = {{610000, 100000000000ULL}};
    book.set_book(bids, 1, asks, 1);
    const Level2Entry hbids[] = {{340000, 100000000000ULL}};
    const Level2Entry hasks[] = {{350000, 100000000000ULL}};
    hedge_book.set_book(hbids, 1, hasks, 1);

    SPSC_RingBuffer<AlphaSignal> signals;
    SPSC_RingBuffer<AccountEvent> account_q;
    SPSC_RingBuffer<JournalEvent> journal_q;
    PositionTracker tracker(cfg.initial_position_shares, 700000);
    RiskLimits limits{};
    limits.stop_loss_pct = cfg.stop_loss_pct;
    limits.hedge_trigger_pct = cfg.hedge_trigger_pct;
    limits.max_daily_loss_usd = cfg.max_daily_loss_usd;
    limits.max_market_exposure_usd = cfg.max_exposure_usd;
    limits.max_portfolio_exposure_usd = cfg.max_portfolio_exposure_usd;
    RiskManager risk(limits);
    EngineLayers layers{};
    layers.account_q = &account_q;
    layers.tracker = &tracker;
    layers.journal_q = &journal_q;
    layers.risk = &risk;
    layers.hedge_book = &hedge_book;

    std::atomic<bool> trading_enabled{true};
    PresignedOrderPool pool(cfg, signer, cfg.presign_ttl_ms);
    MockCLOBClient client(cfg);
    ExecutionEngine<MockCLOBClient> engine(
        cfg, book, signals, signer, pool, client, &trading_enabled, &layers);

    const TickResult result = engine.run_tick();
    CHECK(result == TickResult::RISK_HEDGE && client.submissions() == 1,
          "dip below hedge trigger emits one hedge buy on the complement");
    CHECK(tracker.net_yes() == 100000000ULL && tracker.net_hedge() == 0,
          "YES inventory untouched; hedge not yet filled");

    // The dip persists but the hedge is already complete: no duplicate hedge
    // (shares = net - net_hedge becomes zero after the hedge fill lands).
    account_q.try_push(make_fill(0, 0.35, 100.0, 55, 5501, 1));  // hedge fill
    const TickResult after = engine.run_tick();
    CHECK(after != TickResult::RISK_HEDGE && client.submissions() == 1,
          "no duplicate hedge once the complement leg is covered");
    CHECK(tracker.net_hedge() == 100000000ULL &&
              tracker.net_yes() == 100000000ULL,
          "hedge fill builds complement inventory at venue truth");
    CHECK(tracker.realized_pnl() == 0,
          "hedging locks the spread: nothing realized until resolution");
}

// ── P3: VolatilityGate ───────────────────────────────────────────────────────
static void test_vol_gate_units() {
    std::printf("vol_gate_units\n");
    {   // Regime sampler: NORMAL → ELEVATED (wide spread) → EXTREME → NORMAL.
        MarketConfig cfg;  // defaults: dev 200bps, wide 1500bps, tick 200Hz
        VolatilityGate gate(cfg);
        const uint64_t base_ttl = cfg.presign_ttl_ms;  // 3000
        const uint64_t t0 = 1000000000ULL;
        gate.sample(495000, 505000, t0, base_ttl);  // spread 100bps of mid
        CHECK(gate.regime() == 0 && gate.effective_ttl_ms() == base_ttl &&
                  gate.size_permille() == 1000 && !gate.paused(),
              "normal regime keeps full ladder TTL and size");
        gate.sample(460000, 540000, t0 + 10000000ULL, base_ttl);  // 1600bps
        CHECK(gate.regime() == 1 && gate.effective_ttl_ms() == 500 &&
                  gate.size_permille() == 500 && !gate.paused(),
              "wide spread elevates: TTL shrinks to vol TTL, size halves");
        // Mid churning 450 times in-window while spread stays wide → rate
        // >= 2*200 → EXTREME: paused, deepest shrink and TTL.
        uint64_t t = t0 + 20000000ULL;
        for (int i = 0; i < 450; ++i) {
            const uint64_t mid = 500000 + (i & 1 ? 1000 : 0);
            gate.sample(mid - 40000, mid + 40000, t, base_ttl);
            t += 1000000ULL;
        }
        gate.sample(460000, 540000, t + 1200000000ULL, base_ttl);
        CHECK(gate.regime() == 2 && gate.paused() &&
                  gate.effective_ttl_ms() <= 250 &&
                  gate.size_permille() <= 250,
              "extreme churn pauses passive flow with deepest shrink");
        // Cool-off: narrow spread and quiet mid restore the full ladder.
        gate.sample(495000, 505000, t + 2600000000ULL, base_ttl);
        CHECK(gate.regime() == 0 && !gate.paused() &&
                  gate.effective_ttl_ms() == base_ttl &&
                  gate.size_permille() == 1000,
              "calm book restores full ladder parameters");
    }
    {   // Shock guard: >5% mid jump inside 100 ms arms a 250 ms cooldown.
        MarketConfig cfg;
        VolatilityGate gate(cfg);  // default mid gap 500 bps = 5%
        const uint64_t t0 = 1000000000ULL;
        CHECK(!gate.observe_mid(500000, t0), "first mid arms nothing");
        CHECK(gate.observe_mid(527000, t0 + 50000000ULL),
              "5.4% jump in 50 ms arms the cooldown");
        CHECK(gate.observe_mid(530000, t0 + 60000000ULL),
              "cooldown keeps blocking while armed");
        CHECK(!gate.observe_mid(531000, t0 + 400000000ULL),
              "cooldown lapses after 250 ms");
        CHECK(gate.shocks() == 1, "exactly one shock was counted");
        // A small move inside the window never arms.
        CHECK(!gate.observe_mid(531000, t0 + 500000000ULL), "quiet re-arm");
        CHECK(!gate.observe_mid(535000, t0 + 510000000ULL),
              "0.75% move stays under the 5% bar");
        CHECK(gate.shocks() == 1, "no phantom shocks on small moves");
    }
    {   // Slippage-vs-mid gate honours BOT_POOL_MAX_DEV_BPS (0 = off).
        MarketConfig cfg;  // default 200 bps
        VolatilityGate gate(cfg);
        CHECK(gate.slippage_ok(0, 506000, 500000), "buy 120bps from mid ok");
        CHECK(!gate.slippage_ok(0, 511000, 500000), "buy 220bps rejected");
        CHECK(gate.slippage_ok(1, 494000, 500000), "sell 120bps from mid ok");
        CHECK(!gate.slippage_ok(1, 489000, 500000), "sell 220bps rejected");
        MarketConfig off_cfg;
        off_cfg.pool_max_dev_bps = 0.0;
        VolatilityGate off_gate(off_cfg);
        CHECK(off_gate.slippage_ok(0, 900000, 500000),
              "dev=0 disables the gate explicitly");
    }
}

static void test_pool_dynamic_ttl() {
    std::printf("pool_dynamic_ttl\n");
    MarketConfig cfg;
    EIP712Signer signer;
    init_fixture(cfg, signer);
    PresignedOrderPool pool(cfg, signer, 60000);  // 60 s static TTL
    const uint64_t target =
        KellyEngine::usd_to_shares_fixed(cfg.max_order_usd, 0.505);
    CHECK(pool.rebuild(495000, 505000, target, 10000),
          "ladder builds for the TTL experiment");
    WireBody body{};
    uint64_t size = 0, maker = 0, taker = 0;
    CHECK(pool.acquire_at_most(0, 505000, 10000, target, body, size, maker,
                               taker, 0),
          "static TTL admits a fresh slot (max_age=0)");
    CHECK(pool.rebuild(495000, 505000, target, 10000),
          "second ladder builds");
    std::this_thread::sleep_for(std::chrono::milliseconds(6));
    WireBody body2{};
    CHECK(pool.acquire_at_most(0, 505000, 10000, target, body2, size, maker,
                               taker, 60000),
          "wide dynamic TTL still admits a fresh slot");
    CHECK(pool.rebuild(495000, 505000, target, 10000),
          "third ladder builds");
    std::this_thread::sleep_for(std::chrono::milliseconds(6));
    WireBody body3{};
    CHECK(!pool.acquire_at_most(0, 505000, 10000, target, body3, size, maker,
                                taker, 1),
          "1 ms dynamic TTL expires the rebuiltslot after 6 ms");
}

// ── P3 engine integration: 5%+ jump in <100 ms must not fire a stale order ──
static void test_engine_shock_acceptance() {
    std::printf("engine_shock_acceptance\n");
    MarketConfig cfg;
    EIP712Signer signer;
    init_fixture(cfg, signer);
    cfg.max_exposure_usd = 10000.0;
    cfg.max_portfolio_exposure_usd = 10000.0;
    cfg.max_daily_loss_usd = 10000.0;  // second signal must fit the budget

    OrderBookL2 book;
    book.set_tick_size(10000);
    Level2Entry bids[1] = {{495000, 100000000000ULL}};
    Level2Entry asks[1] = {{505000, 100000000000ULL}};
    book.set_book(bids, 1, asks, 1);  // tight 1 c spread around mid 0.50

    SPSC_RingBuffer<AlphaSignal> signals;
    SPSC_RingBuffer<JournalEvent> journal_q;
    VolatilityGate gate(cfg);
    EngineLayers layers{};
    layers.journal_q = &journal_q;
    layers.volatility = &gate;

    std::atomic<bool> trading_enabled{true};
    PresignedOrderPool pool(cfg, signer, cfg.presign_ttl_ms);
    MockCLOBClient client(cfg);
    ExecutionEngine<MockCLOBClient> engine(
        cfg, book, signals, signer, pool, client, &trading_enabled, &layers);

    // 1. Calm tight book: the gate must not alter the baseline behavior.
    signals.try_push(make_buy_signal(cfg, 0.90, 301));
    CHECK(engine.run_tick() == TickResult::SUBMITTED &&
              client.submissions() == 1,
          "calm tight book trades through the gate untouched");

    // 2. Mid jumps +6% (0.50 -> 0.53) between two consecutive ticks
    // (microseconds apart in wall clock, far under the 100 ms window).
    bids[0] = {525000, 100000000000ULL};
    asks[0] = {535000, 100000000000ULL};
    book.set_book(bids, 1, asks, 1);
    signals.try_push(make_buy_signal(cfg, 0.90, 302));
    const TickResult shocked = engine.run_tick();
    CHECK(shocked == TickResult::VOLATILITY_PAUSED &&
              client.submissions() == 1,
          ">5% jump in <100ms: stale order NOT consumed, flow paused");
    CHECK(gate.shocks() == 1 && gate.aborts() == 0,
          "shock counted exactly once, no slippage aborts on the way");

    // The journal records the suppression (acceptance: "logs and adapts").
    JournalEvent ev{};
    bool stale_logged = false;
    while (journal_q.try_pop(ev)) {
        if (ev.type == JournalEvent::Type::POOL_STALE_DROP) {
            stale_logged = true;
            CHECK(ev.aux0 == 530000 || ev.aux0 == 500000,
                  "journal carries the mid at shock time");
        }
    }
    CHECK(stale_logged, "suppressed stale ladder consumption is journaled");

    // 3. Cooldown lapses (250 ms real time); the new book becomes the new
    // normal and passive flow resumes at the refreshed prices.
    std::this_thread::sleep_for(std::chrono::milliseconds(300));
    signals.try_push(make_buy_signal(cfg, 0.90, 303));
    const TickResult resumed = engine.run_tick();
    CHECK(resumed == TickResult::SUBMITTED && client.submissions() == 2,
          "after the cooldown the gate adapts: flow resumes at new prices");
}

// ── P4: BayesianEngine posterior math ────────────────────────────────────────
static void test_bayes_math() {
    std::printf("bayes_math\n");
    {   // Prior from mid × N0 is an exact Beta(N0·p, N0·(1−p)).
        BayesianEngine eng;
        eng.ensure_prior(0.40, 24.0);
        CHECK(eng.has_prior(), "prior anchored at the book mid");
        CHECK(std::fabs(eng.posterior() - 0.40) < 1e-12,
              "prior posterior equals the mid that seeded it");
        // Acceptance-style COUNT evidence: 32 trials, 22 YES, weight 0.9.
        // α0: 9.6 + 22·0.9 = 29.4;  α1: 14.4 + 10·0.9 = 23.4.
        eng.update_count(0, 32, 22, 900000);
        const double p = eng.posterior();
        CHECK(std::fabs(p - (29.4 / 52.8)) < 1e-12 && p > 0.55 && p < 0.56,
              "beta-binomial posterior lands at 0.557 ($0.40 + high-rel)");
        CHECK(eng.count_events() == 1 && eng.events() == 1 &&
                  eng.lr_events() == 0,
              "event accounting is exact");
    }
    {   // LR evidence multiplies the log-odds exactly through the sigmoid.
        BayesianEngine eng;
        eng.ensure_prior(0.40, 24.0);
        eng.update_lr(693147, 1000000);  // +ln2 at full weight
        const double g = std::log(0.4 / 0.6) + 0.693147;
        const double expected = 1.0 / (1.0 + std::exp(-g));
        CHECK(std::fabs(eng.posterior() - expected) < 1e-9,
              "lr evidence shifts the posterior via the closed sigmoid");
        CHECK(eng.count_events() == 0 && eng.lr_events() == 1,
              "lr accounting kept separate from counts");
    }
    {   // Dirichlet: complement mass renormalizes proportionally over the
        // other slots; totals and shape stay conjugate-exact.
        BayesianEngine eng;
        eng.ensure_prior(0.40, 24.0);
        eng.update_count(2, 12, 9, 1000000);  // 9 of 12 hits on outcome 2
        // α0 = 9.6 + 3·(9.6/24) = 10.8; α1 = 14.4 + 3·(14.4/24) = 16.2;
        // α2 = 9;  total 36 → 0.30 / 0.45 / 0.25.
        CHECK(std::fabs(eng.posterior(0) - 0.30) < 1e-12 &&
                  std::fabs(eng.posterior(1) - 0.45) < 1e-12 &&
                  std::fabs(eng.posterior(2) - 0.25) < 1e-12,
              "dirichlet-multinomial keeps shape-preserving conjugacy");
        const double sum = eng.posterior(0) + eng.posterior(1) +
                           eng.posterior(2);
        CHECK(std::fabs(sum - 1.0) < 1e-12,
              "multi-outcome posteriors stay normalized");
    }
    {   // Structural guards: no prior, bad slots, zero weight are inert.
        BayesianEngine eng;
        eng.update_count(0, 10, 5, 1000000);  // no prior yet
        CHECK(eng.posterior() < 0.0, "no posterior before the prior seeds");
        eng.ensure_prior(0.5, 10.0);
        eng.update_count(0, 10, 5, 0);   // zero weight
        eng.update_count(9, 10, 5, 1000000);  // out-of-range slot
        eng.update_count(0, 10, 11, 1000000); // k > n
        CHECK(std::fabs(eng.posterior() - 0.5) < 1e-12 &&
                  eng.events() == 0,
              "invalid evidence is ignored without touching the state");
        // Idempotent seeding: a second call cannot move the anchor.
        eng.ensure_prior(0.9, 100.0);
        CHECK(std::fabs(eng.posterior() - 0.5) < 1e-12,
              "prior seeding is one-shot by design");
    }
}

// ── P4 engine integration: reliability gating + posterior trigger ────────────
static EvidenceEvent make_count_evidence(uint32_t source, uint32_t n,
                                         uint32_t k, uint32_t hash) {
    EvidenceEvent ev{};
    ev.kind = EvidenceEvent::Kind::COUNT;
    ev.outcome = 0;
    ev.source_id = source;
    ev.count_n = n;
    ev.count_k = k;
    ev.event_hash = hash;
    ev.timestamp_ns = crowdintel::realtime_ns();
    return ev;
}

static void test_engine_brain_acceptance() {
    std::printf("engine_brain_acceptance\n");
    MarketConfig cfg;
    EIP712Signer signer;
    init_fixture(cfg, signer);
    cfg.bayes_prior_strength = 24.0;
    cfg.bayes_signal_threshold = 0.03;
    cfg.bayes_min_reliability = 0.35;
    cfg.bayes_enable = true;
    cfg.max_exposure_usd = 10000.0;
    cfg.max_portfolio_exposure_usd = 10000.0;
    cfg.max_daily_loss_usd = 10000.0;

    OrderBookL2 book;
    book.set_tick_size(10000);
    Level2Entry bids[1] = {{395000, 100000000000ULL}};
    Level2Entry asks[1] = {{405000, 100000000000ULL}};
    book.set_book(bids, 1, asks, 1);  // mid exactly 0.40

    SPSC_RingBuffer<AlphaSignal> signals;
    SPSC_RingBuffer<EvidenceEvent> evidence_q;
    SPSC_RingBuffer<JournalEvent> journal_q;
    BayesianEngine bayes;
    SourceReliability sources;
    sources.set_weight(1, 0.90);    // reliable polling source
    sources.set_weight(2, 0.10);    // unreliable source
    EngineLayers layers{};
    layers.journal_q = &journal_q;
    layers.evidence_q = &evidence_q;
    layers.bayes = &bayes;
    layers.sources = &sources;

    std::atomic<bool> trading_enabled{true};
    PresignedOrderPool pool(cfg, signer, cfg.presign_ttl_ms);
    MockCLOBClient client(cfg);
    ExecutionEngine<MockCLOBClient> engine(
        cfg, book, signals, signer, pool, client, &trading_enabled, &layers);

    // Tick 1 seeds the prior from the mid (0.40); nothing fires.
    CHECK(engine.run_tick() == TickResult::NO_SIGNAL,
          "prior seeding alone never fires");
    CHECK(bayes.has_prior() &&
              std::fabs(bayes.posterior() - 0.40) < 1e-12,
          "posterior anchored at 0.40");

    // Reliability gate FIRST: untrusted source evidence never moves the
    // posterior (acceptance: no order with low reliability).
    evidence_q.try_push(make_count_evidence(2, 32, 22, 9901));
    CHECK(engine.run_tick() == TickResult::NO_SIGNAL &&
              client.submissions() == 0 &&
              std::fabs(bayes.posterior() - 0.40) < 1e-12,
          "low-reliability evidence stalls at the gate");

    // Trusted evidence: 32 trials / 22 YES at weight 0.9 → posterior 0.557,
    // buy divergence = 0.557 − 0.405 ≫ 0.03 → BAYES_SIGNAL order emitted.
    evidence_q.try_push(make_count_evidence(1, 32, 22, 9902));
    const TickResult fired = engine.run_tick();
    CHECK(fired == TickResult::BAYES_SIGNAL && client.submissions() == 1,
          "posterior 0.557 with ask 0.405 emits exactly one buy order");
    CHECK(bayes.posterior() > 0.42,
          "posterior actually moved above the ask");

    // One shot per batch: no new evidence means no re-fire next tick.
    CHECK(engine.run_tick() == TickResult::NO_SIGNAL &&
              client.submissions() == 1,
          "signal fires once per evidence batch, never repeatedly");

    // Journal captures the whole story: gate rejection, posterior update,
    // and the emitted bayesian order.
    JournalEvent ev{};
    bool low_rel = false, update_seen = false, signal_seen = false;
    double journaled_posterior = 0.0;
    while (journal_q.try_pop(ev)) {
        if (ev.type == JournalEvent::Type::BAYES_LOW_RELIABILITY) {
            low_rel = true;
            CHECK(ev.aux0 == 2, "untrusted source id journaled");
        }
        if (ev.type == JournalEvent::Type::BAYES_UPDATE)
            update_seen = true;
        if (ev.type == JournalEvent::Type::BAYES_SIGNAL) {
            signal_seen = true;
            journaled_posterior = static_cast<double>(ev.aux0) * 1e-6;
        }
    }
    CHECK(low_rel && update_seen && signal_seen,
          "journal holds gate, update, and emitted-signal records");
    CHECK(journaled_posterior > 0.42 && journaled_posterior < 0.60,
          "journaled signal carries the firing posterior");
}

int main() {
    std::printf("== CROWDINTEL layer tests ==\n");
    test_tracker_partial_fill();
    test_tracker_vwap_pnl_dedup();
    test_tracker_reservations_snapshot();
    test_user_event_parser();
    test_engine_tracker_integration();
    test_risk_units();
    test_risk_evaluate_ladder();
    test_engine_stop_loss_on_crash();
    test_engine_kill_freezes_orders();
    test_engine_hedge_on_dip();
    test_vol_gate_units();
    test_pool_dynamic_ttl();
    test_engine_shock_acceptance();
    test_bayes_math();
    test_engine_brain_acceptance();
    std::printf("== %s (%d failures) ==\n", g_failures ? "FAILED" : "ALL PASS",
                g_failures);
    return g_failures ? 1 : 0;
}
