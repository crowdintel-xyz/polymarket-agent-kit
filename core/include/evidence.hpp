#ifndef EVIDENCE_HPP
#define EVIDENCE_HPP

// ─────────────────────────────────────────────────────────────────────────────
// EvidenceEvent: one observation from an external information source feeding
// the Bayesian brain (P4).  Produced ONLY by cold-path ingress (HTTP pollers,
// sports-data adapters, macro feeds, NDJSON replay) and consumed ONLY by the
// hot loop, which folds each event into the posterior.  One cache-line POD —
// pushing never allocates and never blocks.
//
// Two flavors share the POD (kind):
//   COUNT  — Beta-Binomial evidence: `count_n` trials with `count_k` positive
//            outcomes (e.g. poll: 32 respondents, 22 YES).  Weighted by
//            source reliability at application time.
//   LR     — log-likelihood-ratio evidence: posterior_odds *= exp(lr_x1e6 *
//            weight / 1e6).  Used by macro/sports adapters that emit a score.
//
// Fixed-point x1e6 everywhere; venue strings never survive into the hot path.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <type_traits>

struct EvidenceEvent {
    enum class Kind : uint8_t { COUNT = 0, LR = 1 };

    Kind kind = Kind::COUNT;
    uint8_t outcome = 0;       // Dirichlet slot for multi-outcome markets
    uint8_t neg_risk = 0;      // 1 when the evidence targets the complement
    uint8_t reserved = 0;
    uint32_t source_id = 0;    // index into SourceReliability (producer-set)
    uint64_t timestamp_ns = 0;  // observation time (realtime epoch ns)
    uint32_t event_hash = 0;   // dedup hash of source-side event id
    uint32_t count_n = 0;      // COUNT: trials (x1, e.g. respondents polled)
    uint32_t count_k = 0;      // COUNT: positive outcomes (x1)
    int32_t lr_x1e6 = 0;       // LR: log-likelihood ratio, x1e6
};

static_assert(std::is_trivially_copyable_v<EvidenceEvent>);
static_assert(sizeof(EvidenceEvent) == 32, "half cache line per evidence event");

#endif  // EVIDENCE_HPP
