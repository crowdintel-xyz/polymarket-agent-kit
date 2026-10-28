// ─────────────────────────────────────────────────────────────────────────────
// request_prioritizer.hpp — Phase 3: Priority-based request queuing
//
// A fixed-capacity, stack-allocated prioritizer with four priority bands
// (CRITICAL > HIGH > NORMAL > LOW).  Within each band requests are served
// FIFO.  No heap allocation — every queue is a std::array-backed ring buffer.
//
// Design choices:
//   • FixedRingBuffer stores uint32_t ids (the priority is implicit in which
//     ring the request lives).
//   • dequeue() returns the Priority of the removed request so callers know
//     what class of work was dispatched; use last_id() to retrieve the id.
//   • cancel(id) scans all four rings — O(maxQueue) but bounded and stack-only.
//
// TODO(P3-T3): integrate with order_gateway for pre-trade throttling.
// ─────────────────────────────────────────────────────────────────────────────
#ifndef REQUEST_PRIORITIZER_HPP
#define REQUEST_PRIORITIZER_HPP

#include <array>
#include <cstddef>
#include <cstdint>

// ── Priority bands ────────────────────────────────────────────────────────────
enum class Priority : uint8_t {
    CRITICAL = 0,  // cancel orders / emergency unwind
    HIGH     = 1,  // close-only mode, risk reduction
    NORMAL   = 2,  // new entry orders
    LOW      = 3,  // telemetry / queries
};

inline constexpr uint8_t PriorityCount = 4;

// ── Request descriptor (API-level, used by callers) ──────────────────────────
struct Request {
    Priority  priority;
    uint64_t  ts_ms;
    uint32_t  id;
};

// ── Fixed-capacity ring buffer (no heap) ──────────────────────────────────────
template <size_t N>
class FixedRingBuffer {
public:
    FixedRingBuffer() = default;

    bool push(uint32_t val) noexcept {
        if (full()) return false;
        buffer_[tail_] = val;
        tail_ = (tail_ + 1) % N;
        ++count_;
        return true;
    }

    bool pop(uint32_t& out) noexcept {
        if (empty()) return false;
        out = buffer_[head_];
        head_ = (head_ + 1) % N;
        --count_;
        return true;
    }

    // Remove the first occurrence of val.  Preserves FIFO order of
    // surviving elements.  Returns true if found and removed.
    bool remove(uint32_t val) noexcept {
        if (count_ == 0) return false;
        for (uint32_t i = 0; i < count_; ++i) {
            uint32_t idx = (head_ + i) % static_cast<uint32_t>(N);
            if (buffer_[idx] == val) {
                // Shift remaining elements left to close the gap.
                for (uint32_t j = i; j < count_ - 1; ++j) {
                    uint32_t src = (head_ + j + 1) % static_cast<uint32_t>(N);
                    uint32_t dst = (head_ + j) % static_cast<uint32_t>(N);
                    buffer_[dst] = buffer_[src];
                }
                --count_;
                tail_ = (head_ + count_) % static_cast<uint32_t>(N);
                return true;
            }
        }
        return false;
    }

    uint32_t size() const noexcept { return count_; }
    bool  empty() const noexcept { return count_ == 0; }
    bool  full()  const noexcept { return count_ >= N; }

private:
    std::array<uint32_t, N> buffer_{};
    uint32_t head_  = 0;
    uint32_t tail_  = 0;
    uint32_t count_ = 0;
};

// ── Request prioritizer ──────────────────────────────────────────────────────
class RequestPrioritizer {
public:
    // Each priority band gets a 64-slot ring buffer → 256 uint32_t = 1 KiB.
    static constexpr size_t MaxPerPriority = 64;

    RequestPrioritizer() noexcept = default;

    // Enqueue a request with the given priority and id.
    // Returns false if the target band is full (request is dropped).
    bool enqueue(Priority p, uint32_t id) noexcept;

    // Dequeue and return the Priority of the highest-priority pending request.
    // Returns Priority::LOW if the queue is empty (caller should check empty()
    // first).  Use last_id() to retrieve the dequeued request's id.
    Priority dequeue() noexcept;

    // Retrieve the id of the most recently dequeued request.
    uint32_t last_id() const noexcept { return last_dequeued_id_; }

    // Cancel a pending request by id.  Returns true if found and removed.
    bool cancel(uint32_t id) noexcept;

    // Number of pending requests at a given priority.
    uint32_t size(Priority p) const noexcept;

    // Total pending requests across all priorities.
    uint32_t size() const noexcept;

    // True when every band is empty.
    bool empty() const noexcept;

private:
    static constexpr std::array<Priority, PriorityCount> priority_order_ = {
        Priority::CRITICAL, Priority::HIGH, Priority::NORMAL, Priority::LOW
    };

    FixedRingBuffer<MaxPerPriority> queues_[PriorityCount];
    uint32_t last_dequeued_id_ = 0;
};

// ── Inline implementations ───────────────────────────────────────────────────

inline bool RequestPrioritizer::enqueue(Priority p, uint32_t id) noexcept {
    return queues_[static_cast<uint8_t>(p)].push(id);
}

inline Priority RequestPrioritizer::dequeue() noexcept {
    uint32_t id = 0;
    for (Priority p : priority_order_) {
        if (queues_[static_cast<uint8_t>(p)].pop(id)) {
            last_dequeued_id_ = id;
            return p;
        }
    }
    return Priority::LOW;  // empty sentinel
}

inline bool RequestPrioritizer::cancel(uint32_t id) noexcept {
    for (uint8_t i = 0; i < PriorityCount; ++i) {
        if (queues_[i].remove(id)) {
            return true;
        }
    }
    return false;
}

inline uint32_t RequestPrioritizer::size(Priority p) const noexcept {
    return queues_[static_cast<uint8_t>(p)].size();
}

inline uint32_t RequestPrioritizer::size() const noexcept {
    uint32_t total = 0;
    for (uint8_t i = 0; i < PriorityCount; ++i) {
        total += queues_[i].size();
    }
    return total;
}

inline bool RequestPrioritizer::empty() const noexcept {
    for (uint8_t i = 0; i < PriorityCount; ++i) {
        if (!queues_[i].empty()) return false;
    }
    return true;
}

#endif // REQUEST_PRIORITIZER_HPP
