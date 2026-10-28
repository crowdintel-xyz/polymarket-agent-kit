// ─────────────────────────────────────────────────────────────────────────────
// test_paper_broker.cpp — P4: Paper trading broker
//
// Ported from polywhales paper.ts (broker/paper.ts) — C++20 implementation,
// NO TypeScript copied.
//
// Tests:
//  1. Taker fill: print at price >= limit, after signal, within TTL
//  2. Maker fill: queue-pessimistic, only aggressive flow through our level
//  3. Price improvement NOT booked (we book at our limit, not print price)
//  4. Guard breach at fill time = STANDDOWN
//  5. Every FILLED has a tape basis (audit invariant)
//  6. Fee = 0.072 × p × (1-p), takers only (maker rebate NOT credited)
//  7. TTL expiry → MISSED
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>

#include "../../core/src/paper_broker.hpp"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  \u274C FAIL: %s\n", msg); g_failures++; } \
    else { printf("  \u2705 PASS: %s\n", msg); } \
} while(0)

static uint64_t g_mock_ms = 1'000'000;
static uint64_t mock_clock() { return g_mock_ms; }
static void mock_set_time(uint64_t ms) { g_mock_ms = ms; }

// ── Mock tape fetcher ────────────────────────────────────────────────────────
struct MockTape {
    std::vector<TapePrint> prints;
    int call_count = 0;

    std::vector<TapePrint> fetch(const std::string& /*condition_id*/, uint64_t /*since_sec*/) {
        call_count++;
        return prints;
    }
};

int main() {
    printf("=== P4: Paper Trading Broker ===\n");

    // ── Test 1: Taker fill ────────────────────────────────────────────────────
    printf("\n--- Test 1: Taker fill (print at >= limit, after signal, within TTL) ---\n");
    {
        mock_set_time(1000);
        MockTape tape;
        tape.prints = {{
            .transaction_hash = "0xabc123",
            .asset = "token-yes",
            .price = 0.50,
            .size = 100,
            .timestamp = 1,  // 1s = after signal at 0s, in ms = 1000
            .side = OrderSide::SELL,
        }};

        PaperBroker::Config cfg{};
        cfg.fetch_tape = [&tape](const std::string& cid, uint64_t s) {
            return tape.fetch(cid, s);
        };
        cfg.now_fn = mock_clock;

        PaperBroker broker(cfg);

        OrderIntent intent{};
        intent.lane = LaneId::SCALED;
        intent.side = OrderSide::BUY;
        intent.px = 0.48;
        intent.usd = 100.0;
        intent.ts = 1;  // 1ms
        intent.token = "token-yes";
        intent.condition_id = "0xcond1";
        intent.market_slug = "test-market";

        auto result = broker.submit(intent, {});
        CHECK(result.accepted, "taker order accepted");

        // Advance time to within TTL
        mock_set_time(5000);  // 5s later, within 120s TTL
        auto fills = broker.settle();
        CHECK(fills.size() >= 1, "at least 1 fill");
        CHECK(fills[0].status == FillStatus::FILLED, "fill is FILLED");
        CHECK(fills[0].shares > 0, "shares > 0");
        CHECK(fills[0].has_basis, "fill has tape basis (audit invariant)");
        CHECK(fills[0].basis.tx_hash == "0xabc123", "basis tx_hash traced");
    }

    // ── Test 2: Price improvement NOT booked ─────────────────────────────────
    printf("\n--- Test 2: Price improvement not booked (book at our limit) ---\n");
    {
        mock_set_time(1000);
        MockTape tape;
        tape.prints = {{
            .transaction_hash = "0xdef456",
            .asset = "token-yes",
            .price = 0.60,  // 12 cents better than our 0.48 limit
            .size = 100,
            .timestamp = 1,
            .side = OrderSide::SELL,
        }};

        PaperBroker::Config cfg{};
        cfg.fetch_tape = [&tape](const std::string& cid, uint64_t s) {
            return tape.fetch(cid, s);
        };
        cfg.now_fn = mock_clock;

        PaperBroker broker(cfg);

        OrderIntent intent{};
        intent.lane = LaneId::SCALED;
        intent.side = OrderSide::BUY;
        intent.px = 0.48;
        intent.usd = 100.0;
        intent.ts = 1;
        intent.token = "token-yes";
        intent.condition_id = "0xcond2";
        intent.market_slug = "test-market";

        broker.submit(intent, {});
        mock_set_time(5000);
        auto fills = broker.settle();
        CHECK(fills.size() >= 1, "fill produced");
        CHECK(fills[0].px == 0.48, "fill priced at OUR limit (0.48), not print (0.60)");
        CHECK(fills[0].basis.print_px == 0.60, "basis records actual print price");
    }

    // ── Test 3: Maker fill (queue-pessimistic) ────────────────────────────────
    printf("\n--- Test 3: Maker fill (queue-pessimistic) ---\n");
    {
        mock_set_time(1000);
        MockTape tape;
        // Two prints: first 50 shares at our price (consumes queue), second 30 (fills us)
        tape.prints = {
            {.transaction_hash = "0x1", .asset = "token-yes", .price = 0.48,
             .size = 50, .timestamp = 2, .side = OrderSide::SELL},
            {.transaction_hash = "0x2", .asset = "token-yes", .price = 0.48,
             .size = 30, .timestamp = 3, .side = OrderSide::SELL},
        };

        PaperBroker::Config cfg{};
        cfg.fetch_tape = [&tape](const std::string& cid, uint64_t s) {
            return tape.fetch(cid, s);
        };
        cfg.now_fn = mock_clock;

        PaperBroker broker(cfg);

        OrderIntent intent{};
        intent.lane = LaneId::MAKER_BID;
        intent.side = OrderSide::BUY;
        intent.px = 0.48;
        intent.usd = 1000.0;
        intent.ts = 1;
        intent.token = "token-yes";
        intent.condition_id = "0xcond3";
        intent.market_slug = "maker-test";

        // Book: 100 shares ahead of us at our price
        OrderBookTop book{};
        book.bids = {{0.48, 100.0}};  // 100 shares at our price — ahead of us

        auto result = broker.submit(intent, { .book = &book });
        CHECK(result.accepted, "maker order accepted");

        mock_set_time(5000);
        auto fills = broker.settle();
        CHECK(fills.size() >= 1, "maker fill after queue consumed");
        CHECK(fills[0].status == FillStatus::FILLED, "fill is FILLED");
        CHECK(fills[0].has_basis, "maker fill has basis");
    }

    // ── Test 4: Guard breach at fill time = STANDDOWN ─────────────────────────
    printf("\n--- Test 4: Guard breach at fill time ---\n");
    {
        mock_set_time(1000);
        MockTape tape;
        tape.prints = {{
            .transaction_hash = "0xguard",
            .asset = "token-yes", .price = 0.50, .size = 100,
            .timestamp = 1, .side = OrderSide::SELL,
        }};

        PaperBroker::Config cfg{};
        cfg.fetch_tape = [&tape](const std::string& cid, uint64_t s) {
            return tape.fetch(cid, s);
        };
        cfg.now_fn = mock_clock;
        // Time-dependent guard: OK at submit (1000ms), stale at fill (5000ms)
        cfg.guard = [](const OrderIntent&, uint64_t now) -> GuardState {
            GuardState g;
            g.stale = (now > 4000);  // goes stale after 4s
            return g;
        };

        PaperBroker broker(cfg);

        OrderIntent intent{};
        intent.lane = LaneId::SCALED;
        intent.side = OrderSide::BUY;
        intent.px = 0.48;
        intent.usd = 100.0;
        intent.ts = 1;
        intent.token = "token-yes";
        intent.condition_id = "0xcond4";
        intent.market_slug = "guard-test";

        auto result = broker.submit(intent, {});
        CHECK(result.accepted, "order accepted at submit (guard OK)");
        mock_set_time(5000);
        auto fills = broker.settle();
        CHECK(fills.size() >= 1, "fill produced");
        CHECK(fills[0].status == FillStatus::STANDDOWN, "guard breach → STANDDOWN");
        CHECK(fills[0].standdown_reason == "stale-city", "reason is stale-city");
    }

    // ── Test 5: Fee calculation (taker only, 0.072 × p × (1-p)) ──────────────
    printf("\n--- Test 5: Fee = 0.072 × p × (1-p), taker only ---\n");
    {
        mock_set_time(1000);
        MockTape tape;
        tape.prints = {{
            .transaction_hash = "0xfee",
            .asset = "token-yes", .price = 0.50, .size = 100,
            .timestamp = 1, .side = OrderSide::SELL,
        }};

        PaperBroker::Config cfg{};
        cfg.fetch_tape = [&tape](const std::string& cid, uint64_t s) {
            return tape.fetch(cid, s);
        };
        cfg.now_fn = mock_clock;

        PaperBroker broker(cfg);

        OrderIntent intent{};
        intent.lane = LaneId::SCALED;
        intent.side = OrderSide::BUY;
        intent.px = 0.50;
        intent.usd = 100.0;
        intent.ts = 1;
        intent.token = "token-yes";
        intent.condition_id = "0xfee";
        intent.market_slug = "fee-test";

        broker.submit(intent, {});
        mock_set_time(5000);
        auto fills = broker.settle();
        CHECK(fills.size() >= 1, "fill produced");
        // FeeCalculator::taker_fee(0.50) = 0.072 * 0.50 * 0.50 = 0.018
        double expected_fee = FeeCalculator::taker_fee(0.50) * 200;
        CHECK(fills[0].fee == expected_fee, "taker fee = 0.072 × p × (1-p) × shares");
    }

    // ── Test 6: TTL expiry → MISSED ───────────────────────────────────────────
    printf("\n--- Test 6: TTL expiry → MISSED ---\n");
    {
        mock_set_time(1000);
        MockTape tape;  // no prints
        tape.prints = {};

        PaperBroker::Config cfg{};
        cfg.fetch_tape = [&tape](const std::string& cid, uint64_t s) {
            return tape.fetch(cid, s);
        };
        cfg.now_fn = mock_clock;

        PaperBroker broker(cfg);

        OrderIntent intent{};
        intent.lane = LaneId::SCALED;
        intent.side = OrderSide::BUY;
        intent.px = 0.50;
        intent.usd = 100.0;
        intent.ts = 1;
        intent.token = "token-yes";
        intent.condition_id = "0xmiss";
        intent.market_slug = "miss-test";

        broker.submit(intent, {});
        // Advance PAST TTL (120s = 120000ms)
        mock_set_time(1'000'000);
        auto fills = broker.settle();
        CHECK(fills.size() >= 1, "fill produced (MISSED)");
        CHECK(fills[0].status == FillStatus::MISSED, "TTL expired → MISSED");
    }

    // ── Test 7: Hard guard breach (invalid price) ─────────────────────────────
    printf("\n--- Test 7: Hard guard breach ---\n");
    {
        PaperBroker::Config cfg{};
        cfg.now_fn = mock_clock;

        PaperBroker broker(cfg);

        OrderIntent intent{};
        intent.lane = LaneId::SCALED;
        intent.side = OrderSide::BUY;
        intent.px = 1.5;  // invalid (> 1.0)
        intent.usd = 100.0;
        intent.ts = 1;
        intent.token = "token-yes";
        intent.condition_id = "0xguard";
        intent.market_slug = "guard-test";

        auto result = broker.submit(intent, {});
        CHECK(!result.accepted, "invalid price rejected at submit");
        CHECK(result.has_fill, "STANDDOWN fill produced");
        CHECK(result.fill.status == FillStatus::STANDDOWN, "status is STANDDOWN");
    }

    printf("\n  P4 test complete: %d failures\n", g_failures);
    return g_failures > 0 ? 1 : 0;
}
