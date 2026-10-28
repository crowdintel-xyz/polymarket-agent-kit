// ─────────────────────────────────────────────────────────────────────────────
// test_maker_bid_metrics.cpp — P5: Maker-bid fill-rate metric
//
// Ported from polywhales lanes/maker-bid.ts — C++20, NO TypeScript copied.
//
// Tests:
//  1. Fill-rate = touched / placed (polywhales: 2/69 = 2.9%)
//  2. MakerBidPricing: improves best+0.01, capped at MAX_ASK
//  3. Would-cross detection (belongs to taker lane)
//  4. Queue-aware tracking (queue_ahead, time-to-fill)
//  5. Per-level fill rates
//  6. Outbid detection within 1-44s (polywhales observation)
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <algorithm>

#include "../../core/src/maker_bid_metrics.hpp"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  \u274C FAIL: %s\n", msg); g_failures++; } \
    else { printf("  \u2705 PASS: %s\n", msg); } \
} while(0)

static uint64_t g_mock_ms = 1'000'000;
static uint64_t mock_clock() { return g_mock_ms; }
#define mock_set_time(ms) g_mock_ms = (ms)

int main() {
    printf("=== P5: Maker-Bid Fill-Rate Metric ===\n");

    // ── Test 1: Fill-rate tracking (2/69 like polywhales) ──────────────────────
    printf("\n--- Test 1: Fill-rate = filled / placed (polywhales: 2/69) ---\n");
    {
        mock_set_time(1'000'000);

        MakerBidMetrics::Config cfg{};
        cfg.now_fn = mock_clock;
        cfg.ttl_ms = 7'200'000;  // 2h TTL

        MakerBidMetrics metrics(cfg);

        // Place 69 bids (matching polywhales observation)
        for (int i = 0; i < 69; i++) {
            MakerBidEntry e{};
            e.market_slug = "evt-" + std::to_string(i);
            e.condition_id = "0xcond" + std::to_string(i);
            e.yes_token = "token-yes-" + std::to_string(i);
            e.bid_price = 0.93;
            e.best_bid_at_post = 0.92;
            e.best_ask_at_post = 0.94;
            e.queue_ahead = 100;
            e.shares = 10000;
            e.post_ts = mock_clock();
            metrics.place_bid(e);
        }

        // 2 bids get filled (matching polywhales: 2 touched)
        metrics.record_fill("evt-0", mock_clock() + 5'000);  // filled 5s later
        metrics.record_fill("evt-1", mock_clock() + 60'000); // filled 1min later

        // Rest expire
        mock_set_time(mock_clock() + 7'300'000);  // past TTL
        metrics.record_expire(mock_clock());

        auto s = metrics.summary();
        CHECK(s.placed == 69, "69 bids placed");
        CHECK(s.filled == 2, "2 bids filled");
        CHECK(s.fill_rate > 0.02 && s.fill_rate < 0.04, "fill_rate ~2.9% (2/69)");
        CHECK(s.avg_queue_ahead == 100.0, "avg queue_ahead = 100");
        CHECK(s.avg_gap_to_ask > 0.019 && s.avg_gap_to_ask < 0.021, "avg gap to MAX_ASK = 0.02");
        CHECK(s.avg_ttf_ms > 0, "avg time-to-fill recorded");
    }

    // ── Test 2: MakerBidPricing — improves best+0.01, capped at MAX_ASK ────────
    printf("\n--- Test 2: Bid improves best+0.01, capped at MAX_ASK ---\n");
    {
        double px = MakerBidPricing::maker_bid_px(0.90);
        CHECK(px == 0.91, "bid = best+0.01 (0.90 -> 0.91)");

        px = MakerBidPricing::maker_bid_px(0.94);
        CHECK(px == 0.95, "bid capped at MAX_ASK (0.94 -> 0.95)");

        px = MakerBidPricing::maker_bid_px(0.95);
        CHECK(px == 0.95, "already at cap stays at MAX_ASK");
    }

    // ── Test 3: Would-cross detection ─────────────────────────────────────────
    printf("\n--- Test 3: Would-cross detection ---\n");
    {
        CHECK(MakerBidPricing::would_cross(0.95, 0.95), "bid >= ask → would cross");
        CHECK(MakerBidPricing::would_cross(0.95, 0.94), "bid > ask → would cross");
        CHECK(!MakerBidPricing::would_cross(0.90, 0.95), "bid < ask → no cross");
    }

    // ── Test 4: Queue-aware time-to-fill ──────────────────────────────────────
    printf("\n--- Test 4: Queue-aware TTF tracking ---\n");
    {
        mock_set_time(1'000'000);

        MakerBidMetrics::Config cfg{};
        cfg.now_fn = mock_clock;
        cfg.ttl_ms = 7'200'000;

        MakerBidMetrics metrics(cfg);

        MakerBidEntry e{};
        e.market_slug = "queue-test";
        e.yes_token = "tok-q";
        e.bid_price = 0.92;
        e.queue_ahead = 500;
        e.shares = 10000;
        e.post_ts = mock_clock();
        metrics.place_bid(e);

        // First print: 200 shares at 0.92 (<= our bid) — touches but not filled (queue ahead)
        metrics.record_tape_print("tok-q", 0.92, mock_clock() + 1'000, 200, true);

        auto s1 = metrics.summary();
        CHECK(s1.touched == 1, "tape print touched level");
        CHECK(s1.filled == 0, "not filled yet (queue ahead)");

        // Second print: 500 shares at 0.92 — consumes queue, then 300 flows to us
        metrics.record_tape_print("tok-q", 0.92, mock_clock() + 5'000, 500, true);
        metrics.record_fill("queue-test", mock_clock() + 5'001);

        auto s2 = metrics.summary();
        CHECK(s2.filled == 1, "filled after queue consumed");
        CHECK(s2.avg_ttf_ms > 0 && s2.avg_ttf_ms < 10'000, "TTF < 10s");
    }

    // ── Test 5: Per-level fill rates ──────────────────────────────────────────
    printf("\n--- Test 5: Per-level fill rates ---\n");
    {
        mock_set_time(1'000'000);

        MakerBidMetrics::Config cfg{};
        cfg.now_fn = mock_clock;
        cfg.ttl_ms = 7'200'000;

        MakerBidMetrics metrics(cfg);

        // Place bids at different price levels
        for (int i = 0; i < 10; i++) {
            MakerBidEntry e{};
            e.market_slug = "lvl-0.91-" + std::to_string(i);
            e.yes_token = "tok-0.91-" + std::to_string(i);
            e.bid_price = 0.91;
            e.post_ts = mock_clock();
            metrics.place_bid(e);
        }
        for (int i = 0; i < 5; i++) {
            MakerBidEntry e{};
            e.market_slug = "lvl-0.92-" + std::to_string(i);
            e.yes_token = "tok-0.92-" + std::to_string(i);
            e.bid_price = 0.92;
            e.post_ts = mock_clock();
            metrics.place_bid(e);
        }

        // Fill 3 at 0.91, 1 at 0.92
        metrics.record_fill("lvl-0.91-0", mock_clock() + 1'000);
        metrics.record_fill("lvl-0.91-1", mock_clock() + 2'000);
        metrics.record_fill("lvl-0.91-2", mock_clock() + 3'000);
        metrics.record_fill("lvl-0.92-0", mock_clock() + 4'000);

        auto levels = metrics.per_level();
        CHECK(levels.size() == 2, "2 price levels tracked");

        auto lvl_091 = std::find_if(levels.begin(), levels.end(),
            [](const auto& l) { return l.price == 0.91; });
        CHECK(lvl_091 != levels.end(), "level 0.91 found");
        CHECK(lvl_091->placed == 10, "10 placed at 0.91");
        CHECK(lvl_091->filled == 3, "3 filled at 0.91");
        CHECK(lvl_091->fill_rate == 0.3, "fill_rate = 3/10 = 0.3 at 0.91");

        auto lvl_092 = std::find_if(levels.begin(), levels.end(),
            [](const auto& l) { return l.price == 0.92; });
        CHECK(lvl_092 != levels.end(), "level 0.92 found");
        CHECK(lvl_092->fill_rate == 0.2, "fill_rate = 1/5 = 0.2 at 0.92");
    }

    // ── Test 6: Outbid detection (polywhales: 1-44s) ───────────────────────────
    printf("\n--- Test 6: Outbid detection ---\n");
    {
        mock_set_time(1'000'000);

        MakerBidMetrics::Config cfg{};
        cfg.now_fn = mock_clock;
        cfg.ttl_ms = 7'200'000;

        MakerBidMetrics metrics(cfg);

        MakerBidEntry e{};
        e.market_slug = "outbid-test";
        e.bid_price = 0.91;
        e.post_ts = mock_clock();
        metrics.place_bid(e);

        // Outbid 30s later (within polywhales 1-44s window... well, 30s is close)
        metrics.record_outbid("outbid-test", mock_clock() + 30'000);

        auto s = metrics.summary();
        CHECK(s.outbid == 1, "outbid detected");
        CHECK(s.avg_outbid_delay_ms == 30'000, "outbid delay = 30s");
        CHECK(s.fill_rate == 0.0, "no fill, outbid only");
        CHECK(s.outbid_rate == 1.0, "outbid_rate = 100%");
    }

    printf("\n  P5 test complete: %d failures\n", g_failures);
    return g_failures > 0 ? 1 : 0;
}
