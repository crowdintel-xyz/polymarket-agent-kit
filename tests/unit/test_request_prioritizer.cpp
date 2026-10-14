// ─────────────────────────────────────────────────────────────────────────────
// test_request_prioritizer — Phase 3 unit tests
//
// Covered scenarios:
//   1. Priority ordering  — CRITICAL dequeues before HIGH before NORMAL before LOW
//   2. Cancel by id       — removes a specific pending request
//   3. Size per priority  — size(Priority) reflects enqueued count
//
// No heap allocation — FixedRingBuffer is std::array-backed.
// ─────────────────────────────────────────────────────────────────────────────
#include <cstdio>
#include <cstdint>

#include "../../core/src/request_prioritizer.hpp"

// ── Tiny test harness ────────────────────────────────────────────────────────
static int g_failures = 0;
#define CHECK(cond, name)                                                    \
    do {                                                                     \
        if (cond) { std::printf("  PASS %s\n", name); }                      \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);        \
               ++g_failures; }                                               \
    } while (0)

#define CHECK_EQ(actual, expected, name)                                     \
    do {                                                                     \
        if ((actual) == (expected)) {                                        \
            std::printf("  PASS %s\n", name);                                \
        } else {                                                             \
            std::printf("  FAIL %s: got %u, expected %u (line %d)\n",        \
                        name,                                                \
                        static_cast<unsigned>(actual),                       \
                        static_cast<unsigned>(expected),                     \
                        __LINE__);                                           \
            ++g_failures;                                                    \
        }                                                                    \
    } while (0)

// ── Test 1: Priority ordering ────────────────────────────────────────────────
static void test_priority_ordering() {
    std::printf("test_priority_ordering\n");

    RequestPrioritizer qp;

    // Enqueue out of priority order: LOW, NORMAL, HIGH, CRITICAL.
    CHECK(qp.enqueue(Priority::LOW, 1),      "enqueue LOW");
    CHECK(qp.enqueue(Priority::NORMAL, 2),   "enqueue NORMAL");
    CHECK(qp.enqueue(Priority::HIGH, 3),     "enqueue HIGH");
    CHECK(qp.enqueue(Priority::CRITICAL, 4), "enqueue CRITICAL");

    // Dequeue must return CRITICAL first, then HIGH, NORMAL, LOW.
    CHECK(qp.dequeue() == Priority::CRITICAL, "dequeue CRITICAL first");
    CHECK_EQ(qp.last_id(), 4, "CRITICAL id = 4");

    CHECK(qp.dequeue() == Priority::HIGH, "dequeue HIGH second");
    CHECK_EQ(qp.last_id(), 3, "HIGH id = 3");

    CHECK(qp.dequeue() == Priority::NORMAL, "dequeue NORMAL third");
    CHECK_EQ(qp.last_id(), 2, "NORMAL id = 2");

    CHECK(qp.dequeue() == Priority::LOW, "dequeue LOW last");
    CHECK_EQ(qp.last_id(), 1, "LOW id = 1");

    CHECK(qp.empty(), "queue empty after full drain");
}

// ── Test 2: Cancel removes a pending request ─────────────────────────────────
static void test_cancel() {
    std::printf("test_cancel\n");

    RequestPrioritizer qp;

    // Enqueue 3 requests: CRITICAL=10, HIGH=20, NORMAL=30.
    CHECK(qp.enqueue(Priority::CRITICAL, 10), "enqueue CRITICAL 10");
    CHECK(qp.enqueue(Priority::HIGH, 20),     "enqueue HIGH 20");
    CHECK(qp.enqueue(Priority::NORMAL, 30),   "enqueue NORMAL 30");

    CHECK_EQ(qp.size(), 3, "3 requests pending");

    // Cancel the HIGH request (id 20).
    CHECK(qp.cancel(20), "cancel id 20 succeeds");
    CHECK_EQ(qp.size(), 2, "2 requests remaining after cancel");

    // Cancel a non-existent id should fail.
    CHECK(!qp.cancel(999), "cancel nonexistent id 999 fails");

    // Dequeue order should be CRITICAL → NORMAL (HIGH was cancelled).
    CHECK(qp.dequeue() == Priority::CRITICAL, "dequeue CRITICAL next");
    CHECK_EQ(qp.last_id(), 10, "CRITICAL id = 10");

    CHECK(qp.dequeue() == Priority::NORMAL, "dequeue NORMAL next (HIGH was cancelled)");
    CHECK_EQ(qp.last_id(), 30, "NORMAL id = 30");

    CHECK(qp.empty(), "queue empty after drain");
}

// ── Test 3: Size per priority ────────────────────────────────────────────────
static void test_size_per_priority() {
    std::printf("test_size_per_priority\n");

    RequestPrioritizer qp;

    // Enqueue at each priority level.
    CHECK(qp.enqueue(Priority::CRITICAL, 1), "enqueue CRITICAL");
    CHECK(qp.enqueue(Priority::CRITICAL, 2), "enqueue CRITICAL");
    CHECK(qp.enqueue(Priority::HIGH, 3),     "enqueue HIGH");
    CHECK(qp.enqueue(Priority::LOW, 4),      "enqueue LOW");

    // Verify counts per priority.
    CHECK_EQ(qp.size(Priority::CRITICAL), 2, "2 CRITICAL pending");
    CHECK_EQ(qp.size(Priority::HIGH), 1,    "1 HIGH pending");
    CHECK_EQ(qp.size(Priority::NORMAL), 0,  "0 NORMAL pending");
    CHECK_EQ(qp.size(Priority::LOW), 1,     "1 LOW pending");
    CHECK_EQ(qp.size(), 4,                  "4 total pending");

    // Dequeue one CRITICAL — count should drop.
    CHECK(qp.dequeue() == Priority::CRITICAL, "dequeue CRITICAL");
    CHECK_EQ(qp.size(Priority::CRITICAL), 1, "1 CRITICAL after dequeue");
    CHECK_EQ(qp.size(), 3,                   "3 total after dequeue");

    CHECK(!qp.empty(), "not empty after dequeue");

    // Fill the CRITICAL band completely (capacity = 64) and verify it stays full.
    RequestPrioritizer qp2;
    for (uint32_t i = 0; i < 64; ++i) {
        CHECK(qp2.enqueue(Priority::CRITICAL, i), "enqueue fill CRITICAL");
    }
    CHECK_EQ(qp2.size(Priority::CRITICAL), 64, "64 CRITICAL at capacity");
    CHECK_EQ(qp2.size(), 64, "64 total at capacity");

    // 65th push should be rejected (ring buffer full).
    CHECK(!qp2.enqueue(Priority::CRITICAL, 999),
          "65th CRITICAL rejected (capacity exceeded)");
}

// ── Main ─────────────────────────────────────────────────────────────────────
int main() {
    std::printf("⏱️ RequestPrioritizer — Unit Tests\n");
    std::printf("=========================================\n\n");

    test_priority_ordering();
    test_cancel();
    test_size_per_priority();

    std::printf("\n=========================================\n");
    std::printf("📊 Results: %d check(s) failed\n", g_failures);
    return g_failures > 0 ? 1 : 0;
}
