#ifndef ALPHA_HTTP_RECEIVER_HPP
#define ALPHA_HTTP_RECEIVER_HPP

// Minimal authenticated HTTP ingress for one JSON signal per POST.  It binds
// to loopback by default and is intended behind a production TLS reverse proxy.
// Parsing is length-bounded; requests, headers and bodies have hard caps.

#include <arpa/inet.h>
#include <atomic>
#include <cerrno>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <netinet/in.h>
#include <sys/select.h>
#include <sys/socket.h>
#include <thread>
#include <unistd.h>

#include "../include/bounded_json.hpp"
#include "alpha_parser.hpp"
#include "market_config.hpp"

class AlphaHttpReceiver {
public:
    AlphaHttpReceiver(const MarketConfig& cfg, AlphaParser& parser)
        : cfg_(cfg), parser_(parser) {}
    ~AlphaHttpReceiver() { stop(); }

    bool start() {
        if (running_.load(std::memory_order_acquire) || thread_.joinable())
            return false;
        int fd = ::socket(AF_INET, SOCK_STREAM, 0);
        if (fd < 0) return false;
        int one = 1;
        ::setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof(one));
        sockaddr_in address{};
        address.sin_family = AF_INET;
        address.sin_port = htons(cfg_.alpha_port);
        if (::inet_pton(AF_INET, cfg_.alpha_bind, &address.sin_addr) != 1 ||
            ::bind(fd, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0 ||
            ::listen(fd, 64) != 0) {
            ::close(fd);
            return false;
        }
        sockaddr_in bound{};
        socklen_t bound_len = sizeof(bound);
        if (::getsockname(fd, reinterpret_cast<sockaddr*>(&bound), &bound_len) != 0) {
            ::close(fd);
            return false;
        }
        bound_port_.store(ntohs(bound.sin_port), std::memory_order_release);
        listen_fd_.store(fd, std::memory_order_release);
        running_.store(true, std::memory_order_release);
        thread_ = std::thread([this] { run(); });
        return true;
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        bound_port_.store(0, std::memory_order_release);
        const int fd = listen_fd_.exchange(-1, std::memory_order_acq_rel);
        if (fd >= 0) {
            ::shutdown(fd, SHUT_RDWR);
            ::close(fd);
        }
        if (thread_.joinable()) thread_.join();
    }

    uint16_t bound_port() const { return bound_port_.load(std::memory_order_acquire); }
    uint64_t accepted() const { return accepted_.load(std::memory_order_relaxed); }
    uint64_t rejected() const { return rejected_.load(std::memory_order_relaxed); }

private:
    static constexpr size_t MAX_REQUEST = 12288;
    static constexpr size_t MAX_BODY = 4096;

    void run() {
        while (running_.load(std::memory_order_acquire)) {
            const int listen_fd = listen_fd_.load(std::memory_order_acquire);
            if (listen_fd < 0) break;
            fd_set readfds;
            FD_ZERO(&readfds);
            FD_SET(listen_fd, &readfds);
            timeval timeout{0, 500000};
            const int ready = ::select(listen_fd + 1, &readfds, nullptr, nullptr,
                                       &timeout);
            if (ready <= 0) continue;
            const int client = ::accept(listen_fd, nullptr, nullptr);
            if (client < 0) continue;
            // Ingress is loopback/proxy-local; a slow client must not monopolize
            // the single bounded parser longer than the alpha latency budget.
            timeval io_timeout{0, 250000};
            ::setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &io_timeout, sizeof(io_timeout));
            ::setsockopt(client, SOL_SOCKET, SO_SNDTIMEO, &io_timeout, sizeof(io_timeout));
            handle_client(client);
            ::close(client);
        }
    }

    static ssize_t recv_until(
            int fd, char* buffer, size_t capacity,
            const std::chrono::steady_clock::time_point& deadline) {
        const auto now = std::chrono::steady_clock::now();
        if (now >= deadline) return -1;
        auto remaining = std::chrono::duration_cast<std::chrono::microseconds>(
            deadline - now);
        if (remaining.count() <= 0) return -1;
        timeval timeout{
            static_cast<time_t>(remaining.count() / 1000000),
            static_cast<suseconds_t>(remaining.count() % 1000000)};
        fd_set reads;
        FD_ZERO(&reads);
        FD_SET(fd, &reads);
        int ready;
        do { ready = ::select(fd + 1, &reads, nullptr, nullptr, &timeout); }
        while (ready < 0 && errno == EINTR);
        if (ready <= 0) return -1;
        ssize_t amount;
        do { amount = ::recv(fd, buffer, capacity, 0); }
        while (amount < 0 && errno == EINTR);
        return amount;
    }

    void handle_client(int fd) {
        const auto deadline = std::chrono::steady_clock::now() +
                              std::chrono::milliseconds(250);
        char request[MAX_REQUEST + 1];
        size_t length = 0;
        size_t header_end = 0;
        while (length < MAX_REQUEST) {
            const ssize_t n = recv_until(fd, request + length,
                                         MAX_REQUEST - length, deadline);
            if (n <= 0) { reject(fd, 400, "incomplete request"); return; }
            length += static_cast<size_t>(n);
            request[length] = '\0';
            const char* marker = bounded_find(request, request + length, "\r\n\r\n", 4);
            if (marker) {
                header_end = static_cast<size_t>(marker - request) + 4;
                break;
            }
        }
        if (header_end == 0) { reject(fd, 431, "headers too large"); return; }
        static constexpr char HTTP11[] = "POST /signal HTTP/1.1\r\n";
        static constexpr char HTTP10[] = "POST /signal HTTP/1.0\r\n";
        if ((length < sizeof(HTTP11) - 1 ||
             std::memcmp(request, HTTP11, sizeof(HTTP11) - 1) != 0) &&
            (length < sizeof(HTTP10) - 1 ||
             std::memcmp(request, HTTP10, sizeof(HTTP10) - 1) != 0)) {
            reject(fd, 404, "POST /signal required"); return;
        }
        if (!authorized(request, header_end)) {
            reject(fd, 401, "unauthorized"); return;
        }

        if (header_present(request, header_end, "Transfer-Encoding")) {
            reject(fd, 400, "transfer encoding is unsupported"); return;
        }
        uint64_t content_length = 0;
        if (!header_u64(request, header_end, "Content-Length", content_length) ||
            content_length == 0 || content_length > MAX_BODY ||
            header_end + content_length > MAX_REQUEST) {
            reject(fd, 413, "invalid content length"); return;
        }
        while (length < header_end + content_length) {
            const ssize_t n = recv_until(
                fd, request + length,
                static_cast<size_t>(header_end + content_length - length),
                deadline);
            if (n <= 0) { reject(fd, 400, "incomplete body"); return; }
            length += static_cast<size_t>(n);
        }

        if (!parse_signal(request + header_end, static_cast<size_t>(content_length))) {
            reject(fd, 422, "invalid, stale, wrong-market, or queue-full signal");
            return;
        }
        accepted_.fetch_add(1, std::memory_order_relaxed);
        send_response(fd, 202, "accepted");
    }

    bool parse_signal(const char* body, size_t length) {
        size_t begin = 0;
        while (begin < length &&
               std::isspace(static_cast<unsigned char>(body[begin]))) ++begin;
        size_t end = length;
        while (end > begin &&
               std::isspace(static_cast<unsigned char>(body[end - 1]))) --end;
        if (begin == end || body[begin] != '{' || body[end - 1] != '}' ||
            !bounded_json::valid_document(body, length))
            return false;
        if (duplicate_json_key(body, length, "market") ||
            duplicate_json_key(body, length, "p_win") ||
            duplicate_json_key(body, length, "confidence") ||
            duplicate_json_key(body, length, "q_value") ||
            duplicate_json_key(body, length, "timestamp_ns") ||
            duplicate_json_key(body, length, "signal_id") ||
            duplicate_json_key(body, length, "direction") ||
            duplicate_json_key(body, length, "direction_hint"))
            return false;
        const char* ignored_end = nullptr;
        if (json_value(body, length, "direction", ignored_end) &&
            json_value(body, length, "direction_hint", ignored_end))
            return false;
        char market[96];
        if (!json_string(body, length, "market", market, sizeof(market))) return false;
        double p_win = 0, confidence = 0, q_value = 0;
        if (!json_double(body, length, "p_win", p_win) ||
            !json_double(body, length, "confidence", confidence) ||
            !json_double(body, length, "q_value", q_value)) return false;

        uint64_t timestamp_ns = 0, signal_id = 0;
        (void)json_u64(body, length, "timestamp_ns", timestamp_ns);
        (void)json_u64(body, length, "signal_id", signal_id);
        uint8_t direction = 2;
        char direction_string[8];
        if (json_string(body, length, "direction", direction_string,
                        sizeof(direction_string))) {
            if (std::strcmp(direction_string, "BUY") == 0) direction = 0;
            else if (std::strcmp(direction_string, "SELL") == 0) direction = 1;
            else if (std::strcmp(direction_string, "AUTO") != 0) return false;
        } else {
            uint64_t numeric_direction = 2;
            if (json_u64(body, length, "direction_hint", numeric_direction)) {
                if (numeric_direction > 2) return false;
                direction = static_cast<uint8_t>(numeric_direction);
            }
        }
        return parser_.process(market, std::strlen(market), p_win, confidence,
                               q_value, direction, timestamp_ns, signal_id);
    }

    bool authorized(const char* headers, size_t length) const {
        char expected[192];
        const int expected_len = std::snprintf(expected, sizeof(expected),
            "Bearer %s", cfg_.alpha_bearer_token);
        char actual[192];
        if (expected_len <= 0 ||
            !header_string(headers, length, "Authorization", actual, sizeof(actual)))
            return false;
        const size_t actual_len = std::strlen(actual);
        const size_t wanted_len = static_cast<size_t>(expected_len);
        unsigned diff = static_cast<unsigned>(actual_len ^ wanted_len);
        const size_t compare_len = actual_len < wanted_len ? actual_len : wanted_len;
        for (size_t i = 0; i < compare_len; ++i)
            diff |= static_cast<unsigned>(actual[i] ^ expected[i]);
        return diff == 0;
    }

    static const char* bounded_find(const char* begin, const char* end,
                                    const char* needle, size_t needle_len) {
        if (needle_len == 0 || static_cast<size_t>(end - begin) < needle_len)
            return nullptr;
        for (const char* p = begin; p + needle_len <= end; ++p)
            if (*p == *needle && std::memcmp(p, needle, needle_len) == 0) return p;
        return nullptr;
    }

    static bool header_present(const char* headers, size_t length,
                               const char* name) {
        const char* begin = headers;
        const char* end = headers + length;
        const size_t name_len = std::strlen(name);
        while (begin < end) {
            const char* line_end = bounded_find(begin, end, "\r\n", 2);
            if (!line_end) break;
            if (static_cast<size_t>(line_end - begin) > name_len &&
                strncasecmp(begin, name, name_len) == 0 &&
                begin[name_len] == ':') return true;
            begin = line_end + 2;
        }
        return false;
    }

    static bool header_string(const char* headers, size_t length, const char* name,
                              char* out, size_t cap) {
        const char* begin = headers;
        const char* end = headers + length;
        const size_t name_len = std::strlen(name);
        bool found = false;
        while (begin < end) {
            const char* line_end = bounded_find(begin, end, "\r\n", 2);
            if (!line_end) break;
            if (static_cast<size_t>(line_end - begin) > name_len + 1 &&
                strncasecmp(begin, name, name_len) == 0 && begin[name_len] == ':') {
                if (found) return false;  // reject ambiguous duplicate headers
                const char* value = begin + name_len + 1;
                while (value < line_end && (*value == ' ' || *value == '\t')) ++value;
                const size_t n = static_cast<size_t>(line_end - value);
                if (n >= cap) return false;
                std::memcpy(out, value, n);
                out[n] = '\0';
                found = true;
            }
            begin = line_end + 2;
        }
        return found;
    }

    static bool header_u64(const char* headers, size_t length, const char* name,
                           uint64_t& value) {
        char text[32];
        if (!header_string(headers, length, name, text, sizeof(text))) return false;
        errno = 0;
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(text, &end, 10);
        if (errno == ERANGE || !end || *end != '\0') return false;
        value = static_cast<uint64_t>(parsed);
        return true;
    }

    static size_t json_key_occurrences(const char* json, size_t length,
                                       const char* key,
                                       const char** first_value = nullptr) {
        if (first_value) *first_value = nullptr;
        const size_t key_len = std::strlen(key);
        int depth = 0;
        size_t count = 0;
        const char* end = json + length;
        for (size_t i = 0; i < length; ++i) {
            if (json[i] == '{' || json[i] == '[') { ++depth; continue; }
            if (json[i] == '}' || json[i] == ']') { --depth; continue; }
            if (json[i] != '"') continue;
            const size_t start = i + 1;
            size_t cursor = start;
            bool escaped = false;
            while (cursor < length) {
                const char ch = json[cursor];
                if (escaped) escaped = false;
                else if (ch == '\\') escaped = true;
                else if (ch == '"') break;
                ++cursor;
            }
            if (cursor == length) return count;
            size_t after = cursor + 1;
            while (after < length &&
                   std::isspace(static_cast<unsigned char>(json[after]))) ++after;
            if (depth == 1 && cursor - start == key_len &&
                std::memcmp(json + start, key, key_len) == 0 &&
                after < length && json[after] == ':') {
                const char* value = json + after + 1;
                while (value < end &&
                       std::isspace(static_cast<unsigned char>(*value))) ++value;
                if (count++ == 0 && first_value) *first_value = value;
            }
            i = cursor;
        }
        return count;
    }

    static const char* json_value(const char* json, size_t length, const char* key,
                                  const char*& end) {
        end = json + length;
        const char* first = nullptr;
        (void)json_key_occurrences(json, length, key, &first);
        return first;
    }

    static bool duplicate_json_key(const char* json, size_t length,
                                   const char* key) {
        return json_key_occurrences(json, length, key) > 1;
    }

    static bool valid_json_value_end(const char* p, const char* end) {
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
        return p < end && (*p == ',' || *p == '}');
    }

    static bool json_string(const char* json, size_t length, const char* key,
                            char* out, size_t cap) {
        const char* end = nullptr;
        const char* p = json_value(json, length, key, end);
        if (!p || p == end || *p++ != '"') return false;
        size_t n = 0;
        while (p < end && *p != '"') {
            if (*p == '\\' || static_cast<unsigned char>(*p) < 0x20 || n + 1 >= cap)
                return false;
            out[n++] = *p++;
        }
        if (p == end || !valid_json_value_end(p + 1, end)) return false;
        out[n] = '\0';
        return true;
    }

    static bool json_number_text(const char* json, size_t length, const char* key,
                                 char* out, size_t cap) {
        const char* end = nullptr;
        const char* p = json_value(json, length, key, end);
        if (!p) return false;
        const char* start = p;
        if (p == end || *p == '+' || *p == '.' ||
            (!std::isdigit(static_cast<unsigned char>(*p)) && *p != '-'))
            return false;
        while (p < end && (std::isdigit(static_cast<unsigned char>(*p)) ||
               *p == '-' || *p == '+' || *p == '.' || *p == 'e' || *p == 'E')) ++p;
        const size_t n = static_cast<size_t>(p - start);
        if (n == 0 || n >= cap || !valid_json_value_end(p, end)) return false;
        std::memcpy(out, start, n); out[n] = '\0';
        return true;
    }

    static bool json_double(const char* json, size_t length, const char* key,
                            double& value) {
        char text[64];
        if (!json_number_text(json, length, key, text, sizeof(text))) return false;
        errno = 0;
        char* end = nullptr;
        value = std::strtod(text, &end);
        return errno != ERANGE && end && *end == '\0' && std::isfinite(value);
    }

    static bool json_u64(const char* json, size_t length, const char* key,
                         uint64_t& value) {
        char text[32];
        if (!json_number_text(json, length, key, text, sizeof(text))) return false;
        if (text[0] == '-') return false;
        errno = 0;
        char* end = nullptr;
        const unsigned long long parsed = std::strtoull(text, &end, 10);
        if (errno == ERANGE || !end || *end != '\0') return false;
        value = static_cast<uint64_t>(parsed);
        return true;
    }

    void reject(int fd, int status, const char* message) {
        rejected_.fetch_add(1, std::memory_order_relaxed);
        send_response(fd, status, message);
    }

    static void send_response(int fd, int status, const char* message) {
        const char* reason = status == 202 ? "Accepted" :
                             status == 401 ? "Unauthorized" :
                             status == 404 ? "Not Found" :
                             status == 413 ? "Payload Too Large" :
                             status == 422 ? "Unprocessable Entity" :
                             status == 431 ? "Request Header Fields Too Large" : "Bad Request";
        char response[512];
        const int n = std::snprintf(response, sizeof(response),
            "HTTP/1.1 %d %s\r\nContent-Type: application/json\r\n"
            "Connection: close\r\nContent-Length: %zu\r\n\r\n{\"message\":\"%s\"}",
            status, reason, std::strlen(message) + 14, message);
        if (n > 0) (void)::send(fd, response, static_cast<size_t>(n), MSG_NOSIGNAL);
    }

    const MarketConfig& cfg_;
    AlphaParser& parser_;
    std::atomic<bool> running_{false};
    std::atomic<int> listen_fd_{-1};
    std::atomic<uint16_t> bound_port_{0};
    std::thread thread_;
    std::atomic<uint64_t> accepted_{0};
    std::atomic<uint64_t> rejected_{0};
};

#endif  // ALPHA_HTTP_RECEIVER_HPP
