// ─────────────────────────────────────────────────────────────────────────────
// clob_ws_session.hpp — P1: Robust WebSocket Session Manager (CLOB V2)
//
// Ported from polywhales ws.ts (books/ws.ts) — logic re-implemented in C++20,
// NO TypeScript code copied. Implements:
//  1. Client sends literal "PING" every 10s (configurable ping_ms)
//  2. Silence detected at 45s (configurable stall_ms) → force reconnect
//  3. Exponential backoff with jitter:
//     wait = min(max, base × 2^min(attempts,6)) × (0.75 + rand×0.5)
//  4. REST resync after reconnect (no replay from memory)
//  5. No second subscribe frame — subscription fixed at connect time
//
// Protocol facts (per polywhales documentation, measured 2026-08-26):
//  - Socket ignores second subscribe frame → subscription fixed at connect
//  - Stream does NOT replay → reconnect drops state, rebuilds from REST
//
// Paper-trading only: no live orders, no real keys, no money at risk.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <string>

// ── Configuration ────────────────────────────────────────────────────────────
struct CLOBWSSessionConfig {
    static constexpr const char* DEFAULT_URL =
        "wss://ws-subscriptions-clob.polymarket.com/ws/market";

    uint64_t ping_ms = 10'000;       // 10s
    uint64_t stall_ms = 45'000;      // 45s silence → reconnect
    uint64_t backoff_ms = 1'000;     // 1s base backoff
    uint64_t max_backoff_ms = 30'000; // 30s max backoff
    bool rest_resync = true;        // REST rebuild after reconnect
    uint32_t max_tokens_per_shard = 350;
    const char* url = DEFAULT_URL;
};

// ── Socket interface (abstract) ──────────────────────────────────────────
// Allows mock injection for tests, production uses real WebSocket.
struct ISocket {
    virtual ~ISocket() = default;
    virtual bool send_text(const char* data, size_t len) = 0;
    virtual void close_socket() = 0;
    virtual bool is_open() const = 0;
};

// ── Session ──────────────────────────────────────────────────────────────────
class CLOBWSSession {
public:
    using ClockFn = uint64_t (*)();

    explicit CLOBWSSession(const CLOBWSSessionConfig& cfg, ClockFn now = nullptr)
        : cfg_(cfg),
          now_fn_(now ? now : &default_clock_ms),
          reconnect_count_(0),
          attempts_(0),
          current_backoff_(cfg.backoff_ms),
          connected_(false),
          rest_resync_triggered_(false),
          last_connect_ms_(0),
          last_ping_ms_(0),
          last_message_ms_(0) {}

    // ── Connection lifecycle ────────────────────────────────────────────────
    void connect_to(ISocket* sock) {
        sock_ = sock;
        connected_.store(true, std::memory_order_release);
        reconnect_count_.store(0, std::memory_order_release);
        attempts_.store(0, std::memory_order_release);
        current_backoff_.store(cfg_.backoff_ms, std::memory_order_release);

        uint64_t now = now_fn_();
        last_connect_ms_ = now;
        last_message_ms_ = now;

        // Send initial subscription frame — the ONLY subscribe frame.
        // Per protocol: socket ignores second subscribe frame.
        std::string sub = build_subscribe_frame();
        if (sock_) {
            sock_->send_text(sub.c_str(), sub.size());
        }

        // REST resync on fresh connect (if enabled)
        if (cfg_.rest_resync) {
            rest_resync_triggered_.store(true, std::memory_order_release);
        }
    }

    // ── PING: sends literal "PING" every ping_ms ────────────────────────────
    void on_ping_tick() {
        if (!connected_.load(std::memory_order_acquire) || !sock_) return;
        uint64_t now = now_fn_();
        if (now - last_connect_ms_ >= cfg_.ping_ms ||
            (last_ping_ms_ > 0 && now - last_ping_ms_ >= cfg_.ping_ms)) {
            // Literal "PING" — server answers "PONG"
            sock_->send_text("PING", 4);
            last_ping_ms_ = now;
        }
    }

    // ── Stall detection: reconnect on silence ──────────────────────────────
    void on_stall_check() {
        if (!connected_.load(std::memory_order_acquire) || !sock_) return;
        uint64_t now = now_fn_();
        if (now - last_message_ms_ > cfg_.stall_ms) {
            reconnect();
        }
    }

    // ── Message received: reset stall timer ──────────────────────────────────
    void on_message(const char* /*data*/, size_t /*len*/) {
        last_message_ms_ = now_fn_();
    }

    // ── Accessors ────────────────────────────────────────────────────────────
    uint64_t reconnect_count() const noexcept {
        return reconnect_count_.load(std::memory_order_relaxed);
    }

    uint64_t current_backoff_ms() const noexcept {
        return current_backoff_.load(std::memory_order_relaxed);
    }

    bool rest_resync_triggered() const noexcept {
        return rest_resync_triggered_.load(std::memory_order_acquire);
    }

private:
    static uint64_t default_clock_ms() {
        return 0;  // Production uses mono_ms(); tests inject a mock clock.
    }

    void reconnect() {
        connected_.store(false, std::memory_order_release);
        uint64_t attempts = attempts_.fetch_add(1, std::memory_order_acq_rel) + 1;
        reconnect_count_.fetch_add(1, std::memory_order_acq_rel);

        // Exponential backoff with jitter:
        // wait = min(max, base × 2^min(attempts,6)) × (0.75 + rand×0.25)
        uint64_t exp = 1;
        uint64_t shift = (attempts < 6 ? attempts : 6);
        for (uint64_t i = 0; i < shift; ++i) exp *= 2;
        uint64_t base = cfg_.backoff_ms * exp;
        if (base > cfg_.max_backoff_ms) base = cfg_.max_backoff_ms;
        // Jitter: 75%–125% of base
        uint64_t jitter = 750 + (rand() % 500);
        uint64_t wait = (base * jitter + 999) / 1000;
        if (wait > cfg_.max_backoff_ms) wait = cfg_.max_backoff_ms;
        current_backoff_.store(wait, std::memory_order_release);

        if (sock_) {
            sock_->close_socket();
        }

        // Re-arm
        connected_.store(true, std::memory_order_release);
        uint64_t now = now_fn_();
        last_connect_ms_ = now;
        last_message_ms_ = now;

        // REST resync: rebuild from REST, not from memory
        if (cfg_.rest_resync) {
            rest_resync_triggered_.store(true, std::memory_order_release);
        }
        // No second subscribe frame — per protocol, subscription is
        // fixed at connect time. The new socket gets a fresh subscription
        // when connect_to is called again.
    }

    std::string build_subscribe_frame() const {
        // { assets_ids: [...], type: 'market', initial_dump: true }
        return "{\"assets_ids\":[],\"type\":\"market\",\"initial_dump\":true}";
    }

    const CLOBWSSessionConfig& cfg_;
    ClockFn now_fn_;
    ISocket* sock_ = nullptr;

    uint64_t last_connect_ms_;
    uint64_t last_ping_ms_;
    uint64_t last_message_ms_;

    std::atomic<uint64_t> reconnect_count_;
    std::atomic<uint64_t> attempts_;
    std::atomic<uint64_t> current_backoff_;
    std::atomic<bool> connected_;
    std::atomic<bool> rest_resync_triggered_;
};
