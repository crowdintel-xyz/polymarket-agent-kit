// CPU-path microbenchmark.  It deliberately excludes network/HMAC/TLS and
// labels results accordingly.  Presigned slots are consumable, so the fixture
// rebuilds a fresh eight-size BUY ladder outside each measured batch.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <thread>
#include <vector>

#include "../../core/crypto/eip712_signer.hpp"
#include "../../core/include/order_book.hpp"
#include "../../core/include/bayesian_engine.hpp"
#include "../../core/include/evidence.hpp"
#include "../../core/include/source_reliability.hpp"
#include "../../core/include/spsc_ring_buffer.hpp"
#include "../../core/src/bench_engine.hpp"
#include "alpha_parser.hpp"
#include "../../core/src/market_config.hpp"
#include "../../core/src/presigned_pool.hpp"

static uint64_t clock_ns() {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

static AlphaSignal make_signal(const MarketConfig& cfg, double p_win,
                               uint64_t id) {
    AlphaSignal signal{};
    signal.type = AlphaSignal::Type::WHALE_TRADE;
    signal.direction_hint = 0;
    signal.p_win = p_win;
    signal.confidence = 0.92;
    signal.q_value = 0.01;
    signal.timestamp_ns = AlphaParser::realtime_ns();
    signal.market_hash = cfg.market_hash;
    signal.signal_id = id;
    return signal;
}

int main() {
    std::printf("CrowdIntel CPU-path latency benchmark (network excluded)\n");
    setenv("BOT_PRIVATE_KEY_HEX",
        "23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b", 1);
    setenv("BOT_MODE", "mock", 1);
    setenv("BOT_MARKET_SLUG", "bench", 1);

    MarketConfig cfg;
    if (const char* error = cfg.load(false, true)) {
        std::printf("FATAL: %s\n", error); return 1;
    }
    cfg.bankroll_usd = 10000.0;
    cfg.kelly_fraction = 0.25;
    cfg.max_order_usd = 100.0;
    cfg.max_exposure_usd = 100000000.0;
    cfg.max_daily_loss_usd = 100000000.0;
    cfg.taker_fee_rate = 0.0;  // isolate mechanics, not fee-policy rejections
    cfg.order_type[0] = 'F'; cfg.order_type[1] = 'A';
    cfg.order_type[2] = 'K'; cfg.order_type[3] = '\0';

    uint8_t key[32];
    parse_hex_bytes(cfg.private_key_hex, 64, key, sizeof(key));
    EIP712Signer signer;
    if (!signer.init(key, cfg.neg_risk)) {
        secure_zero(key, sizeof(key));
        std::printf("FATAL: signer initialization failed\n");
        return 1;
    }
    secure_zero(key, sizeof(key));
    secure_zero(cfg.private_key_hex, sizeof(cfg.private_key_hex));
    if (const char* error = cfg.finalize_identity(signer.signer_address())) {
        std::printf("FATAL: %s\n", error); return 1;
    }

    OrderBookL2 book;
    book.set_tick_size(cfg.tick_size);
    Level2Entry bids[2] = {{470000, 2000000000}, {460000, 2000000000}};
    Level2Entry asks[2] = {{530000, 2000000000}, {540000, 2000000000}};
    book.set_book(bids, 2, asks, 2);

    auto signals = std::make_unique<SPSC_RingBuffer<AlphaSignal>>();
    PresignedOrderPool pool(cfg, signer, 60000);
    const uint64_t target_shares = KellyEngine::usd_to_shares_fixed(
        cfg.max_order_usd, 0.53);
    pool.rebuild(470000, 530000, target_shares, cfg.tick_size);
    BenchEngine engine(cfg, book, *signals, signer, pool);

    constexpr size_t WARMUP = 500;
    constexpr size_t SAMPLES = 10000;
    uint64_t signal_id = 1;
    for (size_t i = 0; i < WARMUP; ++i) {
        if (i % PresignedOrderPool::SIZE_BUCKETS == 0)
            pool.rebuild(470000, 530000, target_shares, cfg.tick_size);
        signals->try_push(make_signal(cfg, 0.75, signal_id++));
        (void)engine.run_tick();
    }

    std::vector<uint64_t> pool_hit, inline_sign, sign_only, pool_lookup,
        layered_hit, bayes_update, bayes_read;
    pool_hit.reserve(SAMPLES); inline_sign.reserve(SAMPLES);
    sign_only.reserve(SAMPLES); pool_lookup.reserve(SAMPLES);
    layered_hit.reserve(SAMPLES); bayes_update.reserve(SAMPLES);
    bayes_read.reserve(SAMPLES);

    size_t successful = 0;
    for (size_t i = 0; i < SAMPLES; ++i) {
        if (i % PresignedOrderPool::SIZE_BUCKETS == 0)
            pool.rebuild(470000, 530000, target_shares, cfg.tick_size);
        signals->try_push(make_signal(cfg, 0.75, signal_id++));
        const uint64_t begin = clock_ns();
        const int result = engine.run_tick();
        const uint64_t end = clock_ns();
        if (result == 1) { pool_hit.push_back(end - begin); ++successful; }
    }
    std::printf("consumable-pool batches: %zu/%zu productive\n", successful, SAMPLES);

    // Hot path with EVERY protective layer attached (P1 tracker, P2 brakes,
    // P3 adverse-selection gate).  Inert layer state and huge caps so every
    // signal trades; the measurement is the steady alpha path — protective
    // housekeeping is the actual production cost included here.
    setenv("BOT_PRIVATE_KEY_HEX",
        "23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b", 1);
    MarketConfig lcfg;
    if (const char* error = lcfg.load(false, true)) {
        std::printf("FATAL: %s\n", error); return 1;
    }
    lcfg.bankroll_usd = 10000.0;
    lcfg.kelly_fraction = 0.25;
    lcfg.max_order_usd = 100.0;
    lcfg.pool_max_dev_bps = 5000.0;      // fixture spread is 6c by design
    lcfg.max_exposure_usd = 1000000000.0;
    lcfg.max_daily_loss_usd = 1000000000.0;
    lcfg.max_portfolio_exposure_usd = 1000000000.0;
    lcfg.taker_fee_rate = 0.0;
    std::strcpy(lcfg.order_type, "FAK");
    secure_zero(lcfg.private_key_hex, sizeof(lcfg.private_key_hex));
    if (const char* error = lcfg.finalize_identity(signer.signer_address())) {
        std::printf("FATAL: %s\n", error); return 1;
    }
    OrderBookL2 lbook;
    lbook.set_tick_size(lcfg.tick_size);
    const Level2Entry lbids[2] = {{470000, 2000000000}, {460000, 2000000000}};
    const Level2Entry lasts[2] = {{530000, 2000000000}, {540000, 2000000000}};
    lbook.set_book(lbids, 2, lasts, 2);
    auto lsignals = std::make_unique<SPSC_RingBuffer<AlphaSignal>>();
    PresignedOrderPool lpool(lcfg, signer, 60000);
    PositionTracker ltracker;
    RiskLimits llimits{};
    llimits.stop_loss_pct = 0.0;
    llimits.hedge_trigger_pct = 0.0;
    llimits.max_daily_loss_usd = 1000000000.0;
    llimits.max_market_exposure_usd = 1000000000.0;
    llimits.max_portfolio_exposure_usd = 1000000000.0;
    RiskManager lrisk(llimits);
    VolatilityGate lgate(lcfg);
    BayesianEngine lbayes;
    SourceReliability lsources;
    auto levidence = std::make_unique<SPSC_RingBuffer<EvidenceEvent>>();
    EngineLayers llayers{};
    llayers.tracker = &ltracker;
    llayers.risk = &lrisk;
    llayers.volatility = &lgate;
    llayers.evidence_q = levidence.get();
    llayers.bayes = &lbayes;
    llayers.sources = &lsources;
    BenchEngine lengine(lcfg, lbook, *lsignals, signer, lpool, &llayers);
    for (size_t i = 0; i < WARMUP; ++i) {
        if (i % PresignedOrderPool::SIZE_BUCKETS == 0)
            lpool.rebuild(470000, 530000, target_shares, lcfg.tick_size);
        lsignals->try_push(make_signal(lcfg, 0.75, signal_id++));
        (void)lengine.run_tick();
    }
    size_t layered_successful = 0;
    for (size_t i = 0; i < SAMPLES; ++i) {
        if (i % PresignedOrderPool::SIZE_BUCKETS == 0)
            lpool.rebuild(470000, 530000, target_shares, lcfg.tick_size);
        lsignals->try_push(make_signal(lcfg, 0.75, signal_id++));
        const uint64_t begin = clock_ns();
        const int result = lengine.run_tick();
        const uint64_t end = clock_ns();
        if (result == 1) {
            layered_hit.push_back(end - begin);
            ++layered_successful;
        }
    }
    std::printf("layered batches: %zu/%zu productive\n",
                layered_successful, SAMPLES);

    // Move top-of-book to a price absent from the active ladder, forcing the
    // exact production fallback path (amount build + Keccak + ECDSA + JSON).
    bids[0] = {450000, 2000000000};
    asks[0] = {550000, 2000000000};
    book.set_book(bids, 2, asks, 2);
    successful = 0;
    for (size_t i = 0; i < SAMPLES; ++i) {
        signals->try_push(make_signal(cfg, 0.75, signal_id++));
        const uint64_t begin = clock_ns();
        const int result = engine.run_tick();
        const uint64_t end = clock_ns();
        if (result == 1) { inline_sign.push_back(end - begin); ++successful; }
    }
    std::printf("inline fallback: %zu/%zu productive\n", successful, SAMPLES);

    uint8_t signature[65];
    for (size_t i = 0; i < SAMPLES; ++i) {
        const uint64_t begin = clock_ns();
        (void)engine.bench_inline_sign(signature);
        const uint64_t end = clock_ns();
        sign_only.push_back(end - begin);
    }

    WireBody body{};
    for (size_t i = 0; i < SAMPLES; ++i) {
        if (i % PresignedOrderPool::SIZE_BUCKETS == 0)
            pool.rebuild(470000, 530000, target_shares, cfg.tick_size);
        const uint64_t begin = clock_ns();
        (void)engine.bench_pool_acquire(K_SIDE_BUY, 530000,
                                        target_shares, body);
        const uint64_t end = clock_ns();
        pool_lookup.push_back(end - begin);
    }

    // P4 brain microbench: closed-form Beta-Binomial COUNT update and the
    // posterior read the hot loop performs per drain/tick.  These are the
    // ONLY brain costs on the steady path and must stay in the tens of ns.
    BayesianEngine beng;
    beng.ensure_prior(0.40, 24.0);
    for (size_t i = 0; i < SAMPLES; ++i) {
        const uint64_t begin = clock_ns();
        beng.update_count(0, 32, 22, 900000);
        const uint64_t end = clock_ns();
        bayes_update.push_back(end - begin);
    }
    double acc = 0.0;
    for (size_t i = 0; i < SAMPLES; ++i) {
        const uint64_t begin = clock_ns();
        acc += beng.posterior();
        const uint64_t end = clock_ns();
        bayes_read.push_back(end - begin);
    }
    if (acc < 0.0) std::printf("unreachable %f\n", acc);  // keep the reads live

    auto report = [](const char* name, std::vector<uint64_t>& samples) {
        std::sort(samples.begin(), samples.end());
        auto percentile = [&](double p) {
            return samples[std::min(samples.size() - 1,
                static_cast<size_t>(p * static_cast<double>(samples.size())))];
        };
        long double total = 0;
        for (uint64_t value : samples) total += value;
        std::printf("%-33s min=%6llu ns p50=%6llu p90=%6llu p99=%6llu max=%8llu mean=%8.1Lf\n",
            name,
            static_cast<unsigned long long>(samples.front()),
            static_cast<unsigned long long>(percentile(0.50)),
            static_cast<unsigned long long>(percentile(0.90)),
            static_cast<unsigned long long>(percentile(0.99)),
            static_cast<unsigned long long>(samples.back()),
            total / samples.size());
    };

    report("decision+pool+mock-submit", pool_hit);
    report("decision+pool+layers+mock-submit", layered_hit);
    report("decision+inline-sign+mock-submit", inline_sign);
    report("sign only (Keccak+ECDSA)", sign_only);
    report("consumable pool lookup+copy", pool_lookup);
    report("bayes posterior update", bayes_update);
    report("bayes posterior read", bayes_read);
    std::printf("No network, HMAC, DNS, TCP or TLS is included in these values.\n");
    return 0;
}
