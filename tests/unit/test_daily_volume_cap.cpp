// ─────────────────────────────────────────────────────────────────────────────
// test_daily_volume_cap.cpp — T1.1: Daily buy-volume counter tracks with tracker
//
// BUG (ID-C02): Line 458 `if (!tracker_)` gates the daily_buy_volume_ counter.
//   main_hot_path.cpp always connects the tracker (line 259), so when
//   tracker_ is attached, daily_buy_volume_ never increments, making the
//   daily loss cap (max_daily_loss_usd) a no-op — unbounded daily exposure.
//
// TEST: With tracker attached, max_daily_loss_usd=50, orders of £10 each.
//   BEFORE FIX: all 15 orders accepted (counter stays 0, cap bypassed).
//   AFTER FIX:  5 accepted, 10 rejected (counter tracks correctly).
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <chrono>

#include "../../core/include/order_book.hpp"
#include "../../core/include/spsc_ring_buffer.hpp"
#include "../../core/include/position_tracker.hpp"
#include "../../core/crypto/eip712_signer.hpp"
#include "../../core/src/market_config.hpp"
#include "../../core/src/mock_client.hpp"
#include "../../core/src/execution_engine.hpp"
#include "alpha_parser.hpp"

using namespace crowdintel;

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  \u274C FAIL: %s\n", msg); g_failures++; } \
    else { printf("  \u2705 PASS: %s\n", msg); } \
} while(0)

int main() {
    printf("=== T1.1: Daily volume cap with tracker attached ===\n");

    const char* key_text =
        "23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b";
    uint8_t key[32];
    CHECK(parse_hex_bytes(key_text, 64, key, sizeof(key)), "signer key parsed");

    EIP712Signer signer;
    CHECK(signer.init(key, false), "signer initialized");
    secure_zero(key, sizeof(key));

    const char* token =
        "71321045679252212594626395510336467040167069592778062791519851593659551227755";

    MarketConfig cfg{};
    cfg.max_order_usd = 10.0;
    cfg.max_exposure_usd = 10000.0;   // high — won't trigger
    cfg.max_daily_loss_usd = 50.0;    // £50 cap → 5 orders of £10
    parse_uint256_dec(token, std::strlen(token), cfg.token_id_be);
    std::snprintf(cfg.token_id_dec, sizeof(cfg.token_id_dec), "%s", token);
    std::snprintf(cfg.market_slug, sizeof(cfg.market_slug), "t11-cap-market");
    cfg.market_hash = alpha_hash_bytes(cfg.market_slug, std::strlen(cfg.market_slug));
    CHECK(cfg.finalize_identity(signer.signer_address()) == nullptr, "identity finalized");
    std::snprintf(cfg.owner_api_key, sizeof(cfg.owner_api_key), "test-owner");

    OrderBookL2 book;
    book.set_tick_size(10000);
    const Level2Entry bids[] = {{470000, 50000000}};
    const Level2Entry asks[] = {{530000, 50000000}};
    book.set_book(bids, 1, asks, 1);

    SPSC_RingBuffer<AlphaSignal> signals;
    PresignedOrderPool pool(cfg, signer, cfg.presign_ttl_ms);
    CHECK(pool.rebuild(470000, 530000, 20000000, 10000), "pool rebuilt");

    // With tracker attached (normal production case)
    PositionTracker tracker;
    EngineLayers layers{};
    layers.tracker = &tracker;

    MockCLOBClient client(cfg);
    ExecutionEngine<MockCLOBClient> engine(cfg, book, signals, signer, pool, client,
                                           nullptr, &layers);

    int accepted = 0, rejected = 0;
    for (int i = 0; i < 15; i++) {
        AlphaSignal signal{};
        signal.direction_hint = K_SIDE_BUY;
        signal.p_win = 0.75;
        signal.confidence = 0.95;
        signal.q_value = 0.01;
        signal.timestamp_ns = AlphaParser::realtime_ns();
        signal.market_hash = cfg.market_hash;
        signal.signal_id = 1000 + i;

        signals.try_push(signal);
        auto result = engine.run_tick();
        if (result == TickResult::SUBMITTED) accepted++;
        else rejected++;
    }

    printf("  Accepted: %d, Rejected: %d\n", accepted, rejected);
    CHECK(accepted == 5, "exactly 5 orders accepted before £50 cap");
    CHECK(rejected == 10, "10 orders rejected after cap (with tracker)");

    printf("\n  T1.1 test complete: %d failures\n", g_failures);
    return g_failures > 0 ? 1 : 0;
}
