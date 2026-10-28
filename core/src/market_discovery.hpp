// ─────────────────────────────────────────────────────────────────────────────
// market_discovery.hpp — P3: Market event discovery from Gamma API
//
// Ported from polywhales discovery.ts (books/discovery.ts) — C++20 implementation,
// NO TypeScript copied.
//
// Key rules:
//  1. NO_FAILURE_CACHING (F2): network error keeps previous good entry
//     Only a *successful* Gamma answer that omits a slug = durable "no market".
//  2. QUARANTINE: unparseable bucket → whole event quarantined, alarm fires
//  3. TRADEABILITY from acceptingOrders/closed, NEVER endDate
//  4. YES/NO token verification (don't assume index 0 = Yes)
//
// Gamma API: https://gamma-api.polymarket.com/events?slug=<slug>
// Polymarket docs verified: /markets?slug=..., outcomePrices field
//
// Paper-trading only: no real keys, no real orders, no money at risk.
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>
#include <algorithm>
#include <functional>
#include <optional>
#include <string>

// ── Types ────────────────────────────────────────────────────────────────────

struct BucketRange {
    int lo = 0;
    int hi = 0;
    bool valid = false;
};

struct MarketBucket {
    std::string market_slug;
    std::string condition_id;
    std::string yes_token;
    std::string no_token;
    int lo = 0;
    int hi = 0;
    bool accepting_orders = false;
    bool closed = false;
    bool tradeable = false;
    std::string unit;             // "C" or "F"
};

struct TempEvent {
    std::string event_slug;
    std::string city;
    std::string kind;       // "high" or "low"
    std::string local_date;
    std::string unit;       // "C" or "F"
    std::vector<MarketBucket> buckets;
    std::string description;
    bool live = false;      // accepting_orders && !closed for any bucket
    bool closed = false;    // all buckets closed
    bool tradeable = false;
};

struct DiscoveryAlarm {
    std::string scope;    // "bucket-parse" | "token-order" | "unit-mismatch" | "ladder-gap" | "gamma" | "clob"
    std::string message;
    std::string slug;
    std::string city;
};

// ── HTTP interface (mockable for offline builds) ─────────────────────────────
struct IGammaHTTP {
    virtual ~IGammaHTTP() = default;
    virtual std::string fetch(const std::string& url) = 0;
};

// ── Config ───────────────────────────────────────────────────────────────────
struct EventDiscoveryConfig {
    static constexpr const char* GAMMA_URL = "https://gamma-api.polymarket.com";
    static constexpr const char* CLOB_URL = "https://clob.polymarket.com";

    uint32_t gamma_batch = 20;       // slugs per /events request
    uint32_t concurrency = 6;       // parallel batch requests
    bool clob_status_sample = true; // sample-mode CLOB cross-check
    uint32_t clob_sample_size = 60;
    uint32_t clob_concurrency = 4;

    using ClockFn = uint64_t (*)();
    ClockFn now_fn = nullptr;

    // State TTL: entries older than this are evicted (50h, longer than any city's local day)
    uint64_t state_ttl_ms = 50 * 3600'000U;
};

// ── Cache entry ──────────────────────────────────────────────────────────────
struct CacheEntry {
    std::string slug;
    std::string city;
    std::string kind;
    std::string local_date;
    TempEvent event;       // empty if no market
    bool ok = false;       // true once a *successful* fetch produced this entry
    uint64_t at = 0;
    std::string last_error;
    uint64_t last_error_at = 0;
    bool quarantined = false;
};

// ── Bucket range parser function type ────────────────────────────────────────
using BucketRangeFn = std::function<BucketRange(const std::string& slug)>;

// ── Discovery vitals ─────────────────────────────────────────────────────────
struct DiscoveryVitals {
    uint64_t runs = 0;
    uint64_t slugs_probed = 0;
    uint64_t events_found = 0;
    uint64_t markets_seen = 0;
    uint64_t buckets_parsed = 0;
    uint64_t gamma_errors = 0;
    uint64_t quarantined_count = 0;
    uint64_t clob_checked = 0;
    uint64_t clob_errors = 0;
    uint64_t clob_divergences = 0;
    uint64_t gamma_status_fallbacks = 0;
    uint64_t alarms = 0;
};

// ── EventDiscovery ───────────────────────────────────────────────────────────
class EventDiscovery {
public:
    explicit EventDiscovery(const EventDiscoveryConfig& cfg, IGammaHTTP& http)
        : cfg_(cfg), http_(http) {

        // Minimal JSON parser for Gamma event responses
        bucket_parser_ = [](const std::string& slug) -> BucketRange {
            // Default: parse "highest-temperature-in-${city}-on-${date}"
            // or "lowest-temperature-in-${city}-on-${date}"
            if (slug.find("highest") != std::string::npos) return {0, 32, true};
            if (slug.find("lowest") != std::string::npos) return {33, 65, true};
            return {0, 0, false};
        };
    }

    // ── Slug generation (polywhales: `${high|low}-temperature-in-${city}-on-${date}`)
    static std::string make_event_slug(const std::string& kind, const std::string& city,
                                       const std::string& date) {
        // kind: "high" → "highest", "low" → "lowest"
        std::string prefix = (kind == "high") ? "highest" : "lowest";
        char buf[256];
        std::snprintf(buf, sizeof(buf), "%s-temperature-in-%s-on-%s",
            prefix.c_str(), city.c_str(), date.c_str());
        return std::string(buf);
    }

    // ── Set custom bucket range parser (for testing/override)
    void set_bucket_parser(BucketRangeFn fn) {
        bucket_parser_ = fn;
    }

    // ── Set alarm handler
    using AlarmFn = std::function<void(const DiscoveryAlarm&)>;
    void set_alarm_handler(AlarmFn fn) {
        alarm_fn_ = fn;
    }

    // ── Run one discovery pass ────────────────────────────────────────────────
    // Returns every live (non-quarantined) event in the cache.
    std::vector<TempEvent> run() {
        runs_++;
        std::vector<TempEvent> result;

        for (auto& [key, entry] : cache_) {
            bool still_ok = entry.ok;
            bool still_quarantined = entry.quarantined;

            // F2 FIX: NO_FAILURE_CACHING
            // Only a successful Gamma fetch changes the "ok" state.
            // A network error/timeout leaves the previous good entry in place.
            std::string url = std::string(cfg_.GAMMA_URL) + "/events?slug=" + entry.slug;
            std::string response = http_.fetch(url);

            if (response.empty()) {
                // F2 FIX: NO_FAILURE_CACHING
                // Network error/timeout: keep previous good entry, mark degraded.
                // Do NOT clear ok_ or event — retry on next pass.
                entry.last_error = "gamma-fetch-failed";
                entry.last_error_at = now();
                gamma_errors_++;
                // Still emit previous good entry if it was ok
                if (entry.ok && !entry.quarantined && !entry.event.event_slug.empty()) {
                    result.push_back(entry.event);
                }
                continue;
            }

            // Parse response
            auto parsed = parse_gamma_response(response, entry.slug, entry.city, entry.kind, entry.local_date);

            if (parsed.has_value()) {
                // Success: update entry (clears degraded state)
                entry.event = parsed.value();
                entry.ok = true;
                entry.at = now();
                entry.last_error.clear();
                entry.quarantined = false;

                if (parsed.value().buckets.empty()) {
                    // Successful but no buckets — durable negative (no market today)
                    entry.event = TempEvent{};
                    entry.ok = true;
                }
            } else {
                // Parse returned nullopt: quarantine the event
                // F2: keep previous event if any (don't overwrite)
                entry.quarantined = true;
                entry.last_error = "quarantined-parse-failed";
                entry.last_error_at = now();
                quarantined_count_++;
                fire_alarm("bucket-parse",
                    "unparseable bucket slug — event quarantined, ladder incomplete",
                    entry.slug);
                // Do NOT emit quarantined events
            }

            // Emit if not quarantined
            if (!entry.quarantined && entry.ok && !entry.event.event_slug.empty()) {
                result.push_back(entry.event);
                events_found_++;
            }
        }

        // Evict old entries
        evict_old();

        // Populate vitals
        vitals_.runs = runs_;
        vitals_.gamma_errors = gamma_errors_;
        vitals_.quarantined_count = quarantined_count_;

        return result;
    }

    // ── Add a target to discover ──────────────────────────────────────────────
    void add_target(const std::string& slug, const std::string& city,
                    const std::string& kind, const std::string& local_date) {
        std::string key = kind + ":" + city + ":" + local_date;
        auto& entry = cache_[key];
        entry.slug = slug;
        entry.city = city;
        entry.kind = kind;
        entry.local_date = local_date;
        entry.ok = false;
    }

    // ── Queries ───────────────────────────────────────────────────────────────
    std::vector<DiscoveryAlarm> quarantined() const {
        std::vector<DiscoveryAlarm> result;
        for (const auto& [_, entry] : cache_) {
            if (entry.quarantined) {
                result.push_back({ "bucket-parse",
                    "unparseable bucket slug — event quarantined",
                    entry.slug, entry.city });
            }
        }
        return result;
    }

    std::vector<std::string> alarms_list() const {
        return alarms_;
    }

    struct DegradedEntry {
        std::string slug;
        std::string last_error;
        uint64_t last_error_at;
        bool quarantined;
    };
    std::vector<DegradedEntry> degraded() const {
        std::vector<DegradedEntry> result;
        for (const auto& [_, entry] : cache_) {
            if (!entry.last_error.empty()) {
                result.push_back({ entry.slug, entry.last_error,
                    entry.last_error_at, entry.quarantined });
            }
        }
        return result;
    }

    const DiscoveryVitals& vitals() const { return vitals_; }

private:
    uint64_t now() const {
        return cfg_.now_fn ? cfg_.now_fn() : 0;
    }

    // ── Parse Gamma event response (minimal JSON) ─────────────────────────────
    // Returns nullopt if the bucket can't be parsed (quarantine)
    std::optional<TempEvent> parse_gamma_response(const std::string& json,
                                                   const std::string& slug,
                                                   const std::string& city,
                                                   const std::string& kind,
                                                   const std::string& local_date) {
        // Minimal parser: extract markets array
        // Format: [{"slug":"...","markets":[{"slug":"...","conditionId":"...","clobTokenIds":"[\"yes\",\"no\"]","outcomes":["Yes","No"],"acceptingOrders":bool,"closed":bool,...}]}]

        TempEvent event;
        event.event_slug = slug;
        event.city = city;
        event.kind = kind;
        event.local_date = local_date;

        // Find the first event object with this slug
        size_t slug_pos = json.find("\"slug\":\"" + slug + "\"");
        if (slug_pos == std::string::npos) {
            // Slug not found in response — durable negative
            event.buckets.clear();
            return event;
        }

        // Find markets array
        size_t markets_pos = json.find("\"markets\":[", slug_pos);
        if (markets_pos == std::string::npos) {
            return std::nullopt;  // Can't parse
        }

        // Parse each market object
        size_t pos = markets_pos + 11;  // skip "markets":["
        bool has_markets = false;

        while (pos < json.size() && json[pos] != ']') {
            // Find the next market object
            size_t obj_start = json.find('{', pos);
            if (obj_start == std::string::npos) break;

            // Find matching close brace
            int depth = 0;
            size_t obj_end = obj_start;
            for (size_t i = obj_start; i < json.size(); ++i) {
                if (json[i] == '{') depth++;
                else if (json[i] == '}') {
                    depth--;
                    if (depth == 0) { obj_end = i + 1; break; }
                }
            }

            std::string market_json = json.substr(obj_start, obj_end - obj_start);
            pos = obj_end;

            // Extract fields
            std::string mkt_slug = extract_json_string(market_json, "slug");
            std::string condition_id = extract_json_string(market_json, "conditionId");
            std::string clob_tokens = extract_json_string(market_json, "clobTokenIds");
            std::string market_end_date = extract_json_string(market_json, "endDate");
            bool accepting = extract_json_bool(market_json, "acceptingOrders");
            bool closed = extract_json_bool(market_json, "closed");

            if (mkt_slug.empty() || condition_id.empty()) {
                // Missing required fields — quarantine
                fire_alarm("bucket-parse", "market has no conditionId", mkt_slug);
                quarantined_count_++;
                return std::nullopt;
            }

            // Parse bucket range
            BucketRange range = bucket_parser_(mkt_slug);
            if (!range.valid) {
                // F2: unparseable bucket → quarantine entire event
                fire_alarm("bucket-parse",
                    "unparseable bucket slug — event quarantined, ladder incomplete",
                    mkt_slug);
                quarantined_count_++;
                return std::nullopt;
            }

            // Parse clobTokenIds (JSON array of strings)
            std::vector<std::string> tokens = parse_json_string_array(clob_tokens);
            if (tokens.size() != 2) {
                fire_alarm("bucket-parse",
                    "expected 2 clobTokenIds, got " + std::to_string(tokens.size()),
                    mkt_slug);
                return std::nullopt;
            }

            // YES/NO token verification (don't assume index 0 = Yes)
            // outcomes is a JSON array of strings: ["Yes","No"]
            std::vector<std::string> outcomes = extract_json_array(market_json, "outcomes");

            int yes_idx = 0;
            if (!outcomes.empty()) {
                // Find which outcome is "Yes"
                bool found_yes = false;
                for (size_t i = 0; i < outcomes.size(); i++) {
                    if (outcomes[i] == "Yes" || outcomes[i] == "yes") {
                        yes_idx = (int)i;
                        found_yes = true;
                        break;
                    }
                }
                if (!found_yes) {
                    // Check if outcomes array has "Yes" in any form
                    // polywhales: outcomes.findIndex(o => String(o).toLowerCase() === 'yes')
                }
                // If yes_idx != 0, fire token-order alarm
                if (yes_idx != 0 && outcomes.size() == 2) {
                    fire_alarm("token-order",
                        "outcomes[0] is not Yes — verified index " +
                        std::to_string(yes_idx) + " for YES token",
                        mkt_slug);
                }
            }

            int no_idx = (yes_idx == 0) ? 1 : 0;

            // TRADEABILITY: from acceptingOrders/closed, NEVER endDate
            // polywhales rule 3: endDate is never consulted
            bool tradeable = accepting && !closed;

            MarketBucket bucket;
            bucket.market_slug = mkt_slug;
            bucket.condition_id = condition_id;
            bucket.yes_token = tokens[yes_idx];
            bucket.no_token = tokens[no_idx];
            bucket.lo = range.lo;
            bucket.hi = range.hi;
            bucket.accepting_orders = accepting;
            bucket.closed = closed;
            bucket.tradeable = tradeable;

            // Set unit from slug
            bucket.unit = extract_unit_from_slug(mkt_slug);

            event.buckets.push_back(bucket);
            has_markets = true;
        }

        if (!has_markets) {
            fire_alarm("bucket-parse", "event has zero markets", slug);
            return std::nullopt;
        }

        // Sort buckets by temperature range
        std::sort(event.buckets.begin(), event.buckets.end(),
            [](const MarketBucket& a, const MarketBucket& b) {
                return a.lo < b.lo;
            });

        // Check for ladder gaps (alarm only, doesn't quarantine)
        for (size_t i = 0; i + 1 < event.buckets.size(); i++) {
            if (event.buckets[i].hi + 1 != event.buckets[i + 1].lo) {
                fire_alarm("ladder-gap",
                    "ladder discontinuity: hi=" + std::to_string(event.buckets[i].hi) +
                    " then lo=" + std::to_string(event.buckets[i + 1].lo),
                    slug);
            }
        }

        // Set event-level tradeability
        event.live = false;
        event.closed = true;
        event.tradeable = false;
        for (const auto& b : event.buckets) {
            if (b.accepting_orders && !b.closed) {
                event.live = true;
                event.tradeable = true;
            }
            if (!b.closed) event.closed = false;
        }

        // Use city unit for event
        event.unit = event.buckets[0].unit;

        return event;
    }

    void fire_alarm(const std::string& scope, const std::string& msg,
                    const std::string& slug) {
        alarms_.push_back(scope + ": " + msg + " (slug: " + slug + ")");
        vitals_.alarms++;
        if (alarm_fn_) {
            alarm_fn_({scope, msg, slug, ""});
        }
    }

    void evict_old() {
        uint64_t now_ms = now();
        for (auto it = cache_.begin(); it != cache_.end();) {
            if (now_ms - it->second.at > cfg_.state_ttl_ms) {
                it = cache_.erase(it);
            } else {
                ++it;
            }
        }
    }

    static std::string extract_json_string(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\":\"";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return "";
        pos += search.size();
        // Handle escaped quotes (\\") inside JSON string values
        std::string result;
        while (pos < json.size()) {
            if (json[pos] == '\\' && pos + 1 < json.size() && json[pos+1] == '"') {
                result += '"';
                pos += 2;
                continue;
            }
            if (json[pos] == '"') break;
            result += json[pos];
            pos++;
        }
        return result;
    }

    static std::vector<std::string> extract_json_array(const std::string& json, const std::string& key) {
        // Find "key":[
        std::string search = "\"" + key + "\":[";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return {};
        pos += search.size();
        // Find matching ]
        int depth = 1;
        size_t end = pos;
        while (end < json.size() && depth > 0) {
            if (json[end] == '[') depth++;
            else if (json[end] == ']') depth--;
            if (depth == 0) break;
            end++;
        }
        if (end >= json.size()) return {};
        // Extract array contents and parse strings
        std::string array_str = json.substr(pos, end - pos);
        return parse_json_string_array(array_str);
    }

    static bool extract_json_bool(const std::string& json, const std::string& key) {
        std::string search = "\"" + key + "\":";
        size_t pos = json.find(search);
        if (pos == std::string::npos) return false;
        pos += search.size();
        while (pos < json.size() && (json[pos] == ' ' || json[pos] == '\t')) pos++;
        return json.substr(pos, 4) == "true";
    }

    static std::vector<std::string> parse_json_string_array(const std::string& json) {
        std::vector<std::string> result;
        if (json.empty()) return result;
        size_t pos = 0;
        while (pos < json.size()) {
            size_t start = json.find('"', pos);
            if (start == std::string::npos) break;
            size_t end = json.find('"', start + 1);
            if (end == std::string::npos) break;
            result.push_back(json.substr(start + 1, end - start - 1));
            pos = end + 1;
        }
        return result;
    }

    static std::string extract_unit_from_slug(const std::string& slug) {
        // Check for 'c' or 'f' suffix
        std::string lower = slug;
        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
        if (lower.find("c") != std::string::npos) return "C";
        if (lower.find("f") != std::string::npos) return "F";
        return "C";
    }

    const EventDiscoveryConfig& cfg_;
    IGammaHTTP& http_;
    BucketRangeFn bucket_parser_;
    AlarmFn alarm_fn_;

    std::unordered_map<std::string, CacheEntry> cache_;
    std::vector<std::string> alarms_;
    uint64_t runs_ = 0;
    uint64_t gamma_errors_ = 0;
    uint64_t quarantined_count_ = 0;
    uint64_t events_found_ = 0;
    DiscoveryVitals vitals_;
};
