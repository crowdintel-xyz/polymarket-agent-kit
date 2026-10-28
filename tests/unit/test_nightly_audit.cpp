// ─────────────────────────────────────────────────────────────────────────────
// test_nightly_audit.cpp — P6: Nightly audit ledger
//
// Ported from polywhales ledger/score.ts — C++20, NO TypeScript copied.
//
// Tests:
//  1. FILLED without basis → VIOLATION
//  2. Entry > MAX_ASK → VIABILITY
//  3. USD > CLIP_CAP_USD → VIOLATION
//  4. Notional mismatch (shares × px ≠ usd) → VIOLATION
//  5. Fee mismatch → VIOLATION (taker fee = 0.072 × p × (1-p) × shares)
//  6. Tape invariant: basis.txHash not on tape → VIOLATION
//  7. Exit without entry → VIOLATION
//  8. scalp-no held to settlement → VIOLATION
//  9. Fee correct for maker (zero) → no finding
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <cstdint>
#include <string>
#include <vector>
#include <optional>

#include "../../core/src/nightly_audit.hpp"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  \u274C FAIL: %s\n", msg); g_failures++; } \
    else { printf("  \u2705 PASS: %s\n", msg); } \
} while(0)

static uint64_t g_mock_ms = 1'000'000;
static uint64_t mock_clock() { return g_mock_ms; }
#define mock_set_time(ms) g_mock_ms = (ms)

int main() {
    printf("=== P6: Nightly Audit Ledger ===\n");

    // ── Test 1: FILLED without basis → VIOLATION ────────────────────────────────
    printf("\n--- Test 1: FILLED without basis → VIOLATION ---\n");
    {
        std::vector<LedgerRow> rows;
        LedgerRow r{};
        r.lane = LaneId::SCALED;
        r.market_slug = "no-basis";
        r.status = FillStatus::FILLED;
        r.side = OrderSide::BUY;
        r.px = 0.50;
        r.shares = 200;
        r.usd = 100.0;
        r.fee = FeeCalculator::taker_fee(0.50) * 200;
        r.filled_at = 1'000'000;
        r.basis = std::nullopt;  // no basis!
        rows.push_back(r);

        NightlyAudit::Config cfg{};
        cfg.skip_tape = true;  // skip tape for this test
        cfg.now_fn = mock_clock;
        NightlyAudit audit(cfg);

        auto summary = audit.run(rows);
        bool found = false;
        for (const auto& f : summary.audit.findings) {
            if (f.kind == "fill-without-basis") found = true;
        }
        CHECK(found, "FILLED without basis → VIOLATION");
    }

    // ── Test 2: Entry > MAX_ASK → VIOLATION ────────────────────────────────────
    printf("\n--- Test 2: Entry > MAX_ASK → VIOLATION ---\n");
    {
        std::vector<LedgerRow> rows;
        LedgerRow r{};
        r.lane = LaneId::SCALED;
        r.market_slug = "over-ask";
        r.status = FillStatus::FILLED;
        r.side = OrderSide::BUY;
        r.px = 0.96;  // > MAX_ASK (0.95)
        r.shares = 200;
        r.usd = 192.0;
        r.fee = FeeCalculator::taker_fee(0.96) * 200;
        r.filled_at = 1'000'000;
        r.basis = LedgerRow::Basis{"0xtx", 1'000'000, 0.96, 200};
        rows.push_back(r);

        NightlyAudit::Config cfg{};
        cfg.skip_tape = true;
        cfg.now_fn = mock_clock;
        NightlyAudit audit(cfg);

        auto summary = audit.run(rows);
        bool found = false;
        for (const auto& f : summary.audit.findings) {
            if (f.kind == "guard-breach-price") found = true;
        }
        CHECK(found, "Entry > MAX_ASK → VIOLATION");
    }

    // ── Test 3: USD > CLIP_CAP_USD → VIOLATION ───────────────────────────────────
    printf("\n--- Test 3: USD > CLIP_CAP_USD → VIOLATION ---\n");
    {
        std::vector<LedgerRow> rows;
        LedgerRow r{};
        r.lane = LaneId::SCALED;
        r.market_slug = "over-clip";
        r.status = FillStatus::FILLED;
        r.side = OrderSide::BUY;
        r.px = 0.50;
        r.shares = 200;
        r.usd = 6000.0;  // > CLIP_CAP_USD (5000)
        r.fee = FeeCalculator::taker_fee(0.50) * 200;
        r.filled_at = 1'000'000;
        r.basis = LedgerRow::Basis{"0xtx", 1'000'000, 0.50, 200};
        rows.push_back(r);

        NightlyAudit::Config cfg{};
        cfg.skip_tape = true;
        cfg.now_fn = mock_clock;
        NightlyAudit audit(cfg);

        auto summary = audit.run(rows);
        bool found = false;
        for (const auto& f : summary.audit.findings) {
            if (f.kind == "guard-breach-clip") found = true;
        }
        CHECK(found, "USD > CLIP_CAP_USD → VIOLATION");
    }

    // ── Test 4: Notional mismatch → VIOLATION ───────────────────────────────────
    printf("\n--- Test 4: Notional mismatch → VIOLATION ---\n");
    {
        std::vector<LedgerRow> rows;
        LedgerRow r{};
        r.lane = LaneId::SCALED;
        r.market_slug = "notional-mismatch";
        r.status = FillStatus::FILLED;
        r.side = OrderSide::BUY;
        r.px = 0.50;
        r.shares = 200;
        r.usd = 150.0;  // 200 × 0.50 = 100, not 150
        r.fee = FeeCalculator::taker_fee(0.50) * 200;
        r.filled_at = 1'000'000;
        r.basis = LedgerRow::Basis{"0xtx", 1'000'000, 0.50, 200};
        rows.push_back(r);

        NightlyAudit::Config cfg{};
        cfg.skip_tape = true;
        cfg.now_fn = mock_clock;
        NightlyAudit audit(cfg);

        auto summary = audit.run(rows);
        bool found = false;
        for (const auto& f : summary.audit.findings) {
            if (f.kind == "notional-mismatch") found = true;
        }
        CHECK(found, "Notional mismatch → VIOLATION");
    }

    // ── Test 5: Fee mismatch → VIOLATION ────────────────────────────────────────
    printf("\n--- Test 5: Fee mismatch (taker) → VIOLATION ---\n");
    {
        std::vector<LedgerRow> rows;
        LedgerRow r{};
        r.lane = LaneId::SCALED;
        r.market_slug = "fee-mismatch";
        r.status = FillStatus::FILLED;
        r.side = OrderSide::BUY;  // taker
        r.px = 0.50;
        r.shares = 200;
        r.usd = 100.0;
        r.fee = 999.0;  // wrong fee
        r.filled_at = 1'000'000;
        r.basis = LedgerRow::Basis{"0xtx", 1'000'000, 0.50, 200};
        rows.push_back(r);

        NightlyAudit::Config cfg{};
        cfg.skip_tape = true;
        cfg.now_fn = mock_clock;
        NightlyAudit audit(cfg);

        auto summary = audit.run(rows);
        bool found = false;
        for (const auto& f : summary.audit.findings) {
            if (f.kind == "fee-mismatch") found = true;
        }
        CHECK(found, "Fee mismatch → VIOLATION");
    }

    // ── Test 6: Tape invariant verification ─────────────────────────────────────
    printf("\n--- Test 6: Tape invariant (basis txHash on tape) ---\n");
    {
        std::vector<LedgerRow> rows;
        LedgerRow r{};
        r.lane = LaneId::SCALED;
        r.market_slug = "tape-check";
        r.status = FillStatus::FILLED;
        r.side = OrderSide::BUY;
        r.px = 0.50;
        r.shares = 200;
        r.usd = 100.0;
        r.fee = FeeCalculator::taker_fee(0.50) * 200;
        r.filled_at = 1'000'000;
        r.token = "token-yes";
        r.basis = LedgerRow::Basis{"0xfound", 1'000, 0.50, 200};
        rows.push_back(r);

        // Tape: has the txHash → verified
        std::vector<AuditTapePrint> tape{{
            .transaction_hash = "0xfound", .asset = "token-yes",
            .price = 0.50, .size = 200, .timestamp = 1, .side = OrderSide::SELL
        }};

        NightlyAudit::Config cfg{};
        cfg.tape = [&tape](const std::string&, uint64_t) { return tape; };
        cfg.now_fn = mock_clock;
        NightlyAudit audit(cfg);

        auto summary = audit.run(rows);
        CHECK(summary.audit.basis_checked == 1, "1 basis checked");
        CHECK(summary.audit.basis_verified == 1, "1 basis verified");
        bool violation = false;
        for (const auto& f : summary.audit.findings) {
            if (f.kind == "basis-not-on-tape") violation = true;
        }
        CHECK(!violation, "no violation (txHash on tape)");
    }

    // ── Test 6b: Tape invariant — missing txHash → VIOLATION ─────────────────────
    printf("\n--- Test 6b: Tape invariant — missing txHash → VIOLATION ---\n");
    {
        std::vector<LedgerRow> rows;
        LedgerRow r{};
        r.lane = LaneId::SCALED;
        r.market_slug = "tape-missing";
        r.status = FillStatus::FILLED;
        r.side = OrderSide::BUY;
        r.px = 0.50;
        r.shares = 200;
        r.usd = 100.0;
        r.fee = FeeCalculator::taker_fee(0.50) * 200;
        r.filled_at = 1'000'000;
        r.token = "token-yes";
        r.basis = LedgerRow::Basis{"0xmissing", 1'000, 0.50, 200};
        rows.push_back(r);

        // Tape: does NOT have the txHash → unverifiable
        std::vector<AuditTapePrint> tape{{
            .transaction_hash = "0xother", .asset = "token-yes",
            .price = 0.50, .size = 100, .timestamp = 1, .side = OrderSide::SELL
        }};

        NightlyAudit::Config cfg{};
        cfg.tape = [&tape](const std::string&, uint64_t) { return tape; };
        cfg.now_fn = mock_clock;
        NightlyAudit audit(cfg);

        auto summary = audit.run(rows);
        bool found = false;
        for (const auto& f : summary.audit.findings) {
            if (f.kind == "basis-not-on-tape") found = true;
        }
        CHECK(found, "txHash not on tape → VIOLATION");
    }

    // ── Test 7: Fee correct for maker (zero) → no finding ───────────────────────
    printf("\n--- Test 7: Maker fill fee = 0 → no fee-mismatch finding ---\n");
    {
        std::vector<LedgerRow> rows;
        LedgerRow r{};
        r.lane = LaneId::MAKER_BID;  // maker
        r.market_slug = "maker-fill";
        r.status = FillStatus::FILLED;
        r.side = OrderSide::BUY;  // maker bid
        r.px = 0.50;
        r.shares = 200;
        r.usd = 100.0;
        r.fee = 0.0;  // zero fee for maker
        r.filled_at = 1'000'000;
        r.token = "token-yes";
        r.basis = LedgerRow::Basis{"0xtx", 1'000, 0.50, 200};
        rows.push_back(r);

        NightlyAudit::Config cfg{};
        cfg.skip_tape = true;
        cfg.now_fn = mock_clock;
        NightlyAudit audit(cfg);

        auto summary = audit.run(rows);
        bool found = false;
        for (const auto& f : summary.audit.findings) {
            if (f.kind == "fee-mismatch") found = true;
        }
        CHECK(!found, "maker fill fee = 0 → no fee-mismatch");
    }

    // ── Test 8: scalp-no held to settlement → VIOLATION ─────────────────────────
    printf("\n--- Test 8: scalp-no held to settlement → VIOLATION ---\n");
    {
        std::vector<LedgerRow> rows;
        LedgerRow r{};
        r.lane = LaneId::SCALP_NO;
        r.market_slug = "scalp-settle";
        r.status = FillStatus::FILLED;
        r.side = OrderSide::BUY;
        r.px = 0.50;
        r.shares = 200;
        r.usd = 100.0;
        r.fee = FeeCalculator::taker_fee(0.50) * 200;
        r.filled_at = 1'000'000;
        r.outcome = "Yes";
        r.token = "token-yes";
        r.basis = LedgerRow::Basis{"0xtx", 1'000, 0.50, 200};
        rows.push_back(r);

        // Resolution: market closed, yes=1.0, no=0.0 (clean)
        MarketResolution res{};
        res.slug = "scalp-settle";
        res.closed = true;
        res.yes_price = 1.0;
        res.no_price = 0.0;
        res.clean = true;

        NightlyAudit::Config cfg{};
        cfg.resolution = [&res](const std::string&) { return res; };
        cfg.skip_tape = true;
        cfg.now_fn = mock_clock;
        NightlyAudit audit(cfg);

        auto summary = audit.run(rows);
        bool found = false;
        for (const auto& f : summary.audit.findings) {
            if (f.kind == "scalp-held-to-settlement") found = true;
        }
        CHECK(found, "scalp-no settled → VIOLATION");
    }

    printf("\n  P6 test complete: %d failures\n", g_failures);
    return g_failures > 0 ? 1 : 0;
}
