#ifndef KELLY_ENGINE_HPP
#define KELLY_ENGINE_HPP

#include <cmath>
#include <cstdint>
#include <limits>

// ─────────────────────────────────────────────────────────────────────────────
// KellyEngine: exact Kelly criterion for binary prediction-market contracts.
//
// Polymarket contracts pay $1 if the outcome occurs, and trade at price p.
// Buying YES at p with true win probability w is a binary bet with net odds
// b = (1−p)/p, and the growth-optimal fraction of bankroll is:
//
//      f*_buy  = (w − p) / (1 − p)
//      f*_sell = (p − w) / p          (selling YES ≡ buying NO at 1−p)
//
// (Derivation: maximize E[log wealth] for a $1 payoff contract. The old
// "ev × confidence" heuristic was not Kelly and over-bet at high prices.)
//
// The engine applies a fractional-Kelly multiplier (variance control) and
// hard caps by bankroll.
// ─────────────────────────────────────────────────────────────────────────────

class KellyEngine {
public:
    // Exact Kelly fraction for buying YES at `price` with win prob `p_win`.
    // Returns 0 when there is no edge or inputs are degenerate.
    static double kelly_buy(double p_win, double price) {
        if (!std::isfinite(p_win) || !std::isfinite(price) ||
            p_win <= price || price <= 0.0 || price >= 1.0 || p_win <= 0.0)
            return 0.0;
        return (p_win - price) / (1.0 - price);
    }

    // Exact Kelly fraction for selling YES at `price` with win prob `p_win`.
    static double kelly_sell(double p_win, double price) {
        if (!std::isfinite(p_win) || !std::isfinite(price) ||
            p_win >= price || price <= 0.0 || price >= 1.0 || p_win >= 1.0)
            return 0.0;
        return (price - p_win) / price;
    }

    // Fractional Kelly → position size in USD (never exceeds the cap).
    static double position_usd(double kelly_f, double fraction_cap,
                               double bankroll_usd) {
        if (!std::isfinite(kelly_f) || !std::isfinite(fraction_cap) ||
            !std::isfinite(bankroll_usd) || kelly_f <= 0.0 ||
            fraction_cap <= 0.0 || bankroll_usd <= 0.0)
            return 0.0;
        double f = kelly_f * fraction_cap;
        if (!std::isfinite(f)) return 0.0;
        if (f > 1.0) f = 1.0;
        const double result = f * bankroll_usd;
        return std::isfinite(result) ? result : 0.0;
    }

    // USD budget → shares (×1e6 fixed), floor-rounded, at `price`.
    static uint64_t usd_to_shares_fixed(double usd, double price) {
        if (!std::isfinite(usd) || !std::isfinite(price) ||
            usd <= 0.0 || price <= 0.0)
            return 0;
        const double scaled = (usd / price) * 1000000.0;
        if (!std::isfinite(scaled) || scaled <= 0.0 ||
            scaled >= static_cast<double>(std::numeric_limits<uint64_t>::max()))
            return 0;
        return static_cast<uint64_t>(scaled);
    }
};

#endif // KELLY_ENGINE_HPP
