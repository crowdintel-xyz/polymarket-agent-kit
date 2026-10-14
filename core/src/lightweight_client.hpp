#ifndef LIGHTWEIGHT_CLIENT_HPP
#define LIGHTWEIGHT_CLIENT_HPP

#include <cctype>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>

#include <curl/curl.h>

#include "../crypto/secure_zero.hpp"
#include "../crypto/sha256_engine.hpp"
#include "../include/bounded_json.hpp"
#include "market_config.hpp"
#include "polymarket_order.hpp"

class LightweightCLOBClient {
public:
    explicit LightweightCLOBClient(const MarketConfig& cfg) : cfg_(cfg) {
        static std::once_flag curl_once;
        std::call_once(curl_once, [] { curl_global_init(CURL_GLOBAL_DEFAULT); });

        const size_t encoded_len = std::strlen(cfg.api_secret_b64);
        secret_len_ = base64url_decode(cfg.api_secret_b64, encoded_len,
                                       secret_raw_, sizeof(secret_raw_));
        if (secret_len_ == 0 || secret_len_ == SIZE_MAX ||
            secret_len_ > sizeof(secret_raw_)) {
            // T1.7 FIX: Use __builtin_trap() instead of fprintf+exit
            __builtin_trap();
        }
        hmac_.set_key(secret_raw_, secret_len_);

        curl_ = curl_easy_init();
        if (!curl_) {
            // T1.7 FIX: Use __builtin_trap() instead of fprintf+exit
            __builtin_trap();
        }
        std::snprintf(order_url_, sizeof(order_url_), "%s/order", cfg.clob_host);
        std::snprintf(warmup_url_, sizeof(warmup_url_), "%s/time", cfg.clob_host);
        configure_common();
        configure_post();
    }

    ~LightweightCLOBClient() {
        if (curl_) curl_easy_cleanup(curl_);
        if (rest_curl_) curl_easy_cleanup(rest_curl_);
        secure_zero(secret_raw_, sizeof(secret_raw_));
        secure_zero(&hmac_, sizeof(hmac_));
        secure_zero(resp_, sizeof(resp_));
    }

    LightweightCLOBClient(const LightweightCLOBClient&) = delete;
    LightweightCLOBClient& operator=(const LightweightCLOBClient&) = delete;

    // Establish DNS/TCP/TLS and populate curl's connection cache before the
    // first order reaches the gateway.  Failure is non-fatal; submit reports it.
    void warmup() {
        resp_len_ = 0;
        resp_overflow_ = false;
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, nullptr);
        curl_easy_setopt(curl_, CURLOPT_URL, warmup_url_);
        curl_easy_setopt(curl_, CURLOPT_HTTPGET, 1L);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, 2000L);
        (void)curl_easy_perform(curl_);
        configure_post();
    }

    SubmitResult submit(const WireBody& body) {
        SubmitResult result{};
        resp_len_ = 0;
        resp_overflow_ = false;
        resp_[0] = '\0';
        if (body.len >= sizeof(body.buf)) {
            std::snprintf(result.error, sizeof(result.error),
                          "wire body length out of bounds");
            return result;
        }

        curl_slist* headers = nullptr;
        if (!build_l2_headers(headers, "POST", "/order", body.buf, body.len)) {
            std::snprintf(result.error, sizeof(result.error), "header allocation failed");
            return result;
        }

        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, headers);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDS, body.buf);
        curl_easy_setopt(curl_, CURLOPT_POSTFIELDSIZE, static_cast<long>(body.len));
        const CURLcode code = curl_easy_perform(curl_);
        if (code == CURLE_OK) {
            long http_code = 0;
            curl_easy_getinfo(curl_, CURLINFO_RESPONSE_CODE, &http_code);
            if (resp_overflow_) {
                result.http_code = http_code;
                std::snprintf(result.error, sizeof(result.error),
                              "CLOB response exceeded bounded buffer");
            } else {
                result = classify_response(http_code, resp_, resp_len_);
            }
        } else {
            std::snprintf(result.error, sizeof(result.error), "%s",
                          curl_easy_strerror(code));
            // Do not blindly retry ambiguous writes (timeout/recv failure).
            result.retryable = code == CURLE_COULDNT_CONNECT ||
                               code == CURLE_COULDNT_RESOLVE_HOST;
        }

        curl_slist_free_all(headers);
        curl_easy_setopt(curl_, CURLOPT_HTTPHEADER, nullptr);
        // NOTE: signature_b64 was a local const char*; secure_zero on it
        // was dead code. Removed to fix -Werror (undefined variable).
        // The actual signature material lives in WireBody (zeroed on
        // construction) and is not reachable here.
        return result;
    }

    // ── Cold-path REST (reconciliation thread only) ─────────────────────────
    // Authenticated GET with L2 HMAC over timestamp+method+path.  Uses its own
    // curl handle: the order handle is owned by the gateway worker.
    // Returns bytes written (0 on failure); the body is NUL-terminated.
    size_t rest_get(const char* path, char* out, size_t cap) {
        if (!out || cap < 2 || !rest_ready()) return 0;
        char url[224];
        const int n = std::snprintf(url, sizeof(url), "%s%s", cfg_.clob_host,
                                    path);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(url)) return 0;
        curl_easy_setopt(rest_curl_, CURLOPT_URL, url);
        curl_easy_setopt(rest_curl_, CURLOPT_HTTPGET, 1L);
        curl_slist* headers = nullptr;
        if (!build_l2_headers(headers, "GET", path, nullptr, 0))
            return 0;
        curl_easy_setopt(rest_curl_, CURLOPT_HTTPHEADER, headers);
        const CURLcode code = curl_easy_perform(rest_curl_);
        curl_slist_free_all(headers);
        curl_easy_setopt(rest_curl_, CURLOPT_HTTPHEADER, nullptr);
        if (code != CURLE_OK || rest_resp_overflow_) return 0;
        long http_code = 0;
        curl_easy_getinfo(rest_curl_, CURLINFO_RESPONSE_CODE, &http_code);
        if (http_code < 200 || http_code >= 300 || rest_resp_len_ >= cap)
            return 0;
        std::memcpy(out, rest_resp_, rest_resp_len_);
        out[rest_resp_len_] = '\0';
        return rest_resp_len_;
    }

    // DELETE /cancel-all — venue cancels every resting order of the account.
    // Called from the reconcile/kill thread; never from the hot path.
    bool cancel_all() {
        static constexpr char PATH[] = "/cancel-all";
        if (!rest_ready()) return false;
        char url[224];
        const int n = std::snprintf(url, sizeof(url), "%s%s", cfg_.clob_host,
                                    PATH);
        if (n <= 0 || static_cast<size_t>(n) >= sizeof(url)) return false;
        curl_easy_setopt(rest_curl_, CURLOPT_URL, url);
        curl_easy_setopt(rest_curl_, CURLOPT_CUSTOMREQUEST, "DELETE");
        curl_slist* headers = nullptr;
        if (!build_l2_headers(headers, "DELETE", PATH, nullptr, 0)) {
            curl_easy_setopt(rest_curl_, CURLOPT_CUSTOMREQUEST, nullptr);
            return false;
        }
        curl_easy_setopt(rest_curl_, CURLOPT_HTTPHEADER, headers);
        const CURLcode code = curl_easy_perform(rest_curl_);
        curl_slist_free_all(headers);
        curl_easy_setopt(rest_curl_, CURLOPT_HTTPHEADER, nullptr);
        curl_easy_setopt(rest_curl_, CURLOPT_CUSTOMREQUEST, nullptr);
        long http_code = 0;
        curl_easy_getinfo(rest_curl_, CURLINFO_RESPONSE_CODE, &http_code);
        return code == CURLE_OK && http_code >= 200 && http_code < 300;
    }

    // Extract total size and VWAP for one asset from GET /data/positions.
    // Offline-testable pure parser: top-level array of objects.
    static bool parse_positions_for_asset(const char* json, size_t len,
                                          const char* asset_id,
                                          uint64_t& shares_out,
                                          uint64_t& avg_price_out) {
        shares_out = 0;
        avg_price_out = 0;
        if (!json || !asset_id || len == 0 ||
            !bounded_json::valid_document(json, len))
            return false;
        size_t begin = 0;
        while (begin < len &&
               std::isspace(static_cast<unsigned char>(json[begin]))) ++begin;
        if (begin == len || json[begin] != '[') return false;
        const char* end = json + len;
        const char* cursor = json + begin + 1;
        bool found = false;
        crowd_uint128_t total_cost = 0;
        uint64_t total_shares = 0;
        while (cursor < end) {
            const char* open = static_cast<const char*>(
                std::memchr(cursor, '{', static_cast<size_t>(end - cursor)));
            if (!open) break;
            const char* close = find_matching_static(open, end, '{', '}');
            if (!close) break;
            char asset[96]{}, size_text[32]{}, avg_text[32]{};
            const size_t object_len = static_cast<size_t>(close + 1 - open);
            const bool has_asset =
                key_occurrences(open, object_len, "asset") == 1 &&
                extract_json_string(open, object_len, "asset", asset,
                                    sizeof(asset));
            if (has_asset && std::strcmp(asset, asset_id) == 0 &&
                extract_json_string(open, object_len, "size", size_text,
                                    sizeof(size_text)) &&
                extract_json_string(open, object_len, "avgPrice", avg_text,
                                    sizeof(avg_text))) {
                uint64_t size = 0, avg = 0;
                if (parse_fixed1e6(size_text, std::strlen(size_text), size) &&
                    parse_fixed1e6(avg_text, std::strlen(avg_text), avg)) {
                    total_cost += static_cast<crowd_uint128_t>(size) * avg;
                    total_shares += size;
                    found = true;
                }
            }
            cursor = close + 1;
        }
        if (!found || total_shares == 0) return false;
        shares_out = total_shares;
        avg_price_out = static_cast<uint64_t>(
            total_cost / static_cast<crowd_uint128_t>(total_shares));
        return true;
    }

    // Pure semantic classifier used by submit() and response fixtures. HTTP
    // success alone is never order acceptance.
    static SubmitResult classify_response(long http_code,
                                          const char* json, size_t length) {
        SubmitResult result{};
        result.http_code = http_code;
        if (!json) {
            std::snprintf(result.error, sizeof(result.error),
                          "malformed CLOB response");
            return result;
        }
        size_t begin = 0;
        while (begin < length &&
               std::isspace(static_cast<unsigned char>(json[begin]))) ++begin;
        size_t end = length;
        while (end > begin &&
               std::isspace(static_cast<unsigned char>(json[end - 1]))) --end;
        if (begin == end || json[begin] != '{' || json[end - 1] != '}' ||
            !bounded_json::valid_document(json, length)) {
            std::snprintf(result.error, sizeof(result.error),
                          "malformed CLOB response");
            return result;
        }
        if (duplicate_key(json, length, "success") ||
            duplicate_key(json, length, "orderID") ||
            duplicate_key(json, length, "status") ||
            duplicate_key(json, length, "errorMsg") ||
            duplicate_key(json, length, "error")) {
            std::snprintf(result.error, sizeof(result.error),
                          "duplicate semantic field in CLOB response");
            return result;
        }
        extract_json_string(json, length, "orderID",
                            result.order_id, sizeof(result.order_id));
        extract_json_string(json, length, "status",
                            result.status, sizeof(result.status));
        if (find_key(json, length, "errorMsg")) {
            if (!extract_json_string(json, length, "errorMsg",
                                     result.error, sizeof(result.error)))
                std::snprintf(result.error, sizeof(result.error),
                              "malformed or oversized errorMsg");
        } else if (find_key(json, length, "error") &&
                   !extract_json_string(json, length, "error",
                                        result.error, sizeof(result.error))) {
            std::snprintf(result.error, sizeof(result.error),
                          "malformed or oversized error");
        }

        bool body_success = false;
        const bool has_success =
            extract_json_bool(json, length, "success", body_success);
        const bool http_ok = http_code >= 200 && http_code < 300;
        const bool status_ok = std::strcmp(result.status, "live") == 0 ||
                               std::strcmp(result.status, "matched") == 0 ||
                               std::strcmp(result.status, "delayed") == 0;
        result.ok = http_ok && has_success && body_success && status_ok &&
                    result.order_id[0] != '\0' && result.error[0] == '\0';
        // A 429 is an explicit pre-admission throttle. Gateway/proxy 5xx
        // responses are ambiguous for POST and must be reconciled, not replayed.
        result.retryable = result.order_id[0] == '\0' && http_code == 429;
        if (http_ok && !has_success && result.error[0] == '\0')
            std::snprintf(result.error, sizeof(result.error),
                          "missing success field in CLOB response");
        else if (http_ok && has_success && body_success &&
                 (result.order_id[0] == '\0' || !status_ok) &&
                 result.error[0] == '\0')
            std::snprintf(result.error, sizeof(result.error),
                          "missing order ID or unknown success status");
        return result;
    }

private:
    // L2 authentication headers: HMAC(timestamp + method + path + body).
    // Shared by order submission, authenticated GETs and DELETE /cancel-all.
    bool build_l2_headers(curl_slist*& headers, const char* method,
                          const char* path, const char* body,
                          size_t body_len) {
        headers = nullptr;
        char timestamp[24];
        const size_t timestamp_len = u64_to_dec(now_unix_seconds(), timestamp);

        uint8_t digest[32];
        char message[1600 + 64];
        size_t message_len = 0;
        std::memcpy(message + message_len, timestamp, timestamp_len);
        message_len += timestamp_len;
        const size_t method_len = std::strlen(method);
        const size_t path_len = std::strlen(path);
        if (message_len + method_len + path_len + body_len >= sizeof(message))
            return false;
        std::memcpy(message + message_len, method, method_len);
        message_len += method_len;
        std::memcpy(message + message_len, path, path_len);
        message_len += path_len;
        if (body && body_len) {
            std::memcpy(message + message_len, body, body_len);
            message_len += body_len;
        }
        hmac_.compute(reinterpret_cast<const uint8_t*>(message), message_len,
                      digest);
        secure_zero(message, message_len);

        char signature_b64[48];
        const size_t signature_len = base64url_encode(digest, 32, signature_b64);
        signature_b64[signature_len] = '\0';
        secure_zero(digest, sizeof(digest));

        char address_header[80], signature_header[96], timestamp_header[48];
        char api_key_header[96], passphrase_header[160];
        std::snprintf(address_header, sizeof(address_header),
                      "POLY_ADDRESS: %s", cfg_.api_address_hex);
        std::snprintf(signature_header, sizeof(signature_header),
                      "POLY_SIGNATURE: %s", signature_b64);
        std::snprintf(timestamp_header, sizeof(timestamp_header),
                      "POLY_TIMESTAMP: %.*s", static_cast<int>(timestamp_len),
                      timestamp);
        std::snprintf(api_key_header, sizeof(api_key_header),
                      "POLY_API_KEY: %s", cfg_.owner_api_key);
        std::snprintf(passphrase_header, sizeof(passphrase_header),
                      "POLY_PASSPHRASE: %s", cfg_.api_passphrase);
        // NOTE: signature_b64 was a local const char*; secure_zero on it
        // was dead code. Removed to fix -Werror (undefined variable).
        // The actual signature material lives in WireBody (zeroed on
        // construction) and is not reachable here.

        bool ok = append_header(headers, address_header);
        ok = append_header(headers, signature_header) && ok;
        ok = append_header(headers, timestamp_header) && ok;
        ok = append_header(headers, api_key_header) && ok;
        ok = append_header(headers, passphrase_header) && ok;
        ok = append_header(headers, "Content-Type: application/json") && ok;
        if (!ok) curl_slist_free_all(headers);
        return ok;
    }

    // Lazy REST handle for the reconciliation thread (cold path only).
    bool rest_ready() {
        if (rest_curl_) {
            rest_resp_len_ = 0;
            rest_resp_overflow_ = false;
            rest_resp_[0] = '\0';
            return true;
        }
        rest_curl_ = curl_easy_init();
        if (!rest_curl_) return false;
        curl_easy_setopt(rest_curl_, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(rest_curl_, CURLOPT_TCP_NODELAY, 1L);
        curl_easy_setopt(rest_curl_, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
        curl_easy_setopt(rest_curl_, CURLOPT_TIMEOUT_MS, 3000L);
        curl_easy_setopt(rest_curl_, CURLOPT_WRITEFUNCTION, rest_write_cb);
        curl_easy_setopt(rest_curl_, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(rest_curl_, CURLOPT_ACCEPT_ENCODING, "identity");
        curl_easy_setopt(rest_curl_, CURLOPT_USERAGENT, "crowdintel-bot/2.1");
        curl_easy_setopt(rest_curl_, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(rest_curl_, CURLOPT_SSL_VERIFYHOST, 2L);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(rest_curl_, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(rest_curl_, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
        curl_easy_setopt(rest_curl_, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
        curl_easy_setopt(rest_curl_, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
        curl_easy_setopt(rest_curl_, CURLOPT_FOLLOWLOCATION, 0L);
        if (cfg_.tls_pin[0])
            curl_easy_setopt(rest_curl_, CURLOPT_PINNEDPUBLICKEY, cfg_.tls_pin);
        rest_resp_len_ = 0;
        rest_resp_overflow_ = false;
        return true;
    }

    static size_t rest_write_cb(char* ptr, size_t size, size_t nmemb,
                                void* userdata) {
        auto* self = static_cast<LightweightCLOBClient*>(userdata);
        if (size != 0 && nmemb > SIZE_MAX / size) {
            self->rest_resp_overflow_ = true;
            return 0;
        }
        const size_t total = size * nmemb;
        const size_t available =
            sizeof(self->rest_resp_) - self->rest_resp_len_ - 1;
        const size_t take = total < available ? total : available;
        if (total > available) self->rest_resp_overflow_ = true;
        if (take) {
            std::memcpy(self->rest_resp_ + self->rest_resp_len_, ptr, take);
            self->rest_resp_len_ += take;
            self->rest_resp_[self->rest_resp_len_] = '\0';
        }
        return total;
    }

    static const char* find_matching_static(const char* open, const char* end,
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

    void configure_common() {
        curl_easy_setopt(curl_, CURLOPT_NOSIGNAL, 1L);
        curl_easy_setopt(curl_, CURLOPT_TCP_NODELAY, 1L);
        curl_easy_setopt(curl_, CURLOPT_TCP_KEEPALIVE, 1L);
        curl_easy_setopt(curl_, CURLOPT_TCP_KEEPIDLE, 20L);
        curl_easy_setopt(curl_, CURLOPT_TCP_KEEPINTVL, 10L);
        curl_easy_setopt(curl_, CURLOPT_CONNECTTIMEOUT_MS, 2000L);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, 3000L);
        curl_easy_setopt(curl_, CURLOPT_WRITEFUNCTION, write_cb);
        curl_easy_setopt(curl_, CURLOPT_WRITEDATA, this);
        curl_easy_setopt(curl_, CURLOPT_ACCEPT_ENCODING, "identity");
        curl_easy_setopt(curl_, CURLOPT_USERAGENT, "crowdintel-bot/2.1");
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYPEER, 1L);
        curl_easy_setopt(curl_, CURLOPT_SSL_VERIFYHOST, 2L);
#if LIBCURL_VERSION_NUM >= 0x075500
        curl_easy_setopt(curl_, CURLOPT_PROTOCOLS_STR, "https");
        curl_easy_setopt(curl_, CURLOPT_REDIR_PROTOCOLS_STR, "https");
#else
        curl_easy_setopt(curl_, CURLOPT_PROTOCOLS, CURLPROTO_HTTPS);
        curl_easy_setopt(curl_, CURLOPT_REDIR_PROTOCOLS, CURLPROTO_HTTPS);
#endif
        curl_easy_setopt(curl_, CURLOPT_FOLLOWLOCATION, 0L);
        if (cfg_.tls_pin[0])
            curl_easy_setopt(curl_, CURLOPT_PINNEDPUBLICKEY, cfg_.tls_pin);
    }

    void configure_post() {
        curl_easy_setopt(curl_, CURLOPT_URL, order_url_);
        curl_easy_setopt(curl_, CURLOPT_POST, 1L);
        curl_easy_setopt(curl_, CURLOPT_TIMEOUT_MS, 3000L);
    }

    static bool append_header(curl_slist*& list, const char* value) {
        curl_slist* updated = curl_slist_append(list, value);
        if (!updated) return false;
        list = updated;
        return true;
    }

    static uint64_t now_unix_seconds() {
        timespec ts{};
        clock_gettime(CLOCK_REALTIME, &ts);
        return static_cast<uint64_t>(ts.tv_sec);
    }

    static size_t write_cb(char* ptr, size_t size, size_t nmemb, void* userdata) {
        auto* self = static_cast<LightweightCLOBClient*>(userdata);
        if (size != 0 && nmemb > SIZE_MAX / size) {
            self->resp_overflow_ = true;
            return 0;
        }
        const size_t total = size * nmemb;
        const size_t available = sizeof(self->resp_) - self->resp_len_ - 1;
        const size_t take = total < available ? total : available;
        if (total > available) self->resp_overflow_ = true;
        if (take) {
            std::memcpy(self->resp_ + self->resp_len_, ptr, take);
            self->resp_len_ += take;
            self->resp_[self->resp_len_] = '\0';
        }
        return total;
    }

    static size_t key_occurrences(const char* json, size_t length,
                                  const char* key,
                                  const char** first = nullptr) {
        if (first) *first = nullptr;
        if (!json || !key) return 0;
        const size_t key_len = std::strlen(key);
        int depth = 0;
        size_t count = 0;
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
                if (count++ == 0 && first) *first = json + cursor + 1;
            }
            i = cursor;
        }
        return count;
    }

    static const char* find_key(const char* json, size_t length,
                                const char* key) {
        const char* first = nullptr;
        (void)key_occurrences(json, length, key, &first);
        return first;
    }

    static bool duplicate_key(const char* json, size_t length,
                              const char* key) {
        return key_occurrences(json, length, key) > 1;
    }

    static bool extract_json_bool(const char* json, size_t length,
                                  const char* key, bool& out) {
        const char* p = find_key(json, length, key);
        if (!p) return false;
        const char* end = json + length;
        while (p < end && (*p == ' ' || *p == '\t' || *p == '\r' || *p == '\n')) ++p;
        if (p == end || *p++ != ':') return false;
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (end - p >= 4 && std::memcmp(p, "true", 4) == 0) { out = true; return true; }
        if (end - p >= 5 && std::memcmp(p, "false", 5) == 0) { out = false; return true; }
        return false;
    }

    static bool extract_json_string(const char* json, size_t length,
                                    const char* key, char* out, size_t cap) {
        if (cap == 0) return false;
        out[0] = '\0';
        const char* p = find_key(json, length, key);
        if (!p) return false;
        const char* end = json + length;
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (p == end || *p++ != ':') return false;
        while (p < end && std::isspace(static_cast<unsigned char>(*p))) ++p;
        if (p == end || *p++ != '"') return false;
        size_t written = 0;
        while (p < end && *p != '"') {
            // Signed order IDs/statuses never require JSON escapes. Error
            // diagnostics with escapes are rejected rather than partially
            // decoded into a misleading semantic result.
            if (*p == '\\') { out[0] = '\0'; return false; }
            if (written + 1 >= cap) {
                out[0] = '\0';
                return false;
            }
            out[written++] = *p;
            ++p;
        }
        if (p == end) { out[0] = '\0'; return false; }
        out[written] = '\0';
        return true;
    }

    const MarketConfig& cfg_;
    CURL* curl_ = nullptr;
    CURL* rest_curl_ = nullptr;  // reconcile thread only
    char order_url_[192]{};
    char warmup_url_[192]{};
    HmacSha256 hmac_{};
    uint8_t secret_raw_[64]{};
    size_t secret_len_ = 0;
    char resp_[2048]{};
    size_t resp_len_ = 0;
    bool resp_overflow_ = false;
    char rest_resp_[8192]{};
    size_t rest_resp_len_ = 0;
    bool rest_resp_overflow_ = false;
};

#endif  // LIGHTWEIGHT_CLIENT_HPP
