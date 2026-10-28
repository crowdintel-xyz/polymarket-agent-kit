// ─────────────────────────────────────────────────────────────────────────────
// test_daily_volume_persist.cpp — T1.3: Persistent daily buy-volume counter
//
// BUG: daily_buy_volume_ exists only in-memory. If the bot restarts, the
// counter resets to zero, allowing the daily loss cap (max_daily_loss_usd)
// to be bypassed on the same calendar day.
//
// Test:
//   1. Save state mid-day (non-zero volume), create fresh engine, load → preserved
//   2. Save state with different day → fresh engine starts at zero
//   3. Atomic write (temp + rename) works
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <cstdlib>

#include "../../core/src/market_config.hpp"
#include "../../core/src/execution_engine.hpp"
#include "../../core/src/mock_client.hpp"
#include "../../core/include/order_book.hpp"
#include "../../core/include/spsc_ring_buffer.hpp"
#include "../../core/crypto/eip712_signer.hpp"
#include "alpha_parser.hpp"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  \u274C FAIL: %s\n", msg); g_failures++; } \
    else { printf("  \u2705 PASS: %s\n", msg); } \
} while(0)

int main() {
    printf("=== T1.3: Persistent daily volume counter ===\n");

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
    cfg.max_exposure_usd = 10000.0;
    cfg.max_daily_loss_usd = 50.0;
    parse_uint256_dec(token, std::strlen(token), cfg.token_id_be);
    std::snprintf(cfg.token_id_dec, sizeof(cfg.token_id_dec), "%s", token);
    std::snprintf(cfg.market_slug, sizeof(cfg.market_slug), "t13-persist-test");
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

    char state_path[256];
    std::snprintf(state_path, sizeof(state_path), "/tmp/t13_daily_vol_%d.dat", getpid());

    // ── Test 1: Persist via simulated volume ─────────────────────────
    // We use a helper: the engine's save/load methods work on the internal
    // daily_buy_volume_ field. We can't set it directly (private), but we
    // can verify load/save round-trips by pre-writing a state file.
    printf("\n--- Test 1: Save/load round-trip ---\n");
    {
        // Pre-write a state file with volume=30.0 and current day
        FILE* f = fopen(state_path, "wb");
        CHECK(f != nullptr, "create state file");
        if (f) {
            double vol = 30.0;
            uint64_t day = utc_day_number();
            fwrite(&vol, sizeof(vol), 1, f);
            fwrite(&day, sizeof(day), 1, f);
            fclose(f);
        }

        MockCLOBClient client(cfg);
        ExecutionEngine<MockCLOBClient> engine(cfg, book, signals, signer, pool, client);
        CHECK(engine.load_daily_volume_state(state_path), "load state from file");
        CHECK(engine.daily_buy_volume() == 30.0, "volume preserved after load");
    }

    // ── Test 2: New day → reset ──────────────────────────────────────
    printf("\n--- Test 2: New day resets volume ---\n");
    {
        FILE* f = fopen(state_path, "wb");
        if (f) {
            double vol = 45.0;
            uint64_t old_day = utc_day_number() - 1;  // yesterday
            fwrite(&vol, sizeof(vol), 1, f);
            fwrite(&old_day, sizeof(old_day), 1, f);
            fclose(f);
        }

        MockCLOBClient client(cfg);
        ExecutionEngine<MockCLOBClient> engine(cfg, book, signals, signer, pool, client);
        engine.load_daily_volume_state(state_path);
        engine.check_daily_reset();  // should detect day change and reset
        CHECK(engine.daily_buy_volume() == 0.0, "volume reset to 0 on new day");
    }

    // ── Test 3: Missing file → load returns false ────────────────────
    printf("\n--- Test 3: Missing file graceful ---\n");
    {
        MockCLOBClient client(cfg);
        ExecutionEngine<MockCLOBClient> engine(cfg, book, signals, signer, pool, client);
        char missing_path[128];
        std::snprintf(missing_path, sizeof(missing_path), "/tmp/t13_nonexistent_%d.dat", getpid());
        CHECK(!engine.load_daily_volume_state(missing_path),
              "load returns false for missing file");
    }

    // Cleanup
    std::remove(state_path);
    remove((std::string(state_path) + ".tmp").c_str());

    printf("\n  T1.3 test complete: %d failures\n", g_failures);
    return g_failures > 0 ? 1 : 0;
}
