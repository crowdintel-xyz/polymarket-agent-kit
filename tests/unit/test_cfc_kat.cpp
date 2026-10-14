// ─────────────────────────────────────────────────────────────────────────────
// test_cfc_kat.cpp — Known-Answer Tests for C++20 CfC inference kernel
//
// Reads vectors from core/src/cfc_network_test.hpp (auto-generated from
// ml_training/kat_vectors.json, which was produced by the PyTorch reference
// in ml_training/train_cfc.py).  Each vector feeds the C++ infer() and
// compares against Python reference outputs within ±1e-4 tolerance.
//
// Test categories:
//   1. KAT vectors: 10 pre-computed vectors from PyTorch reference
//   2. SHA256 hash verification of model weights
//   3. Binary file loading (load_weights)
//   4. Reset semantics (hidden state cleared, prior logit applied)
//   5. NaN guard (fail-closed: NaN → 0.5)
//   6. Edge cases (zero input, large input, sequential inference)
//   7. Determinism (same input → same output)
//   8. API surface (n_hidden, n_input, model_hash)
//
// Exit code 0 = all pass. Zero external deps.
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <string>

#include "../../core/src/cfc_network.hpp"
#include "../../core/src/cfc_network_test.hpp"

// ── Test framework ─────────────────────────────────────────────────────────────
static int g_failures = 0;
static int g_tests = 0;

#define CHECK(cond, name)                                                     \
    do {                                                                      \
        ++g_tests;                                                            \
        if (cond) { std::printf("  PASS %s\n", name); }                       \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);          \
               ++g_failures; }                                                \
    } while (0)

static bool approxf(float a, float b, float eps = 1e-4f) {
    return std::fabs(a - b) <= eps * (1.0f + std::fabs(a) + std::fabs(b));
}

// ── Test 1: KAT vectors — core known-answer verification ───────────────────────
static void test_kat_vectors() {
    std::printf("\n=== KAT Vectors (P2-T3 reference match) ===\n");

    CfCNetwork net;
    load_kat_weights(net);

    // Verify SHA256 hash matches expected
    CHECK(net.verify_hash(CFC_KAT_SHA256), "SHA256 verification of KAT weights");

    int pass_count = 0;
    for (int i = 0; i < CFC_KAT_N_VECTORS; i++) {
        const auto& v = CFC_KAT_VECTORS[i];

        // Setup state with known initial hidden state
        CfCState state{};
        net.reset(state, 0.0f);
        for (uint32_t k = 0; k < CfCConfig::N_HIDDEN; k++) {
            state.h[k] = v.h_init[k];
        }

        // dt handling: if last_update_ns == 0, dt = DT_SECONDS (0.1)
        // Otherwise dt = now_ns - last_update_ns.
        // KAT vectors use arbitrary dt, so set last_update_ns to a small
        // non-zero offset to force the dt branch.
        uint64_t now_ns;
        if (v.dt == CfCConfig::DT_SECONDS) {
            state.last_update_ns = 0;
            now_ns = 0;
        } else {
            state.last_update_ns = 1000;  // small non-zero offset
            now_ns = static_cast<uint64_t>(v.dt * 1e9) + 1000;
        }

        // Build input
        CfCInput input{};
        std::memcpy(input.features, v.input, sizeof(float) * CfCConfig::D_INPUT);
        input.timestamp_ns = now_ns;

        // Run inference
        CfCSignal result = net.infer(state, input, now_ns);

        char label[128];

        // Check logit
        bool logit_ok = approxf(result.logit, v.y_ref, 1e-4f);
        snprintf(label, sizeof(label), "KAT[%d] logit (%.10f vs %.10f)", i,
                 result.logit, v.y_ref);
        CHECK(logit_ok, label);

        // Check probability
        bool prob_ok = approxf(result.probability_up, v.p_up_ref, 1e-4f);
        snprintf(label, sizeof(label), "KAT[%d] p_up (%.10f vs %.10f)", i,
                 result.probability_up, v.p_up_ref);
        CHECK(prob_ok, label);

        // Check no NaN guard
        snprintf(label, sizeof(label), "KAT[%d] no_nan_guard", i);
        CHECK(!result.nan_guard_triggered, label);

        // Check inference count
        snprintf(label, sizeof(label), "KAT[%d] inference_count", i);
        CHECK(result.inference_cycle == 1, label);

        // Check hidden state update
        bool hidden_ok = true;
        float max_diff = 0.0f;
        for (uint32_t k = 0; k < CfCConfig::N_HIDDEN; k++) {
            float diff = std::fabs(state.h[k] - v.h_new_ref[k]);
            if (diff > max_diff) max_diff = diff;
            if (diff > 1e-4f) hidden_ok = false;
        }
        snprintf(label, sizeof(label), "KAT[%d] h_new (max_diff=%.2e)", i, max_diff);
        CHECK(hidden_ok, label);

        if (logit_ok && prob_ok && hidden_ok && !result.nan_guard_triggered) {
            ++pass_count;
        }
    }

    std::printf("  KAT: %d/%d vectors fully match Python reference\n",
                pass_count, CFC_KAT_N_VECTORS);
}

// ── Test 2: Binary file loading ─────────────────────────────────────────────────
static void test_binary_loading() {
    std::printf("\n=== Binary File Loading ===\n");

    CfCNetwork net;
    const char* model_path = "infra/models/cfc_btc_5m_v1.bin";

    bool loaded = net.load_weights(model_path);
    if (!loaded) {
        // Try alternate path
        loaded = net.load_weights("/home/adlg/Escritorio/Proyectos/Bot_BajaLatencia/Bot_Crowdintel/infra/models/cfc_btc_5m_v1.bin");
    }
    CHECK(loaded, "load_weights from binary file");

    if (loaded) {
        CHECK(net.verify_hash(CFC_KAT_SHA256), "verify_hash of loaded weights");
        CHECK(std::strlen(net.model_hash()) == 64, "model_hash() returns 64-char hex");
        CHECK(net.model_hash()[0] != '\0', "model_hash() is non-empty");
        CHECK(net.n_hidden() == 32, "n_hidden()==32");
        CHECK(net.n_input() == 6, "n_input()==6");
    }
}

// ── Test 3: Reset semantics ────────────────────────────────────────────────────
static void test_reset() {
    std::printf("\n=== Reset Semantics ===\n");

    CfCNetwork net;
    load_kat_weights(net);

    CfCState state{};
    // Set some non-zero state
    for (uint32_t i = 0; i < CfCConfig::N_HIDDEN; i++) {
        state.h[i] = 0.5f;
    }
    state.last_update_ns = 999999;
    state.last_output = 0.7f;
    state.inference_count = 42;
    state.active = false;

    // Reset with prior_logit=0.0 → sigmoid(0.0) = 0.5
    net.reset(state, 0.0f);

    bool h_zeroed = true;
    for (uint32_t i = 0; i < CfCConfig::N_HIDDEN; i++) {
        if (state.h[i] != 0.0f) h_zeroed = false;
    }
    CHECK(h_zeroed, "reset zeroes hidden state");
    CHECK(state.last_update_ns == 0, "reset zeros last_update_ns");
    CHECK(state.inference_count == 0, "reset zeros inference_count");
    CHECK(state.active == true, "reset sets active=true");
    CHECK(approxf(state.last_output, 0.5f, 1e-6f), "reset prior_logit=0 → 0.5");

    // Reset with prior_logit=1.0 → sigmoid(1.0) ≈ 0.7311
    net.reset(state, 1.0f);
    CHECK(approxf(state.last_output, 0.7310590f, 1e-5f), "reset prior_logit=1.0 → sigmoid(1.0)");
    CHECK(approxf(state.last_output, 1.0f / (1.0f + std::exp(-1.0f)), 1e-6f),
          "reset prior_logit uses sigmoid_fast (matches std::exp)");
}

// ── Test 4: NaN guard (fail-closed) ─────────────────────────────────────────────
static void test_nan_guard() {
    std::printf("\n=== NaN Guard (fail-closed) ===\n");

    CfCNetwork net;
    load_kat_weights(net);

    CfCState state{};
    net.reset(state);

    // Input with NaN
    CfCInput bad_input{};
    for (uint32_t i = 0; i < CfCConfig::D_INPUT; i++) {
        bad_input.features[i] = 0.5f;
    }
    bad_input.features[3] = std::numeric_limits<float>::quiet_NaN();
    bad_input.timestamp_ns = 1000;

    CfCSignal result = net.infer(state, bad_input, 2000);
    CHECK(result.nan_guard_triggered, "NaN input triggers guard");
    CHECK(approxf(result.probability_up, 0.5f, 1e-6f), "NaN input returns 0.5");
    CHECK(approxf(result.logit, 0.0f, 1e-6f), "NaN input returns logit 0.0");

    // Input with Inf
    CfCState state2{};
    net.reset(state2);
    CfCInput inf_input{};
    for (uint32_t i = 0; i < CfCConfig::D_INPUT; i++) {
        inf_input.features[i] = 0.5f;
    }
    inf_input.features[0] = std::numeric_limits<float>::infinity();
    inf_input.timestamp_ns = 1000;

    CfCSignal result2 = net.infer(state2, inf_input, 2000);
    CHECK(result2.nan_guard_triggered, "Inf input triggers guard");
    CHECK(approxf(result2.probability_up, 0.5f, 1e-6f), "Inf input returns 0.5");
}

// ── Test 5: Edge cases ─────────────────────────────────────────────────────────
static void test_edge_cases() {
    std::printf("\n=== Edge Cases ===\n");

    CfCNetwork net;
    load_kat_weights(net);

    // Zero input
    CfCState state{};
    net.reset(state);
    CfCInput zero_input{};
    zero_input.timestamp_ns = 1000;
    CfCSignal result = net.infer(state, zero_input, 1000);
    CHECK(!result.nan_guard_triggered, "zero input: no NaN guard");
    CHECK(result.probability_up >= 0.0f && result.probability_up <= 1.0f,
          "zero input: output in [0,1]");

    // Large input (stress test)
    CfCState state2{};
    net.reset(state2);
    CfCInput large_input{};
    for (uint32_t i = 0; i < CfCConfig::D_INPUT; i++) {
        large_input.features[i] = 100.0f;
    }
    large_input.timestamp_ns = 1000;
    CfCSignal result2 = net.infer(state2, large_input, 1100);
    CHECK(!result2.nan_guard_triggered, "large input: no NaN guard");
    CHECK(result2.probability_up >= 0.0f && result2.probability_up <= 1.0f,
          "large input: output in [0,1]");

    // Sequential inference (hidden state persists, monotonicity)
    CfCState state3{};
    net.reset(state3);
    for (uint32_t t = 0; t < 10; t++) {
        CfCInput inp{};
        for (uint32_t i = 0; i < CfCConfig::D_INPUT; i++) {
            inp.features[i] = static_cast<float>(t * 0.1 + i * 0.01);
        }
        inp.timestamp_ns = static_cast<uint64_t>(t * 100'000'000);
        uint64_t now_ns = static_cast<uint64_t>((t + 1) * 100'000'000);
        CfCSignal r = net.infer(state3, inp, now_ns);
        CHECK(r.inference_cycle == static_cast<uint32_t>(t + 1),
              "sequential: inference_count increments");
        CHECK(!r.nan_guard_triggered, "sequential: no NaN");
    }
    CHECK(state3.inference_count == 10, "state tracks 10 inferences");
}

// ── Test 6: Determinism ────────────────────────────────────────────────────────
static void test_determinism() {
    std::printf("\n=== Determinism ===\n");

    CfCNetwork net;
    load_kat_weights(net);

    const auto& v = CFC_KAT_VECTORS[0];

    // Run twice with identical inputs
    CfCState s1{}, s2{};
    net.reset(s1);
    net.reset(s2);
    for (uint32_t k = 0; k < CfCConfig::N_HIDDEN; k++) {
        s1.h[k] = v.h_init[k];
        s2.h[k] = v.h_init[k];
    }

    CfCInput input{};
    std::memcpy(input.features, v.input, sizeof(float) * CfCConfig::D_INPUT);

    uint64_t now_ns = static_cast<uint64_t>(v.dt * 1e9) + 1000;
    s1.last_update_ns = 1000;
    s2.last_update_ns = 1000;

    CfCSignal r1 = net.infer(s1, input, now_ns);
    CfCSignal r2 = net.infer(s2, input, now_ns);

    CHECK(approxf(r1.logit, r2.logit, 0.0f), "determinism: logits identical");
    CHECK(approxf(r1.probability_up, r2.probability_up, 0.0f),
          "determinism: probabilities identical");

    // Hidden states identical
    bool h_match = true;
    for (uint32_t k = 0; k < CfCConfig::N_HIDDEN; k++) {
        if (s1.h[k] != s2.h[k]) h_match = false;
    }
    CHECK(h_match, "determinism: hidden states identical");
}

// ── Test 7: API surface ────────────────────────────────────────────────────────
static void test_api() {
    std::printf("\n=== API Surface ===\n");

    CfCNetwork net;
    CHECK(net.n_hidden() == 32, "n_hidden()==32");
    CHECK(net.n_input() == 6, "n_input()==6");
    CHECK(std::string(net.model_hash()) == "", "initial model_hash is empty");

    // Test that load_weights + verify_hash work together
    load_kat_weights(net);
    CHECK(net.verify_hash(CFC_KAT_SHA256), "verify_hash after load_kat_weights");
    CHECK(std::string(net.model_hash()) == CFC_KAT_SHA256, "model_hash matches after load");
}

// ── Main ───────────────────────────────────────────────────────────────────────
int main() {
    std::printf("=== CROWDINTEL CfC Network — Known-Answer Tests ===\n");
    std::printf("N_HIDDEN=%u, D_INPUT=%u, DT=%.3fs\n",
                CfCConfig::N_HIDDEN, CfCConfig::D_INPUT, CfCConfig::DT_SECONDS);

    test_kat_vectors();
    test_binary_loading();
    test_reset();
    test_nan_guard();
    test_edge_cases();
    test_determinism();
    test_api();

    std::printf("\n=== Summary ===\n");
    std::printf("  Total checks: %d\n", g_tests);
    std::printf("  Failures:     %d\n", g_failures);
    std::printf("  Result:       %s\n", g_failures ? "FAILED" : "ALL PASS");

    return g_failures ? 1 : 0;
}
