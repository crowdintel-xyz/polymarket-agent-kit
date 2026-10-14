#ifndef ALPHA_RECEIVER_HPP
#define ALPHA_RECEIVER_HPP

#include <cstddef>
#include <cstdint>
#include <type_traits>

// Cache-line-sized message passed from the single alpha-ingest producer to the
// execution consumer.  Market identity and deduplication keys are hashes so no
// variable-length/string parsing occurs on the hot path.
struct AlphaSignal {
    enum class Type : uint8_t {
        WHALE_TRADE = 0,
        FUNDING_CLUSTER = 1,
        MACHINE_DETECTION = 2,
        MARKET_EVENT = 3
    };

    Type type = Type::WHALE_TRADE;
    uint8_t direction_hint = 2;  // 0 BUY, 1 SELL, 2 choose by edge
    uint8_t reserved[14]{};
    double p_win = 0.0;
    double confidence = 0.0;
    double q_value = 1.0;
    uint64_t timestamp_ns = 0;   // source/event time, CLOCK_REALTIME epoch ns
    uint64_t market_hash = 0;    // stable FNV-1a of configured market slug
    uint64_t signal_id = 0;      // producer-provided id or payload fingerprint
};

static_assert(std::is_trivially_copyable_v<AlphaSignal>);
static_assert(sizeof(AlphaSignal) == 64, "one cache line per alpha signal");

inline uint64_t alpha_hash_bytes(const char* data, size_t len) noexcept {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint8_t>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

#endif  // ALPHA_RECEIVER_HPP
