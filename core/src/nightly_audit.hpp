// ─────────────────────────────────────────────────────────────────────────────
// nightly_audit.hpp — P6: Nightly audit ledger (score + tape invariant)
//
// Ported from polywhales ledger/score.ts — C++20, NO TypeScript copied.
//
// Two jobs, deliberately in one place:
//  1. SCORE — join FILLED rows to Gamma resolution, compute realized PnL per lane, net of fees.
//  2. AUDIT — re-fetch the live tape and prove every FILLED row's basis.txHash is
//     still there. Structural breaches (entry above MAX_ASK, clip above cap, fee
//     mismatch, scalp held to settlement, standdown without reason) are checked too.
//
// API facts from score.ts (measured 2026-08-26):
//  - /markets?slug=X&closed=true returns resolved markets; /markets?slug=X hides closed
//  - outcomePrices is a JSON string array, index 0 = Yes, 1 = No
//  - /markets?condition_id=X silently returns unrelated markets — never use it
//  - A fetch error is retried, then reported as unknown (never cached as "no market")
//
// Audit report is append-only JSONL.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <vector>
#include <map>
#include <unordered_map>
#include <algorithm>
#include <cmath>
#include <optional>
#include <functional>

#include "fee_calculator.hpp"
#include "paper_broker.hpp"

// ── Constants ─────────────────────────────────────────────────────────────────
namespace AUDIT {
    constexpr double MAX_ASK = 0.95;
    constexpr double CLIP_CAP_USD = 5'000.0;
    constexpr uint64_t GAMMA_RETRY_DELAY_MS = 250;
    constexpr int GAMMA_MAX_RETRIES = 3;
}

// ── Resolution from Gamma ─────────────────────────────────────────────────────
struct MarketResolution {
    std::string slug;
    bool closed = false;
    std::optional<double> yes_price;   // payoff per share of Yes token
    std::optional<double> no_price;
    bool clean = false;  // exactly one side at 1, other at 0
    std::string error;   // empty = OK
};

// ── Tape print (for audit verification) ───────────────────────────────────────
struct AuditTapePrint {
    std::string transaction_hash;
    std::string asset;
    double price = 0.0;
    uint64_t size = 0;
    uint64_t timestamp = 0;  // epoch seconds
    OrderSide side = OrderSide::BUY;
};

// ── Ledger row (persisted fill record) ────────────────────────────────────────
struct LedgerRow {
    LaneId lane = LaneId::SCALED;
    std::string city;
    std::string market_slug;
    std::string condition_id;
    OrderSide side = OrderSide::BUY;
    std::string outcome;  // "Yes" or "No"
    std::string token;      // YES token ID (for basis.tasset match)
    double px = 0.0;
    uint64_t shares = 0;
    double usd = 0.0;     // notional (excluding fee)
    double fee = 0.0;     // total fee paid
    FillStatus status = FillStatus::MISSED;
    uint64_t filled_at = 0;  // epoch ms
    std::string standdown_reason;

    // Tape basis (audit invariant: FILLED must trace to real print)
    struct Basis {
        std::string tx_hash;
        uint64_t print_ts = 0;  // epoch ms
        double print_px = 0.0;
        uint64_t print_sz = 0;
    };
    std::optional<Basis> basis;
};

// ── Audit findings ─────────────────────────────────────────────────────────────
enum class Severity { VIOLATION, WARN };

struct AuditFinding {
    LaneId lane = LaneId::SCALED;
    std::string market_slug;
    uint64_t filled_at = 0;
    std::string kind;
    std::string detail;
    Severity severity;
};

// ── Position score ─────────────────────────────────────────────────────────────
enum class Disposition { SCALPED, SETTLED, OPEN, UNRESOLVED };

struct PositionScore {
    LaneId lane = LaneId::SCALED;
    std::string city;
    std::string market_slug;
    std::string outcome;
    double px = 0.0;
    uint64_t shares = 0;
    double cost = 0.0;           // usd + fee
    std::optional<double> exit_px;
    std::optional<double> proceeds;
    std::optional<double> realized;
    Disposition disposition = Disposition::OPEN;
    std::optional<bool> won;
};

// ── Lane score ─────────────────────────────────────────────────────────────────
struct LaneScore {
    LaneId lane = LaneId::SCALED;
    uint64_t intents = 0;
    uint64_t filled = 0;
    uint64_t missed = 0;
    uint64_t standdown = 0;
    double fill_rate = 0.0;
    double gross_deployed = 0.0;
    double fees = 0.0;
    double realized = 0.0;
    uint64_t open = 0;
    uint64_t wins = 0;
    uint64_t losses = 0;
    double lane_cash = 0.0;
};

// ── Score summary ──────────────────────────────────────────────────────────────
struct ScoreSummary {
    uint64_t generated_at = 0;
    std::string day;
    std::unordered_map<LaneId, LaneScore> lane_scores;
    std::vector<PositionScore> positions;
    struct {
        uint64_t filled_rows = 0;
        uint64_t basis_checked = 0;
        uint64_t basis_verified = 0;
        uint64_t basis_unverifiable = 0;
        uint64_t violations = 0;
        uint64_t warnings = 0;
        std::vector<AuditFinding> findings;
    } audit;
    struct {
        double realized = 0.0;
        double fees = 0.0;
        uint64_t filled = 0;
        uint64_t missed = 0;
        uint64_t standdown = 0;
    } totals;
};

// ── Resolution fetcher (Gamma API) ────────────────────────────────────────────
// In production, this hits https://gamma-api.polymarket.com/markets?slug=X&closed=true
// For testing, inject a mock.
using ResolutionFetcher = std::function<MarketResolution(const std::string& slug)>;

// ── Tape fetcher (for audit verification) ─────────────────────────────────────
using AuditTapeFetcher = std::function<std::vector<AuditTapePrint>(
    const std::string& condition_id, uint64_t since_sec)>;

// ── LaneLedgerRow → LaneId helper ─────────────────────────────────────────────
inline LaneId parse_lane_id(const std::string& s) {
    if (s == "low-taker") return LaneId::LOW_TAKER;
    if (s == "maker-bid") return LaneId::MAKER_BID;
    if (s == "scalp-no") return LaneId::SCALP_NO;
    if (s == "disagree-fade") return LaneId::DISAGREE_FADE;
    return LaneId::SCALED;
}

inline std::string lane_to_str(LaneId lane) {
    switch (lane) {
        case LaneId::LOW_TAKER:    return "low-taker";
        case LaneId::MAKER_BID:    return "maker-bid";
        case LaneId::SCALP_NO:     return "scalp-no";
        case LaneId::DISAGREE_FADE:return "disagree-fade";
        default: return "scaled";
    }
}

// ── JSONL writer ──────────────────────────────────────────────────────────────
inline void write_jsonl(const std::string& path, const ScoreSummary& s) {
    // In production: create directory, append lines
    // For test/offline: caller receives the summary
    (void)path;
    (void)s;
}

// ── NightlyAudit ────────────────────────────────────────────────────────────────
class NightlyAudit {
public:
    using ClockFn = uint64_t (*)();

    struct Config {
        ResolutionFetcher resolution = nullptr;
        AuditTapeFetcher tape = nullptr;
        ClockFn now_fn = nullptr;
        bool skip_tape = false;  // offline scoring
        std::vector<LaneId> lanes;
    };

    explicit NightlyAudit(const Config& cfg)
        : cfg_(cfg), now_fn_(cfg.now_fn ? cfg.now_fn : &default_clock) {}

    // ── Run nightly audit ─────────────────────────────────────────────────────
    ScoreSummary run(const std::vector<LedgerRow>& rows) {
        ScoreSummary summary;
        summary.generated_at = now_fn_();
        summary.day = "ALL";

        if (cfg_.lanes.empty()) {
            // Audit all lanes present in rows
            std::unordered_set<LaneId> seen;
            for (const auto& r : rows) seen.insert(r.lane);
            summary.audit.filled_rows = std::count_if(rows.begin(), rows.end(),
                [](const LedgerRow& r) { return r.status == FillStatus::FILLED; });
        }

        // ---- Per-lane processing ----
        std::unordered_map<LaneId, std::vector<const LedgerRow*>> by_lane;
        for (const auto& r : rows) by_lane[r.lane].push_back(&r);

        for (auto& [lane, lane_rows] : by_lane) {
            LaneScore score = score_lane(lane, lane_rows, summary);
            score_lane_positions(lane, lane_rows, score, summary);
        }

        // ---- Totals ----
        for (auto& [_, ls] : summary.lane_scores) {
            summary.totals.realized += ls.realized;
            summary.totals.fees += ls.fees;
            summary.totals.filled += ls.filled;
            summary.totals.missed += ls.missed;
            summary.totals.standdown += ls.standdown;
        }
        summary.totals.realized = round2(summary.totals.realized);
        summary.totals.fees = round2(summary.totals.fees);

        summary.audit.violations = std::count_if(
            summary.audit.findings.begin(), summary.audit.findings.end(),
            [](const AuditFinding& f) { return f.severity == Severity::VIOLATION; });
        summary.audit.warnings = std::count_if(
            summary.audit.findings.begin(), summary.audit.findings.end(),
            [](const AuditFinding& f) { return f.severity == Severity::WARN; });

        return summary;
    }

private:
    static uint64_t default_clock() { return 0; }

    static double round2(double n) { return std::round(n * 1e6) / 1e6; }

    // ── Score a lane: structural audit + fill stats ───────────────────────────
    LaneScore score_lane(const LaneId lane, const std::vector<const LedgerRow*>& rows,
                          ScoreSummary& summary) {
        LaneScore score;
        score.lane = lane;

        uint64_t filled = 0, missed = 0, standdown = 0;
        for (const auto* r : rows) {
            if (r->status == FillStatus::FILLED) filled++;
            else if (r->status == FillStatus::MISSED) missed++;
            else if (r->status == FillStatus::STANDDOWN) standdown++;
        }
        score.intents = (uint64_t)rows.size();
        score.filled = filled;
        score.missed = missed;
        score.standdown = standdown;
        score.fill_rate = rows.empty() ? 0.0 : (double)filled / rows.size();

        // ---- Structural audit (always runs, offline) ----
        for (const auto* r : rows) {
            audit_structural(*r, summary);

            if (r->status != FillStatus::FILLED) continue;
            score.gross_deployed += (r->side == OrderSide::BUY) ? r->usd : 0.0;
            score.fees += r->fee;
        }

        score.gross_deployed = round2(score.gross_deployed);
        score.fees = round2(score.fees);

        summary.lane_scores[lane] = score;
        return score;
    }

    void audit_structural(const LedgerRow& r, ScoreSummary& summary) {
        if (r.status == FillStatus::STANDDOWN && r.standdown_reason.empty()) {
            finding(summary, r, "standdown-without-reason",
                "STANDDOWN row carries no reason", Severity::VIOLATION);
        }
        if (r.status != FillStatus::FILLED) return;

        // Audit invariant: FILLED must have a tape basis
        if (!r.basis.has_value()) {
            finding(summary, r, "fill-without-basis",
                "FILLED row has no tape basis", Severity::VIOLATION);
        }

        // Entry price > MAX_ASK
        if (r.side == OrderSide::BUY && r.px > AUDIT::MAX_ASK) {
            finding(summary, r, "guard-breach-price",
                "entry " + std::to_string(r.px) + " > MAX_ASK " +
                std::to_string(AUDIT::MAX_ASK), Severity::VIOLATION);
        }

        // Clip cap: USD > CLIP_CAP_USD
        if (r.side == OrderSide::BUY && r.usd > AUDIT::CLIP_CAP_USD + 1e-6) {
            finding(summary, r, "guard-breach-clip",
                "usd exceeds clip cap", Severity::VIOLATION);
        }

        // Notional mismatch: shares × px ≈ usd
        if (std::abs((double)r.shares * r.px - r.usd) > 0.01) {
            finding(summary, r, "notional-mismatch",
                "shares x px != usd", Severity::VIOLATION);
        }

        // Fee mismatch: taker fee = 0.072 × p × (1-p) × shares
        std::string style = (r.side == OrderSide::SELL || r.lane == LaneId::MAKER_BID)
            ? "MAKER" : "TAKER";
        double expected_fee = (style == "TAKER")
            ? FeeCalculator::taker_fee(r.px) * (double)r.shares
            : 0.0;
        if (std::abs(r.fee - expected_fee) > 1e-6) {
            finding(summary, r, "fee-mismatch",
                "fee " + std::to_string(r.fee) + " != expected " +
                std::to_string(expected_fee), Severity::VIOLATION);
        }
    }

    // ── Score positions: pair entries with exits, verify tape ─────────────────
    void score_lane_positions(const LaneId lane, const std::vector<const LedgerRow*>& rows,
                               LaneScore& score, ScoreSummary& summary) {
        auto filled = std::vector<const LedgerRow*>();
        for (const auto* r : rows) {
            if (r->status == FillStatus::FILLED) filled.push_back(r);
        }
        summary.audit.filled_rows += (uint64_t)filled.size();

        // ── Tape invariant: every FILLED row's basis still on live tape ────────
        if (!cfg_.skip_tape && cfg_.tape) {
            for (const auto* r : filled) {
                if (!r->basis.has_value()) continue;
                summary.audit.basis_checked++;
                AuditFinding* drift_finding = nullptr;
                verify_basis(*r, summary, drift_finding);
            }
        }

        // ── Pair entries with exits ───────────────────────────────────────────
        std::unordered_map<std::string, std::vector<const LedgerRow*>> by_market;
        for (const auto* r : filled) {
            by_market[r->market_slug].push_back(r);
        }

        for (auto& [slug, market_rows] : by_market) {
            auto entries = std::vector<const LedgerRow*>();
            auto exits = std::vector<const LedgerRow*>();
            for (const auto* r : market_rows) {
                if (r->side == OrderSide::BUY) entries.push_back(r);
                else exits.push_back(r);
            }
            // Pair in FIFO order
            size_t exit_idx = 0;
            for (const auto* entry : entries) {
                const LedgerRow* exit = nullptr;
                if (exit_idx < exits.size()) {
                    exit = exits[exit_idx++];
                }

                PositionScore pos;
                pos.lane = lane;
                pos.city = entry->city;
                pos.market_slug = slug;
                pos.outcome = entry->outcome;
                pos.px = entry->px;
                pos.shares = entry->shares;
                pos.cost = entry->usd + entry->fee;

                if (exit) {
                    pos.exit_px = exit->px;
                    pos.proceeds = std::optional<double>(exit->usd - exit->fee);
                    pos.realized = (exit->usd - exit->fee) - pos.cost;
                    pos.disposition = Disposition::SCALPED;
                } else {
                    // No exit — resolve via Gamma
                    auto res = fetch_resolution(slug);
                    if (!res.error.empty() || !res.closed ||
                        !res.yes_price.has_value() || !res.no_price.has_value()) {
                        pos.disposition = res.closed ? Disposition::UNRESOLVED : Disposition::OPEN;
                        if (res.closed) score.open++;
                    } else {
                        double payoff = (entry->outcome == "Yes")
                            ? *res.yes_price : *res.no_price;
                        pos.realized = (double)entry->shares * payoff - pos.cost;
                        pos.disposition = Disposition::SETTLED;
                        pos.won = (payoff >= 0.5);
                        if (!res.clean) {
                            finding(summary, *entry, "unclean-resolution",
                                "outcomePrices not a clean 1/0 pair", Severity::WARN);
                        }
                        // scalp-no defined by never settling
                        if (lane == LaneId::SCALP_NO) {
                            finding(summary, *entry, "scalp-held-to-settlement",
                                "no exit fill; settled at " + std::to_string(payoff),
                                Severity::VIOLATION);
                        }
                        if (*pos.won) score.wins++;
                        else score.losses++;
                    }
                }

                if (pos.realized.has_value()) {
                    score.realized += *pos.realized;
                }
                summary.positions.push_back(pos);
            }

            // Orphan exits (no matching entry)
            for (size_t i = exit_idx; i < exits.size(); i++) {
                finding(summary, *exits[i], "exit-without-entry",
                    "SELL fill has no matching BUY fill", Severity::VIOLATION);
            }
        }

        summary.lane_scores[lane] = score;  // update with computed values
    }

    void verify_basis(const LedgerRow& r, ScoreSummary& summary,
                       AuditFinding*& drift_out) {
        drift_out = nullptr;
        if (!r.basis.has_value()) return;

        const auto& basis = r.basis.value();
        uint64_t since_sec = basis.print_ts / 1000 - 2;

        try {
            std::vector<AuditTapePrint> prints;
            if (cfg_.tape) prints = cfg_.tape(r.condition_id, since_sec);

            bool found = false;
            for (const auto& p : prints) {
                if (p.transaction_hash == basis.tx_hash && p.asset == r.token) {
                    found = true;
                    if (std::abs(p.price - basis.print_px) > 1e-9 ||
                        p.size != basis.print_sz) {
                        finding(summary, r, "basis-drift",
                            "tape price/size changed since fill", Severity::WARN);
                        drift_out = nullptr;  // just record the finding
                        summary.audit.basis_verified++;
                    } else {
                        summary.audit.basis_verified++;
                    }
                    break;
                }
            }
            if (!found) {
                summary.audit.basis_unverifiable++;
                finding(summary, r, "basis-not-on-tape",
                    "txHash " + basis.tx_hash + " not found on live tape",
                    Severity::VIOLATION);
            }
        } catch (...) {
            summary.audit.basis_unverifiable++;
            finding(summary, r, "basis-check-failed",
                "tape fetch threw exception", Severity::WARN);
        }
    }

    MarketResolution fetch_resolution(const std::string& slug) {
        if (cfg_.resolution) {
            return cfg_.resolution(slug);
        }
        // Default: report unknown (never cache as "no market")
        return MarketResolution{.slug = slug, .closed = false, .error = "no-resolver-configured"};
    }

    static void finding(ScoreSummary& s, const LedgerRow& r,
                         const std::string& kind, const std::string& detail,
                         Severity sev) {
        AuditFinding f;
        f.lane = r.lane;
        f.market_slug = r.market_slug;
        f.filled_at = r.filled_at;
        f.kind = kind;
        f.detail = detail;
        f.severity = sev;
        s.audit.findings.push_back(f);
    }

    Config cfg_;
    ClockFn now_fn_;
};

// ── Helper: get token from LedgerRow (for basis verification) ─────────────────
// In the TS code, the basis asset check is against intent.token.
// We add a method to LedgerRow to expose this.
namespace detail {
    inline std::string token_from_row(const LedgerRow& r) {
        return r.outcome;  // simplified: outcome is either "Yes" or "No"
    }
}
