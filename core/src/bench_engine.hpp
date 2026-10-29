#ifndef BENCH_ENGINE_HPP
#define BENCH_ENGINE_HPP

// Benchmark adapter around the production ExecutionEngine.  There is no
// second strategy implementation to drift from direction, fee, tick or risk
// semantics; only isolated crypto/pool probes remain here.

#include <atomic>
#include <cstring>

#include "execution_engine.hpp"
#include "mock_client.hpp"

class BenchEngine {
public:
    BenchEngine(const MarketConfig& cfg, OrderBookL2& book,
                SPSC_RingBuffer<AlphaSignal>& signals,
                const EIP712Signer& signer, PresignedOrderPool& pool,
                const EngineLayers* layers = nullptr)
        : cfg_(cfg), signer_(signer), pool_(pool), client_(cfg),
          enabled_(true),
          engine_(cfg, book, signals, signer, pool, client_, &enabled_,
                  layers) {}

    int run_tick() {
        const TickResult result = engine_.run_tick();
        return result == TickResult::SUBMITTED ? 1 :
               result == TickResult::NO_SIGNAL ? 0 :
               -static_cast<int>(result);
    }

    bool bench_inline_sign(uint8_t signature[65]) {
        OrderV2 order{};
        order.salt = rng_.next_salt();
        order.timestamp_ms = PresignedOrderPool::now_ms();
        std::memcpy(order.maker, cfg_.maker, 20);
        std::memcpy(order.signer, cfg_.signer, 20);
        std::memcpy(order.token_id, cfg_.token_id_be, 32);
        order.maker_amount = 5500000;
        order.taker_amount = 10000000;
        order.side = K_SIDE_BUY;
        order.signature_type = cfg_.signature_type;
        return signer_.sign_order(order, signature);
    }

    bool bench_pool_acquire(uint8_t side, uint64_t price, uint64_t max_size,
                            WireBody& body) {
        uint64_t size = 0, maker = 0, taker = 0;
        return pool_.acquire_at_most(side, price, cfg_.tick_size, max_size,
                                     body, size, maker, taker);
    }

    MockCLOBClient& client() { return client_; }

private:
    const MarketConfig& cfg_;
    const EIP712Signer& signer_;
    PresignedOrderPool& pool_;
    MockCLOBClient client_;
    std::atomic<bool> enabled_;
    ExecutionEngine<MockCLOBClient> engine_;
    FastRandom rng_;
};

#endif  // BENCH_ENGINE_HPP
