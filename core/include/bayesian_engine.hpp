#ifndef BAYESIAN_ENGINE_HPP
#define BAYESIAN_ENGINE_HPP

// ─────────────────────────────────────────────────────────────────────────────
// BayesianEngine: the closed-form signal prior/posterior (P4).
//
// One Dirichlet state over at most MAX_OUTCOMES slots — the binary Polymarket
// market uses slots 0 (YES) and 1 (NO), which is exactly the Beta-Binomial
// conjugate pair; neg-risk multi-outcome markets use additional slots.  The
// prior (α, β) is built from the market mid × prior strength N0
// (BOT_BAYES_PRIOR_STRENGTH), anchoring the posterior to the order book.
//
// Evidence flattens into the conjugate state:
//   COUNT (Beta-Binomial):       α_j += k·w and the complement mass (n−k)·w
//                                renormalized over the other slots;
//   LR    (log-odds shift):      s += w·lr, applied to the binary odds at
//                                read time via p = σ(ln(α₀/α₁) + s).
//
// Constraints honoured by construction: no heap allocation, no virtual
// calls, no IO.  A COUNT update is a handful of adds/multiplies (~10-20 ns
// RDTSC); a posterior read is one divide (COUNT-only state) or a divide plus
// one exp() when LR mass is present.  The hot loop calls update/posterior
// inline; the cold path never touches this object.
// ─────────────────────────────────────────────────────────────────────────────

#include <cmath>
#include <cstdint>

class BayesianEngine {
public:
    static constexpr size_t MAX_OUTCOMES = 8;

    // Hot: seed the prior from the current mid price.  Idempotent by design:
    // only the first call after construction/reset takes effect.
    void ensure_prior(double mid, double prior_strength) noexcept {
        if (has_prior_) return;
        if (!(mid > 0.0 && mid < 1.0)) return;   // book not ready yet
        alpha_[0] = mid * prior_strength;
        alpha_[1] = (1.0 - mid) * prior_strength;
        for (size_t i = 2; i < MAX_OUTCOMES; ++i)  alpha_[i] = 0.0;
        log_odds_shift_ = 0.0;
        has_prior_ = true;
    }

    void reset() noexcept {
        for (size_t i = 0; i < MAX_OUTCOMES; ++i) alpha_[i] = 0.0;
        log_odds_shift_ = 0.0;
        has_prior_ = false;
        events_ = 0;
        count_events_ = 0;
        lr_events_ = 0;
    }

    bool has_prior() const noexcept { return has_prior_; }

    // Beta-Binomial conjugate update.  `weight_x1e6` down-weights the whole
    // batch by source reliability (0 = ignored at the engine gate).
    void update_count(uint8_t outcome, uint32_t n, uint32_t k,
                      uint32_t weight_x1e6) noexcept {
        if (!has_prior_ || outcome >= MAX_OUTCOMES || n == 0 || k > n ||
            weight_x1e6 == 0)
            return;
        const double w = static_cast<double>(weight_x1e6) * 1e-6;
        alpha_[outcome] += static_cast<double>(k) * w;
        const double complement = static_cast<double>(n - k) * w;
        if (complement <= 0.0) { ++events_; ++count_events_; return; }
        // Distribute the complement mass over the OTHER slots, proportional
        // to their current concentration (shape-preserving renormalization).
        double others = 0.0;
        for (size_t i = 0; i < MAX_OUTCOMES; ++i)
            if (i != outcome) others += alpha_[i];
        if (others <= 0.0) { ++events_; ++count_events_; return; }
        for (size_t i = 0; i < MAX_OUTCOMES; ++i) {
            if (i == outcome) continue;
            alpha_[i] += complement * (alpha_[i] / others);
        }
        ++events_;
        ++count_events_;
    }

    // Log-likelihood shift: posterior odds for outcome-vs-complement are
    // multiplied by exp(w·lr).  Cheap accumulate; exp happens at read time.
    void update_lr(int32_t lr_x1e6, uint32_t weight_x1e6) noexcept {
        if (!has_prior_ || weight_x1e6 == 0 || lr_x1e6 == 0) return;
        log_odds_shift_ += static_cast<double>(lr_x1e6) * 1e-6 *
                           (static_cast<double>(weight_x1e6) * 1e-6);
        ++events_;
        ++lr_events_;
    }

    // Posterior probability of `outcome`.  COUNT-only states read as a
    // single divide; LR mass routes the binary state through the sigmoid.
    double posterior(uint8_t outcome = 0) const noexcept {
        if (!has_prior_ || outcome >= MAX_OUTCOMES) return -1.0;
        double total = 0.0;
        for (size_t i = 0; i < MAX_OUTCOMES; ++i) total += alpha_[i];
        if (total <= 0.0) return -1.0;
        double mean = alpha_[outcome] / total;
        if (log_odds_shift_ != 0.0 &&
            (outcome == 0 || outcome == 1) && alpha_[0] > 0.0 &&
            alpha_[1] > 0.0) {
            const double g = std::log(alpha_[0] / alpha_[1]) +
                             log_odds_shift_;
            const double p = 1.0 / (1.0 + std::exp(-g));
            mean = outcome == 0 ? p : 1.0 - p;
        }
        return mean;
    }

    uint64_t events() const noexcept { return events_; }
    uint64_t count_events() const noexcept { return count_events_; }
    uint64_t lr_events() const noexcept { return lr_events_; }

private:
    double alpha_[MAX_OUTCOMES]{};
    double log_odds_shift_ = 0.0;
    bool has_prior_ = false;
    uint64_t events_ = 0;
    uint64_t count_events_ = 0;
    uint64_t lr_events_ = 0;
};

#endif  // BAYESIAN_ENGINE_HPP
