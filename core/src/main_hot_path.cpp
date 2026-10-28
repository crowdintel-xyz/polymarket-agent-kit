// Bot CrowdIntel — live topology:
// market WSS -> atomic top/depth book; authenticated HTTP alpha -> SPSC;
// presigner -> consumable ladder; execution -> async network gateway.

#include <atomic>
#include <chrono>
#include <cerrno>
#include <csignal>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <pthread.h>
#include <string>
#include <sys/mman.h>
#include <thread>
#include <unistd.h>

#include "../crypto/eip712_signer.hpp"
#include "../crypto/secure_zero.hpp"
#include "../include/account_events.hpp"
#include "../include/bayesian_engine.hpp"
#include "../include/evidence.hpp"
#include "../include/journal.hpp"
#include "../include/source_reliability.hpp"
#include "../include/order_book.hpp"
#include "../include/position_tracker.hpp"
#include "../include/risk_manager.hpp"
#include "../include/volatility_gate.hpp"
#include "../include/spsc_ring_buffer.hpp"
#include "../include/time_utils.hpp"
#include "alpha_parser.hpp"
#include "evidence_ingress.hpp"
#include "execution_engine.hpp"
#include "market_config.hpp"
#include "mock_client.hpp"
#include "presigned_pool.hpp"

#if defined(CROWDINTEL_HAVE_NETWORK) && !defined(CROWDINTEL_FORCE_MOCK)
#include "alpha_http_receiver.hpp"
#include "lightweight_client.hpp"
#include "order_gateway.hpp"
#include "ws_market_listener.hpp"
#include "ws_user_listener.hpp"
#define CROWDINTEL_LIVE 1
#endif

static std::atomic<bool> g_running{true};
static void on_signal(int) { g_running.store(false, std::memory_order_release); }

namespace {

void idle_wait() {
#if defined(__x86_64__) || defined(__i386__)
    for (int i = 0; i < 256; ++i) __builtin_ia32_pause();
#else
    std::this_thread::yield();
#endif
    timespec delay{0, 50000};
    nanosleep(&delay, nullptr);
}

void pin_to_cpu(int cpu) {
#if defined(__linux__)
    if (cpu < 0) return;
    cpu_set_t set;
    CPU_ZERO(&set);
    CPU_SET(cpu, &set);
    if (pthread_setaffinity_np(pthread_self(), sizeof(set), &set) != 0)
        std::fprintf(stderr, "warning: could not pin thread to cpu %d\n", cpu);
#else
    (void)cpu;
#endif
}

void mock_feed(SPSC_RingBuffer<AlphaSignal>& queue, OrderBookL2& book,
               const MarketConfig& cfg, std::atomic<bool>& running) {
    // Paper-honest mock market: a tight, tradeable book (~100 bps execution
    // slippage vs mid — inside the P3 gate) that is refreshed continuously so
    // the staleness guard never gates the demo.  Mid never moves, so the
    // volatility regime stays NORMAL.  p_win alternates 0.65 / 0.35 so both
    // BUY and SELL edges fire: with paper fills enabled, positions round-trip
    // and the brakes (P2) observe real mark-to-market.
    const Level2Entry bids[3] = {
        {495000, 40000000}, {494000, 60000000}, {493000, 90000000}};
    const Level2Entry asks[3] = {
        {505000, 30000000}, {506000, 55000000}, {507000, 80000000}};
    book.set_tick_size(cfg.tick_size);
    book.set_book(bids, 3, asks, 3);
    uint64_t id = 1;
    while (running.load(std::memory_order_acquire)) {
        book.set_book(const_cast<Level2Entry*>(bids), 3,
                      const_cast<Level2Entry*>(asks), 3);
        AlphaSignal signal{};
        signal.type = AlphaSignal::Type::WHALE_TRADE;
        // Alternate BUY (0.65 vs 0.505 ask) and SELL (0.35 vs 0.495 bid)
        // hints so paper positions open and close.
        const bool buy_leg = (id % 2) != 0;
        signal.direction_hint = buy_leg ? 0 : 1;
        signal.p_win = buy_leg ? 0.65 : 0.35;
        signal.confidence = 0.92;
        signal.q_value = 0.01;
        signal.timestamp_ns = AlphaParser::realtime_ns();
        signal.market_hash = cfg.market_hash;
        signal.signal_id = id++;
        (void)queue.try_push(signal);
        std::this_thread::sleep_for(std::chrono::milliseconds(200));
    }
}

template <typename Engine>
long run_engine(Engine& engine, const MarketConfig& cfg,
                uint64_t* counts, size_t count_size) {
    long productive = 0;
    while (g_running.load(std::memory_order_acquire)) {
        const TickResult result = engine.run_tick();
        const size_t index = static_cast<size_t>(result);
        if (index < count_size) ++counts[index];
        if (result == TickResult::NO_SIGNAL) idle_wait();
        else ++productive;
        if (cfg.max_ticks > 0 && productive >= cfg.max_ticks) break;
    }
    return productive;
}

#if defined(CROWDINTEL_LIVE)
// One reconciliation round: fetch authoritative positions, compare them to
// the local tracker, and enqueue a restatement (+kill latch on big drift).
void reconcile_once_with(LightweightCLOBClient& client,
                         const MarketConfig& cfg, PositionTracker& tracker,
                         SPSC_RingBuffer<TrackerRestate, 16>& restate_q) {
    static constexpr char PATH[] = "/data/positions?limit=100";
    char body[8192];
    const size_t len = client.rest_get(PATH, body, sizeof(body) - 1);
    if (!len) {
        std::fprintf(stderr, "RCN positions fetch failed (keeping local state)\n");
        return;
    }
    uint64_t yes = 0, yes_avg = 0, hedge = 0, hedge_avg = 0;
    (void)LightweightCLOBClient::parse_positions_for_asset(
        body, len, cfg.token_id_dec, yes, yes_avg);
    if (cfg.hedge_token_id_dec[0])
        (void)LightweightCLOBClient::parse_positions_for_asset(
            body, len, cfg.hedge_token_id_dec, hedge, hedge_avg);

    PositionTracker::Snapshot view{};
    if (!tracker.snapshot(view)) return;
    const auto diff_shares = [](uint64_t a, uint64_t b) {
        return a > b ? a - b : b - a;
    };
    const uint64_t max_drift = static_cast<uint64_t>(
        cfg.reconcile_max_drift_shares * 1000000.0);
    const uint64_t drift = diff_shares(view.net_yes, yes) +
                           diff_shares(view.net_hedge, hedge);
    TrackerRestate restate{};
    restate.yes_shares = yes;
    restate.yes_avg = yes_avg;
    restate.hedge_shares = hedge;
    restate.hedge_avg = hedge_avg;
    restate.drift_exceeded = drift > max_drift && max_drift != 0 ? 1 : 0;
    if (drift > 0) {
        std::fprintf(stderr,
            "RCN drift: local yes=%llu hedge=%llu vs venue yes=%llu "
            "hedge=%llu (drift=%llu x1e6, kill=%llu)\n",
            static_cast<unsigned long long>(view.net_yes),
            static_cast<unsigned long long>(view.net_hedge),
            static_cast<unsigned long long>(yes),
            static_cast<unsigned long long>(hedge),
            static_cast<unsigned long long>(drift),
            static_cast<unsigned long long>(restate.drift_exceeded));
        (void)restate_q.try_push(restate);
    }
}
#endif

}  // namespace

int main() {
    std::signal(SIGINT, on_signal);
    std::signal(SIGTERM, on_signal);
#ifdef SIGPIPE
    std::signal(SIGPIPE, SIG_IGN);  // OpenSSL can ultimately write(2) a dead peer
#endif

    const bool mock =
#if !defined(CROWDINTEL_LIVE)
        true;
#else
        std::getenv("BOT_MODE") && std::string(std::getenv("BOT_MODE")) == "mock";
#endif

    MarketConfig cfg;
    if (const char* error = cfg.load(!mock, mock)) {
        std::fprintf(stderr, "FATAL: %s\n", error);
        return 1;
    }

    // Parse the private key exactly once, initialize the only signer, then
    // remove the original environment entry and wipe the temporary bytes.
    uint8_t private_key[32];
    if (!parse_hex_bytes(cfg.private_key_hex, std::strlen(cfg.private_key_hex),
                         private_key, sizeof(private_key))) {
        std::fprintf(stderr, "FATAL: invalid BOT_PRIVATE_KEY_HEX\n");
        return 1;
    }
    EIP712Signer signer;
    if (!signer.init(private_key, cfg.neg_risk)) {
        secure_zero(private_key, sizeof(private_key));
        std::fprintf(stderr, "FATAL: signer initialization failed\n");
        return 1;
    }
    secure_zero(private_key, sizeof(private_key));
    secure_zero(cfg.private_key_hex, sizeof(cfg.private_key_hex));
    if (const char* error = cfg.finalize_identity(signer.signer_address())) {
        std::fprintf(stderr, "FATAL: %s\n", error);
        return 1;
    }

#if defined(__linux__)
    if (!mock && mlockall(MCL_CURRENT | MCL_FUTURE) != 0) {
        std::fprintf(stderr,
                     "FATAL: mlockall failed; refusing live mode with swappable secrets\n");
        return 1;
    }
#endif

    std::fprintf(stdout,
        "CrowdIntel 2.1 mode=%s market=%s tick=%.4f sig=%u order=%s\n",
        mock ? "mock" : "live", cfg.market_slug,
        static_cast<double>(cfg.tick_size) * 1e-6,
        cfg.signature_type, cfg.order_type);

    auto book = std::make_unique<OrderBookL2>();
    std::unique_ptr<OrderBookL2> hedge_book;
    if (cfg.hedge_token_id_dec[0])
        hedge_book = std::make_unique<OrderBookL2>();  // complement leg (P2)
    auto signals = std::make_unique<SPSC_RingBuffer<AlphaSignal>>();
    // P1 — the "eyes": user-channel account queue, shared inventory state
    // and the audit journal.  All pre-reserved before threads start.
    auto account_q = std::make_unique<SPSC_RingBuffer<AccountEvent>>();
    auto journal_q = std::make_unique<SPSC_RingBuffer<JournalEvent>>();
    auto restate_q = std::make_unique<SPSC_RingBuffer<TrackerRestate, 16>>();
    PositionTracker tracker(
        cfg.initial_position_shares,
        static_cast<uint64_t>(cfg.initial_position_avg_price * 1000000.0));

    // P2 — the brakes.  Limits come from validated configuration; the cold
    // recalibration path (reconciler, supervisor) may later tighten them.
    RiskLimits limits{};
    limits.stop_loss_pct = cfg.stop_loss_pct;
    limits.hedge_trigger_pct = cfg.hedge_trigger_pct;
    limits.max_daily_loss_usd = cfg.max_daily_loss_usd;
    limits.max_market_exposure_usd = cfg.max_exposure_usd;
    limits.max_portfolio_exposure_usd = cfg.max_portfolio_exposure_usd;
    RiskManager risk(limits);

    EngineLayers layers{};
    layers.account_q = account_q.get();
    layers.tracker = &tracker;
    layers.journal_q = journal_q.get();
    layers.restate_q = restate_q.get();
    layers.risk = &risk;
    if (hedge_book) layers.hedge_book = hedge_book.get();

    // P3 — adverse-selection brake.  The presign thread samples the book;
    // the hot loop reads the published regime and owns the shock FSM.
    VolatilityGate volatility(cfg);
    layers.volatility = &volatility;

    // P4 — the brain.  Source trust comes from BOT_BAYES_SOURCES plus the
    // optional recalibration file; evidence arrives on its own SPSC queue
    // from whatever cold producer runs (replayer, poller, mock script).
    auto evidence_q = std::make_unique<SPSC_RingBuffer<EvidenceEvent>>();
    BayesianEngine bayes;
    SourceReliability sources;
    const size_t sources_configured =
        evidence_ingress::parse_sources_text(cfg.bayes_sources, sources);
    if (cfg.bayes_enable && cfg.bayes_sources[0] && sources_configured == 0) {
        std::fprintf(stderr, "FATAL: BOT_BAYES_SOURCES is set but no valid "
                             "id:weight pairs were parsed\n");
        return 1;
    }
    if (cfg.bayes_enable) {
        layers.evidence_q = evidence_q.get();
        layers.bayes = &bayes;
        layers.sources = &sources;
        std::fprintf(stdout, "bayes brain: %zu sources, prior strength %.1f, "
                     "threshold %.3f, min reliability %.2f\n",
                     sources_configured, cfg.bayes_prior_strength,
                     cfg.bayes_signal_threshold, cfg.bayes_min_reliability);
    }

    PresignedOrderPool pool(cfg, signer, cfg.presign_ttl_ms);
    MockCLOBClient mock_client(cfg);
    // Paper trading: accepted mock orders synthesize venue fills into the
    // account queue, so tracker/exposure/move-stop/brain all see real flow.
    // Live mode never attaches (fills come from the private WSS channel).
    if (mock) mock_client.attach_fill_queue(account_q.get());
    std::atomic<bool> workers_running{true};
    std::atomic<bool> trading_enabled{true};

    // Cold journal consumer: the only renderer of hot-path audit events.
    std::thread journal_thread([&] {
        pin_to_cpu(cfg.cold_cpu);
        JournalEvent event{};
        while (workers_running.load(std::memory_order_acquire) ||
               !journal_q->empty()) {
            if (!journal_q->try_pop(event)) {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                continue;
            }
            std::fprintf(stdout,
                "JRN {\"type\":%u,\"pnl\":%lld,\"a0\":%llu,\"a1\":%llu,"
                "\"a2\":%llu,\"mono_ns\":%llu}\n",
                static_cast<unsigned>(event.type),
                static_cast<long long>(event.pnl),
                static_cast<unsigned long long>(event.aux0),
                static_cast<unsigned long long>(event.aux1),
                static_cast<unsigned long long>(event.aux2),
                static_cast<unsigned long long>(event.mono_ns));
        }
    });

    std::thread mock_thread;
    std::thread mock_evidence_thread;
    std::thread kill_switch_thread;
    std::thread reconcile_thread;
    std::unique_ptr<EvidenceFileReplayer> evidence_replayer;
    if (cfg.bayes_enable && cfg.evidence_file[0]) {
        // Paper-trading/backfill substrate: replay recorded NDJSON evidence,
        // then tail new lines; recalibrates source weights from the recal
        // file on the same cold thread.
        evidence_replayer = std::make_unique<EvidenceFileReplayer>(
            cfg.evidence_file, *evidence_q, &sources, cfg.bayes_recal_file);
        const bool ok = evidence_replayer->start();
        std::fprintf(stdout, ok ? "evidence replay: %s\n"
                                : "evidence replay: %s (not found yet; "
                                  "tailing)\n", cfg.evidence_file);
    }
#if defined(CROWDINTEL_LIVE)
    std::unique_ptr<WsMarketListener> market_listener;
    std::unique_ptr<WsMarketListener> hedge_listener;  // complement book (P2)
    std::unique_ptr<WsUserListener> user_listener;
    std::unique_ptr<AlphaParser> alpha_parser;
    std::unique_ptr<AlphaHttpReceiver> alpha_receiver;
    std::unique_ptr<LightweightCLOBClient> live_client;
    std::unique_ptr<OrderGateway<LightweightCLOBClient>> gateway;
#endif

    if (mock) {
        mock_thread = std::thread([&] {
            pin_to_cpu(cfg.cold_cpu);
            mock_feed(*signals, *book, cfg, workers_running);
        });
        if (cfg.bayes_enable && sources_configured > 0) {
            // Paper-trading brain demo: scripted COUNT observations on the
            // FIRST configured source.  Alternating hit rates exercise both
            // trigger directions against the synthetic book; each batch is
            // dedup-proof (unique hash) and fires at most once.
            mock_evidence_thread = std::thread([&] {
                pin_to_cpu(cfg.cold_cpu);
                const char* scan = cfg.bayes_sources;
                uint32_t source = 0;
                while (*scan && !std::isdigit(
                           static_cast<unsigned char>(*scan)))
                    ++scan;
                char* end = nullptr;
                const long parsed = std::strtol(scan, &end, 10);
                if (end && end != scan) source = static_cast<uint32_t>(parsed);
                uint32_t round = 0;
                while (workers_running.load(std::memory_order_acquire)) {
                    EvidenceEvent ev{};
                    ev.kind = EvidenceEvent::Kind::COUNT;
                    ev.source_id = source;
                    ev.count_n = 32;
                    ev.count_k = (round % 2) ? 10 : 22;  // sell / buy leans
                    ev.event_hash = 0xE1000000U + round;
                    ev.timestamp_ns = crowdintel::realtime_ns();
                    (void)evidence_q->try_push(ev);
                    ++round;
                    for (uint32_t i = 0;
                         i < 2500 &&
                            workers_running.load(std::memory_order_acquire);
                         i += 50)
                        std::this_thread::sleep_for(
                            std::chrono::milliseconds(50));
                }
            });
        }
    }
#if defined(CROWDINTEL_LIVE)
    else {
        market_listener = std::make_unique<WsMarketListener>(cfg, *book);
        market_listener->start();

        // Complement-token book for the hedging brake: its own connection and
        // its own cold thread, subscribed to the hedge asset id only.
        if (hedge_book) {
            hedge_listener = std::make_unique<WsMarketListener>(
                cfg, *hedge_book, cfg.hedge_token_id_dec);
            hedge_listener->start();
        }

        // The private user channel is the only authoritative source of fills;
        // without it the engine would trade blind against local reservations.
        user_listener = std::make_unique<WsUserListener>(cfg, *account_q);
        user_listener->start();

        alpha_parser = std::make_unique<AlphaParser>(
            *signals, cfg.market_slug, cfg.max_q_value, cfg.min_confidence);
        alpha_receiver = std::make_unique<AlphaHttpReceiver>(cfg, *alpha_parser);
        if (!alpha_receiver->start()) {
            std::fprintf(stderr, "FATAL: cannot bind alpha receiver at %s:%u\n",
                         cfg.alpha_bind, cfg.alpha_port);
            market_listener->stop();
            return 1;
        }

        live_client = std::make_unique<LightweightCLOBClient>(cfg);
        gateway = std::make_unique<OrderGateway<LightweightCLOBClient>>(
            *live_client, &trading_enabled);
        gateway->start();

        // Startup reconciliation: adopt the venue's authoritative open
        // positions before the first tick so a restarted process never
        // doubles exposure against inventory it already owns.  The restate
        // itself is applied by the hot loop from the restate queue.
        reconcile_once_with(*live_client, cfg, tracker, *restate_q);

        reconcile_thread = std::thread([&] {
            pin_to_cpu(cfg.cold_cpu);
            bool cancel_sent = false;
            uint64_t ticks_100ms = 0;
            const uint64_t rounds = cfg.reconcile_interval_sec != 0
                ? cfg.reconcile_interval_sec * 10 : 0;
            while (workers_running.load(std::memory_order_acquire)) {
                // Fast kill watcher: one cancel-all per kill latch.
                if (risk.killed() && !cancel_sent) {
                    const bool ok = live_client->cancel_all();
                    std::fprintf(stderr,
                        "RCN kill-switch latched: cancel-all %s\n",
                        ok ? "acknowledged" : "FAILED (operator action required)");
                    cancel_sent = true;
                }
                if (rounds != 0 && ++ticks_100ms >= rounds) {
                    ticks_100ms = 0;
                    reconcile_once_with(*live_client, cfg, tracker,
                                        *restate_q);
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });

        kill_switch_thread = std::thread([&] {
            pin_to_cpu(cfg.cold_cpu);
            while (workers_running.load(std::memory_order_acquire)) {
                errno = 0;
                const int present = ::access(cfg.kill_switch_file, F_OK);
                if (present == 0 || errno != ENOENT) {
                    trading_enabled.store(false, std::memory_order_release);
                    g_running.store(false, std::memory_order_release);
                    std::fprintf(stderr,
                        present == 0 ? "KILL SWITCH active: %s\n"
                                     : "KILL SWITCH check failed closed: %s\n",
                        cfg.kill_switch_file);
                    break;
                }
                std::this_thread::sleep_for(std::chrono::milliseconds(100));
            }
        });
        std::fprintf(stdout, "alpha ingress: http://%s:%u/signal\n",
                     cfg.alpha_bind, cfg.alpha_port);
    }
#endif

    // Rebuild only when top/tick changes or freshness reaches half-TTL.
    std::thread presign_thread([&] {
        pin_to_cpu(cfg.cold_cpu);
        uint64_t last_bid = 0, last_ask = 0, last_tick = 0, last_build = 0;
        while (workers_running.load(std::memory_order_acquire)) {
            OrderBookL2::Top top{};
            if (book->read_top(top) && top.bid.size && top.ask.size) {
                // Feed the adverse-selection regime sampler (P3) on every
                // observed top, ~100 Hz while the book is fresh.
                volatility.sample(top.bid.price, top.ask.price,
                                  crowdintel::mono_ns(), cfg.presign_ttl_ms);
                const uint64_t tick = book->tick_size(cfg.tick_size);
                const uint64_t now = PresignedOrderPool::now_ms();
                if (top.bid.price != last_bid || top.ask.price != last_ask ||
                    tick != last_tick || now - last_build >= cfg.presign_ttl_ms / 2) {
                    const double price =
                        static_cast<double>(top.ask.price) * 1e-6;
                    const uint64_t target = KellyEngine::usd_to_shares_fixed(
                        cfg.max_order_usd, price);
                    if (target >= cfg.min_size_shares &&
                        pool.rebuild(top.bid.price, top.ask.price, target, tick)) {
                        last_bid = top.bid.price;
                        last_ask = top.ask.price;
                        last_tick = tick;
                        last_build = now;
                    }
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        }
    });

    pin_to_cpu(cfg.pin_cpu);
    constexpr size_t RESULT_COUNT = static_cast<size_t>(TickResult::COUNT);
    uint64_t counts[RESULT_COUNT]{};
    const auto start = std::chrono::steady_clock::now();

    if (mock) {
        ExecutionEngine<MockCLOBClient> engine(
            cfg, *book, *signals, signer, pool, mock_client, &trading_enabled,
            &layers);
        run_engine(engine, cfg, counts, RESULT_COUNT);
    }
#if defined(CROWDINTEL_LIVE)
    else {
        ExecutionEngine<OrderGateway<LightweightCLOBClient>> engine(
            cfg, *book, *signals, signer, pool, *gateway, &trading_enabled,
            &layers);
        run_engine(engine, cfg, counts, RESULT_COUNT);
    }
#endif

    workers_running.store(false, std::memory_order_release);
#if defined(CROWDINTEL_LIVE)
    if (alpha_receiver) alpha_receiver->stop();
    if (user_listener) user_listener->stop();
    if (market_listener) market_listener->stop();
    if (hedge_listener) hedge_listener->stop();
    if (gateway) gateway->stop();
#endif
    if (mock_evidence_thread.joinable()) mock_evidence_thread.join();
    if (mock_thread.joinable()) mock_thread.join();
    if (kill_switch_thread.joinable()) kill_switch_thread.join();
    if (reconcile_thread.joinable()) reconcile_thread.join();
    if (evidence_replayer) {
        evidence_replayer->stop();
        std::fprintf(stdout, "  evidence replay parsed=%llu dropped=%llu\n",
            static_cast<unsigned long long>(evidence_replayer->parsed()),
            static_cast<unsigned long long>(evidence_replayer->dropped()));
    }
    if (cfg.bayes_enable)
        std::fprintf(stdout,
            "  bayes events=%llu count=%llu lr=%llu posterior=%.4f\n",
            static_cast<unsigned long long>(bayes.events()),
            static_cast<unsigned long long>(bayes.count_events()),
            static_cast<unsigned long long>(bayes.lr_events()),
            bayes.posterior());
    presign_thread.join();
    journal_thread.join();

    const double seconds = std::chrono::duration<double>(
        std::chrono::steady_clock::now() - start).count();
    std::fprintf(stdout, "\nSession %.2fs\n", seconds);
    for (size_t i = 1; i < RESULT_COUNT; ++i) {
        if (counts[i]) std::fprintf(stdout, "  %-20s %llu\n",
            tick_result_name(static_cast<TickResult>(i)),
            static_cast<unsigned long long>(counts[i]));
    }
    PositionTracker::Snapshot tracker_view{};
    if (tracker.snapshot(tracker_view)) {
        std::fprintf(stdout,
            "  inventory yes=%llu@%.4f hedge=%llu open_buy=%llu open_sell=%llu "
            "fills=%llu anomalies=%llu realized_pnl=%.4f\n",
            static_cast<unsigned long long>(tracker_view.net_yes),
            static_cast<double>(tracker_view.yes_avg) * 1e-6,
            static_cast<unsigned long long>(tracker_view.net_hedge),
            static_cast<unsigned long long>(tracker_view.open_buy),
            static_cast<unsigned long long>(tracker_view.open_sell),
            static_cast<unsigned long long>(tracker_view.fills),
            static_cast<unsigned long long>(tracker_view.anomalies),
            static_cast<double>(tracker_view.realized_pnl) * 1e-6);
    }
    if (mock) std::fprintf(stdout,
        "  paper orders=%llu fills=%llu\n",
        static_cast<unsigned long long>(mock_client.submissions()),
        static_cast<unsigned long long>(mock_client.paper_fills()));
#if defined(CROWDINTEL_LIVE)
    if (gateway) std::fprintf(stdout,
        "  gateway enqueued=%llu accepted=%llu rejected=%llu dropped=%llu retries=%llu cancelled=%llu\n",
        static_cast<unsigned long long>(gateway->enqueued()),
        static_cast<unsigned long long>(gateway->accepted()),
        static_cast<unsigned long long>(gateway->rejected()),
        static_cast<unsigned long long>(gateway->dropped()),
        static_cast<unsigned long long>(gateway->retried()),
        static_cast<unsigned long long>(gateway->cancelled()));
    if (market_listener) std::fprintf(stdout, "  ws events=%llu reconnects=%llu\n",
        static_cast<unsigned long long>(market_listener->events_seen()),
        static_cast<unsigned long long>(market_listener->reconnects()));
    if (user_listener) std::fprintf(stdout,
        "  user-ws events=%llu pushed=%llu dropped=%llu reconnects=%llu\n",
        static_cast<unsigned long long>(user_listener->events_seen()),
        static_cast<unsigned long long>(user_listener->pushed()),
        static_cast<unsigned long long>(user_listener->dropped()),
        static_cast<unsigned long long>(user_listener->reconnects()));
    if (alpha_receiver) std::fprintf(stdout, "  alpha accepted=%llu rejected=%llu\n",
        static_cast<unsigned long long>(alpha_receiver->accepted()),
        static_cast<unsigned long long>(alpha_receiver->rejected()));
#endif
    return 0;
}
