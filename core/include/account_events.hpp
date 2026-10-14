#ifndef ACCOUNT_EVENTS_HPP
#define ACCOUNT_EVENTS_HPP

// ─────────────────────────────────────────────────────────────────────────────
// AccountEvent: normalized user-channel (private WebSocket) account fact.
//
// One cache-line message produced ONLY by the private-WS feed thread (or the
// simulation venue) and consumed ONLY by the hot loop, which applies each
// event to the PositionTracker.  This keeps the tracker single-writer and
// satisfies the repository SPSC ownership invariant: nobody touches the
// tracker directly off the hot path; facts travel through the typed queue.
//
// All quantities are fixed-point x1e6.  Venue string identifiers (order id,
// trade id) are hashed to u64 so no variable-length parsing survives into
// the hot path.  A FILL whose order_hash matches a tracked open order also
// reduces that order's outstanding quantity.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <type_traits>

struct AccountEvent {
    enum class Type : uint8_t {
        FILL = 0,         // trade matched (partial fills arrive as several FILLs)
        FILL_MINED = 1,   // on-chain mined/confirmed duplicate of a FILL
        OPEN = 2,         // order accepted/resting on the venue book
        CANCEL = 3,       // order cancelled (partial cancel keeps remaining)
        REJECT = 4,       // order rejected by the venue
        FAILED = 5,       // trade failed/rolled back after a prior MATCHED
    };

    Type type = Type::FILL;
    uint8_t side = 0;       // K_SIDE_BUY / K_SIDE_SELL (user perspective)
    uint8_t asset = 0;      // 0 = primary token, 1 = hedge/complement token
    uint8_t reserved[5]{};
    uint64_t price = 0;         // fill/order price, x1e6
    uint64_t size = 0;          // quantity of THIS event, shares x1e6
    uint64_t remaining = 0;     // venue-reported outstanding qty (0 = unknown)
    uint64_t timestamp_ns = 0;  // venue event time, realtime epoch ns
    uint64_t market_hash = 0;   // FNV-1a of the configured market slug
    uint64_t order_hash = 0;    // FNV-1a of the venue order id (0 = unknown)
    uint64_t event_id = 0;      // FNV-1a of trade/event id for dedup (0 = n/a)
};

static_assert(std::is_trivially_copyable_v<AccountEvent>);
static_assert(sizeof(AccountEvent) == 64, "one cache line per account event");

#endif  // ACCOUNT_EVENTS_HPP
