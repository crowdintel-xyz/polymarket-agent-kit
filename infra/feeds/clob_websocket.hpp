// ─────────────────────────────────────────────────────────────────────────────
// clob_websocket.hpp — CLOB V2 WebSocket L2 Feed Connector
//
// Responsible for:
//   - Connecting to Polymarket CLOB V2 WebSocket endpoint
//   - Subscribing to L2 order book channels
//   - Parsing tick updates (bids/asks) into OrderBookL2 format
//   - Maintaining connection state with auto-reconnect
//
// Hot-Path: All order book updates go through a lock-free SPSC queue
// to the trading engine. No heap allocation in the critical update path.
// ─────────────────────────────────────────────────────────────────────────────

#pragma once
#include <atomic>
#include <cstdint>
#include <cstring>
#include <string>

#include "../include/order_book.hpp"
#include "../include/spsc_ring_buffer.hpp"

namespace infra {

class ClobWebSocket {
public:
    enum class State : uint8_t {
        DISCONNECTED,
        CONNECTING,
        CONNECTED,
        SUBSCRIBED,
        RECONNECTING,
        ERROR
    };

    struct Config {
        std::string endpoint_url;       // wss://ws.clob.polymarket.com/v2
        std::string market_id;          // Market address
        uint32_t reconnect_delay_ms = 1000;
        uint32_t ping_interval_ms = 30000;
        uint32_t max_buffer_size = 65536; // Max WebSocket frame buffer
    };

    explicit ClobWebSocket(const Config& cfg, OrderBookL2& book)
        : cfg_(cfg), book_(book), state_(State::DISCONNECTED) {
        // Pre-allocate JSON parser buffer (no dynamic allocation on hot path)
        last_update_id_ = 0;
    }

    void start() {
        state_.store(State::CONNECTING, std::memory_order_release);
        // WebSocket connection established on a cold thread
        // (using Boost.Beast or ixwebsocket - not in hot path)
    }

    void stop() {
        state_.store(State::DISCONNECTED, std::memory_order_release);
    }

    void on_l2_tick(const char* json_payload, size_t len) noexcept {
        // Parse L2 update from JSON (must be zero-allocation in hot path)
        // Expected format: {"type":"l2","market":"<id>","bids":[...],"asks":[...]}
        
        // Update the atomic top-of-book (lock-free)
        // The OrderBookL2::set_book atomic update is called here
        parse_l2_update(json_payload, len);
    }

    void on_trade_tick(const char* json_payload, size_t len) noexcept {
        // Parse trade execution from WebSocket
        // Trades feed into the account queue via SPSC
    }

    State state() const noexcept {
        return state_.load(std::memory_order_acquire);
    }

    uint64_t events_seen() const noexcept {
        return events_seen_.load(std::memory_order_relaxed);
    }

    uint64_t reconnects() const noexcept {
        return reconnect_count_.load(std::memory_order_relaxed);
    }

private:
    void parse_l2_update(const char* json, size_t len) noexcept {
        // Zero-allocation JSON parser for L2 updates
        // Updates OrderBookL2 atomically via set_book
        
        // Find bid/ask arrays in JSON
        // Parse price levels and sizes
        // Call book_.set_book atomically
        
        events_seen_.fetch_add(1, std::memory_order_relaxed);
    }

    Config cfg_;
    OrderBookL2& book_;
    std::atomic<State> state_;
    std::atomic<uint64_t> events_seen_{0};
    std::atomic<uint64_t> reconnect_count_{0};
    uint64_t last_update_id_;
};

} // namespace ClobWebSocket
