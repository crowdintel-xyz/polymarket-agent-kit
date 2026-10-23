#ifndef SPSC_RING_BUFFER_HPP
#define SPSC_RING_BUFFER_HPP

// Bounded wait-free single-producer/single-consumer queue.
//
// The queue owns real T objects rather than overlaying a byte array through a
// cached interior pointer.  This avoids strict-aliasing/lifetime UB and makes
// accidental copies impossible (a copied interior pointer used to reference
// the original queue).  Large queues should be allocated on the heap by their
// owner; the type itself performs no allocation.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstring>
#include <type_traits>

template <typename T, size_t Capacity = 4096>
class SPSC_RingBuffer {
    static_assert((Capacity & (Capacity - 1)) == 0,
                  "Capacity must be a power of two");
    static_assert(Capacity > 1, "Capacity must be > 1");
    static_assert(std::is_trivially_copyable_v<T>,
                  "T must be trivially copyable");

public:
    SPSC_RingBuffer() = default;
    SPSC_RingBuffer(const SPSC_RingBuffer&) = delete;
    SPSC_RingBuffer& operator=(const SPSC_RingBuffer&) = delete;
    SPSC_RingBuffer(SPSC_RingBuffer&&) = delete;
    SPSC_RingBuffer& operator=(SPSC_RingBuffer&&) = delete;

    // One slot remains unused so full and empty are distinguishable using only
    // the monotonically increasing counters.
    inline bool try_push(const T& item) noexcept {
        const size_t h = head_.load(std::memory_order_relaxed);
        if (h - tail_cache_ >= Capacity - 1) {
            tail_cache_ = tail_.load(std::memory_order_acquire);
            if (h - tail_cache_ >= Capacity - 1) return false;
        }
        std::memcpy(&slots_[h & MASK], &item, sizeof(T));
        head_.store(h + 1, std::memory_order_release);
        return true;
    }

    inline bool try_pop(T& out) noexcept {
        const size_t t = tail_.load(std::memory_order_relaxed);
        if (t == head_cache_) {
            head_cache_ = head_.load(std::memory_order_acquire);
            if (t == head_cache_) return false;
        }
        std::memcpy(&out, &slots_[t & MASK], sizeof(T));
        tail_.store(t + 1, std::memory_order_release);
        return true;
    }

    [[nodiscard]] bool empty() const noexcept {
        return tail_.load(std::memory_order_acquire) ==
               head_.load(std::memory_order_acquire);
    }

    static constexpr size_t capacity() noexcept { return Capacity; }

private:
    static constexpr size_t MASK = Capacity - 1;

    alignas(64) std::array<T, Capacity> slots_{};
    alignas(64) std::atomic<size_t> head_{0};
    alignas(64) std::atomic<size_t> tail_{0};
    size_t tail_cache_ = 0;  // producer-owned
    size_t head_cache_ = 0;  // consumer-owned
};

#endif  // SPSC_RING_BUFFER_HPP
