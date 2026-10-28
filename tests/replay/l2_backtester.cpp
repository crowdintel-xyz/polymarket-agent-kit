// L2 replay diagnostic.
//
// A trade is marked on the NEXT tick, never at the same tick that supplied its
// spread.  The final open trade is reported but excluded from PnL/win rate.
// Taker fees use the live V2 curve C * rate * p * (1-p).  This remains a short
// smoke replay, not evidence of strategy profitability; production validation
// needs long data and resolution/fill labels.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <fstream>
#include <iostream>
#include <string>
#include <vector>

#include "../../core/include/order_book.hpp"
#include "kelly_engine.hpp"

struct ReplayTick {
    double bid = 0, bid_size = 0, ask = 0, ask_size = 0;
    double p_win = 0;
    double confidence = 0.95;
    double q_value = 0.01;
};

class L2Backtester {
public:
    L2Backtester(double bankroll_usd, double kelly_fraction = 0.25,
                 double min_edge = 0.02, double taker_fee_rate = 0.05,
                 double min_confidence = 0.85, double max_q = 0.05)
        : initial_bankroll_(bankroll_usd), kelly_fraction_(kelly_fraction),
          min_edge_(min_edge), fee_rate_(taker_fee_rate),
          min_confidence_(min_confidence), max_q_(max_q) {}

    struct Trade {
        uint64_t tick = 0;
        uint8_t side = 0;
        double entry = 0;
        double shares = 0;
        double exit_mid = 0;
        double fee = 0;
        double pnl = 0;
        bool marked = false;
    };

    bool on_tick(uint64_t tick_index, const ReplayTick& tick, OrderBookL2& book) {
        const double current_mid = 0.5 * (tick.bid + tick.ask);
        mark_open_trades(current_mid);

        Level2Entry bid{static_cast<uint64_t>(tick.bid * 1e6 + 0.5),
                        static_cast<uint64_t>(tick.bid_size * 1e6 + 0.5)};
        Level2Entry ask{static_cast<uint64_t>(tick.ask * 1e6 + 0.5),
                        static_cast<uint64_t>(tick.ask_size * 1e6 + 0.5)};
        book.set_book(&bid, 1, &ask, 1);

        if (tick.confidence < min_confidence_ || tick.q_value > max_q_ ||
            !(tick.p_win > 0 && tick.p_win < 1)) return false;
        const double buy_fee = fee_rate_ * tick.ask * (1.0 - tick.ask);
        const double sell_fee = fee_rate_ * tick.bid * (1.0 - tick.bid);
        const double buy_edge = tick.p_win - tick.ask - buy_fee;
        const double sell_edge = tick.bid - tick.p_win - sell_fee;
        if (buy_edge < min_edge_ && sell_edge < min_edge_) return false;

        Trade trade{};
        trade.tick = tick_index;
        const double bankroll = std::max(0.0, initial_bankroll_ + realized_pnl_);
        if (buy_edge >= sell_edge) {
            trade.side = 0;
            trade.entry = tick.ask;
            const double kelly = KellyEngine::kelly_buy(
                tick.p_win, std::min(0.999999, tick.ask + buy_fee));
            const double usd = KellyEngine::position_usd(
                kelly, kelly_fraction_, bankroll);
            trade.shares = std::min(usd / tick.ask, tick.ask_size);
        } else {
            trade.side = 1;
            trade.entry = tick.bid;
            const double kelly = KellyEngine::kelly_sell(
                tick.p_win, std::max(0.000001, tick.bid - sell_fee));
            const double usd = KellyEngine::position_usd(
                kelly, kelly_fraction_, bankroll);
            trade.shares = std::min(usd / tick.bid, tick.bid_size);
        }
        if (!(trade.shares > 0)) return false;
        trade.fee = trade.shares * fee_rate_ * trade.entry * (1.0 - trade.entry);
        trades_.push_back(trade);
        return true;
    }

    void report() const {
        size_t marked = 0, wins = 0, open = 0;
        double slippage = 0, peak = 0, max_drawdown = 0;
        for (const Trade& trade : trades_) {
            if (!trade.marked) { ++open; continue; }
            ++marked;
            if (trade.pnl > 0) ++wins;
            slippage += std::abs(trade.entry - trade.exit_mid) * trade.shares;
        }
        for (double equity : equity_curve_) {
            peak = std::max(peak, equity);
            max_drawdown = std::max(max_drawdown, peak - equity);
        }
        if (marked == 0) {
            std::printf("replay done: trades=%zu marked=0 open=%zu (insufficient future ticks)\n",
                        trades_.size(), open);
            return;
        }
        std::printf("replay done: trades=%zu next_tick_marked=%zu open=%zu "
                    "win_rate=%.1f%% pnl_after_fees=%.2f avg_abs_move_cost=%.6f "
                    "max_drawdown=%.2f\n",
                    trades_.size(), marked, open,
                    100.0 * static_cast<double>(wins) / static_cast<double>(marked),
                    realized_pnl_, slippage / static_cast<double>(marked), max_drawdown);
    }

private:
    void mark_open_trades(double next_mid) {
        for (Trade& trade : trades_) {
            if (trade.marked) continue;
            trade.exit_mid = next_mid;
            const double gross = trade.side == 0
                ? (next_mid - trade.entry) * trade.shares
                : (trade.entry - next_mid) * trade.shares;
            trade.pnl = gross - trade.fee;
            trade.marked = true;
            realized_pnl_ += trade.pnl;
            equity_curve_.push_back(realized_pnl_);
        }
    }

    double initial_bankroll_;
    double kelly_fraction_;
    double min_edge_;
    double fee_rate_;
    double min_confidence_;
    double max_q_;
    double realized_pnl_ = 0;
    std::vector<double> equity_curve_;
    std::vector<Trade> trades_;
};

int main(int argc, char** argv) {
    std::ifstream file;
    std::istream* input = &std::cin;
    if (argc > 1) {
        file.open(argv[1]);
        if (!file) {
            std::fprintf(stderr, "cannot open %s\n", argv[1]);
            return 1;
        }
        input = &file;
    }

    L2Backtester backtester(10000.0);
    OrderBookL2 book;
    std::string line;
    uint64_t index = 0, parsed = 0;
    while (std::getline(*input, line)) {
        if (line.empty() || line[0] == '#') continue;
        ReplayTick tick;
        double confidence = 0.95, q_value = 0.01;
        if (std::sscanf(line.c_str(), "%lf,%lf,%lf,%lf,%lf,%lf,%lf",
                        &tick.bid, &tick.bid_size, &tick.ask, &tick.ask_size,
                        &tick.p_win, &confidence, &q_value) >= 5) {
            tick.confidence = confidence;
            tick.q_value = q_value;
            backtester.on_tick(index++, tick, book);
            ++parsed;
        }
    }
    std::printf("parsed %llu ticks\n", static_cast<unsigned long long>(parsed));
    backtester.report();
    return 0;
}
