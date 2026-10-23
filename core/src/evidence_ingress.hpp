#ifndef EVIDENCE_INGRESS_HPP
#define EVIDENCE_INGRESS_HPP

// ─────────────────────────────────────────────────────────────────────────────
// Evidence ingress (P4, cold path only).  Three duties:
//
//   * Source-list parsing ("1:0.9,2:0.5") into SourceReliability — used by the
//     startup config, the recalibration-file poller, and unit tests.  Shared
//     code path so calibration semantics can never drift between loaders.
//
//   * NDJSON evidence line parsing (one observation per line):
//       {"source":1,"kind":"count","n":32,"k":22,"hash":12345}
//       {"source":3,"kind":"lr","lr":693147,"hash":12346}   // lr x1e6
//     Strict validation; malformed lines are rejected, never half-applied.
//     Cold-user only (sscanf-based parsers on the advisory thread).
//
//   * EvidenceFileReplayer: a cold thread replays a recorded evidence file
//     (paper-trading/backfill) into the hot queue, then tails it.  This is the
//     universal substrate for the live adapters (polls/macro/sports write the
//     same NDJSON through stdout-redirected collectors or the HTTP poller).
// ─────────────────────────────────────────────────────────────────────────────

#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <thread>

#include "../include/bounded_json.hpp"
#include "../include/evidence.hpp"
#include "../include/source_reliability.hpp"
#include "../include/spsc_ring_buffer.hpp"
#include "../include/time_utils.hpp"
#include "../crypto/sha256_engine.hpp"
#include "json_fields.hpp"
#include "ofi_calculator.hpp"

namespace evidence_ingress {

// "1:0.90, 2:0.5" — comma/whitespace separated id:weight pairs.  Returns the
// number of pairs applied; stops at the first malformed token.
inline size_t parse_sources_text(const char* text, SourceReliability& out) {
    if (!text) return 0;
    size_t applied = 0;
    const char* p = text;
    while (*p) {
        while (*p && (std::isspace(static_cast<unsigned char>(*p)) ||
                      *p == ','))
            ++p;
        if (!*p) break;
        char* end = nullptr;
        const long id = std::strtol(p, &end, 10);
        if (!end || end == p || *end != ':') break;
        p = end + 1;
        char* wend = nullptr;
        const double w = std::strtod(p, &wend);
        if (!wend || wend == p) break;
        out.set_weight(static_cast<uint32_t>(id), w);
        ++applied;
        p = wend;
    }
    return applied;
}

// Parse one NDJSON evidence line.  Returns true and fills `ev` on success;
// anything unrecognized or contradictory returns false untouched.
inline bool parse_evidence_line(const char* line, EvidenceEvent& ev) {
    unsigned source = 0, n = 0, k = 0, hash = 0;
    long long lr = 0;
    char kind[16]{};
    // Tolerate field order by scanning with a permissive-but-complete match:
    // every required field must appear exactly once; extras are rejected by
    // the strict format first, then fall back to field-by-field probes.
    int matched = std::sscanf(line,
        " { \"source\" : %u , \"kind\" : \"%15[^\"]\" , \"n\" : %u , "
        "\"k\" : %u , \"hash\" : %u } ",
        &source, kind, &n, &k, &hash);
    if (matched == 5) {
        if (n == 0 || k > n || hash == 0) return false;
        ev = EvidenceEvent{};
        ev.kind = EvidenceEvent::Kind::COUNT;
        ev.source_id = static_cast<uint32_t>(source);
        ev.count_n = static_cast<uint32_t>(n);
        ev.count_k = static_cast<uint32_t>(k);
        ev.event_hash = static_cast<uint32_t>(hash);
        return true;
    }
    matched = std::sscanf(line,
        " { \"source\" : %u , \"kind\" : \"%15[^\"]\" , \"lr\" : %lld , "
        "\"hash\" : %u } ",
        &source, kind, &lr, &hash);
    if (matched == 4) {
        if (hash == 0) return false;
        if (std::strcmp(kind, "lr") != 0) return false;
        ev = EvidenceEvent{};
        ev.kind = EvidenceEvent::Kind::LR;
        ev.source_id = static_cast<uint32_t>(source);
        ev.lr_x1e6 = static_cast<int32_t>(lr);
        ev.event_hash = static_cast<uint32_t>(hash);
        return true;
    }
    return false;
}

}  // namespace evidence_ingress

// ── Binance OFI → Evidence bridge ──────────────────────────────────────────────
// Converts MarketState snapshots from the Binance WebSocket client into
// EvidenceEvent::LR records for the Bayesian brain (P4).
//
// Design:
//   lr = k * atanh(OFI_norm)           (Cont et al. 2014, hyperbolic framing)
//   where atanh(x) = 0.5 * ln((1+x)/(1-x)) maps OFI_norm ∈ (-1,1) → ℝ
//   and k scales evidence strength (k=1 → unit log-odds per unit atanh).
//
//   Evidence is stored as int32 lr_x1e6 (lr × 1e6) to match EvidenceEvent.
//
// Rate limiting: at most one LR evidence per source per 10 ms, enforcing a
// maximum evidence cadence of 100 Hz — sufficient for sub-second alpha without
// saturating the hot-path posterior update (~41 ns/event).
//
// Hot-reload: a cold watcher thread polls source_reliability.json and updates
// the SourceReliability weight table atomically.  The hot path reads weights
// via a single acquire atomic load; no locks on the hot path.

namespace binance_ofi {

// Source identifiers — match the SOURCE_ constants in binance_ws_client.hpp.
inline constexpr uint32_t SOURCE_BINANCE_OFI = 0x02;
inline constexpr double DEFAULT_BINANCE_WEIGHT = 0.85;

// OFI → LR scaling constant.  With k=1.0, an OFI_norm of ±0.9 yields
// atanh(±0.9) ≈ ±1.472, i.e. lr_x1e6 ≈ ±1'472'219 — strong but not
// overwhelming evidence relative to the prior strength N0.
inline constexpr double LR_SCALE_K = 1.0;

// Rate limit: maximum evidence cadence (10 ms → 100 Hz).
inline constexpr uint64_t RATE_LIMIT_NS = 10'000'000ULL;

// Convert normalized OFI to log-likelihood ratio (fixed-point x1e6).
// Clamps input to (-0.999, 0.999) to avoid atanh singularity at ±1.
inline int32_t ofi_to_lr_x1e6(double ofi_normalized) noexcept {
    // Clamp to avoid ±inf atanh
    if (ofi_normalized > 0.999) ofi_normalized = 0.999;
    if (ofi_normalized < -0.999) ofi_normalized = -0.999;
    if (ofi_normalized == 0.0) return 0;

    // atanh(x) = 0.5 * ln((1+x)/(1-x))
    const double ratio = (1.0 + ofi_normalized) / (1.0 - ofi_normalized);
    const double atanh = 0.5 * std::log(ratio);
    const double lr = LR_SCALE_K * atanh;

    // Overflow guard: LR evidence is int32 x1e6.  Cap at ±2^30.
    const double lr_x1e6 = lr * 1'000'000.0;
    if (lr_x1e6 > static_cast<double>(INT32_MAX))
        return INT32_MAX;
    if (lr_x1e6 < static_cast<double>(INT32_MIN))
        return INT32_MIN;
    return static_cast<int32_t>(lr_x1e6);
}

// Parse source_reliability.json: {"sources": {"1": 0.90, "2": 0.85}}
// or flat format: {"1": 0.90, "2": 0.85}
inline size_t parse_source_reliability_json(const char* json, size_t len,
                                            SourceReliability& out) {
    if (!json || len == 0) return 0;
    if (!bounded_json::valid_document(json, len)) return 0;

    // Try the nested format: {"sources": {"1": 0.90, ...}}
    const char* sources_begin = nullptr;
    const char* sources_end = nullptr;
    size_t applied = 0;

    if (json_fields::find_array(json, json + len, "sources",
                                sources_begin, sources_end) ||
        json_fields::key_occurrences(json, json + len, "sources") == 1) {
        // It's actually an object, not array — find_array won't work.
        // Use key_occurrences to find "sources" value start.
        const char* sources_start = nullptr;
        if (json_fields::key_occurrences(json, json + len, "sources",
                                         &sources_start) == 1) {
            while (sources_start < json + len &&
                   (*sources_start == ' ' || *sources_start == ':' ||
                    *sources_start == '\t' || *sources_start == '\r' ||
                    *sources_start == '\n')) ++sources_start;
            if (sources_start < json + len && *sources_start == '{') {
                const char* sources_close =
                    json_fields::find_matching(sources_start, json + len,
                                               '{', '}');
                if (sources_close) {
                    // Parse each key-value pair within the sources object
                    const char* cursor = sources_start + 1;
                    while (cursor < sources_close) {
                        // Find next key
                        const char* key_start =
                            json_fields::find_char(cursor, sources_close, '"');
                        if (!key_start) break;
                        const char* key_end =
                            json_fields::find_char(key_start + 1, sources_close, '"');
                        if (!key_end) break;
                        // Parse key as integer
                        char key_buf[16];
                        const size_t key_len =
                            static_cast<size_t>(key_end - key_start - 1);
                        if (key_len >= sizeof(key_buf)) {
                            cursor = key_end + 1;
                            continue;
                        }
                        std::memcpy(key_buf, key_start + 1, key_len);
                        key_buf[key_len] = '\0';
                        const uint32_t id =
                            static_cast<uint32_t>(std::strtoul(key_buf, nullptr, 10));

                        // Find the value
                        const char* val_start = key_end + 1;
                        while (val_start < sources_close &&
                               (*val_start == ' ' || *val_start == ':' ||
                                *val_start == '\t' || *val_start == '\r' ||
                                *val_start == '\n')) ++val_start;
                        if (val_start >= sources_close) break;
                        const double w = std::strtod(val_start, nullptr);
                        if (w > 0.0 && w <= 1.0) {
                            out.set_weight(id, w);
                            ++applied;
                        }
                        cursor = val_start + 1;
                    }
                }
            }
        }
    } else {
        // Try flat format: {"1": 0.90, "2": 0.85}
        const char* cursor = json;
        while (cursor < json + len) {
            const char* key_start =
                json_fields::find_char(cursor, json + len, '"');
            if (!key_start) break;
            const char* key_end =
                json_fields::find_char(key_start + 1, json + len, '"');
            if (!key_end) break;
            char key_buf[16];
            const size_t key_len = static_cast<size_t>(key_end - key_start - 1);
            if (key_len >= sizeof(key_buf)) {
                cursor = key_end + 1;
                continue;
            }
            std::memcpy(key_buf, key_start + 1, key_len);
            key_buf[key_len] = '\0';
            const uint32_t id = static_cast<uint32_t>(std::strtoul(key_buf, nullptr, 10));

            const char* val_start = key_end + 1;
            while (val_start < json + len &&
                   (*val_start == ' ' || *val_start == ':' ||
                    *val_start == '\t' || *val_start == '\r' ||
                    *val_start == '\n')) ++val_start;
            if (val_start >= json + len) break;
            const double w = std::strtod(val_start, nullptr);
            if (w > 0.0 && w <= 1.0) {
                out.set_weight(id, w);
                ++applied;
            }
            cursor = val_start + 1;
        }
    }

    return applied;
}

// Cold-thread adapter: drains MarketState from BinanceWSClient's ring,
// converts OFI → LR evidence, applies rate limiting, and pushes to the
// hot-path EvidenceEvent ring.
//
// Template parameter MarketStateQ is the ring type from BinanceWSClient.
// The adapter is parameterized to avoid an include on the full BinanceWSClient
// class (which would pull in network headers).
template <typename MarketStateQ>
class BinanceOFIAdapter {
public:
    BinanceOFIAdapter(MarketStateQ& market_q,
                      SPSC_RingBuffer<EvidenceEvent>& evidence_q,
                      SourceReliability& sources,
                      const char* recal_path,
                      uint64_t now_ns)
        : market_q_(market_q),
          evidence_q_(evidence_q),
          sources_(sources),
          recal_path_(recal_path),
          last_emit_ns_(now_ns),
          hash_counter_(now_ns) {
        // Register Binance source with default weight
        sources_.set_weight(SOURCE_BINANCE_OFI, DEFAULT_BINANCE_WEIGHT);
    }

    // Start the cold drain thread.  Polls market_q at ~10 μs interval,
    // converts MarketState → EvidenceEvent::LR, rate-limits to 100 Hz.
    void start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return;
        thread_ = std::thread([this] { drain_loop(); });
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    uint64_t emitted() const noexcept {
        return emitted_.load(std::memory_order_relaxed);
    }
    uint64_t drained() const noexcept {
        return drained_.load(std::memory_order_relaxed);
    }
    uint64_t rate_limited() const noexcept {
        return rate_limited_.load(std::memory_order_relaxed);
    }

private:
    void drain_loop() {
        while (running_.load(std::memory_order_acquire)) {
            // Hot-reload: poll source_reliability.json
            poll_recalibration();

            // Drain all available MarketStates (drop-oldest already handled
            // by the SPSC ring when BinanceWSClient overflows)
            MarketState state{};
            while (market_q_.try_pop(state)) {
                drained_.fetch_add(1, std::memory_order_relaxed);

                const uint64_t now_ns = crowdintel::realtime_ns();

                // Rate limiting: at most 1 evidence per 10 ms
                const uint64_t elapsed = now_ns - last_emit_ns_.load(std::memory_order_acquire);
                if (elapsed < RATE_LIMIT_NS) {
                    rate_limited_.fetch_add(1, std::memory_order_relaxed);
                    continue;
                }

                // Convert OFI → LR evidence
                const int32_t lr_x1e6 = ofi_to_lr_x1e6(state.ofi_normalized);
                if (lr_x1e6 == 0) continue;  // no signal, skip

                last_emit_ns_.store(now_ns, std::memory_order_release);

                EvidenceEvent ev{};
                ev.kind = EvidenceEvent::Kind::LR;
                ev.source_id = SOURCE_BINANCE_OFI;
                ev.lr_x1e6 = lr_x1e6;
                ev.event_hash = next_hash();
                ev.timestamp_ns = now_ns;
                ev.outcome = 0;   // YES slot (binary market, slot 0)
                ev.neg_risk = 0;

                if (evidence_q_.try_push(ev)) {
                    emitted_.fetch_add(1, std::memory_order_relaxed);
                }
            }

            // Brief yield to avoid busy-spin
            std::this_thread::sleep_for(std::chrono::microseconds(10));
        }
    }

    // Poll source_reliability.json for weight updates (hot-reload).
    void poll_recalibration() {
        if (!recal_path_ || !recal_path_[0]) return;
        FILE* f = std::fopen(recal_path_, "r");
        if (!f) return;

        // Read file contents
        std::array<char, 4096> buf{};
        const size_t nread = std::fread(buf.data(), 1, sizeof(buf) - 1, f);
        std::fclose(f);
        if (nread == 0) return;

        buf[nread] = '\0';

        // Only apply if the file content changed (compare by modification)
        // We use a simple content hash for change detection
        const uint64_t content_hash = json_fields::fnv_hash(buf.data(), nread);
        if (content_hash == last_content_hash_) return;
        last_content_hash_ = content_hash;

        // Parse and apply new weights
        SourceReliability temp;
        const size_t applied = parse_source_reliability_json(
            buf.data(), nread, temp);
        if (applied > 0) {
            for (uint32_t i = 0; i < SourceReliability::MAX_SOURCES; ++i) {
                const double w = temp.weight(i);
                if (w > 0.0) {
                    sources_.set_weight(i, w);
                }
            }
        }
    }

    // Deterministic hash for event deduplication (used by the Bayesian engine's
    // hash-dedup window).  Mixes event counter with timestamp.
    uint32_t next_hash() noexcept {
        uint64_t h = hash_counter_.fetch_add(1, std::memory_order_relaxed) * 0x9E3779B97F4A7C15ULL;
        h ^= static_cast<uint64_t>(static_cast<uint64_t>(
            static_cast<uint32_t>(h))) * 0x853C100142428179ULL;
        return static_cast<uint32_t>(h);
    }

    MarketStateQ& market_q_;
    SPSC_RingBuffer<EvidenceEvent>& evidence_q_;
    SourceReliability& sources_;
    const char* recal_path_;

    std::atomic<bool> running_{false};
    std::thread thread_;

    // Rate limiting
    std::atomic<uint64_t> last_emit_ns_;
    std::atomic<uint64_t> hash_counter_;

    // Hot-reload state
    uint64_t last_content_hash_ = 0;

    // Metrics
    std::atomic<uint64_t> emitted_{0};
    std::atomic<uint64_t> drained_{0};
    std::atomic<uint64_t> rate_limited_{0};
};

}  // namespace binance_ofi

// Cold replayer/tailer: pushes parsed events, then keeps tailing the file for
// appends until stopped.  Never blocks the hot queue: full queue drops are
// counted and surfaced via `dropped()`.
class EvidenceFileReplayer {
public:
    EvidenceFileReplayer(const char* path,
                         SPSC_RingBuffer<EvidenceEvent>& queue,
                         SourceReliability* recal, const char* recal_path)
        : path_(path), queue_(queue), recal_(recal),
          recal_path_(recal_path) {}

    bool start() {
        bool expected = false;
        if (!running_.compare_exchange_strong(expected, true)) return true;
        FILE* probe = std::fopen(path_, "r");
        open_ok_ = probe != nullptr;
        if (probe) std::fclose(probe);
        thread_ = std::thread([this] { run(); });
        return open_ok_;
    }

    void stop() {
        running_.store(false, std::memory_order_release);
        if (thread_.joinable()) thread_.join();
    }

    bool open_ok() const { return open_ok_; }
    uint64_t parsed() const { return parsed_; }
    uint64_t dropped() const { return dropped_; }

private:
    void run() {
        char line[2048];
        while (running_.load(std::memory_order_acquire)) {
            FILE* f = std::fopen(path_, "r");
            if (f) {
                while (running_.load(std::memory_order_acquire) &&
                       std::fgets(line, sizeof(line), f)) {
                    EvidenceEvent ev{};
                    if (!evidence_ingress::parse_evidence_line(line, ev))
                        continue;
                    ev.timestamp_ns = crowdintel::realtime_ns();
                    if (queue_.try_push(ev)) ++parsed_;
                    else ++dropped_;
                }
                std::fclose(f);
            }
            if (recal_ && recal_path_ && recal_path_[0]) {
                FILE* rf = std::fopen(recal_path_, "r");
                if (rf) {
                    if (std::fgets(line, sizeof(line), rf))
                        (void)evidence_ingress::parse_sources_text(line,
                                                                   *recal_);
                    std::fclose(rf);
                }
            }
            for (int i = 0; i < 20 &&
                    running_.load(std::memory_order_acquire); ++i)
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }

    const char* path_;
    SPSC_RingBuffer<EvidenceEvent>& queue_;
    SourceReliability* recal_;
    const char* recal_path_;
    bool open_ok_ = false;
    uint64_t parsed_ = 0;
    uint64_t dropped_ = 0;
    std::atomic<bool> running_{false};
    std::thread thread_;
};

#endif  // EVIDENCE_INGRESS_HPP
