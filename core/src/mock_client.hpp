#ifndef MOCK_CLIENT_HPP
#define MOCK_CLIENT_HPP

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../include/account_events.hpp"
#include "../include/spsc_ring_buffer.hpp"
#include "market_config.hpp"
#include "polymarket_order.hpp"

class MockCLOBClient {
public:
    explicit MockCLOBClient(const MarketConfig& cfg)
        : token_id_dec_(cfg.token_id_dec),
          hedge_token_id_dec_(cfg.hedge_token_id_dec) {}

    // Paper-trading fill emission (P1/P4 paper mode).  When a queue is
    // attached, every accepted ("matched") order synthesizes the venue FILL
    // into it — the offline substitute for the private user channel, so
    // mock/paper sessions exercise tracker, brakes, and brain end to end.
    // The benchmark and unit fixtures never attach a queue: their wire cost
    // stays byte-for-byte identical to the pre-paper behavior.
    void attach_fill_queue(SPSC_RingBuffer<AccountEvent>* q) noexcept {
        fill_q_ = q;
    }

    SubmitResult submit(const WireBody& body) {
        const uint64_t seq = submissions_.fetch_add(
            1, std::memory_order_relaxed) + 1;
        SubmitResult result{};
        result.ok = true;
        result.final = true;
        result.http_code = 200;
        std::snprintf(result.status, sizeof(result.status), "matched");
        emit_fill(body, seq);
        return result;
    }

    // Cold-path REST surface used by the reconcile thread (mock equivalents).
    size_t rest_get(const char*, char*, size_t) { return 0; }
    bool cancel_all() {
        cancellations_.fetch_add(1, std::memory_order_relaxed);
        return true;
    }

    uint64_t submissions() const {
        return submissions_.load(std::memory_order_relaxed);
    }
    uint64_t cancellations() const {
        return cancellations_.load(std::memory_order_relaxed);
    }
    uint64_t paper_fills() const {
        return fills_.load(std::memory_order_relaxed);
    }

private:
    // Wire body fields (see polymarket_order.hpp):
    //   BUY : makerAmount = USDC cost, takerAmount = size (shares x1e6)
    //   SELL: makerAmount = size,       takerAmount = USDC cost
    static const char* scan_key(const char* within, const char* key) {
        const char* hit = std::strstr(within, key);
        return hit ? hit + std::strlen(key) : nullptr;
    }
    static bool scan_u64(const char* within, const char* key,
                         uint64_t& out) {
        const char* p = scan_key(within, key);
        if (!p) return false;
        uint64_t value = 0;
        size_t digits = 0;
        while (*p >= '0' && *p <= '9') {
            value = value * 10ULL + static_cast<uint64_t>(*p - '0');
            ++p;
            ++digits;
            if (digits > 20) return false;
        }
        if (digits == 0) return false;
        out = value;
        return true;
    }
    static bool scan_flag(const char* within, const char* key) {
        return std::strstr(within, key) != nullptr;
    }

    void emit_fill(const WireBody& body, uint64_t seq) {
        if (!fill_q_) return;
        const char* json = body.buf;
        const bool is_buy = scan_flag(json, "\"side\":\"BUY\"");
        const bool is_sell = scan_flag(json, "\"side\":\"SELL\"");
        if (!is_buy && !is_sell) return;
        uint64_t maker = 0, taker = 0;
        if (!scan_u64(json, "\"makerAmount\":\"", maker) ||
            !scan_u64(json, "\"takerAmount\":\"", taker))
            return;
        const uint64_t size = is_buy ? taker : maker;
        const uint64_t cost = is_buy ? maker : taker;
        if (size == 0 || cost == 0) return;
        AccountEvent ev{};
        ev.type = AccountEvent::Type::FILL;
        ev.side = is_buy ? 0 : 1;
        // Recover the execution price (paper precision is sub-tick).
        ev.price = static_cast<uint64_t>(
            (static_cast<crowd_uint128_t>(cost) * 1000000ULL) / size);
        ev.size = size;
        ev.event_id = 0xF1C5'0000ULL ^ seq;  // unique per submission
        ev.timestamp_ns = 0;                 // engine stamps on ingest
        fill_q_->try_push(ev);
        fills_.fetch_add(1, std::memory_order_relaxed);
        (void)token_id_dec_;
        (void)hedge_token_id_dec_;
    }

    const char* token_id_dec_;
    const char* hedge_token_id_dec_;
    SPSC_RingBuffer<AccountEvent>* fill_q_ = nullptr;
    std::atomic<uint64_t> submissions_{0};
    std::atomic<uint64_t> cancellations_{0};
    std::atomic<uint64_t> fills_{0};
};

#endif  // MOCK_CLIENT_HPP
