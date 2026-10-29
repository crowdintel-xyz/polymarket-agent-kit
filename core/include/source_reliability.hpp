#ifndef SOURCE_RELIABILITY_HPP
#define SOURCE_RELIABILITY_HPP

// ─────────────────────────────────────────────────────────────────────────────
// SourceReliability: trust calibration for evidence producers (P4).
//
// A fixed table of at most MAX_SOURCES weights, one std::atomic<uint32_t>
// per source (weight x1e6, range [0, 1]).  The cold path recalibrates
// weights (config reload, recalibration file, adapter self-scoring) with a
// plain atomic store; the hot loop reads a weight with one acquire load —
// no seqlock, no contention, ~5 ns.
//
// Unknown/unregistered sources read as weight 0, which the engine treats as
// "below any reliability floor": their evidence is journalled and discarded.
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <cstdint>

class SourceReliability {
public:
    static constexpr size_t MAX_SOURCES = 64;

    // Cold path.  Out-of-range ids are ignored (never crash on bad config).
    void set_weight(uint32_t source_id, double reliability) noexcept {
        if (source_id >= MAX_SOURCES) return;
        if (reliability < 0.0) reliability = 0.0;
        if (reliability > 1.0) reliability = 1.0;
        weights_[source_id].store(
            static_cast<uint32_t>(reliability * 1000000.0 + 0.5),
            std::memory_order_release);
    }

    // Hot path: one atomic load.  x1e6 weight; 0 for unknown sources.
    uint32_t weight_x1e6(uint32_t source_id) const noexcept {
        if (source_id >= MAX_SOURCES) return 0;
        return weights_[source_id].load(std::memory_order_acquire);
    }

    double weight(uint32_t source_id) const noexcept {
        return static_cast<double>(weight_x1e6(source_id)) * 1e-6;
    }

private:
    std::atomic<uint32_t> weights_[MAX_SOURCES]{};
};

#endif  // SOURCE_RELIABILITY_HPP
