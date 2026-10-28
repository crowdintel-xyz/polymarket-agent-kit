#ifndef PRESIGNED_POOL_HPP
#define PRESIGNED_POOL_HPP

// Consumable pre-signed ladder keyed by the current best bid/ask and eight
// conservative size buckets.  The hot path chooses the largest fresh bucket
// not exceeding its Kelly/risk limit; it never mutates a signed amount.
//
// Metadata and bodies are separate so a lookup touches compact cache lines.
// Three buffers plus per-buffer reader counts/writer locks prevent the producer
// from reusing memory while one or more readers copy a body. Every slot is
// claimed once with CAS, so
// salts/timestamps cannot be submitted twice.

#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <ctime>

#include "../crypto/eip712_signer.hpp"
#include "../crypto/fast_random.hpp"
#include "market_config.hpp"
#include "polymarket_order.hpp"

class PresignedOrderPool {
public:
    static constexpr size_t SIZE_BUCKETS = 8;
    static constexpr size_t SLOT_COUNT = SIZE_BUCKETS * 2;

    PresignedOrderPool(const MarketConfig& cfg,
                       const EIP712Signer& signer,
                       uint64_t ttl_ms)
        : cfg_(cfg), signer_(signer), ttl_ms_(ttl_ms) {}

    PresignedOrderPool(const PresignedOrderPool&) = delete;
    PresignedOrderPool& operator=(const PresignedOrderPool&) = delete;

    // Returns false only when both inactive buffers are temporarily protected.
    bool rebuild(uint64_t best_bid, uint64_t best_ask,
                 uint64_t target_shares, uint64_t tick_size) {
        const uint32_t current = active_.load(std::memory_order_acquire);
        int32_t next = -1;
        for (int32_t i = 0; i < 3; ++i) {
            if (i == static_cast<int32_t>(current)) continue;
            uint32_t expected = 0;
            if (access_[i].compare_exchange_strong(
                    expected, WRITER_LOCK, std::memory_order_acq_rel)) {
                next = i;
                break;
            }
        }
        if (next < 0) return false;

        Buffer& dst = buffers_[next];
        dst.count = 0;
        const uint64_t now = now_ms();
        const bool market_order = std::strcmp(cfg_.order_type, "FAK") == 0 ||
                                  std::strcmp(cfg_.order_type, "FOK") == 0;
        const uint64_t expiration = cfg_.wire_expiration(now / 1000ULL);

        for (uint8_t side = K_SIDE_BUY; side <= K_SIDE_SELL; ++side) {
            const uint64_t price = side == K_SIDE_BUY ? best_ask : best_bid;
            if (price == 0 || price >= 1000000) continue;
            uint64_t previous_size = 0;
            for (size_t bucket = 1; bucket <= SIZE_BUCKETS; ++bucket) {
                const uint64_t requested = static_cast<uint64_t>(
                    static_cast<crowd_uint128_t>(target_shares) * bucket /
                    SIZE_BUCKETS);
                uint64_t maker = 0, taker = 0, effective = 0;
                if (!compute_order_amounts(side, price, requested, tick_size,
                                           market_order, maker, taker, effective) ||
                    effective < cfg_.min_size_shares || effective == previous_size)
                    continue;
                previous_size = effective;

                OrderV2 order{};
                order.salt = rng_.next_salt();
                order.timestamp_ms = now;
                std::memcpy(order.maker, cfg_.maker, 20);
                std::memcpy(order.signer, cfg_.signer, 20);
                std::memcpy(order.token_id, cfg_.token_id_be, 32);
                order.maker_amount = maker;
                order.taker_amount = taker;
                order.side = side;
                order.signature_type = cfg_.signature_type;

                uint8_t signature[65];
                if (!signer_.sign_order(order, signature)) continue;

                const size_t n = dst.count;
                if (n >= SLOT_COUNT) break;
                if (!build_wire_body(order, signature, cfg_.token_id_dec,
                                     cfg_.maker_hex, cfg_.signer_hex,
                                     cfg_.owner_api_key, cfg_.order_type,
                                     dst.bodies[n], expiration))
                    continue;
                Meta& meta = dst.meta[n];
                meta.price = price;
                meta.tick_size = tick_size;
                meta.size = effective;
                meta.maker_amount = maker;
                meta.taker_amount = taker;
                meta.built_ms = now;
                meta.side = side;
                meta.consumed.store(0, std::memory_order_relaxed);
                ++dst.count;
            }
        }

        // Publish only after the complete buffer is visible. Unlocking before
        // the active index changes is safe: readers can only register against
        // the still-active old buffer until the release publication below.
        access_[next].store(0, std::memory_order_release);
        active_.store(static_cast<uint32_t>(next), std::memory_order_release);
        return true;
    }

    // Largest unconsumed bucket <= max_size.  Returns signed amounts so the
    // risk ledger reserves exactly what will be submitted.  `max_age_ms` is an
    // optional dynamic TTL cap from the volatility gate (P3): when non-zero
    // and tighter than the static pool TTL, it wins.  0 = static TTL only.
    bool acquire_at_most(uint8_t side, uint64_t price, uint64_t tick_size,
                         uint64_t max_size, WireBody& out, uint64_t& actual_size,
                         uint64_t& maker_amount, uint64_t& taker_amount,
                         uint64_t max_age_ms = 0) const {
        const uint64_t ttl_ms =
            max_age_ms != 0 && max_age_ms < ttl_ms_ ? max_age_ms : ttl_ms_;
        for (int retry = 0; retry < 8; ++retry) {
            const uint32_t index = active_.load(std::memory_order_acquire);
            uint32_t access = access_[index].load(std::memory_order_acquire);
            if ((access & WRITER_LOCK) != 0 || access == WRITER_LOCK - 1)
                continue;
            if (!access_[index].compare_exchange_weak(
                    access, access + 1, std::memory_order_acq_rel))
                continue;
            if (index != active_.load(std::memory_order_acquire)) {
                access_[index].fetch_sub(1, std::memory_order_release);
                continue;
            }

            const Buffer& src = buffers_[index];
            const uint64_t now = now_ms();
            size_t best = SLOT_COUNT;
            uint64_t best_size = 0;
            for (size_t i = 0; i < src.count; ++i) {
                const Meta& meta = src.meta[i];
                if (meta.side == side && meta.price == price &&
                    meta.tick_size == tick_size && meta.size <= max_size &&
                    meta.size > best_size &&
                    now >= meta.built_ms && now - meta.built_ms <= ttl_ms &&
                    meta.consumed.load(std::memory_order_relaxed) == 0) {
                    best = i;
                    best_size = meta.size;
                }
            }

            bool claimed = false;
            if (best != SLOT_COUNT) {
                uint8_t expected = 0;
                claimed = src.meta[best].consumed.compare_exchange_strong(
                    expected, 1, std::memory_order_acq_rel);
                if (claimed) {
                    const WireBody& body = src.bodies[best];
                    std::memcpy(out.buf, body.buf, body.len + 1);
                    out.len = body.len;
                    actual_size = src.meta[best].size;
                    maker_amount = src.meta[best].maker_amount;
                    taker_amount = src.meta[best].taker_amount;
                }
            }
            access_[index].fetch_sub(1, std::memory_order_release);
            if (claimed) return true;
        }
        return false;
    }

    size_t built_count() const {
        for (int retry = 0; retry < 8; ++retry) {
            const uint32_t index = active_.load(std::memory_order_acquire);
            uint32_t access = access_[index].load(std::memory_order_acquire);
            if ((access & WRITER_LOCK) != 0 || access == WRITER_LOCK - 1)
                continue;
            if (!access_[index].compare_exchange_weak(
                    access, access + 1, std::memory_order_acq_rel))
                continue;
            if (index != active_.load(std::memory_order_acquire)) {
                access_[index].fetch_sub(1, std::memory_order_release);
                continue;
            }
            const size_t result = buffers_[index].count;
            access_[index].fetch_sub(1, std::memory_order_release);
            return result;
        }
        return 0;
    }

    static uint64_t now_ms() {
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
               static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
    }

private:
    struct Meta {
        uint64_t price = 0;
        uint64_t tick_size = 0;
        uint64_t size = 0;
        uint64_t maker_amount = 0;
        uint64_t taker_amount = 0;
        uint64_t built_ms = 0;
        uint8_t side = 0;
        // HIGH-4 FIX (NEMESIS): Default to available (0). rebuild() sets
    // consumed=0 for active slots. Default 0 is safer — if rebuild()
    // is not called, slots are available (fail-open for pool use,
    // not a security issue since order signing still requires valid keys).
    mutable std::atomic<uint8_t> consumed{0};
    };

    struct Buffer {
        std::array<Meta, SLOT_COUNT> meta{};
        std::array<WireBody, SLOT_COUNT> bodies{};
        size_t count = 0;
    };

    static constexpr uint32_t WRITER_LOCK = 1U << 31;

    const MarketConfig& cfg_;
    const EIP712Signer& signer_;
    FastRandom rng_;
    uint64_t ttl_ms_;
    mutable std::array<std::atomic<uint32_t>, 3> access_{};
    std::atomic<uint32_t> active_{0};
    std::array<Buffer, 3> buffers_{};
};

#endif  // PRESIGNED_POOL_HPP
