#ifndef WS_MARKET_LISTENER_HPP
#define WS_MARKET_LISTENER_HPP

#include <openssl/ssl.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cerrno>
#include <chrono>
#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <fcntl.h>
#include <netdb.h>
#include <netinet/in.h>
#include <netinet/tcp.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>
#include <strings.h>

#include "../crypto/fast_random.hpp"
#include "../crypto/sha256_engine.hpp"
#include "../include/bounded_json.hpp"
#include "../include/order_book.hpp"
#include "market_config.hpp"
#include "polymarket_order.hpp"

class WsMarketListener {
public:
    // `subscribe_token` overrides the subscribed asset id; when null the
    // primary cfg token is used.  A second instance on the complement token
    // feeds the hedge book (P2 brakes) through an isolated connection, so
    // book ownership stays single-writer per book.
    WsMarketListener(const MarketConfig& cfg, OrderBookL2& book,
                     const char* subscribe_token = nullptr)
        : cfg_(cfg), book_(book), subscribe_token_(subscribe_token) {}
    ~WsMarketListener() { stop(); }

    void start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return;
        thread_ = std::thread([this] { run_loop(); });
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        const int fd = active_fd_.load(std::memory_order_acquire);
        if (fd >= 0) ::shutdown(fd, SHUT_RDWR);
        if (thread_.joinable()) thread_.join();
    }

    bool connected() const { return connected_.load(std::memory_order_acquire); }
    uint64_t events_seen() const { return events_.load(std::memory_order_relaxed); }
    uint64_t reconnects() const { return reconnects_.load(std::memory_order_relaxed); }

    static bool msg_is_book(const char* json, size_t len) {
        char event[32];
        return bounded_json::valid_document(json, len) &&
               event_type(json, len, event, sizeof(event)) &&
               std::strcmp(event, "book") == 0;
    }

    static bool msg_is_price_change(const char* json, size_t len) {
        char event[32];
        return bounded_json::valid_document(json, len) &&
               event_type(json, len, event, sizeof(event)) &&
               std::strcmp(event, "price_change") == 0;
    }

    void handle_message(const char* json, size_t len) {
        if (!json || len == 0 || !bounded_json::valid_document(json, len)) return;
        events_.fetch_add(1, std::memory_order_relaxed);
        char event[32];
        if (!event_type(json, len, event, sizeof(event))) return;
        if (std::strcmp(event, "book") == 0) {
            parse_book_snapshot(json, len);
        } else if (std::strcmp(event, "price_change") == 0) {
            parse_price_change(json, len);
        } else if (std::strcmp(event, "tick_size_change") == 0) {
            parse_tick_size_change(json, len);
        }
    }

private:
    static constexpr size_t MAX_MESSAGE = 1U << 20;
    static constexpr size_t MAX_LEVELS = OrderBookL2::MAX_LEVELS;

    void run_loop() {
        uint32_t backoff_ms = 250;
        while (running_.load(std::memory_order_acquire)) {
            rbuf_len_ = 0;
            book_.invalidate();
            if (session()) backoff_ms = 250;
            connected_.store(false, std::memory_order_release);
            book_.invalidate();
            if (!running_.load(std::memory_order_acquire)) break;
            reconnects_.fetch_add(1, std::memory_order_relaxed);
            const uint32_t slices = std::max(1U, backoff_ms / 10U);
            for (uint32_t i = 0; i < slices && running_.load(); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(10));
            backoff_ms = std::min(backoff_ms * 2U, 5000U);
        }
    }

    bool session() {
        char host[160]{}, path[192] = "/ws/market";
        int port = 443;
        bool tls = true;
        if (!parse_url(cfg_.ws_host, host, sizeof(host), path, sizeof(path),
                       port, tls)) return false;

        const int fd = tcp_connect(host, port, 3000);
        if (fd < 0) return false;
        active_fd_.store(fd, std::memory_order_release);
        set_socket_options(fd);

        SSL* ssl = nullptr;
        if (tls) {
            SSL_CTX* context = ssl_context();
            if (!context || !(ssl = SSL_new(context))) {
                cleanup(nullptr, fd); return false;
            }
            if (SSL_set_fd(ssl, fd) != 1 ||
                SSL_set_tlsext_host_name(ssl, host) != 1 ||
                SSL_set1_host(ssl, host) != 1) {
                cleanup(ssl, fd); return false;
            }
            if (SSL_connect(ssl) != 1 ||
                SSL_get_verify_result(ssl) != X509_V_OK) {
                cleanup(ssl, fd); return false;
            }
        }

        uint8_t key_raw[16];
        FastRandom random;
        for (size_t i = 0; i < sizeof(key_raw); i += 8) {
            const uint64_t word = random.next_u64();
            std::memcpy(key_raw + i, &word, 8);
        }
        char key_b64[32]{};
        base64_encode(key_raw, sizeof(key_raw), key_b64);
        char authority[176];
        const bool default_port = (tls && port == 443) || (!tls && port == 80);
        const int authority_len = default_port
            ? std::snprintf(authority, sizeof(authority), "%s", host)
            : std::snprintf(authority, sizeof(authority), "%s:%d", host, port);
        char request[768];
        const int request_len = std::snprintf(request, sizeof(request),
            "GET %s HTTP/1.1\r\nHost: %s\r\nUpgrade: websocket\r\n"
            "Connection: Upgrade\r\nSec-WebSocket-Key: %s\r\n"
            "Sec-WebSocket-Version: 13\r\n\r\n", path, authority, key_b64);
        if (authority_len <= 0 ||
            static_cast<size_t>(authority_len) >= sizeof(authority) ||
            request_len <= 0 || static_cast<size_t>(request_len) >= sizeof(request) ||
            !send_all(ssl, fd, request, static_cast<size_t>(request_len))) {
            cleanup(ssl, fd); return false;
        }

        char accept_expected[32]{};
        websocket_accept(key_b64, accept_expected);
        if (!read_http_upgrade(ssl, fd, accept_expected)) {
            cleanup(ssl, fd); return false;
        }

        const char* asset = subscribe_token_ && subscribe_token_[0]
            ? subscribe_token_ : cfg_.token_id_dec;
        char subscription[320];
        const int subscription_len = std::snprintf(subscription, sizeof(subscription),
            "{\"assets_ids\":[\"%s\"],\"type\":\"market\","
            "\"custom_feature_enabled\":true}", asset);
        if (subscription_len <= 0 ||
            static_cast<size_t>(subscription_len) >= sizeof(subscription) ||
            !send_frame(ssl, fd, 0x1, subscription,
                        static_cast<size_t>(subscription_len))) {
            cleanup(ssl, fd); return false;
        }

        connected_.store(true, std::memory_order_release);
        const bool clean = read_frames(ssl, fd);
        cleanup(ssl, fd);
        return clean;
    }

    static bool parse_url(const char* url, char* host, size_t host_cap,
                          char* path, size_t path_cap, int& port, bool& tls) {
        const char* cursor = url;
        if (std::strncmp(cursor, "wss://", 6) == 0) {
            tls = true; port = 443; cursor += 6;
        } else if (std::strncmp(cursor, "ws://", 5) == 0) {
            tls = false; port = 80; cursor += 5;
        } else return false;
        const char* slash = std::strchr(cursor, '/');
        const char* authority_end = slash ? slash : cursor + std::strlen(cursor);
        if (cursor == authority_end) return false;
        const char* colon = nullptr;
        for (const char* p = cursor; p < authority_end; ++p) {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (*p == ':' && colon) return false;  // IPv6 literals unsupported
            if (*p == ':') colon = p;
            else if (*p == '@' || *p == '?' || *p == '#' ||
                     std::isspace(c)) return false;
        }
        const char* host_end = colon ? colon : authority_end;
        const size_t host_len = static_cast<size_t>(host_end - cursor);
        if (host_len == 0 || host_len >= host_cap) return false;
        std::memcpy(host, cursor, host_len); host[host_len] = '\0';
        if (colon) {
            char port_text[8];
            const size_t n = static_cast<size_t>(authority_end - colon - 1);
            if (n == 0 || n >= sizeof(port_text)) return false;
            std::memcpy(port_text, colon + 1, n); port_text[n] = '\0';
            errno = 0;
            char* port_end = nullptr;
            const long parsed_port = std::strtol(port_text, &port_end, 10);
            if (errno == ERANGE || !port_end || *port_end != '\0' ||
                parsed_port <= 0 || parsed_port > 65535)
                return false;
            port = static_cast<int>(parsed_port);
        }
        if (slash) {
            const size_t path_len = std::strlen(slash);
            if (path_len == 0 || path_len >= path_cap ||
                std::strchr(slash, '#') || std::strchr(slash, ' '))
                return false;
            std::memcpy(path, slash, path_len + 1);
        }
        return true;
    }

    static int tcp_connect(const char* host, int port, int timeout_ms) {
        char service[8];
        std::snprintf(service, sizeof(service), "%d", port);
        addrinfo hints{};
        hints.ai_family = AF_UNSPEC;
        hints.ai_socktype = SOCK_STREAM;
        addrinfo* addresses = nullptr;
        if (::getaddrinfo(host, service, &hints, &addresses) != 0) return -1;
        int connected_fd = -1;
        for (addrinfo* address = addresses; address; address = address->ai_next) {
            int fd = ::socket(address->ai_family, address->ai_socktype,
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

    static void set_socket_options(int fd) {
        int one = 1;
        ::setsockopt(fd, IPPROTO_TCP, TCP_NODELAY, &one, sizeof(one));
        ::setsockopt(fd, SOL_SOCKET, SO_KEEPALIVE, &one, sizeof(one));
        timeval timeout{3, 0};
        ::setsockopt(fd, SOL_SOCKET, SO_RCVTIMEO, &timeout, sizeof(timeout));
        ::setsockopt(fd, SOL_SOCKET, SO_SNDTIMEO, &timeout, sizeof(timeout));
    }

    static SSL_CTX* ssl_context() {
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

    static bool send_all(SSL* ssl, int fd, const char* data, size_t len) {
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

    static ssize_t receive(SSL* ssl, int fd, char* data, size_t capacity) {
        return ssl ? SSL_read(ssl, data, static_cast<int>(capacity))
                   : ::recv(fd, data, capacity, 0);
    }

    static bool header_value(const char* header, size_t length,
                             const char* name, const char* expected,
                             bool token_list = false,
                             bool case_sensitive = false) {
        const size_t name_len = std::strlen(name);
        const size_t expected_len = std::strlen(expected);
        size_t matches = 0;
        const char* cursor = static_cast<const char*>(
            std::memchr(header, '\n', length));
        if (!cursor) return false;
        ++cursor;
        const char* end = header + length;
        while (cursor < end) {
            const char* line_end = find_bytes(cursor, end, "\r\n", 2);
            if (!line_end || line_end == cursor) break;
            const char* colon = find_char(cursor, line_end, ':');
            if (colon && static_cast<size_t>(colon - cursor) == name_len &&
                strncasecmp(cursor, name, name_len) == 0) {
                if (++matches > 1) return false;
                const char* value = colon + 1;
                while (value < line_end && std::isspace(
                           static_cast<unsigned char>(*value))) ++value;
                const char* value_end = line_end;
                while (value_end > value && std::isspace(
                           static_cast<unsigned char>(value_end[-1]))) --value_end;
                if (!token_list) {
                    if (static_cast<size_t>(value_end - value) != expected_len ||
                        (case_sensitive
                            ? std::memcmp(value, expected, expected_len) != 0
                            : strncasecmp(value, expected, expected_len) != 0))
                        return false;
                } else {
                    bool found = false;
                    const char* token = value;
                    while (token < value_end) {
                        while (token < value_end &&
                               (*token == ',' || std::isspace(
                                   static_cast<unsigned char>(*token)))) ++token;
                        const char* token_end = token;
                        while (token_end < value_end && *token_end != ',')
                            ++token_end;
                        const char* trimmed = token_end;
                        while (trimmed > token && std::isspace(
                                   static_cast<unsigned char>(trimmed[-1]))) --trimmed;
                        if (static_cast<size_t>(trimmed - token) == expected_len &&
                            strncasecmp(token, expected, expected_len) == 0)
                            found = true;
                        token = token_end;
                    }
                    if (!found) return false;
                }
            }
            cursor = line_end + 2;
        }
        return matches == 1;
    }

    bool read_http_upgrade(SSL* ssl, int fd, const char* expected) {
        char header[4096];
        size_t length = 0;
        while (length + 1 < sizeof(header)) {
            const ssize_t n = receive(ssl, fd, header + length, 1);
            if (n <= 0) return false;
            length += static_cast<size_t>(n);
            header[length] = '\0';
            if (length >= 4 && std::memcmp(header + length - 4, "\r\n\r\n", 4) == 0) {
                if (length < 13 || std::strncmp(header, "HTTP/1.1 101", 12) != 0 ||
                    (header[12] != ' ' && header[12] != '\r'))
                    return false;
                return header_value(header, length, "Sec-WebSocket-Accept",
                                    expected, false, true) &&
                       header_value(header, length, "Upgrade", "websocket") &&
                       header_value(header, length, "Connection", "upgrade", true);
            }
        }
        return false;
    }

    static void websocket_accept(const char* key, char out[32]) {
        static constexpr char GUID[] = "258EAFA5-E914-47DA-95CA-C5AB0DC85B11";
        char input[96];
        const size_t key_len = std::strlen(key);
        std::memcpy(input, key, key_len);
        std::memcpy(input + key_len, GUID, sizeof(GUID) - 1);
        uint8_t hash[20];
        sha1(reinterpret_cast<const uint8_t*>(input), key_len + sizeof(GUID) - 1, hash);
        base64_encode(hash, sizeof(hash), out);
    }

    bool send_frame(SSL* ssl, int fd, uint8_t opcode,
                    const char* payload, size_t len) {
        if (len > MAX_MESSAGE) return false;
        uint8_t header[14];
        size_t header_len = 0;
        header[header_len++] = static_cast<uint8_t>(0x80U | opcode);
        if (len < 126) {
            header[header_len++] = static_cast<uint8_t>(0x80U | len);
        } else if (len <= 0xFFFF) {
            header[header_len++] = 0x80U | 126U;
            header[header_len++] = static_cast<uint8_t>(len >> 8);
            header[header_len++] = static_cast<uint8_t>(len);
        } else {
            header[header_len++] = 0x80U | 127U;
            for (int i = 7; i >= 0; --i)
                header[header_len++] = static_cast<uint8_t>(len >> (i * 8));
        }
        FastRandom random;
        const uint64_t mask_word = random.next_u64();
        uint8_t mask[4];
        std::memcpy(mask, &mask_word, sizeof(mask));
        std::memcpy(header + header_len, mask, sizeof(mask));
        header_len += sizeof(mask);
        if (!send_all(ssl, fd, reinterpret_cast<const char*>(header), header_len))
            return false;
        for (size_t i = 0; i < len; ++i)
            send_buffer_[i] = static_cast<char>(payload[i] ^ mask[i & 3]);
        return len == 0 || send_all(ssl, fd, send_buffer_.data(), len);
    }

    bool read_frames(SSL* ssl, int fd) {
        size_t fragment_len = 0;
        bool fragmenting = false;
        uint64_t last_ping = now_mono_ms();
        uint64_t last_receive = last_ping;

        while (running_.load(std::memory_order_acquire)) {
            const uint64_t now = now_mono_ms();
            if (now - last_ping >= 8000) {
                // Polymarket heartbeat is an application text message, not an
                // RFC 6455 control ping.
                if (!send_frame(ssl, fd, 0x1, "PING", 4)) return false;
                last_ping = now;
            }
            if (now - last_receive >= 20000) return false;

            fd_set reads;
            FD_ZERO(&reads); FD_SET(fd, &reads);
            timeval timeout{0, 200000};
            const int ready = ::select(fd + 1, &reads, nullptr, nullptr, &timeout);
            if (!running_.load()) return true;
            if (ready < 0) { if (errno == EINTR) continue; return false; }
            if (ready == 0) continue;
            if (rbuf_len_ == rbuf_.size()) return false;
            const ssize_t n = receive(ssl, fd, rbuf_.data() + rbuf_len_,
                                      rbuf_.size() - rbuf_len_);
            if (n <= 0) return false;
            rbuf_len_ += static_cast<size_t>(n);
            last_receive = now_mono_ms();

            size_t offset = 0;
            while (offset + 2 <= rbuf_len_) {
                const uint8_t b0 = static_cast<uint8_t>(rbuf_[offset]);
                const uint8_t b1 = static_cast<uint8_t>(rbuf_[offset + 1]);
                const bool final = (b0 & 0x80U) != 0;
                const uint8_t opcode = b0 & 0x0FU;
                if ((b0 & 0x70U) != 0 || (b1 & 0x80U) != 0) return false;
                uint64_t payload_len = b1 & 0x7FU;
                size_t header_len = 2;
                if (payload_len == 126) {
                    if (offset + 4 > rbuf_len_) break;
                    payload_len = static_cast<uint8_t>(rbuf_[offset + 2]) * 256ULL +
                                  static_cast<uint8_t>(rbuf_[offset + 3]);
                    if (payload_len < 126) return false;  // non-canonical
                    header_len = 4;
                } else if (payload_len == 127) {
                    if (offset + 10 > rbuf_len_) break;
                    if ((static_cast<uint8_t>(rbuf_[offset + 2]) & 0x80U) != 0)
                        return false;
                    payload_len = 0;
                    for (int i = 0; i < 8; ++i)
                        payload_len = (payload_len << 8) |
                            static_cast<uint8_t>(rbuf_[offset + 2 + i]);
                    if (payload_len <= 0xFFFFU) return false;  // non-canonical
                    header_len = 10;
                }
                const bool control = (opcode & 0x08U) != 0;
                if ((opcode != 0x0 && opcode != 0x1 && opcode != 0x8 &&
                     opcode != 0x9 && opcode != 0xA) ||
                    (control && (!final || payload_len > 125)))
                    return false;
                if (payload_len > MAX_MESSAGE) return false;
                if (offset + header_len + payload_len > rbuf_len_) break;
                const char* payload = rbuf_.data() + offset + header_len;
                offset += header_len + static_cast<size_t>(payload_len);

                if (opcode == 0x8) {
                    (void)send_frame(ssl, fd, 0x8, payload,
                                     std::min<size_t>(payload_len, 125));
                    return false;
                }
                if (opcode == 0x9) {
                    if (!send_frame(ssl, fd, 0xA, payload, payload_len)) return false;
                    continue;
                }
                if (opcode == 0xA) continue;
                if (opcode == 0x1 && final) {
                    if (fragmenting) return false;
                    if (!(payload_len == 4 && std::memcmp(payload, "PONG", 4) == 0))
                        handle_message(payload, static_cast<size_t>(payload_len));
                    continue;
                }
                if (opcode == 0x1 && !final) {
                    if (fragmenting || payload_len > fragment_.size()) return false;
                    std::memcpy(fragment_.data(), payload, payload_len);
                    fragment_len = static_cast<size_t>(payload_len);
                    fragmenting = true;
                    continue;
                }
                if (opcode == 0x0) {
                    if (!fragmenting ||
                        payload_len > fragment_.size() - fragment_len)
                        return false;
                    std::memcpy(fragment_.data() + fragment_len, payload, payload_len);
                    fragment_len += static_cast<size_t>(payload_len);
                    if (final) {
                        handle_message(fragment_.data(), fragment_len);
                        fragmenting = false;
                        fragment_len = 0;
                    }
                }
            }
            if (offset) {
                std::memmove(rbuf_.data(), rbuf_.data() + offset, rbuf_len_ - offset);
                rbuf_len_ -= offset;
            }
        }
        return true;
    }

    void parse_book_snapshot(const char* json, size_t len) {
        if (!message_matches_token(json, len)) return;
        const char* ignored_begin = nullptr;
        const char* ignored_end = nullptr;
        if (!find_array(json, json + len, "bids", ignored_begin, ignored_end) ||
            !find_array(json, json + len, "asks", ignored_begin, ignored_end)) {
            book_.invalidate();
            return;
        }
        Level2Entry bids[MAX_LEVELS], asks[MAX_LEVELS];
        const size_t nb = parse_levels(json, len, "bids", bids, MAX_LEVELS, true);
        const size_t na = parse_levels(json, len, "asks", asks, MAX_LEVELS, false);
        // A valid empty snapshot is authoritative and must clear stale depth.
        book_.set_book(bids, nb, asks, na);
    }

    void parse_price_change(const char* json, size_t len) {
        Level2Entry bids[MAX_LEVELS], asks[MAX_LEVELS];
        size_t nb = 0, na = 0;
        book_.snapshot(bids, nb, asks, na);

        const char* end = json + len;
        const char* array_begin = nullptr;
        const char* array_end = nullptr;
        const size_t change_keys = key_occurrences(json, end, "price_changes") +
                                   key_occurrences(json, end, "priceChanges") +
                                   key_occurrences(json, end, "changes");
        if (change_keys != 1 ||
            (!find_array(json, end, "price_changes", array_begin, array_end) &&
             !find_array(json, end, "priceChanges", array_begin, array_end) &&
             !find_array(json, end, "changes", array_begin, array_end))) {
            book_.invalidate();
            return;
        }

        const char* cursor = array_begin + 1;
        bool changed = false;
        while (cursor < array_end) {
            const char* object = find_char(cursor, array_end, '{');
            if (!object) break;
            const char* object_end = find_matching(object, array_end, '{', '}');
            if (!object_end) break;
            char asset[96]{};
            const size_t asset_keys =
                key_occurrences(object, object_end + 1, "asset_id") +
                key_occurrences(object, object_end + 1, "tokenId");
            const bool has_asset = asset_keys == 1 &&
                (extract_string(object, object_end + 1,
                    "asset_id", asset, sizeof(asset)) ||
                 extract_string(object, object_end + 1,
                    "tokenId", asset, sizeof(asset)));
            if ((asset_keys == 0 || has_asset) &&
                (!has_asset || std::strcmp(asset, subscription_asset()) == 0)) {
                char price_text[24]{}, size_text[24]{}, side_text[8]{};
                if (extract_string(object, object_end + 1, "price",
                                   price_text, sizeof(price_text)) &&
                    extract_string(object, object_end + 1, "size",
                                   size_text, sizeof(size_text)) &&
                    extract_string(object, object_end + 1, "side",
                                   side_text, sizeof(side_text))) {
                    uint64_t price = 0, size = 0;
                    if (parse_fixed1e6(price_text, std::strlen(price_text), price) &&
                        parse_fixed1e6(size_text, std::strlen(size_text), size)) {
                        const bool bid = side_text[0] == 'B' || side_text[0] == 'b';
                        upsert_level(bid ? bids : asks, bid ? nb : na,
                                     price, size, bid);
                        changed = true;
                    }
                }
            }
            cursor = object_end + 1;
        }
        if (changed) {
            sort_levels(bids, nb, true);
            sort_levels(asks, na, false);
            book_.set_book(bids, nb, asks, na);
        }
    }

    void parse_tick_size_change(const char* json, size_t len) {
        if (!message_matches_token(json, len)) return;
        char text[24];
        const char* end = json + len;
        const size_t tick_keys = key_occurrences(json, end, "new_tick_size") +
                                 key_occurrences(json, end, "newTickSize");
        if (tick_keys != 1 ||
            !(extract_string(json, end, "new_tick_size", text, sizeof(text)) ||
              extract_string(json, end, "newTickSize", text, sizeof(text)))) {
            book_.invalidate();
            return;
        }
        uint64_t tick = 0;
        if (parse_fixed1e6(text, std::strlen(text), tick) &&
            amount_quantum_for_tick(tick) != 0) {
            book_.set_tick_size(tick);
        } else {
            // Continuing on an unknown venue grid could sign invalid or
            // economically different amounts. Halt via book invalidation.
            book_.invalidate();
        }
    }

    bool message_matches_token(const char* json, size_t len) const {
        char token[96];
        const char* end = json + len;
        const size_t asset_keys = key_occurrences(json, end, "asset_id");
        const size_t token_keys = key_occurrences(json, end, "tokenId");
        if (asset_keys + token_keys > 1) return false;
        if (extract_string(json, end, "asset_id", token, sizeof(token)) ||
            extract_string(json, end, "tokenId", token, sizeof(token)))
            return std::strcmp(token, subscription_asset()) == 0;
        return true;  // legacy fixtures omit it; subscription is token-scoped
    }

    static bool event_type(const char* json, size_t len, char* out, size_t cap) {
        const char* end = json + len;
        if (key_occurrences(json, end, "event_type") +
            key_occurrences(json, end, "type") != 1)
            return false;
        return extract_string(json, end, "event_type", out, cap) ||
               extract_string(json, end, "type", out, cap);
    }

    static const char* find_bytes(const char* begin, const char* end,
                                  const char* needle, size_t needle_len) {
        if (needle_len == 0 || static_cast<size_t>(end - begin) < needle_len)
            return nullptr;
        for (const char* p = begin; p + needle_len <= end; ++p)
            if (*p == *needle && std::memcmp(p, needle, needle_len) == 0) return p;
        return nullptr;
    }

    static const char* find_char(const char* begin, const char* end, char wanted) {
        return static_cast<const char*>(std::memchr(begin, wanted,
                                                   static_cast<size_t>(end - begin)));
    }

    static const char* find_matching(const char* open, const char* end,
                                     char open_char, char close_char) {
        int depth = 0;
        bool in_string = false, escaped = false;
        for (const char* p = open; p < end; ++p) {
            if (in_string) {
                if (escaped) escaped = false;
                else if (*p == '\\') escaped = true;
                else if (*p == '"') in_string = false;
                continue;
            }
            if (*p == '"') in_string = true;
            else if (*p == open_char) ++depth;
            else if (*p == close_char && --depth == 0) return p;
        }
        return nullptr;
    }

    // Return actual keys at the current object's top level. Field-like text
    // embedded in values and keys in nested objects are ignored.
    static size_t key_occurrences(const char* begin, const char* end,
                                  const char* key,
                                  const char** first = nullptr) {
        if (first) *first = nullptr;
        const size_t key_len = std::strlen(key);
        int depth = 0;
        size_t count = 0;
        for (const char* p = begin; p < end; ++p) {
            if (*p == '{' || *p == '[') { ++depth; continue; }
            if (*p == '}' || *p == ']') { --depth; continue; }
            if (*p != '"') continue;
            const char* start = p + 1;
            const char* cursor = start;
            bool escaped = false;
            while (cursor < end) {
                if (escaped) escaped = false;
                else if (*cursor == '\\') escaped = true;
                else if (*cursor == '"') break;
                ++cursor;
            }
            if (cursor == end) return count;
            const char* after = cursor + 1;
            while (after < end && std::isspace(
                       static_cast<unsigned char>(*after))) ++after;
            if (depth == 1 &&
                static_cast<size_t>(cursor - start) == key_len &&
                std::memcmp(start, key, key_len) == 0 &&
                after < end && *after == ':') {
                if (count++ == 0 && first) *first = cursor + 1;
            }
            p = cursor;
        }
        return count;
    }

    static bool extract_string(const char* begin, const char* end, const char* key,
                               char* out, size_t cap) {
        const char* hit = nullptr;
        if (key_occurrences(begin, end, key, &hit) != 1 || cap == 0)
            return false;
        while (hit < end && (*hit == ' ' || *hit == '\t' || *hit == '\r' || *hit == '\n')) ++hit;
        if (hit == end || *hit++ != ':') return false;
        while (hit < end && (*hit == ' ' || *hit == '\t' || *hit == '\r' || *hit == '\n')) ++hit;
        if (hit == end || *hit++ != '"') return false;
        size_t n = 0;
        while (hit < end && *hit != '"') {
            if (*hit == '\\' || n + 1 >= cap) return false;
            out[n++] = *hit++;
        }
        if (hit == end) return false;
        out[n] = '\0';
        return true;
    }

    static bool find_array(const char* begin, const char* end, const char* key,
                           const char*& array_begin, const char*& array_end) {
        const char* hit = nullptr;
        if (key_occurrences(begin, end, key, &hit) != 1) return false;
        while (hit < end && std::isspace(static_cast<unsigned char>(*hit))) ++hit;
        if (hit == end || *hit++ != ':') return false;
        while (hit < end && std::isspace(static_cast<unsigned char>(*hit))) ++hit;
        if (hit == end || *hit != '[') return false;
        const char* close = find_matching(hit, end, '[', ']');
        if (!close) return false;
        array_begin = hit; array_end = close;
        return true;
    }

    static size_t parse_levels(const char* json, size_t len, const char* key,
                               Level2Entry* out, size_t cap, bool bids) {
        const char* array = nullptr; const char* array_end = nullptr;
        if (!find_array(json, json + len, key, array, array_end)) return 0;
        size_t count = 0;
        const char* cursor = array + 1;
        while (cursor < array_end) {
            while (cursor < array_end && (*cursor == ' ' || *cursor == '\t' ||
                   *cursor == '\r' || *cursor == '\n' || *cursor == ',')) ++cursor;
            if (cursor == array_end) break;
            uint64_t price = 0, size = 0;
            bool valid = false;
            if (*cursor == '{') {
                const char* close = find_matching(cursor, array_end, '{', '}');
                if (!close) break;
                char price_text[24]{}, size_text[24]{};
                valid = extract_string(cursor, close + 1, "price", price_text,
                                       sizeof(price_text)) &&
                        extract_string(cursor, close + 1, "size", size_text,
                                       sizeof(size_text)) &&
                        parse_fixed1e6(price_text, std::strlen(price_text), price) &&
                        parse_fixed1e6(size_text, std::strlen(size_text), size);
                cursor = close + 1;
            } else if (*cursor == '[') {
                const char* close = find_matching(cursor, array_end, '[', ']');
                if (!close) break;
                char values[2][24]{};
                const char* p = cursor + 1;
                int index = 0;
                while (p < close && index < 2) {
                    p = find_char(p, close, '"');
                    if (!p) break;
                    const char* quote = find_char(p + 1, close, '"');
                    if (!quote) break;
                    const size_t n = static_cast<size_t>(quote - p - 1);
                    if (n >= sizeof(values[0])) break;
                    std::memcpy(values[index], p + 1, n);
                    values[index][n] = '\0';
                    ++index; p = quote + 1;
                }
                valid = index == 2 &&
                        parse_fixed1e6(values[0], std::strlen(values[0]), price) &&
                        parse_fixed1e6(values[1], std::strlen(values[1]), size);
                cursor = close + 1;
            } else {
                ++cursor;
            }
            if (valid && size != 0) retain_best(out, count, cap, {price, size}, bids);
        }
        sort_levels(out, count, bids);
        return count;
    }

    static void retain_best(Level2Entry* levels, size_t& count, size_t cap,
                            Level2Entry value, bool bids) {
        if (count < cap) { levels[count++] = value; return; }
        size_t worst = 0;
        for (size_t i = 1; i < count; ++i) {
            if (bids ? levels[i].price < levels[worst].price
                     : levels[i].price > levels[worst].price) worst = i;
        }
        if (bids ? value.price > levels[worst].price
                 : value.price < levels[worst].price) levels[worst] = value;
    }

    static void upsert_level(Level2Entry* levels, size_t& count,
                             uint64_t price, uint64_t size, bool bids) {
        for (size_t i = 0; i < count; ++i) {
            if (levels[i].price != price) continue;
            if (size == 0) {
                for (size_t j = i + 1; j < count; ++j) levels[j - 1] = levels[j];
                --count;
            } else levels[i].size = size;
            return;
        }
        if (size != 0) retain_best(levels, count, MAX_LEVELS, {price, size}, bids);
    }

    static void sort_levels(Level2Entry* levels, size_t count, bool descending) {
        std::sort(levels, levels + count, [descending](const auto& left, const auto& right) {
            return descending ? left.price > right.price : left.price < right.price;
        });
    }

    static uint64_t now_mono_ms() {
        return static_cast<uint64_t>(
            std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now().time_since_epoch()).count());
    }

    void cleanup(SSL* ssl, int fd) {
        connected_.store(false, std::memory_order_release);
        active_fd_.store(-1, std::memory_order_release);
        if (ssl) SSL_free(ssl);
        if (fd >= 0) ::close(fd);
    }

    // SHA-1 is used only for RFC 6455 handshake validation.
    static void sha1(const uint8_t* data, size_t len, uint8_t out[20]) {
        uint32_t h[5] = {0x67452301, 0xEFCDAB89, 0x98BADCFE, 0x10325476, 0xC3D2E1F0};
        uint64_t bits = len * 8;
        size_t offset = 0;
        while (offset + 64 <= len) { sha1_block(h, data + offset); offset += 64; }
        uint8_t tail[128]{};
        const size_t remaining = len - offset;
        std::memcpy(tail, data + offset, remaining);
        tail[remaining] = 0x80;
        const size_t final_offset = remaining >= 56 ? 120 : 56;
        for (int i = 0; i < 8; ++i)
            tail[final_offset + i] = static_cast<uint8_t>(bits >> (56 - i * 8));
        sha1_block(h, tail);
        if (remaining >= 56) sha1_block(h, tail + 64);
        for (int i = 0; i < 5; ++i) {
            out[i * 4] = static_cast<uint8_t>(h[i] >> 24);
            out[i * 4 + 1] = static_cast<uint8_t>(h[i] >> 16);
            out[i * 4 + 2] = static_cast<uint8_t>(h[i] >> 8);
            out[i * 4 + 3] = static_cast<uint8_t>(h[i]);
        }
    }

    static void sha1_block(uint32_t h[5], const uint8_t* p) {
        uint32_t w[80];
        for (int i = 0; i < 16; ++i)
            w[i] = static_cast<uint32_t>(p[i*4]) << 24 |
                   static_cast<uint32_t>(p[i*4+1]) << 16 |
                   static_cast<uint32_t>(p[i*4+2]) << 8 |
                   static_cast<uint32_t>(p[i*4+3]);
        for (int i = 16; i < 80; ++i) {
            const uint32_t v = w[i-3] ^ w[i-8] ^ w[i-14] ^ w[i-16];
            w[i] = (v << 1) | (v >> 31);
        }
        uint32_t a=h[0],b=h[1],c=h[2],d=h[3],e=h[4];
        for (int i = 0; i < 80; ++i) {
            uint32_t f, k;
            if (i < 20) { f=(b&c)|(~b&d); k=0x5A827999; }
            else if (i < 40) { f=b^c^d; k=0x6ED9EBA1; }
            else if (i < 60) { f=(b&c)|(b&d)|(c&d); k=0x8F1BBCDC; }
            else { f=b^c^d; k=0xCA62C1D6; }
            const uint32_t t=((a<<5)|(a>>27))+f+e+k+w[i];
            e=d; d=c; c=(b<<30)|(b>>2); b=a; a=t;
        }
        h[0]+=a; h[1]+=b; h[2]+=c; h[3]+=d; h[4]+=e;
    }

    // Returns the subscription token: either the explicit market-level
    // token (subscribe_token_) or the fallback to the asset token_id.
    const char* subscription_asset() const {
        return subscribe_token_ && subscribe_token_[0]
               ? subscribe_token_ : cfg_.token_id_dec;
    }

    const MarketConfig& cfg_;
    OrderBookL2& book_;
    const char* subscribe_token_ = nullptr;
    std::thread thread_;
    std::atomic<bool> running_{false};
    std::atomic<bool> connected_{false};
    std::atomic<int> active_fd_{-1};
    std::atomic<uint64_t> events_{0};
    std::atomic<uint64_t> reconnects_{0};
    std::array<char, MAX_MESSAGE + 14> rbuf_{};
    size_t rbuf_len_ = 0;
    std::array<char, MAX_MESSAGE> fragment_{};
    std::array<char, MAX_MESSAGE> send_buffer_{};
};

#endif  // WS_MARKET_LISTENER_HPP
