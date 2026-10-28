// ─────────────────────────────────────────────────────────────────────────────
// test_market_discovery.cpp — P3: Market event discovery (Gamma API)
//
// Ported from polywhales discovery.ts (books/discovery.ts) — C++20 implementation,
// NO TypeScript copied.
//
// Key rules ported:
//  1. NO FAILURE_CACHING (F2): network error keeps previous good entry
//  2. QUARANTINE: unparseable bucket → quarantine entire event
//  3. TRADEABILITY from acceptingOrders/closed, NOT endDate
//
// Tests use a mock HTTP responder (no network calls).
// ─────────────────────────────────────────────────────────────────────────────
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <unordered_map>

#include "../../core/src/market_discovery.hpp"

static int g_failures = 0;
#define CHECK(cond, msg) do { \
    if (!(cond)) { printf("  \u274C FAIL: %s\n", msg); g_failures++; } \
    else { printf("  \u2705 PASS: %s\n", msg); } \
} while(0)

// ── Mock clock ────────────────────────────────────────────────────────────────
static uint64_t g_mock_ms = 1'000'000;
static uint64_t mock_clock() { return g_mock_ms; }
static void mock_set_time(uint64_t ms) { g_mock_ms = ms; }

// ── Mock HTTP responder ───────────────────────────────────────────────────────
struct MockHTTP : public IGammaHTTP {
    std::string last_url;
    int call_count = 0;
    std::unordered_map<std::string, std::string> slug_to_response;
    std::string fail_url;  // if set, this URL fails

    std::string fetch(const std::string& url) {
        last_url = url;
        call_count++;
        if (url == fail_url) return "";  // simulate failure
        auto it = slug_to_response.find(extract_slug(url));
        if (it != slug_to_response.end()) return it->second;
        return "";
    }

    std::string extract_slug(const std::string& url) {
        auto pos = url.find("slug=");
        if (pos == std::string::npos) return "";
        pos += 5;
        auto end = url.find('&', pos);
        if (end == std::string::npos) end = url.size();
        return url.substr(pos, end - pos);
    }
};

// ── Mock bucket range for testing ─────────────────────────────────────────────
struct TestBucketRange {
    int lo;
    int hi;
    bool valid;
};

static BucketRange mock_bucket_range(const std::string& slug) {
    if (slug.find("highest") != std::string::npos) return {0, 32, true};
    if (slug.find("lowest") != std::string::npos) return {33, 65, true};
    if (slug.find("broken") != std::string::npos) return {0, 0, false};
    if (slug.find("test-market") != std::string::npos) return {0, 32, true};
    if (slug.find("tradeable-event") != std::string::npos) return {0, 32, true};
    if (slug.find("yes-first") != std::string::npos) return {0, 32, true};
    if (slug.find("no-first") != std::string::npos) return {0, 32, true};
    return {0, 0, false};
}

int main() {
    printf("=== P3: Market Event Discovery ===\n");

    // ── Test 1: Slug generation ───────────────────────────────────────────────
    printf("\n--- Test 1: Slug generation ---\n");
    {
        // polywhales: `${highest|lowest}-temperature-in-${city}-on-${dateSlug}`
        std::string slug = EventDiscovery::make_event_slug("high", "austin", "2026-08-26");
        CHECK(slug == "highest-temperature-in-austin-on-2026-08-26", "high → highest slug");
        CHECK(slug.find("austin") != std::string::npos, "slug contains city");

        std::string slug2 = EventDiscovery::make_event_slug("low", "denver", "2026-08-27");
        CHECK(slug2 == "lowest-temperature-in-denver-on-2026-08-27", "low → lowest slug");
    }

    // ── Test 2: NO_FAILURE_CACHING (F2 bug) ─────────────────────────────────────
    printf("\n--- Test 2: NO_FAILURE_CACHING (F2 fix) ---\n");
    {
        mock_set_time(1'000'000);

        MockHTTP http;
        // First request succeeds
        http.slug_to_response["test-market"] = R"([{"slug":"test-market","markets":[{"slug":"test-market","conditionId":"0xabc","clobTokenIds":"[\"yes\",\"no\"]","outcomes":["Yes","No"]"}]}])";

        EventDiscoveryConfig cfg{};
        cfg.gamma_batch = 1;
        cfg.now_fn = mock_clock;

        EventDiscovery disc(cfg, http);
        disc.set_bucket_parser(mock_bucket_range);

        // Add target before running
        disc.add_target("test-market", "testcity", "high", "2026-08-26");

        auto events_before = disc.run();
        CHECK(events_before.size() >= 1, "1 event found on success");
        CHECK(events_before[0].buckets.size() >= 1, "event has at least 1 bucket");

        // Now simulate network failure (Gamma error)
        http.fail_url = "https://gamma-api.polymarket.com/events?slug=test-market";
        auto events_after_fail = disc.run();
        CHECK(events_after_fail.size() >= 1, "F2 FIXED: previous good entry retained on error");
        CHECK(disc.degraded().size() >= 1, "previous entry marked degraded");

        // Recovery: clear failure
        http.fail_url = "";
        auto events_recovered = disc.run();
        CHECK(events_recovered.size() == 1, "recovers after successful re-fetch");
        CHECK(disc.degraded().size() == 0, "no degraded entries after recovery");
    }

    // ── Test 3: QUARANTINE on unparseable bucket ───────────────────────────────
    printf("\n--- Test 3: Quarantine unparseable bucket ---\n");
    {
        mock_set_time(1'000'000);

        MockHTTP http;
        // Response with a market we can't parse the bucket from
        http.slug_to_response["broken-event"] = R"([{"slug":"broken-event","markets":[{"slug":"broken-weird-slug-abc","conditionId":"0xdef","clobTokenIds":"[\"yes\",\"no\"]","outcomes":["Yes","No"]}]}])";

        EventDiscoveryConfig cfg{};
        cfg.gamma_batch = 1;
        cfg.now_fn = mock_clock;

        EventDiscovery disc(cfg, http);
        disc.set_bucket_parser(mock_bucket_range);

        // Add target
        disc.add_target("broken-event", "testcity", "high", "2026-08-26");

        std::vector<std::string> alarms;
        disc.set_alarm_handler([&](const DiscoveryAlarm& a) {
            alarms.push_back(a.scope + std::string(": ") + a.message);
        });

        auto events = disc.run();
        CHECK(events.size() == 0, "quarantined event not emitted as live");
        CHECK(alarms.size() > 0, "alarm fired for unparseable bucket");
        CHECK(disc.quarantined().size() > 0, "event quarantined");

        // Verify alarm reason mentions bucket-parse
        bool found_bucket_alarm = false;
        for (const auto& a : alarms) {
            if (a.find("bucket-parse") != std::string::npos) {
                found_bucket_alarm = true;
                break;
            }
        }
        CHECK(found_bucket_alarm, "alarm scope is 'bucket-parse'");
    }

    // ── Test 4: Tradeability from acceptingOrders/closed, NOT endDate ──────────
    printf("\n--- Test 4: Tradeability from acceptingOrders/closed ---\n");
    {
        mock_set_time(1'000'000);

        MockHTTP http;
        // Market with endDate but acceptingOrders=false (should be NOT tradeable)
        http.slug_to_response["tradeable-event"] = R"([{"slug":"tradeable-event","markets":[{"slug":"highest-temperature-in-test-on-2026-08-26","conditionId":"0x123","clobTokenIds":"[\"yes\",\"no\"]","outcomes":["Yes","No"],"acceptingOrders":false,"closed":true,"endDate":"2026-08-26T12:00:00Z"}]}])";

        EventDiscoveryConfig cfg{};
        cfg.gamma_batch = 1;
        cfg.now_fn = mock_clock;

        EventDiscovery disc(cfg, http);
        disc.set_bucket_parser(mock_bucket_range);

        // Add target
        disc.add_target("tradeable-event", "testcity", "high", "2026-08-26");

        auto events = disc.run();
        CHECK(events.size() > 0, "event found");
        CHECK(!events[0].live, "event marked NOT live (acceptingOrders=false)");
        CHECK(events[0].closed, "event marked closed=true");
        CHECK(events[0].tradeable == false, "event NOT tradeable despite endDate present");
    }

    // ── Test 5: YES/NO token verification ───────────────────────────────────────
    printf("\n--- Test 5: YES/NO token ordering verification ---\n");
    {
        mock_set_time(1'000'000);

        MockHTTP http;
        // Outcomes with YES first (expected order)
        http.slug_to_response["yes-first"] = R"([{"slug":"yes-first","markets":[{"slug":"highest-temperature-in-test-on-2026-08-26","conditionId":"0x456","clobTokenIds":"[\"token-yes\",\"token-no\"]","outcomes":["Yes","No"],"acceptingOrders":true,"closed":false}]}])";

        EventDiscoveryConfig cfg{};
        cfg.gamma_batch = 1;
        cfg.now_fn = mock_clock;

        EventDiscovery disc(cfg, http);
        disc.set_bucket_parser(mock_bucket_range);

        // Add target
        disc.add_target("yes-first", "testcity", "high", "2026-08-26");

        std::vector<std::string> alarms;
        bool found_yes = false;
        bool found_no = false;

        auto events = disc.run();
        CHECK(events.size() > 0, "event found");
        CHECK(events[0].buckets.size() > 0, "has buckets");

        // Verify YES token is at index 0
        for (const auto& b : events[0].buckets) {
            if (b.yes_token == "token-yes") found_yes = true;
            if (b.no_token == "token-no") found_no = true;
        }
        CHECK(found_yes, "YES token correctly identified at index 0");
        CHECK(found_no, "NO token correctly identified at index 1");

        // Now test reversed outcomes order
        http.slug_to_response["no-first"] = R"([{"slug":"no-first","markets":[{"slug":"highest-temperature-in-test-on-2026-08-27","conditionId":"0x789","clobTokenIds":"[\"token-yes2\",\"token-no2\"]","outcomes":["No","Yes"],"acceptingOrders":true,"closed":false}]}])";

        EventDiscovery disc2(cfg, http);
        disc2.set_bucket_parser(mock_bucket_range);
        // Add target for reversed outcomes
        disc2.add_target("no-first", "testcity", "high", "2026-08-27");
        disc2.set_alarm_handler([&](const DiscoveryAlarm& a) {
            alarms.push_back(a.scope + std::string(": ") + a.message);
        });

        auto events2 = disc2.run();
        CHECK(events2.size() > 0, "event with reversed outcomes found");

        // Should have fired token-order alarm
        bool found_token_order_alarm = false;
        for (const auto& a : alarms) {
            if (a.find("token-order") != std::string::npos) {
                found_token_order_alarm = true;
                break;
            }
        }
        CHECK(found_token_order_alarm, "token-order alarm fired for reversed outcomes");
    }

    printf("\n  P3 test complete: %d failures\n", g_failures);
    return g_failures > 0 ? 1 : 0;
}
