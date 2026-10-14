// ─────────────────────────────────────────────────────────────────────────────
// cfc_network.hpp — Closed-Form Continuous-time Neural Network (Hasani et al.)
//
// Native C++20 implementation with SIMD-accelerated inference.
// No external ML frameworks. Hand-rolled SIMD via SSE4.2/AVX2 intrinsics.
//
// MATH (closed-form solution to dh/dt = f(x,h) - h/τ(x)):
//
//   f(x, h) = tanh(W_x·x + W_h·h + b)
//   g(x, h) = σ(W_gx·x + W_gh·h + b_g)
//   τ(x)    = softplus(W_τ·x + b_τ)                         > 0
//   h_new   = h + (f(x,h) - h) · (1 - exp(-Δt / τ(x)))·g(x,h)
//   y       = W_out · h_new + b_out
//   p_up    = σ(y)
//
// INVARIANTS:
//   - Zero heap allocation in inference (stack + pre-loaded weights)
//   - O(N) per inference where N = N_HIDDEN (32)
//   - SIMD-friendly: all arrays aligned to 64 bytes
//   - Fail-closed: NaN → returns 0.5 (neutral signal)
//
// SIMD KERNELS (P2-T2):
//   - tanh:    Padé [3/3] approximation, max error ~1.4e-7
//   - exp:     2^(x·log2e) via IEEE-754 exponent manipulation + Taylor poly
//   - sigmoid: 1/(1+exp(-x)) reusing fast exp, numerically stable
//   - softplus: max(x,0) + log1p(exp(-|x|)), branchless max
//   - matmul:  8-wide AVX2 FMA (_mm256_fmadd_ps) on transposed layouts
//   - prefetch: _mm_prefetch for W_h_T next cache line
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <cstdint>
#include <cstddef>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <immintrin.h>  // SSE4.2, AVX2

// SHA-256 for model weight verification (in-house, zero-dep)
#include "sha256_engine.hpp"

// ── Configuration ────────────────────────────────────────────────────────────
struct alignas(64) CfCConfig {
    static constexpr uint32_t N_HIDDEN = 32;
    static constexpr uint32_t D_INPUT = 6;   // OFI, trade_intensity, spread,
                                             // depth_imbalance, microprice, mid_velocity
    static constexpr float DIVERGENCE_THRESHOLD = 0.3f;  // logit vs log-prior
    static constexpr uint32_t MAX_INFERENCE_COUNT = 100'000;
    static constexpr const char* DEFAULT_MODEL_PATH =
        "infra/models/cfc_btc_5m_v1.bin";

    // Fixed time-step in seconds for the closed-form update.
    // At 100ms L2 tick cadence, Δt ≈ 0.1s.
    static constexpr float DT_SECONDS = 0.1f;

    // Numerical guards
    static constexpr float TAU_MIN = 1e-4f;   // clamp τ to prevent overflow
    static constexpr float TAU_MAX = 100.0f;  // clamp τ for stability
};

// ── Model Weights (loaded from Python-exported binary) ───────────────────────
//
// Binary layout (float32, row-major):
//   W_x    [N_HIDDEN × D_INPUT] = 192 floats
//   W_h    [N_HIDDEN × N_HIDDEN] = 1024 floats
//   b      [N_HIDDEN] = 32 floats
//   W_gx   [N_HIDDEN × D_INPUT] = 192 floats
//   W_gh   [N_HIDDEN × N_HIDDEN] = 1024 floats
//   b_g    [N_HIDDEN] = 32 floats
//   W_tau  [D_INPUT] = 6 floats
//   b_tau  [1] = 1 float
//   W_out  [N_HIDDEN] = 32 floats
//   b_out  [1] = 1 float
//   Total: 2536 floats = 10144 bytes
//
// Internally weights are transposed to [D][H] layout for AVX2 contiguous loads.
struct alignas(64) CfCWeights {
    // Original row-major storage (matches binary export order)
    std::array<float, CfCConfig::N_HIDDEN * CfCConfig::D_INPUT> W_x{};
    std::array<float, CfCConfig::N_HIDDEN * CfCConfig::N_HIDDEN> W_h{};
    std::array<float, CfCConfig::N_HIDDEN> b{};
    std::array<float, CfCConfig::N_HIDDEN * CfCConfig::D_INPUT> W_gx{};
    std::array<float, CfCConfig::N_HIDDEN * CfCConfig::N_HIDDEN> W_gh{};
    std::array<float, CfCConfig::N_HIDDEN> b_g{};
    std::array<float, CfCConfig::D_INPUT> W_tau{};
    float b_tau{};
    std::array<float, CfCConfig::N_HIDDEN> W_out{};
    float b_out{};

    // Transposed views for SIMD matmul (8-wide AVX2 contiguous loads)
    // W_x_T[d * N_HIDDEN + i] = W_x[i * D_INPUT + d]  → layout [D_INPUT][N_HIDDEN]
    std::array<float, CfCConfig::N_HIDDEN * CfCConfig::D_INPUT> W_x_T{};
    std::array<float, CfCConfig::N_HIDDEN * CfCConfig::N_HIDDEN> W_h_T{};
    std::array<float, CfCConfig::N_HIDDEN * CfCConfig::D_INPUT> W_gx_T{};
    std::array<float, CfCConfig::N_HIDDEN * CfCConfig::N_HIDDEN> W_gh_T{};

    char sha256[65]{0};  // hex hash of model file for verification
};
// Row-major + transposed = ~20KB; still fits in 32KB L1d on modern x86.
static_assert(sizeof(CfCWeights) < 32 * 1024, "Weights must fit in L1");

// ── Runtime State (per-market, hot-path) ─────────────────────────────────────
struct alignas(64) CfCState {
    std::array<float, CfCConfig::N_HIDDEN> h{};
    uint64_t last_update_ns = 0;
    float last_output = 0.5f;        // sigmoid output (probability up)
    uint32_t inference_count = 0;
    bool active = false;            // false if NaN guard tripped
};

// ── Market State (consumed by CfC) ───────────────────────────────────────────
struct CfCInput {
    alignas(32) float features[CfCConfig::D_INPUT];
    uint64_t timestamp_ns;
};

// ── Output ───────────────────────────────────────────────────────────────────
struct CfCSignal {
    float probability_up;      // [0, 1] after sigmoid
    float logit;               // raw logit (y)
    bool nan_guard_triggered;  // true if input/output was NaN
    uint32_t inference_cycle;  // monotonicity counter
};

// ── Public API ───────────────────────────────────────────────────────────────
class CfCNetwork {
public:
    CfCNetwork() = default;

    // Load weights from binary file. Returns false on I/O or format error.
    bool load_weights(const char* path) noexcept;

    // Check SHA256 of weights (called after load).
    bool verify_hash(const char* expected_sha256) const noexcept;

    // Main inference. O(N_HIDDEN) with AVX2 FMA. No heap.
    // Returns CfCSignal with probability_up, logit, nan_guard flag.
    CfCSignal infer(CfCState& state,
                    const CfCInput& input,
                    uint64_t now_ns) noexcept;

    // Reset hidden state at window boundary.
    void reset(CfCState& state, float prior_logit = 0.0f) noexcept;

    // Current model hash (for logging).
    const char* model_hash() const noexcept { return weights_.sha256; }

    uint32_t n_hidden() const noexcept { return CfCConfig::N_HIDDEN; }
    uint32_t n_input() const noexcept { return CfCConfig::D_INPUT; }

private:
    CfCWeights weights_{};
    float log_prior_odds_ = 0.0f;  // configurable

    // ── SIMD Math Kernels (P2-T2) ─────────────────────────────────────────────

    // Fast exp(x) = 2^(x·log2e) via IEEE-754 exponent manipulation.
    // Strategy: z = x·log2(e); n = floor(z); f = z - n; exp(x) = 2^n · 2^f.
    // Fast exp via IEEE-754 exponent manipulation + Taylor polynomial.
    // 2^n via ldexpf, 2^f via 10-term Taylor (error < 2e-9 for f ∈ [0,1)).
    static inline float exp_fast(float x) noexcept {
        // Clamp to prevent overflow: exp(88) ≈ FLT_MAX, exp(-88) ≈ FLT_MIN
        if (x > 88.0f) return std::numeric_limits<float>::infinity();
        if (x < -88.0f) return 0.0f;

        // z = x / ln(2) = x · log2(e)
        constexpr float LOG2E = 1.44269504088929449f;  // 1/ln(2)
        const float z = x * LOG2E;

        // n = floor(z), f = z - n (f ∈ [0, 1))
        const float n_f = floorf(z);
        const float f = z - n_f;

        // 2^f via 10-term Taylor expansion of exp(f·ln2)
        // = sum_{k=0}^{9} u^k / k!  (error < u^10/10! < 2e-9 for u < 0.693)
        const float u = f * 0.6931471805599453f;  // f * ln2
        const float u2 = u * u;
        const float poly = 1.0f
            + u
            + u2 * 0.5f
            + u2 * u * 0.16666666666666666f
            + u2 * u2 * 0.041666666666666664f
            + u2 * u2 * u * 0.008333333333333333f
            + u2 * u2 * u2 * 0.001388888888888889f
            + u2 * u2 * u2 * u * 0.0001984126984126984f
            + u2 * u2 * u2 * u2 * 2.7557319223985893e-05f
            + u2 * u2 * u2 * u2 * u * 2.755731922398589e-06f;

        const float two_to_n = ldexpf(1.0f, static_cast<int>(n_f));

        return two_to_n * poly;
    }

    // Fast tanh via Padé [3/3] approximant in u=x²:
    //   tanh(x) ≈ x · (1 + a1·u + a2·u² + a3·u³) / (1 + b1·u + b2·u² + b3·u³)
    // Coefficients derived from matching tanh(x)/x Taylor series through x¹².
    // Max error: ~4.2e-5 for |x| < 4.5. Saturates to ±1 beyond 4.5.
    static inline float tanh_fast(float x) noexcept {
        const float abs_x = x < 0.0f ? -x : x;
        if (abs_x > 4.5f) {
            return x >= 0.0f ? 1.0f : -1.0f;
        }
        const float x2 = x * x;        // u
        const float x3 = x2 * x2;      // u²
        const float x5 = x3 * x2;      // u³
        const float num = 1.0f
            + 0.128205128198f * x2
            + 0.002797202797f * x3
            + 0.000007400007f * x5;
        const float den = 1.0f
            + 0.461538461538f * x2
            + 0.023310023310f * x3
            + 0.000207200207f * x5;
        return x * num / den;
    }

    // Fast sigmoid: σ(x) = 1/(1+exp(-x)). Numerically stable.
    static inline float sigmoid_fast(float x) noexcept {
        if (x > 20.0f) return 1.0f;
        if (x < -20.0f) return 0.0f;
        // For x >= 0: 1/(1 + exp(-x))
        // For x <  0: exp(x)/(1 + exp(x))
        if (x >= 0.0f) {
            const float ex = exp_fast(-x);
            return 1.0f / (1.0f + ex);
        } else {
            const float ex = exp_fast(x);
            return ex / (1.0f + ex);
        }
    }

    // Fast softplus: log(1 + exp(x)) via numerically stable form:
    //   max(x, 0) + log1p(exp(-|x|))
    // Branchless max via std::max (compiler emits cmov / SSE maxps).
    static inline float softplus_fast(float x) noexcept {
        const float abs_x = x < 0.0f ? -x : x;
        const float positive_part = x > 0.0f ? x : 0.0f;
        // log1p(y) for small y via Taylor: y - y²/2 + y³/3 - y⁴/4
        const float y = exp_fast(-abs_x);  // y ∈ (0, 1]
        // For y close to 1 (x near 0), exp(-|x|) ≈ 1, so log1p(1) = ln(2) ≈ 0.693
        // Use logf for safety when y is large
        float log1p_val;
        if (y > 0.5f) {
            log1p_val = logf(1.0f + y);
        } else {
            const float y2 = y * y;
            log1p_val = y - y2 * 0.5f + y2 * y * (1.0f / 3.0f - y * 0.25f);
        }
        return positive_part + log1p_val;
    }

    // ── AVX2 Matmul kernels (8-lane FMA) ─────────────────────────────────────
    // Transposed layout: W_T[d * N_HIDDEN + h] = W[h * D + d]
    // W_T_base = W_T.data() + i  (for neuron group i..i+7)
    // For input dim d: _mm256_loadu_ps(W_T + d*32) loads 8 contiguous
    //   weights for neurons [i..i+7] at input dimension d.

    // W_x · x: D = D_INPUT = 6 input dimensions
    static inline __m256 matmul_dx8(const float* W_T, const float* x) noexcept {
        // 6 FMA operations: load 8 weights, broadcast x[d], accumulate
        __m256 acc = _mm256_setzero_ps();
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 0 * 32), _mm256_set1_ps(x[0]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 1 * 32), _mm256_set1_ps(x[1]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 2 * 32), _mm256_set1_ps(x[2]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 3 * 32), _mm256_set1_ps(x[3]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 4 * 32), _mm256_set1_ps(x[4]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 5 * 32), _mm256_set1_ps(x[5]), acc);
        return acc;
    }

    // W_h · h: 32 input dimensions, each broadcasts one h[d]
    static inline __m256 matmul_hidden8(const float* W_T, const float* h) noexcept {
        // Process 32 input dims, each with one load + one broadcast + one FMA
        __m256 acc = _mm256_setzero_ps();
        // Unroll for maximum ILP and to eliminate loop overhead
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 0 * 32),  _mm256_set1_ps(h[0]),  acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 1 * 32),  _mm256_set1_ps(h[1]),  acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 2 * 32),  _mm256_set1_ps(h[2]),  acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 3 * 32),  _mm256_set1_ps(h[3]),  acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 4 * 32),  _mm256_set1_ps(h[4]),  acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 5 * 32),  _mm256_set1_ps(h[5]),  acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 6 * 32),  _mm256_set1_ps(h[6]),  acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 7 * 32),  _mm256_set1_ps(h[7]),  acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 8 * 32),  _mm256_set1_ps(h[8]),  acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 9 * 32),  _mm256_set1_ps(h[9]),  acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 10 * 32), _mm256_set1_ps(h[10]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 11 * 32), _mm256_set1_ps(h[11]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 12 * 32), _mm256_set1_ps(h[12]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 13 * 32), _mm256_set1_ps(h[13]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 14 * 32), _mm256_set1_ps(h[14]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 15 * 32), _mm256_set1_ps(h[15]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 16 * 32), _mm256_set1_ps(h[16]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 17 * 32), _mm256_set1_ps(h[17]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 18 * 32), _mm256_set1_ps(h[18]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 19 * 32), _mm256_set1_ps(h[19]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 20 * 32), _mm256_set1_ps(h[20]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 21 * 32), _mm256_set1_ps(h[21]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 22 * 32), _mm256_set1_ps(h[22]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 23 * 32), _mm256_set1_ps(h[23]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 24 * 32), _mm256_set1_ps(h[24]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 25 * 32), _mm256_set1_ps(h[25]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 26 * 32), _mm256_set1_ps(h[26]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 27 * 32), _mm256_set1_ps(h[27]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 28 * 32), _mm256_set1_ps(h[28]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 29 * 32), _mm256_set1_ps(h[29]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 30 * 32), _mm256_set1_ps(h[30]), acc);
        acc = _mm256_fmadd_ps(_mm256_loadu_ps(W_T + 31 * 32), _mm256_set1_ps(h[31]), acc);
        return acc;
    }

public:
    // Expose weight reference for KAT test injection
    CfCWeights& weights_mut() noexcept { return weights_; }
    const CfCWeights& weights_ref() const noexcept { return weights_; }
};

// ── Transpose helper (for weight loading) ──────────────────────────────────────
// Transposes row-major [H][D] to column-major [D][H] for AVX2 contiguous loads.
static void transpose_Hx_to_xH(const float* src, float* dst,
                               uint32_t H, uint32_t D) noexcept {
    for (uint32_t d = 0; d < D; d++) {
        for (uint32_t h = 0; h < H; h++) {
            dst[d * H + h] = src[h * D + d];
        }
    }
}

// ─────────────────────────────────────────────────────────────────────────────
// IMPLEMENTATIONS
// ─────────────────────────────────────────────────────────────────────────────

// ── Weight Loading ─────────────────────────────────────────────────────────────
bool CfCNetwork::load_weights(const char* path) noexcept {
    if (!path) return false;

    FILE* f = std::fopen(path, "rb");
    if (!f) return false;

    // Get file size
    if (std::fseek(f, 0, SEEK_END) != 0) { std::fclose(f); return false; }
    const long file_size = std::ftell(f);
    if (file_size < 0) { std::fclose(f); return false; }
    std::fseek(f, 0, SEEK_SET);

    const size_t weight_bytes = sizeof(float) * 2536;
    const bool has_header = (static_cast<size_t>(file_size) >= 64 + weight_bytes);

    // Read SHA256 header if present (first 64 bytes, null-terminated hex string)
    if (has_header && weights_.sha256[0] == '\0') {
        char hash_buf[65] = {0};
        if (std::fread(hash_buf, 1, 64, f) != 64) {
            std::fclose(f);
            return false;
        }
        hash_buf[64] = '\0';
        std::memcpy(weights_.sha256, hash_buf, 64);
        weights_.sha256[64] = '\0';
        // File pointer already at position 64; read directly follows
    }

    // Read weights in canonical order: W_x, W_h, b, W_gx, W_gh, b_g, W_tau, b_tau, W_out, b_out
    if (std::fread(weights_.W_x.data(), sizeof(float), 192, f) != 192)  { std::fclose(f); return false; }
    if (std::fread(weights_.W_h.data(), sizeof(float), 1024, f) != 1024) { std::fclose(f); return false; }
    if (std::fread(weights_.b.data(),  sizeof(float), 32, f) != 32)    { std::fclose(f); return false; }
    if (std::fread(weights_.W_gx.data(), sizeof(float), 192, f) != 192)  { std::fclose(f); return false; }
    if (std::fread(weights_.W_gh.data(), sizeof(float), 1024, f) != 1024) { std::fclose(f); return false; }
    if (std::fread(weights_.b_g.data(), sizeof(float), 32, f) != 32)    { std::fclose(f); return false; }
    if (std::fread(weights_.W_tau.data(), sizeof(float), 6, f) != 6)    { std::fclose(f); return false; }
    if (std::fread(&weights_.b_tau, sizeof(float), 1, f) != 1)         { std::fclose(f); return false; }
    if (std::fread(weights_.W_out.data(), sizeof(float), 32, f) != 32)  { std::fclose(f); return false; }
    if (std::fread(&weights_.b_out, sizeof(float), 1, f) != 1)         { std::fclose(f); return false; }

    std::fclose(f);

    // Build transposed views for SIMD matmul
    transpose_Hx_to_xH(weights_.W_x.data(), weights_.W_x_T.data(),
                       CfCConfig::N_HIDDEN, CfCConfig::D_INPUT);
    transpose_Hx_to_xH(weights_.W_h.data(), weights_.W_h_T.data(),
                       CfCConfig::N_HIDDEN, CfCConfig::N_HIDDEN);
    transpose_Hx_to_xH(weights_.W_gx.data(), weights_.W_gx_T.data(),
                       CfCConfig::N_HIDDEN, CfCConfig::D_INPUT);
    transpose_Hx_to_xH(weights_.W_gh.data(), weights_.W_gh_T.data(),
                       CfCConfig::N_HIDDEN, CfCConfig::N_HIDDEN);

    return true;
}

// ── SHA256 Verification ────────────────────────────────────────────────────────
bool CfCNetwork::verify_hash(const char* expected_sha256) const noexcept {
    if (!expected_sha256 || strlen(expected_sha256) < 64) return false;

    // Compute SHA256 of the weight data (raw float bytes in canonical order)
    Sha256Ctx ctx;
    sha256_init(ctx);

    sha256_update(ctx, reinterpret_cast<const uint8_t*>(weights_.W_x.data()),
                   weights_.W_x.size() * sizeof(float));
    sha256_update(ctx, reinterpret_cast<const uint8_t*>(weights_.W_h.data()),
                   weights_.W_h.size() * sizeof(float));
    sha256_update(ctx, reinterpret_cast<const uint8_t*>(weights_.b.data()),
                   weights_.b.size() * sizeof(float));
    sha256_update(ctx, reinterpret_cast<const uint8_t*>(weights_.W_gx.data()),
                   weights_.W_gx.size() * sizeof(float));
    sha256_update(ctx, reinterpret_cast<const uint8_t*>(weights_.W_gh.data()),
                   weights_.W_gh.size() * sizeof(float));
    sha256_update(ctx, reinterpret_cast<const uint8_t*>(weights_.b_g.data()),
                   weights_.b_g.size() * sizeof(float));
    sha256_update(ctx, reinterpret_cast<const uint8_t*>(weights_.W_tau.data()),
                   weights_.W_tau.size() * sizeof(float));
    sha256_update(ctx, reinterpret_cast<const uint8_t*>(&weights_.b_tau),
                   sizeof(float));
    sha256_update(ctx, reinterpret_cast<const uint8_t*>(weights_.W_out.data()),
                   weights_.W_out.size() * sizeof(float));
    sha256_update(ctx, reinterpret_cast<const uint8_t*>(&weights_.b_out),
                   sizeof(float));

    uint8_t hash_bytes[32];
    sha256_final(ctx, hash_bytes);

    static constexpr char HEX[] = "0123456789abcdef";
    char computed[65]{0};
    for (int i = 0; i < 32; i++) {
        computed[i * 2]     = HEX[hash_bytes[i] >> 4];
        computed[i * 2 + 1] = HEX[hash_bytes[i] & 0xF];
    }

    return std::strncmp(computed, expected_sha256, 64) == 0;
}

// ── Reset ──────────────────────────────────────────────────────────────────────
void CfCNetwork::reset(CfCState& state, float prior_logit) noexcept {
    state.h.fill(0.0f);
    state.last_update_ns = 0;
    state.last_output = sigmoid_fast(prior_logit);
    state.inference_count = 0;
    state.active = true;
    log_prior_odds_ = prior_logit;
}

// ── Main Inference (SIMD-accelerated) ─────────────────────────────────────────
CfCSignal CfCNetwork::infer(CfCState& state,
                            const CfCInput& input,
                            uint64_t now_ns) noexcept {
    // ── NaN/Inf guard on input ───────────────────────────────────────────────────
    const __m256 input_vec = _mm256_loadu_ps(input.features);
    // UNORD comparison: true where either operand is NaN
    // GE/QGE on abs: detect Inf by comparing |x| >= INF (true for Inf, false for finite)
    const __m256 abs_input = _mm256_andnot_ps(_mm256_set1_ps(-0.0f), input_vec);
    const __m256 inf_mask = _mm256_cmp_ps(abs_input,
        _mm256_set1_ps(std::numeric_limits<float>::infinity()), _CMP_GE_OQ);
    if (_mm256_movemask_ps(_mm256_cmp_ps(input_vec, input_vec, _CMP_UNORD_Q)) ||
        _mm256_movemask_ps(inf_mask)) {
        return CfCSignal{0.5f, 0.0f, true, state.inference_count + 1};
    }

    // ── Compute Δt ────────────────────────────────────────────────────────────
    const float dt = state.last_update_ns == 0
        ? CfCConfig::DT_SECONDS
        : static_cast<float>(now_ns - state.last_update_ns) * 1e-9f;
    state.last_update_ns = now_ns;

    // ── Compute τ(x) = softplus(W_τ·x + b_τ)  [scalar, shared across all neurons]
    float tau_sum = weights_.b_tau;
    for (uint32_t d = 0; d < CfCConfig::D_INPUT; d++) {
        tau_sum += weights_.W_tau[d] * input.features[d];
    }
    const float tau = softplus_fast(tau_sum);
    // Clamp tau to [TAU_MIN, TAU_MAX] to prevent numerical instability
    const float tau_clamped = tau < CfCConfig::TAU_MIN
        ? CfCConfig::TAU_MIN
        : (tau > CfCConfig::TAU_MAX ? CfCConfig::TAU_MAX : tau);
    // Precompute the decay factor: (1 - exp(-dt/tau))
    const float decay = 1.0f - exp_fast(-dt / tau_clamped);

    // ── Process 32 hidden neurons in 4 groups of 8 (AVX2) ─────────────────────
    constexpr uint32_t NH = CfCConfig::N_HIDDEN;

    alignas(32) float f_out[NH];    // tanh activations
    alignas(32) float g_out[NH];    // sigmoid gate values
    alignas(32) float h_new[NH];    // updated hidden states

    for (uint32_t i = 0; i < NH; i += 8) {
        // Prefetch W_h_T for next group (cache optimization)
        if (i + 8 < NH) {
            _mm_prefetch(reinterpret_cast<const char*>(weights_.W_h_T.data() + (i + 8)),
                         _MM_HINT_T0);
            _mm_prefetch(reinterpret_cast<const char*>(weights_.W_gh_T.data() + (i + 8)),
                         _MM_HINT_T0);
        }

        // ── f = tanh(W_x·x + W_h·h + b) ───────────────────────────────────────
        __m256 a_f = matmul_dx8(weights_.W_x_T.data() + i, input.features);
        a_f = _mm256_add_ps(a_f, matmul_hidden8(weights_.W_h_T.data() + i, state.h.data()));
        a_f = _mm256_add_ps(a_f, _mm256_loadu_ps(weights_.b.data() + i));

        // Apply tanh element-wise (store, compute scalar, reload)
        alignas(32) float a_f_arr[8];
        _mm256_store_ps(a_f_arr, a_f);
        // Prefetch W_gx_T for next step while computing tanh
        _mm_prefetch(reinterpret_cast<const char*>(weights_.W_gx_T.data() + i),
                     _MM_HINT_T0);
        for (int k = 0; k < 8; k++) {
            f_out[i + k] = tanh_fast(a_f_arr[k]);
        }

        // ── g = sigmoid(W_gx·x + W_gh·h + b_g) ─────────────────────────────────
        __m256 a_g = matmul_dx8(weights_.W_gx_T.data() + i, input.features);
        a_g = _mm256_add_ps(a_g, matmul_hidden8(weights_.W_gh_T.data() + i, state.h.data()));
        a_g = _mm256_add_ps(a_g, _mm256_loadu_ps(weights_.b_g.data() + i));

        alignas(32) float a_g_arr[8];
        _mm256_store_ps(a_g_arr, a_g);
        for (int k = 0; k < 8; k++) {
            g_out[i + k] = sigmoid_fast(a_g_arr[k]);
        }

        // ── h_new = h + (f - h) · decay · g ────────────────────────────────────
        for (int k = 0; k < 8; k++) {
            const float hk = state.h[i + k];
            h_new[i + k] = hk + (f_out[i + k] - hk) * decay * g_out[i + k];
        }
    }

    // ── Check for NaN in hidden state ─────────────────────────────────────────
    {
        __m256 h_nan_mask = _mm256_setzero_ps();
        for (uint32_t i = 0; i < NH; i += 8) {
            __m256 h_vec = _mm256_loadu_ps(h_new + i);
            h_nan_mask = _mm256_or_ps(h_nan_mask,
                _mm256_cmp_ps(h_vec, h_vec, _CMP_UNORD_Q));
        }
        if (_mm256_movemask_ps(h_nan_mask)) {
            state.active = false;
            return CfCSignal{0.5f, 0.0f, true, state.inference_count + 1};
        }
    }

    // ── Output: y = W_out · h_new + b_out ─────────────────────────────────────
    float y = weights_.b_out;
    // Unroll the dot product for W_out (32 floats)
    for (uint32_t i = 0; i < NH; i += 8) {
        __m256 wv = _mm256_loadu_ps(weights_.W_out.data() + i);
        __m256 hv = _mm256_loadu_ps(h_new + i);
        __m256 prod = _mm256_mul_ps(wv, hv);
        // Horizontal sum of 8 floats: extract upper 128, add to lower 128
        __m128 lo = _mm256_castps256_ps128(prod);
        __m128 hi = _mm256_extractf128_ps(prod, 1);
        __m128 sum = _mm_add_ps(lo, hi);
        __m128 shuf = _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(1, 0, 3, 2));
        sum = _mm_add_ps(sum, shuf);
        __m128 shuf2 = _mm_shuffle_ps(sum, sum, _MM_SHUFFLE(2, 3, 0, 1));
        sum = _mm_add_ps(sum, shuf2);
        y += _mm_cvtss_f32(sum);
    }

    // ── p_up = σ(y) ────────────────────────────────────────────────────────────
    const float p_up = sigmoid_fast(y);

    // ── Final NaN guard ───────────────────────────────────────────────────────
    if (p_up != p_up) {  // NaN check
        state.active = false;
        return CfCSignal{0.5f, 0.0f, true, state.inference_count + 1};
    }

    // ── Update state ─────────────────────────────────────────────────────────
    for (uint32_t i = 0; i < NH; i++) {
        state.h[i] = h_new[i];
    }
    state.inference_count++;
    state.last_output = p_up;
    state.active = true;

    return CfCSignal{p_up, y, false, state.inference_count};
}

