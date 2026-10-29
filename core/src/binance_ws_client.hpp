// ─────────────────────────────────────────────────────────────────────────────
// binance_ws_client.hpp — Phase 1 full implementation (DATA-PIPELINE-AGENT)
//
// Persistent WebSocket client for Binance Spot combined streams. Runs on a cold
// thread. Produces MarketState snapshots to a SPSC ring buffer for hot-path
// drain.
//
// Streams (combined endpoint):
//   {symbol}@depth20@100ms  — top-20 order book every 100ms
//   {symbol}@trade          — trade events
//   {symbol}@kline_1m       — 1-minute kline/candle
//
// INVARIANTS:
//   - NEVER allocates on the hot path (ring buffer is fixed-size)
//   - NEVER blocks the hot path (lock-free SPSC push, drop-inherent policy)
//   - Uses CLOCK_MONOTONIC_RAW for all internal timestamps
//
// When CROWDINTEL_HAVE_NETWORK is not defined (offline/mock build), start()
// runs a deterministic synthetic feed so the downstream OFI pipeline can be
// exercised and benchmarked without network.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <thread>

#include "../include/spsc_ring_buffer.hpp"
#include "../include/bounded_json.hpp"
#include "../include/time_utils.hpp"
#include "../crypto/sha256_engine.hpp"
#include "json_fields.hpp"
#include "ofi_calculator.hpp"

// Forward-declare SSL types only when networking is available.
#if defined(CROWDINTEL_HAVE_NETWORK)
#include <openssl/ssl.h>
#include <openssl/crypto.h>
#include <sys/socket.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <arpa/inet.h>
#include <netdb.h>
#include <fcntl.h>
#include <sys/select.h>
#include <unistd.h>
#endif

// ── Source identifier for evidence integration ────────────────────────────────
// Registered with the Bayesian engine (P4) as source 0x02 with weight 0.85.
// The Binance L2/Trade feed is the highest-reliability external alpha source.
namespace SOURCE {
    inline constexpr uint32_t BINANCE_OFI = 0x02;
}

// ── Configuration (constexpr-compatible for compile-time tuning) ────────────
struct BinanceConfig {
    static constexpr const char* API_ENDPOINT =
        "wss://stream.binance.com:9443/stream";
    static constexpr uint32_t RING_CAPACITY = 4096;
    static constexpr uint64_t HEARTBEAT_INTERVAL_NS = 30'000'000'000ULL; // 30s
    static constexpr uint32_t MAX_RECONNECT_ATTEMPTS = 10;
    static constexpr uint64_t RECONNECT_BASE_DELAY_NS = 1'000'000'000ULL;  // 1s
    static constexpr uint64_t RECONNECT_MAX_DELAY_NS = 30'000'000'000ULL;  // 30s

    // Streams: configurable kline interval for 5m/15m strategy alignment.
    // Default uses 1m (for high-frequency signal); switch to 5m/15m for
    // direct candle aggregation and reduced client-side computation.
    const char* symbol = "btcusdt";
    const char* depth_stream = "@depth20@100ms";
    const char* trade_stream = "@trade";
    const char* kline_1m_stream = "@kline_1m";
    const char* kline_5m_stream = "@kline_5m";
    const char* kline_15m_stream = "@kline_15m";
    const char* kline_stream = "@kline_5m";  // ✅ PHASE-1: default to 5m

    // Strategy window: 300 = 5m, 900 = 15m
    uint32_t strategy_window_seconds = 300;

    // Hot-reload from infra/config/source_reliability.json
    const char* source_reliability_path = "infra/config/source_reliability.json";
    double binance_weight = 0.85;

    // Optional CircuitBreaker integration (nullptr = legacy reconnect)
    // When set, reconnect logic checks CB state: if OPEN, suspend reconnection.
    void* circuit_breaker = nullptr;  // CircuitBreaker*
};

// ── Public Interface ───────────────────────────────────────────────────────────
class BinanceWSClient {
public:
    explicit BinanceWSClient(const BinanceConfig& cfg)
        : cfg_(cfg),
          ofi_(OFIConfig{}),
          running_(false),
          connected_(false),
          reconnect_count_(0),
          events_produced_(0),
          last_heartbeat_ns_(0),
          active_fd_(-1) {}

    ~BinanceWSClient() { stop(); }

    // ── Thread lifecycle (cold path) ─────────────────────────────────────────
    // Spawns the cold reader thread.  On offline/mock builds, runs a
    // deterministic synthetic feed.  Returns immediately after spawning.
    void start();

    // Sets running_=false, signals thread exit, then joins.
    void stop();

    bool is_connected() const noexcept {
        return connected_.load(std::memory_order_acquire);
    }

    // ── Access to the output ring buffer (consumed by hot path) ─────────────
    using MarketStateQ = SPSC_RingBuffer<MarketState, BinanceConfig::RING_CAPACITY>;
    MarketStateQ& market_q() noexcept { return market_q_; }
    const MarketStateQ& market_q() const noexcept { return market_q_; }

    // ── Metrics (cold path only, never called from hot path) ────────────────
    uint64_t events_produced() const noexcept {
        return events_produced_.load(std::memory_order_relaxed);
    }
    uint64_t reconnect_count() const noexcept {
        return reconnect_count_.load(std::memory_order_relaxed);
    }

    // ── Mock injection (offline builds / tests) ──────────────────────────────
    // Push a synthetic market event so the OFI pipeline can be exercised
    // without a live WebSocket connection.
    void inject_event(double bid_px, double ask_px,
                      double bid_vol, double ask_vol,
                      bool is_trade, uint64_t now_ns) noexcept {
        process_event(bid_px, ask_px, bid_vol, ask_vol, is_trade, now_ns);
    }

private:
    // ── Timestamp helper (CLOCK_MONOTONIC_RAW) ───────────────────────────────
    static uint64_t mono_raw_ns() noexcept {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC_RAW, &ts);
        return static_cast<uint64_t>(ts.tv_sec) * 1'000'000'000ULL +
               static_cast<uint64_t>(ts.tv_nsec);
    }

    // ── Thread entry points ───────────────────────────────────────────────────
    void run_mock_loop();

#if defined(CROWDINTEL_HAVE_NETWORK)
    void run_network_loop();

    // ── WebSocket primitives ───────────────────────────────────────────────
    static int tcp_connect(const char* host, int port, int timeout_ms);
    static void set_socket_options(int fd);
    static SSL_CTX* ssl_context();
    static bool send_raw(SSL* ssl, int fd, const char* data, size_t len);
    static ssize_t receive_raw(SSL* ssl, int fd, char* data, size_t cap);
    static bool send_frame(SSL* ssl, int fd, uint8_t opcode,
                           const char* payload, size_t len, bool mask = true);
    static bool send_text(SSL* ssl, int fd, const char* payload) {
        return send_frame(ssl, fd, 0x1, payload, std::strlen(payload), true);
    }
    void cleanup(SSL* ssl, int fd);
    bool read_http_upgrade(SSL* ssl, int fd);
    bool read_frames(SSL* ssl, int fd);

    // ── Binance message parsing ────────────────────────────────────────────
    void handle_stream_message(const char* json, size_t len);
    void parse_depth_update(const char* json, size_t len);
    void parse_trade(const char* json, size_t len);
    void parse_kline(const char* json, size_t len);

    // Helper: extract the "data" sub-object from a combined stream message
    static bool extract_data_object(const char* json, size_t len,
                                    const char*& obj_begin,
                                    const char*& obj_end);

    // Helper: parse a single [price, size] level pair from a subarray string
    static bool parse_level_pair(const char* begin, const char* end,
                                 double& out_price, double& out_size);

    // ── Order book state (top-20, maintained by depth updates) ───────────────
    static constexpr size_t MAX_DEPTH = 20;
    struct Level {
        double price = 0.0;
        double size = 0.0;
    };
    std::array<Level, MAX_DEPTH> bids_{};
    std::array<Level, MAX_DEPTH> asks_{};
    size_t bid_count_ = 0;
    size_t ask_count_ = 0;

    // Receive / fragment buffers
    static constexpr size_t MAX_FRAME = 1u << 20;  // 1 MB
    std::array<char, MAX_FRAME> stream_buffer_{};
    size_t stream_buf_len_ = 0;
    std::array<char, MAX_FRAME> fragment_{};
    size_t fragment_len_ = 0;
    bool fragmenting_ = false;
#endif

    // ── OFI pipeline ─────────────────────────────────────────────────────────
    // Called by both mock and network paths after parsing a message.
    void process_event(double bid_px, double ask_px,
                       double bid_vol, double ask_vol,
                       bool is_trade, uint64_t now_ns) noexcept {
        ofi_.on_event(bid_px, ask_px, bid_vol, ask_vol, is_trade, now_ns);
        MarketState state{};
        if (ofi_.current_state(state)) {
            events_produced_.fetch_add(1, std::memory_order_relaxed);
            (void)market_q_.try_push(state);
        }
    }

    BinanceConfig cfg_;
    OFICalculator ofi_;

    MarketStateQ market_q_;

    std::atomic<bool> running_;
    std::atomic<bool> connected_;
    std::atomic<uint64_t> reconnect_count_;
    std::atomic<uint64_t> events_produced_;
    std::atomic<uint64_t> last_heartbeat_ns_;
    std::atomic<int> active_fd_;
    std::thread thread_;
};

// ── Implementation ─────────────────────────────────────────────────────────────

inline void BinanceWSClient::start() {
    bool expected = false;
    if (!running_.compare_exchange_strong(expected, true)) return;
#if defined(CROWDINTEL_HAVE_NETWORK)
    thread_ = std::thread([this] { run_network_loop(); });
#else
    thread_ = std::thread([this] { run_mock_loop(); });
#endif
}

inline void BinanceWSClient::stop() {
    running_.store(false, std::memory_order_release);
    const int fd = active_fd_.load(std::memory_order_acquire);
    if (fd >= 0) {
#if defined(CROWDINTEL_HAVE_NETWORK)
        ::shutdown(fd, SHUT_RDWR);
#endif
    }
    if (thread_.joinable()) thread_.join();
}

// ── Mock loop (offline / testing) ─────────────────────────────────────────────
// Generates a deterministic sawtooth market so OFI exercises every code path
// without touching the network.  Each 100 ms tick emits a depth update; every
// 3rd tick also emits a trade (alternating buyer/seller-initiated).
inline void BinanceWSClient::run_mock_loop() {
    uint64_t tick = 0;
    while (running_.load(std::memory_order_acquire)) {
        const uint64_t now = mono_raw_ns();
        const double base = 26500.0;
        const double mid = base + 50.0 * std::sin(static_cast<double>(tick) * 0.1);
        const double spread = 10.0;
        const double bid_px = mid - spread * 0.5;
        const double ask_px = mid + spread * 0.5;
        const double bid_vol = 10.0 + static_cast<double>(tick % 7);
        const double ask_vol = 8.0 + static_cast<double>(tick % 5);

        process_event(bid_px, ask_px, bid_vol, ask_vol, false, now);

        // Every 3rd tick: simulate a trade
        if (tick % 3 == 0) {
            const double trade_vol = 1.5 + static_cast<double>(tick % 3) * 0.5;
            // Buyer-initiated if tick is even, seller-initiated if odd
            const double signed_vol = ((tick / 3) % 2 == 0)
                ? trade_vol : -trade_vol;
            process_event(bid_px, ask_px, signed_vol, 0.0, true, now);
        }

        ++tick;
        std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }
    connected_.store(false, std::memory_order_release);
}

#if defined(CROWDINTEL_HAVE_NETWORK)

// ── Network loop (live Binance) ────────────────────────────────────────────────
inline void BinanceWSClient::run_network_loop() {
    // Build the combined stream path
    char path[256];
    const int path_len = std::snprintf(path, sizeof(path),
        "/stream?streams=%s%s/%s%s/%s%s",
        cfg_.symbol, cfg_.depth_stream,
        cfg_.symbol, cfg_.trade_stream,
        cfg_.symbol, cfg_.kline_stream);
    if (path_len <= 0 || static_cast<size_t>(path_len) >= sizeof(path)) return;

    // Parse the API endpoint URL for host/port
    char host[128]{}, url_host[128]{};
    int port = 9443;
    const char* url = BinanceConfig::API_ENDPOINT;
    if (std::strncmp(url, "wss://", 6) == 0) {
        const char* p = url + 6;
        const char* end = p;
        while (*end && *end != '/' && *end != ':') ++end;
        const size_t hl = static_cast<size_t>(end - p);
        if (hl < sizeof(url_host)) {
            std::memcpy(url_host, p, hl);
            url_host[hl] = '\0';
        }
        if (*end == ':') port = std::atoi(end + 1);
    }
    std::snprintf(host, sizeof(host), "%s", url_host);

    uint64_t delay_ns = BinanceConfig::RECONNECT_BASE_DELAY_NS;
    uint32_t attempt = 0;

    while (running_.load(std::memory_order_acquire)) {
        // Reset per-session state
        stream_buf_len_ = 0;
        fragment_len_ = 0;
        fragmenting_ = false;
        bid_count_ = 0;
        ask_count_ = 0;
        bids_.fill(Level{});
        asks_.fill(Level{});

        if (attempt > 0) {
            // ✅ PHASE-1: CircuitBreaker integration
            // If CircuitBreaker is OPEN, suspend reconnection until HALF_OPEN.
            if (cfg_.circuit_breaker) {
                auto* cb = static_cast<CircuitBreaker*>(cfg_.circuit_breaker);
                if (cb->state() == CircuitBreaker::State::OPEN) {
                    // Respect CB cooldown — sleep longer
                    std::this_thread::sleep_for(std::chrono::milliseconds(
                        CircuitBreaker::RECOVERY_CHECK_INTERVAL_MS));
                    continue;
                }
            }

            const uint64_t actual_delay = std::min(
                delay_ns, BinanceConfig::RECONNECT_MAX_DELAY_NS);
            const uint64_t secs = actual_delay / 1'000'000'000ULL;
            const uint64_t nsec = actual_delay % 1'000'000'000ULL;
            std::this_thread::sleep_for(
                std::chrono::seconds(static_cast<uint64_t>(secs)) +
                std::chrono::nanoseconds(static_cast<uint64_t>(nsec)));
            delay_ns = std::min(delay_ns * 2, BinanceConfig::RECONNECT_MAX_DELAY_NS);
        }

        const int fd = tcp_connect(host, port, 5000);
        if (fd < 0) {
            ++reconnect_count_;
            ++attempt;
            if (attempt >= BinanceConfig::MAX_RECONNECT_ATTEMPTS) {
                reconnect_count_.store(0, std::memory_order_release);
                attempt = 0;
                delay_ns = BinanceConfig::RECONNECT_BASE_DELAY_NS;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            continue;
        }
        active_fd_.store(fd, std::memory_order_release);
        set_socket_options(fd);

        SSL_CTX* context = ssl_context();
        SSL* ssl = nullptr;
        if (context) {
            ssl = SSL_new(context);
            if (ssl) {
                if (SSL_set_fd(ssl, fd) != 1 ||
                    SSL_set_tlsext_host_name(ssl, host) != 1 ||
                    SSL_set1_host(ssl, host) != 1) {
                    SSL_free(ssl);
                    ssl = nullptr;
                }
            }
        }

        if (!ssl || SSL_connect(ssl) != 1 ||
            SSL_get_verify_result(ssl) != X509_V_OK) {
            cleanup(ssl, fd);
            ++reconnect_count_;
            ++attempt;
            if (attempt >= BinanceConfig::MAX_RECONNECT_ATTEMPTS) {
                reconnect_count_.store(0, std::memory_order_release);
                attempt = 0;
                delay_ns = BinanceConfig::RECONNECT_BASE_DELAY_NS;
                std::this_thread::sleep_for(std::chrono::seconds(1));
            }
            continue;
        }

        // WebSocket handshake
        char ws_key[32];
        {
            // Generate Sec-WebSocket-Key (16 random bytes → base64)
            static std::atomic<uint64_t> rng_seed{0x853c'1001'4242'8179ULL};
            uint64_t seed = rng_seed.fetch_add(0x9e3779b97f4a7c15ULL,
                                               std::memory_order_relaxed);
            uint8_t key_raw[16];
            for (int i = 0; i < 8; ++i) {
                seed = seed * 6364136223846793005ULL + 1442695040888963407ULL;
                uint64_t w = seed;
                std::memcpy(key_raw + i * 8, &w, 8);
            }
            base64_encode(key_raw, 16, ws_key);
        }

        char request[512];
        const int req_len = std::snprintf(request, sizeof(request),
            "GET %s HTTP/1.1\r\n"
            "Host: %s:%d\r\n"
            "Upgrade: websocket\r\n"
            "Connection: Upgrade\r\n"
            "Sec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n",
            path, host, port, ws_key);
        if (req_len <= 0 || !send_raw(ssl, fd, request,
                                      static_cast<size_t>(req_len))) {
            cleanup(ssl, fd);
            ++reconnect_count_;
            ++attempt;
            continue;
        }

        if (!read_http_upgrade(ssl, fd)) {
            cleanup(ssl, fd);
            ++reconnect_count_;
            ++attempt;
            continue;
        }

        connected_.store(true, std::memory_order_release);
        last_heartbeat_ns_.store(mono_raw_ns(), std::memory_order_release);
        // Reset reconnect counter on successful connect
        reconnect_count_.store(0, std::memory_order_release);
        attempt = 0;
        delay_ns = BinanceConfig::RECONNECT_BASE_DELAY_NS;

        // Read frames until error or stop
        const bool clean = read_frames(ssl, fd);
        connected_.store(false, std::memory_order_release);
        cleanup(ssl, fd);

        if (!running_.load(std::memory_order_acquire)) break;
        if (!clean) ++reconnect_count_;
    }

    connected_.store(false, std::memory_order_release);
}

// ── TCP connect (non-blocking with timeout) ───────────────────────────────────
inline int BinanceWSClient::tcp_connect(const char* host, int port,
                                        int timeout_ms) {
    char service[8];
    std::snprintf(service, sizeof(service), "%d", port);
    addrinfo hints{};
    hints.ai_family = AF_INET;  // prefer IPv4 for latency
    hints.ai_socktype = SOCK_STREAM;
    addrinfo* addresses = nullptr;
    if (::getaddrinfo(host, service, &hints, &addresses) != 0) return -1;
    int connected_fd = -1;
    for (addrinfo* address = addresses; address; address = address->ai_next) {
        const int fd = ::socket(address->ai_family, address->ai_socktype,
                                address->ai_protocol);
        if (fd < 0) continue;
        const int flags = ::fcntl(fd, F_GETFL, 0);
        ::fcntl(fd, F_SETFL, flags | O_NONBLOCK);
        int rc = ::connect(fd, address->ai_addr, address->ai_addrlen);
        if (rc != 0 && errno == EINPROGRESS) {
            fd_set writes;
            FD_ZERO(&writes); FD_SET(fd, &writes);
            timeval timeout{timeout_ms / 1000, (timeout_ms % 1000) * 1000};
            rc = ::select(fd + 1, nullptr, &writes, nullptr, &timeout);
            if (rc > 0) {
                int error = 0; socklen_t error_len = sizeof(error);
                ::getsockopt(fd, SOL_SOCKET, SO_ERROR, &error, &error_len);
                rc = error == 0 ? 0 : -1;
            } else rc = -1;
        }
        if (rc == 0) {
            ::fcntl(fd, F_SETFL, flags);
            connected_fd = fd;
            break;
        }
        ::close(fd);
    }
    ::freeaddrinfo(addresses);
    return connected_fd;
}

inline void BinanceWSClient::set_socket_options(int fd) {
    int one = 1;
    ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
    ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
    timeval timeout{3, 0};
    ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
    ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
}

inline SSL_CTX* BinanceWSClient::ssl_context() {
    static SSL_CTX* context = [] {
        SSL_CTX* value = SSL_CTX_new(TLS_client_method());
        if (!value) return static_cast<SSL_CTX*>(nullptr);
        SSL_CTX_set_verify(value, SSL_VERIFY_PEER, nullptr);
        SSL_CTX_set_options(value, SSL_OP_NO_COMPRESSION);
        if (SSL_CTX_set_min_proto_version(value, TLS1_2_VERSION) != 1 ||
            SSL_CTX_set_default_verify_paths(value) != 1) {
            SSL_CTX_free(value);
            return static_cast<SSL_CTX*>(nullptr);
        }
        return value;
    }();
    return context;
}

inline bool BinanceWSClient::send_raw(SSL* ssl, int fd,
                                      const char* data, size_t len) {
    size_t offset = 0;
    while (offset < len) {
        const ssize_t n = ssl
            ? SSL_write(ssl, data + offset, static_cast<int>(len - offset))
            : ::send(fd, data + offset, len - offset, MSG_NOSIGNAL);
        if (n <= 0) return false;
        offset += static_cast<size_t>(n);
    }
    return true;
}

inline ssize_t BinanceWSClient::receive_raw(SSL* ssl, int fd,
                                            char* data, size_t cap) {
    if (ssl)
        return SSL_read(ssl, data, static_cast<int>(cap));
    return ::recv(fd, data, cap, 0);
}

// ── WebSocket frame writer (RFC 6455, client-masked) ──────────────────────────
inline bool BinanceWSClient::send_frame(SSL* ssl, int fd, uint8_t opcode,
                                        const char* payload, size_t len,
                                        bool mask) {
    if (len > MAX_FRAME) return false;
    uint8_t header[14];
    size_t header_len = 0;
    // FIN bit set (single frame)
    header[header_len++] = static_cast<uint8_t>(0x80U | opcode);

    // Payload length
    uint8_t mask_and_len = 0;
    if (mask) mask_and_len |= 0x80U;  // Set mask bit

    size_t payload_start = header_len;
    if (len < 126) {
        header[header_len++] = static_cast<uint8_t>(mask_and_len | len);
    } else if (len <= 0xFFFF) {
        header[header_len++] = static_cast<uint8_t>(mask_and_len | 126U);
        header[header_len++] = static_cast<uint8_t>(len >> 8);
        header[header_len++] = static_cast<uint8_t>(len);
    } else {
        header[header_len++] = static_cast<uint8_t>(mask_and_len | 127U);
        for (int i = 7; i >= 0; --i)
            header[header_len++] = static_cast<uint8_t>(len >> (i * 8));
    }

    // Masking key (client → server frames must be masked)
    uint8_t mask_key[4] = {0x12, 0x34, 0x56, 0x78};
    if (mask) {
        // Use a simple PRNG derived from address + time for the mask key
        static uint64_t mask_state = 0x9E3779B97F4A7C15ULL;
        mask_state = mask_state * 6364136223846793005ULL + 1442695040888963407ULL;
        std::memcpy(mask_key, &mask_state, 4);
        std::memcpy(header + header_len, mask_key, 4);
        header_len += 4;
    }

    if (!send_raw(ssl, fd,
                  reinterpret_cast<const char*>(header), header_len))
        return false;

    // Mask the payload and send
    if (len > 0) {
        // Copy payload into a buffer to avoid modifying the source
        // and mask it
        std::array<char, MAX_FRAME> masked{};
        for (size_t i = 0; i < len; ++i)
            masked[i] = static_cast<char>(
                payload[i] ^ mask_key[i & 3]);
        if (!send_raw(ssl, fd, masked.data(), len))
            return false;
    }

    (void)payload_start; // unused; kept for clarity
    return true;
}

inline void BinanceWSClient::cleanup(SSL* ssl, int fd) {
    active_fd_.store(-1, std::memory_order_release);
    if (ssl) SSL_free(ssl);
    if (fd >= 0) ::close(fd);
}

// ── HTTP upgrade response validation ──────────────────────────────────────────
inline bool BinanceWSClient::read_http_upgrade(SSL* ssl, int fd) {
    char header[4096];
    size_t length = 0;
    while (length + 1 < sizeof(header)) {
        const ssize_t n = receive_raw(ssl, fd, header + length, 1);
        if (n <= 0) return false;
        length += static_cast<size_t>(n);
        header[length] = '\0';
        if (length >= 4 && std::memcmp(header + length - 4, "\r\n\r\n", 4) == 0) {
            return length >= 12 &&
                   std::strncmp(header, "HTTP/1.1 101", 12) == 0;
        }
    }
    return false;
}

// ── WebSocket frame reader ────────────────────────────────────────────────────
inline bool BinanceWSClient::read_frames(SSL* ssl, int fd) {
    while (running_.load(std::memory_order_acquire)) {
        // Heartbeat: send PING (text "PING") every 30s
        const uint64_t now_ns = mono_raw_ns();
        const uint64_t last_hb = last_heartbeat_ns_.load(std::memory_order_acquire);
        if (now_ns - last_hb >= BinanceConfig::HEARTBEAT_INTERVAL_NS) {
            if (!send_text(ssl, fd, "PING")) return false;
            last_heartbeat_ns_.store(now_ns, std::memory_order_release);
        }

        fd_set reads;
        FD_ZERO(&reads); FD_SET(fd, &reads);
        timeval timeout{0, 200000};  // 200ms poll
        const int ready = ::select(fd + 1, &reads, nullptr, nullptr, &timeout);
        if (!running_.load()) return true;
        if (ready < 0) { if (errno == EINTR) continue; return false; }
        if (ready == 0) continue;

        const ssize_t n = receive_raw(ssl, fd,
                                      stream_buffer_.data() + stream_buf_len_,
                                      MAX_FRAME - stream_buf_len_);
        if (n <= 0) return false;
        stream_buf_len_ += static_cast<size_t>(n);

        // Parse WebSocket frames from the buffer
        size_t offset = 0;
        while (offset + 2 <= stream_buf_len_) {
            const uint8_t b0 = static_cast<uint8_t>(stream_buffer_[offset]);
            const uint8_t b1 = static_cast<uint8_t>(stream_buffer_[offset + 1]);
            const bool fin = (b0 & 0x80U) != 0;
            const uint8_t opcode = b0 & 0x0FU;
            const bool masked = (b1 & 0x80U) != 0;
            uint64_t payload_len = b1 & 0x7FU;
            size_t header_len = 2;

            if (payload_len == 126) {
                if (offset + 4 > stream_buf_len_) break;
                payload_len = static_cast<uint8_t>(stream_buffer_[offset + 2]) * 256ULL +
                              static_cast<uint8_t>(stream_buffer_[offset + 3]);
                header_len = 4;
            } else if (payload_len == 127) {
                if (offset + 10 > stream_buf_len_) break;
                payload_len = 0;
                for (int i = 0; i < 8; ++i)
                    payload_len = (payload_len << 8) |
                        static_cast<uint8_t>(stream_buffer_[offset + 2 + i]);
                header_len = 10;
            }

            // Server-to-client frames must NOT be masked per RFC 6455 §5.3
            if (masked) return false;

            if (offset + header_len + payload_len > stream_buf_len_) break;

            const char* payload = stream_buffer_.data() + offset + header_len;

            // Handle control frames
            if (opcode == 0x8) return false;  // close
            if (opcode == 0x9) continue;      // ping → (we could auto-pong but
                                              // Binance sends application pings)
            if (opcode == 0xA) continue;      // pong

            if (opcode == 0x1 && fin) {
                // Complete text frame
                if (payload_len > MAX_FRAME) {
                    offset += header_len + payload_len;
                    continue;
                }
                char buf[4096];
                size_t copy_len = std::min(payload_len, sizeof(buf) - 1);
                std::memcpy(buf, payload, copy_len);
                buf[copy_len] = '\0';
                handle_stream_message(buf, copy_len);
            } else if (opcode == 0x1 && !fin) {
                // Fragment start
                if (payload_len > fragment_.size()) {
                    offset += header_len + payload_len;
                    continue;
                }
                fragmenting_ = true;
                fragment_len_ = 0;
                std::memcpy(fragment_.data(), payload, payload_len);
                fragment_len_ = payload_len;
            } else if (opcode == 0x0 && fragmenting_) {
                // Continuation frame
                if (fragment_len_ + payload_len > fragment_.size()) {
                    fragmenting_ = false;
                    offset += header_len + payload_len;
                    continue;
                }
                std::memcpy(fragment_.data() + fragment_len_,
                            payload, payload_len);
                fragment_len_ += payload_len;
                if (fin) {
                    fragment_[fragment_len_] = '\0';
                    handle_stream_message(fragment_.data(), fragment_len_);
                    fragmenting_ = false;
                    fragment_len_ = 0;
                }
            }

            offset += header_len + static_cast<size_t>(payload_len);
        }

        // Compact buffer
        if (offset) {
            std::memmove(stream_buffer_.data(),
                         stream_buffer_.data() + offset,
                         stream_buf_len_ - offset);
            stream_buf_len_ -= offset;
        }
    }
    return true;
}

// ── Binance message dispatch ──────────────────────────────────────────────────
// Combined stream format: {"stream":"btcusdt@depth","data":{...}}
inline bool BinanceWSClient::extract_data_object(const char* json, size_t len,
                                                 const char*& obj_begin,
                                                 const char*& obj_end) {
    const char* hit = nullptr;
    if (json_fields::key_occurrences(json, json + len, "data", &hit) != 1)
        return false;
    // Skip whitespace and ':'
    while (hit < json + len && (*hit == ' ' || *hit == ':' ||
           *hit == '\t' || *hit == '\r' || *hit == '\n')) ++hit;
    if (hit == json + len || *hit != '{') return false;
    obj_begin = hit;
    obj_end = json_fields::find_matching(hit, json + len, '{', '}');
    if (!obj_end) return false;
    obj_end++;  // include closing brace
    return true;
}

inline void BinanceWSClient::handle_stream_message(const char* json, size_t len) {
    if (!bounded_json::valid_document(json, len)) return;

    char stream_name[64]{};
    if (!json_fields::extract_string(json, json + len, "stream",
                                     stream_name, sizeof(stream_name))) return;

    const char* data_begin = nullptr;
    const char* data_end = nullptr;
    if (!extract_data_object(json, len, data_begin, data_end)) return;

    const size_t data_len = static_cast<size_t>(data_end - data_begin);

    // Route by stream suffix
    if (std::strstr(stream_name, "@depth") != nullptr) {
        parse_depth_update(data_begin, data_len);
    } else if (std::strstr(stream_name, "@trade") != nullptr) {
        parse_trade(data_begin, data_len);
    } else if (std::strstr(stream_name, "@kline") != nullptr) {
        parse_kline(data_begin, data_len);
    }
}

inline bool BinanceWSClient::parse_level_pair(const char* begin,
                                              const char* end,
                                              double& out_price,
                                              double& out_size) {
    // Binance level format: ["46312.50","1.50000000"]
    char price_text[32]{}, size_text[32]{};
    const char* p = begin;
    int count = 0;
    while (count < 2 && p < end) {
        p = json_fields::find_char(p, end, '"');
        if (!p) break;
        const char* end_quote = json_fields::find_char(p + 1, end, '"');
        if (!end_quote) break;
        const size_t n = static_cast<size_t>(end_quote - p - 1);
        if (count == 0) {
            if (n < sizeof(price_text)) {
                std::memcpy(price_text, p + 1, n);
                price_text[n] = '\0';
            }
        } else {
            if (n < sizeof(size_text)) {
                std::memcpy(size_text, p + 1, n);
                size_text[n] = '\0';
            }
        }
        ++count;
        p = end_quote + 1;
    }
    if (count < 2) return false;
    out_price = std::strtod(price_text, nullptr);
    out_size = std::strtod(size_text, nullptr);
    return out_price > 0.0 && out_size >= 0.0;
}

inline void BinanceWSClient::parse_depth_update(const char* json, size_t len) {
    if (!bounded_json::valid_document(json, len)) return;

    const char* bids_begin = nullptr;
    const char* bids_end = nullptr;
    const char* asks_begin = nullptr;
    const char* asks_end = nullptr;

    // Support both Binance WebSocket spec ("b"/"a") and verbose ("bids"/"asks")
    if (!json_fields::find_array(json, json + len, "bids",
                                 bids_begin, bids_end)) {
        if (!json_fields::find_array(json, json + len, "b",
                                     bids_begin, bids_end)) return;
    }
    if (!json_fields::find_array(json, json + len, "asks",
                                 asks_begin, asks_end)) {
        if (!json_fields::find_array(json, json + len, "a",
                                     asks_begin, asks_end)) return;
    }

    // Parse levels from bids array
    bid_count_ = 0;
    const char* cursor = bids_begin + 1;  // skip '['
    while (bid_count_ < MAX_DEPTH && cursor < bids_end) {
        const char* open_bracket =
            json_fields::find_char(cursor, bids_end, '[');
        if (!open_bracket) break;
        const char* close_bracket =
            json_fields::find_matching(open_bracket, bids_end, '[', ']');
        if (!close_bracket) break;
        double price = 0.0, size = 0.0;
        if (parse_level_pair(open_bracket + 1, close_bracket, price, size)) {
            bids_[bid_count_].price = price;
            bids_[bid_count_].size = size;
            ++bid_count_;
        }
        cursor = close_bracket + 1;
    }

    // Parse levels from asks array
    ask_count_ = 0;
    cursor = asks_begin + 1;
    while (ask_count_ < MAX_DEPTH && cursor < asks_end) {
        const char* open_bracket =
            json_fields::find_char(cursor, asks_end, '[');
        if (!open_bracket) break;
        const char* close_bracket =
            json_fields::find_matching(open_bracket, asks_end, '[', ']');
        if (!close_bracket) break;
        double price = 0.0, size = 0.0;
        if (parse_level_pair(open_bracket + 1, close_bracket, price, size)) {
            asks_[ask_count_].price = price;
            asks_[ask_count_].size = size;
            ++ask_count_;
        }
        cursor = close_bracket + 1;
    }

    // Emit a MarketState if we have a top-of-book
    if (bid_count_ > 0 && ask_count_ > 0 &&
        bids_[0].price > 0.0 && asks_[0].price > 0.0 &&
        bids_[0].price < asks_[0].price) {
        const uint64_t now = mono_raw_ns();
        process_event(bids_[0].price, asks_[0].price,
                      bids_[0].size, asks_[0].size, false, now);
    }
}

inline void BinanceWSClient::parse_trade(const char* json, size_t len) {
    if (!bounded_json::valid_document(json, len)) return;

    char price_text[32]{}, size_text[32]{};
    if (!json_fields::extract_string(json, json + len, "p",
                                     price_text, sizeof(price_text)) ||
        !json_fields::extract_string(json, json + len, "q",
                                     size_text, sizeof(size_text))) return;

    const double trade_px = std::strtod(price_text, nullptr);
    const double trade_size = std::strtod(size_text, nullptr);
    if (trade_px <= 0.0 || trade_size <= 0.0) return;

    // Determine trade direction from the "m" field.
    // m=true  → buyer is taker (trade hit ask) → buyer-initiated → positive flow
    // m=false → seller is taker (trade hit bid) → seller-initiated → negative flow
    bool buyer_initiated = true;
    const char* m_hit = nullptr;
    if (json_fields::key_occurrences(json, json + len, "m", &m_hit) == 1) {
        while (m_hit < json + len && (*m_hit == ' ' || *m_hit == ':' ||
               *m_hit == '\t' || *m_hit == '\r' || *m_hit == '\n')) ++m_hit;
        if (m_hit < json + len) {
            if (*m_hit == 't' || *m_hit == '1')
                buyer_initiated = false;
            else if (*m_hit == 'f' || *m_hit == '0')
                buyer_initiated = true;
        }
    }

    // If we have a depth book, adjust the best level; otherwise use trade price
    // as the mid estimate.
    if (bid_count_ == 0 && ask_count_ == 0) {
        const double mid = trade_px;
        const double bid_px = mid;
        const double ask_px = mid + 0.01;
        const double signed_vol = buyer_initiated ? trade_size : -trade_size;
        const uint64_t now = mono_raw_ns();
        process_event(bid_px, ask_px, signed_vol, 0.0, true, now);
    } else {
        double bid_vol = bids_[0].size;
        double ask_vol = asks_[0].size;
        if (bids_[0].price > 0 && asks_[0].price > 0) {
            if (buyer_initiated)
                ask_vol = std::max(0.0, asks_[0].size - trade_size);
            else
                bid_vol = std::max(0.0, bids_[0].size - trade_size);
        }
        const double bid_px = bids_[0].price > 0 ? bids_[0].price : trade_px;
        const double ask_px = asks_[0].price > 0 ? asks_[0].price : trade_px;
        const uint64_t now = mono_raw_ns();
        process_event(bid_px, ask_px, bid_vol, ask_vol, true, now);
    }
}

inline void BinanceWSClient::parse_kline(const char* json, size_t len) {
    // Kline/candle data.  For P1 we validate and timestamp; full kline
    // processing (realized volatility, volume) is reserved for P2.
    if (!bounded_json::valid_document(json, len)) return;
    // Accepted and validated; kline → MarketState emission deferred to P2.
}

#endif  // CROWDINTEL_HAVE_NETWORK
