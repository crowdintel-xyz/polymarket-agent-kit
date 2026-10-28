// ─────────────────────────────────────────────────────────────────────────────
// test_ws_session.cpp — P1: Robust WebSocket session manager
//
// TDD: Test created before full implementation verification.
// Ported patterns from polywhales ws.ts (books/ws.ts):
//  1. Client sends literal "PING" every 10s
//  2. Silence detected at 45s → force reconnect
//  3. Exponential backoff with jitter
//  4. REST resync after reconnect (no replay from memory)
//  5. No second subscribe frame on reconnect
//
// NO TypeScript code copied — C++20 implementation from scratch.
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

#include "../../core/src/clob_ws_session.hpp"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  \u274C FAIL: %s\n", msg); g_failures++; } \
    else { printf("  \u2705 PASS: %s\n", msg); } \
} while(0)

// ── Mock clock ────────────────────────────────────────────────────────────────
static uint64_t mock_now_ms = 1'000'000;
static uint64_t mock_clock_fn() { return mock_now_ms; }
static void mock_set_time(uint64_t ms) { mock_now_ms = ms; }

// ── Mock socket ──────────────────────────────────────────────────────────────
struct MockSocket : public ISocket {
    std::vector<std::string> sent;
    std::atomic<bool> closed{false};

    bool send_text(const char* data, size_t len) override {
        sent.emplace_back(std::string(data, len));
        return true;
    }
    void close_socket() override { closed.store(true); }
    bool is_open() const override { return !closed.load(); }
};

int main() {
    printf("=== P1: Robust WebSocket Session Manager ===\n");

    // ── Test 1: PING every 10s ───────────────────────────────────────────────
    printf("\n--- Test 1: PING interval (10s default) ---\n");
    {
        CLOBWSSessionConfig cfg{};
        cfg.ping_ms = 10'000;
        cfg.stall_ms = 45'000;
        cfg.backoff_ms = 1'000;
        cfg.max_backoff_ms = 30'000;
        cfg.rest_resync = false;

        mock_set_time(1'000'000);
        CLOBWSSession session(cfg, &mock_clock_fn);
        MockSocket sock;
        session.connect_to(&sock);

        CHECK(!sock.sent.empty(), "subscribe frame sent on connect");

        // At +10s, ping tick should send PING
        mock_set_time(1'010'000);
        session.on_ping_tick();
        bool found_ping = false;
        for (size_t i = 1; i < sock.sent.size(); ++i) {  // skip initial subscribe
            if (sock.sent[i] == "PING") { found_ping = true; break; }
        }
        CHECK(found_ping, "PING sent after 10s interval");
    }

    // ── Test 2: Stall detection at 45s ───────────────────────────────────────
    printf("\n--- Test 2: Stall detection (45s silence → reconnect) ---\n");
    {
        CLOBWSSessionConfig cfg{};
        cfg.ping_ms = 10'000;
        cfg.stall_ms = 45'000;
        cfg.backoff_ms = 100;
        cfg.max_backoff_ms = 1'000;
        cfg.rest_resync = false;

        mock_set_time(1'000'000);
        CLOBWSSession session(cfg, &mock_clock_fn);
        MockSocket sock;
        session.connect_to(&sock);

        // 45s with no message → stall detected
        mock_set_time(1'045'001);
        session.on_stall_check();
        CHECK(session.reconnect_count() == 1, "stall at 45s triggers reconnect");
    }

    // ── Test 3: Exponential backoff ──────────────────────────────────────────
    printf("\n--- Test 3: Exponential backoff ---\n");
    {
        CLOBWSSessionConfig cfg{};
        cfg.ping_ms = 10'000;
        cfg.stall_ms = 45'000;
        cfg.backoff_ms = 1'000;
        cfg.max_backoff_ms = 30'000;
        cfg.rest_resync = false;

        mock_set_time(1'000'000);
        CLOBWSSession session(cfg, &mock_clock_fn);
        MockSocket sock;

        session.connect_to(&sock);

        // Trigger 5 reconnects via stall
        for (int i = 0; i < 5; i++) {
            mock_set_time(1'045'001 + i * 50'000);
            session.on_stall_check();
        }

        uint64_t backoff = session.current_backoff_ms();
        CHECK(backoff > 1'000, "backoff increases with reconnect attempts");
        CHECK(backoff <= 30'000, "backoff capped at max (30s)");
    }

    // ── Test 4: No second subscribe frame ─────────────────────────────────────
    printf("\n--- Test 4: No second subscribe on reconnect ---\n");
    {
        CLOBWSSessionConfig cfg{};
        cfg.ping_ms = 10'000;
        cfg.stall_ms = 45'000;
        cfg.backoff_ms = 100;
        cfg.max_backoff_ms = 1'000;
        cfg.rest_resync = false;

        mock_set_time(1'000'000);
        CLOBWSSession session(cfg, &mock_clock_fn);
        MockSocket sock;
        session.connect_to(&sock);

        size_t initial_sub_count = 0;
        for (const auto& s : sock.sent) {
            if (s.find("assets_ids") != std::string::npos) {
                initial_sub_count++;
            }
        }

        // Trigger reconnect
        mock_set_time(1'045'001);
        session.on_stall_check();

        size_t total_sub_count = 0;
        for (const auto& s : sock.sent) {
            if (s.find("assets_ids") != std::string::npos) {
                total_sub_count++;
            }
        }
        CHECK(total_sub_count == initial_sub_count,
              "no second subscribe frame after reconnect");
    }

    // ── Test 5: REST resync flag ──────────────────────────────────────────────
    printf("\n--- Test 5: rest_resync flag ---\n");
    {
        CLOBWSSessionConfig cfg{};
        cfg.rest_resync = true;
        cfg.stall_ms = 45'000;

        mock_set_time(1'000'000);
        CLOBWSSession session(cfg, &mock_clock_fn);
        MockSocket sock;
        session.connect_to(&sock);

        CHECK(session.rest_resync_triggered(), "REST resync on initial connect");

        // Trigger reconnect
        mock_set_time(1'045'001);
        session.on_stall_check();
        CHECK(session.rest_resync_triggered(), "REST resync triggered after reconnect");

        // With rest_resync=false
        CLOBWSSessionConfig cfg2{};
        cfg2.rest_resync = false;
        cfg2.stall_ms = 45'000;

        CLOBWSSession session2(cfg2, &mock_clock_fn);
        MockSocket sock2;
        session2.connect_to(&sock2);
        mock_set_time(1'045'001);
        session2.on_stall_check();
        CHECK(!session2.rest_resync_triggered(), "REST resync NOT triggered when disabled");
    }

    printf("\n  P1 test complete: %d failures\n", g_failures);
    return g_failures > 0 ? 1 : 0;
}
