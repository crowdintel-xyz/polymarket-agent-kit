#ifndef JOURNAL_HPP
#define JOURNAL_HPP

// ─────────────────────────────────────────────────────────────────────────────
// JournalEvent: auditable record of every economically relevant hot-path
// decision.  The hot loop is the only producer; a cold thread is the only
// consumer and renders NDJSON.  Entries are pre-sized PODs: pushing an event
// never allocates and never blocks (drops are counted by the consumer).
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <type_traits>

struct JournalEvent {
    enum class Type : uint8_t {
        ACCOUNT_FILL = 0,
        ACCOUNT_REJECT = 1,
        RESERVATION_STALE_RELEASE = 2,
        STOP_LOSS_TRIGGERED = 3,
        HEDGE_TRIGGERED = 4,
        KILL_SWITCH = 5,
        RECONCILE_DRIFT = 6,
        VOLATILITY_ENTER = 7,
        VOLATILITY_EXIT = 8,
        STALE_PRICE_ABORT = 9,
        POOL_STALE_DROP = 10,
        BAYES_UPDATE = 11,
        BAYES_SIGNAL = 12,
        BAYES_LOW_RELIABILITY = 13,
        ORDER_ACCEPTED = 14,
        ORDER_FAILED = 15,
        DAY_RESET = 16,
    };

    Type type = Type::ORDER_FAILED;
    uint8_t reserved0[7]{};
    int64_t pnl = 0;            // signed x1e6 USD where applicable
    uint64_t aux0 = 0;          // per-type payload (e.g. price x1e6)
    uint64_t aux1 = 0;          // second payload (e.g. shares x1e6)
    uint64_t aux2 = 0;          // third payload (e.g. threshold / version)
    uint64_t mono_ns = 0;       // CLOCK_MONOTONIC event time
    uint64_t aux3 = 0;          // fourth payload (e.g. posterior x1e6)
    uint64_t reserved1 = 0;
};

static_assert(std::is_trivially_copyable_v<JournalEvent>);
static_assert(sizeof(JournalEvent) == 64, "one cache line per journal event");

#endif  // JOURNAL_HPP
