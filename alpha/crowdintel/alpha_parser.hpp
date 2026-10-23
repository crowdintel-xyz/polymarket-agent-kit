#ifndef ALPHA_PARSER_HPP
#define ALPHA_PARSER_HPP

#include "alpha_receiver.hpp"
#include "../../core/include/spsc_ring_buffer.hpp"  // LOW-3 FIX: explicit path from alpha/crowdintel/

#include <array>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>

// Cold-path validator/normalizer.  It deliberately accepts already-decoded
// fields so transport-specific code (HTTP, replay, tests) remains separate.
class AlphaParser {
public:
    AlphaParser(SPSC_RingBuffer<AlphaSignal>& queue,
                const char* expected_market,
                double max_q = 0.05,
                double min_confidence = 0.85)
        : queue_(queue), max_q_(max_q), min_confidence_(min_confidence) {
        if (!expected_market) return;
        expected_market_len_ = std::strlen(expected_market);
        if (expected_market_len_ == 0 ||
            expected_market_len_ >= expected_market_.size()) {
            expected_market_len_ = 0;
            return;
        }
        std::memcpy(expected_market_.data(), expected_market,
                    expected_market_len_ + 1);
        expected_market_hash_ =
            alpha_hash_bytes(expected_market, expected_market_len_);
    }

    bool process(const char* market, size_t market_len,
                 double p_win, double confidence, double q_value,
                 uint8_t direction_hint = 2,
                 uint64_t timestamp_ns = 0,
                 uint64_t signal_id = 0,
                 AlphaSignal::Type type = AlphaSignal::Type::WHALE_TRADE) {
        if (!market || market_len == 0 || expected_market_hash_ == 0 ||
            market_len != expected_market_len_ ||
            std::memcmp(market, expected_market_.data(), market_len) != 0)
            return false;
        const uint64_t market_hash = alpha_hash_bytes(market, market_len);
        if (!std::isfinite(p_win) || !std::isfinite(confidence) ||
            !std::isfinite(q_value)) return false;
        if (!(p_win > 0.0 && p_win < 1.0) ||
            !(confidence >= 0.0 && confidence <= 1.0) ||
            q_value < 0.0 || q_value > max_q_ ||
            confidence < min_confidence_ || direction_hint > 2) return false;

        const uint64_t now = realtime_ns();
        AlphaSignal s{};
        s.type = type;
        s.direction_hint = direction_hint;
        s.p_win = p_win;
        s.confidence = confidence;
        s.q_value = q_value;
        s.timestamp_ns = timestamp_ns ? timestamp_ns : now;
        s.market_hash = market_hash;
        s.signal_id = signal_id ? signal_id : fingerprint(s);
        return queue_.try_push(s);
    }

    bool process_webhook_payload(const char* market, double p_win,
                                 double confidence, double q_value,
                                 uint8_t direction_hint = 2) {
        return process(market, market ? std::strlen(market) : 0,
                       p_win, confidence, q_value, direction_hint);
    }

    uint64_t expected_market_hash() const noexcept { return expected_market_hash_; }

    static uint64_t realtime_ns() noexcept {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(
                std::chrono::system_clock::now().time_since_epoch()).count());
    }

private:
    static uint64_t fingerprint(const AlphaSignal& s) noexcept {
        // Hash normalized economic fields and event time.  This is an identity
        // fingerprint, not a cryptographic authenticator (HTTP auth handles that).
        return alpha_hash_bytes(reinterpret_cast<const char*>(&s),
                                offsetof(AlphaSignal, signal_id));
    }

    SPSC_RingBuffer<AlphaSignal>& queue_;
    std::array<char, 96> expected_market_{};
    size_t expected_market_len_ = 0;
    uint64_t expected_market_hash_ = 0;
    double max_q_;
    double min_confidence_;
};

#endif  // ALPHA_PARSER_HPP
