#ifndef POSITION_TRACKER_HPP
#define POSITION_TRACKER_HPP

// ─────────────────────────────────────────────────────────────────────────────
// PositionTracker: authoritative in-memory inventory/P&L state.
//
// Concurrency model (repository invariant compliant):
//   * Exactly ONE writer: the hot loop applies AccountEvents drained from the
//     user-feed SPSC queue and manages local reservations around submit().
//   * Any number of COLD readers: a seqlock snapshot (same publication pattern
//     as OrderBookL2) exposes a coherent copy for REST reconciliation and
//     metrics.  Cold readers never mutate.
//
// Semantics:
//   * Reservations are NOT fills.  A BUY reserve increments open-buy worst
//     cost; only a venue FILL converts reservation into inventory.
//   * FILL_MINED/CONFIRMED duplicates a venue `MATCHED` trade id; a two-epoch
//     Bloom filter over event ids suppresses the duplicate safely (a false
//     positive only drops a duplicate; a false negative is impossible within
//     an epoch pair).
//   * SELL realized P&L uses the tracked average entry (VWAP).  Inconsistent
//     events never underflow counters: they saturate, bump anomaly counters
//     and are left for REST reconciliation to correct.
// ─────────────────────────────────────────────────────────────────────────────

#include <array>
#include <atomic>
#include <cstdint>

#include "account_events.hpp"
#include "polymarket_order.hpp"  // crowd_uint128_t / crowd_int128_t
#include "time_utils.hpp"

class PositionTracker {
public:
    static constexpr size_t OPEN_ORDER_SLOTS = 16;

    struct Snapshot {
        uint64_t net_yes = 0;        // primary-token inventory, shares x1e6
        uint64_t net_hedge = 0;      // complement-token inventory
        uint64_t yes_avg = 0;        // VWAP entry of net_yes, price x1e6
        uint64_t hedge_avg = 0;
        int64_t realized_pnl = 0;    // signed USD x1e6 (yes + hedge books)
        uint64_t open_buy = 0;       // in-flight BUY quantity (reserved)
        uint64_t open_sell = 0;      // in-flight SELL quantity (reserved)
        uint64_t open_buy_cost = 0;  // worst-case cost of open BUYs, USD x1e6
        uint64_t fills = 0;
        uint64_t anomalies = 0;
        uint64_t orders_tracked = 0;
        uint64_t updated_ns = 0;     // CLOCK_MONOTONIC
        uint64_t version = 0;
    };

    PositionTracker() { publish(); }

    explicit PositionTracker(uint64_t initial_yes_shares,
                             uint64_t initial_avg_price = 500000) {
        net_yes_ = initial_yes_shares;
        yes_avg_ = initial_yes_shares ? initial_avg_price : 0;
        publish();
    }

    // ── Local reservations (hot writer only) ────────────────────────────────
    void reserve_buy(uint64_t qty, uint64_t price) noexcept {
        if (qty == 0 || price == 0) return;
        open_buy_ += qty;
        const uint64_t cost = ceil_cost(qty, price);
        open_buy_cost_ = cost > UINT64_MAX - open_buy_cost_
            ? UINT64_MAX : open_buy_cost_ + cost;  // saturate, never wrap
        publish();
    }

    void release_buy(uint64_t qty, uint64_t price) noexcept {
        open_buy_ = qty >= open_buy_ ? 0 : open_buy_ - qty;
        const uint64_t cost = ceil_cost(qty, price);
        open_buy_cost_ = cost >= open_buy_cost_ ? 0 : open_buy_cost_ - cost;
        publish();
    }

    void reserve_sell(uint64_t qty) noexcept {
        if (qty == 0) return;
        open_sell_ += qty;
        publish();
    }

    void release_sell(uint64_t qty) noexcept {
        open_sell_ = qty >= open_sell_ ? 0 : open_sell_ - qty;
        publish();
    }

    // ── Venue fact application (hot writer only) ────────────────────────────
    void apply(const AccountEvent& ev) noexcept {
        if (ev.event_id != 0 && seen_before(ev.event_id)) {
            ++anomalies_;  // duplicate delivery (e.g. MATCHED then MINED)
            publish();
            return;
        }
        switch (ev.type) {
            case AccountEvent::Type::FILL:
                apply_fill(ev.side, ev.asset, ev.price, ev.size, ev.order_hash);
                ++fills_;
                break;
            case AccountEvent::Type::FILL_MINED:
                // Same trade id already handled via dedup; a MINED with an
                // unknown id is an inconsistency to reconcile, not a fill.
                ++anomalies_;
                break;
            case AccountEvent::Type::OPEN:
                track_open(ev.order_hash, ev.side, ev.asset,
                           ev.remaining ? ev.remaining : ev.size);
                break;
            case AccountEvent::Type::CANCEL:
            case AccountEvent::Type::REJECT:
            case AccountEvent::Type::FAILED:
                close_order(ev);
                break;
        }
        updated_ns_ = crowdintel::mono_ns();
        publish();
    }

    // Release reservation state for venue orders that produced no account
    // event within the TTL (ambiguous network outcome).  REST reconciliation
    // remains the backstop for any divergence this heuristic creates.
    size_t release_stale(uint64_t now_ns, uint64_t ttl_ns) noexcept {
        if (last_stale_scan_ns_ != 0 && now_ns - last_stale_scan_ns_ < ttl_ns / 4)
            return 0;
        last_stale_scan_ns_ = now_ns;
        size_t released = 0;
        for (OpenOrder& slot : orders_) {
            if (slot.hash == 0 || slot.opened_ns == 0 ||
                now_ns < slot.opened_ns + ttl_ns)
                continue;
            release_reservation_for(slot.side, slot.qty);
            slot.hash = 0;
            slot.qty = 0;
            ++released;
        }
        if (released) {
            ++anomalies_;
            publish();
        }
        return released;
    }

    // ── Hot-side direct reads (same thread as the writer) ───────────────────
    uint64_t net_yes() const noexcept { return net_yes_; }
    uint64_t net_hedge() const noexcept { return net_hedge_; }
    uint64_t yes_avg() const noexcept { return yes_avg_; }
    uint64_t hedge_avg() const noexcept { return hedge_avg_; }
    int64_t realized_pnl() const noexcept { return realized_pnl_; }
    uint64_t open_buy() const noexcept { return open_buy_; }
    uint64_t open_sell() const noexcept { return open_sell_; }
    uint64_t open_buy_cost() const noexcept { return open_buy_cost_; }
    uint64_t fills() const noexcept { return fills_; }
    uint64_t anomalies() const noexcept { return anomalies_; }

    uint64_t sellable() const noexcept {
        return open_sell_ >= net_yes_ ? 0 : net_yes_ - open_sell_;
    }

    // Worst-case committed BUY exposure: inventory at entry cost plus the
    // full worst cost of in-flight BUYs.  Fills move quantity from the second
    // term into the first; total exposure is monotone and never double counts.
    uint64_t exposure_worst_cost() const noexcept {
        const uint64_t inventory_cost = mul_x1e6(net_yes_, yes_avg_);
        if (open_buy_cost_ > UINT64_MAX - inventory_cost) return UINT64_MAX;
        return inventory_cost + open_buy_cost_;
    }

    // ── Cold readers: coherent seqlock snapshot (invariant-safe) ────────────
    bool snapshot(Snapshot& out) const noexcept {
        const uint64_t s1 = seq_.load(std::memory_order_seq_cst);
        if (s1 & 1U) return false;
        out.net_yes = net_yes_p_.load(std::memory_order_seq_cst);
        out.net_hedge = net_hedge_p_.load(std::memory_order_seq_cst);
        out.yes_avg = yes_avg_p_.load(std::memory_order_seq_cst);
        out.hedge_avg = hedge_avg_p_.load(std::memory_order_seq_cst);
        out.realized_pnl = realized_pnl_p_.load(std::memory_order_seq_cst);
        out.open_buy = open_buy_p_.load(std::memory_order_seq_cst);
        out.open_sell = open_sell_p_.load(std::memory_order_seq_cst);
        out.open_buy_cost = open_buy_cost_p_.load(std::memory_order_seq_cst);
        out.fills = fills_p_.load(std::memory_order_seq_cst);
        out.anomalies = anomalies_p_.load(std::memory_order_seq_cst);
        out.orders_tracked = orders_tracked_p_.load(std::memory_order_seq_cst);
        out.updated_ns = updated_ns_p_.load(std::memory_order_seq_cst);
        const uint64_t s2 = seq_.load(std::memory_order_seq_cst);
        if (s1 != s2 || (s2 & 1U)) return false;
        out.version = s2;
        return true;
    }

    // Adopt the venue's authoritative inventory.  Single-writer discipline:
    // called ONLY by the hot loop, at startup or after popping a
    // TrackerRestate from the dedicated SPSC restate queue (the cold
    // reconcile thread is never a second writer of tracker state).
    void override_inventory(uint64_t yes_shares, uint64_t avg_price,
                            uint64_t hedge_shares = 0,
                            uint64_t hedge_avg_price = 0) noexcept {
        net_yes_ = yes_shares;
        yes_avg_ = yes_shares ? avg_price : 0;
        net_hedge_ = hedge_shares;
        hedge_avg_ = hedge_shares ? hedge_avg_price : 0;
        open_buy_ = 0;
        open_sell_ = 0;
        open_buy_cost_ = 0;
        orders_.fill(OpenOrder{});
        publish();
    }

private:
    struct OpenOrder {
        uint64_t hash = 0;
        uint64_t qty = 0;
        uint64_t opened_ns = 0;
        uint8_t side = 0;
        uint8_t asset = 0;
    };

    static uint64_t mul_x1e6(uint64_t a, uint64_t b) noexcept {
        return static_cast<uint64_t>(
            static_cast<crowd_uint128_t>(a) * b / 1000000ULL);
    }

    static uint64_t ceil_cost(uint64_t qty, uint64_t price) noexcept {
        const crowd_uint128_t num = static_cast<crowd_uint128_t>(qty) * price;
        return static_cast<uint64_t>((num + 1000000ULL - 1) / 1000000ULL);
    }

    // Two rotating Bloom epochs: no false negatives for recent trade ids;
    // false positives only drop duplicates, which is fail-safe.
    static constexpr size_t BLOOM_WORDS = 1024;  // 65k bits per epoch
    static constexpr size_t BLOOM_MASK = (BLOOM_WORDS * 64) - 1;

    static uint64_t dedupe_hash(uint64_t value) noexcept {
        value += 0x9e3779b97f4a7c15ULL;
        value = (value ^ (value >> 30)) * 0xbf58476d1ce4e5b9ULL;
        value = (value ^ (value >> 27)) * 0x94d049bb133111ebULL;
        return value ^ (value >> 31);
    }

    bool seen_before(uint64_t id) noexcept {
        static constexpr uint64_t EPOCH_NS = 30000000000ULL;  // 30 s
        const uint64_t epoch = crowdintel::mono_ns() / EPOCH_NS;
        if (dedupe_epoch_ == 0) {
            dedupe_epoch_ = epoch;
        } else if (epoch > dedupe_epoch_) {
            bloom_previous_ = (epoch == dedupe_epoch_ + 1)
                ? bloom_current_ : std::array<uint64_t, BLOOM_WORDS>{};
            bloom_current_.fill(0);
            dedupe_epoch_ = epoch;
        }
        const std::array<size_t, 3> pos = {
            static_cast<size_t>(dedupe_hash(id)) & BLOOM_MASK,
            static_cast<size_t>(dedupe_hash(id ^ 0xa0761d6478bd642fULL)) &
                BLOOM_MASK,
            static_cast<size_t>(dedupe_hash(id ^ 0xe7037ed1a0b428dbULL)) &
                BLOOM_MASK};
        bool present = true;
        for (const size_t p : pos)
            present &= ((bloom_current_[p >> 6] >> (p & 63U)) & 1U) == 1U ||
                       ((bloom_previous_[p >> 6] >> (p & 63U)) & 1U) == 1U;
        if (present) return true;
        for (const size_t p : pos)
            bloom_current_[p >> 6] |= 1ULL << (p & 63U);
        return false;
    }

    void track_open(uint64_t hash, uint8_t side, uint8_t asset,
                    uint64_t qty) noexcept {
        if (hash == 0 || qty == 0) return;
        for (OpenOrder& slot : orders_) {
            if (slot.hash == hash) {
                slot.qty = qty;
                slot.opened_ns = crowdintel::mono_ns();
                return;
            }
        }
        for (OpenOrder& slot : orders_) {
            if (slot.hash == 0) {
                slot.hash = hash;
                slot.qty = qty;
                slot.side = side;
                slot.asset = asset;
                slot.opened_ns = crowdintel::mono_ns();
                ++orders_tracked_plain_;
                return;
            }
        }
        ++anomalies_;  // open-order table exhausted: reconcile will repair
    }

    void close_order(const AccountEvent& ev) noexcept {
        uint64_t qty = ev.remaining;
        for (OpenOrder& slot : orders_) {
            if (slot.hash != 0 && slot.hash == ev.order_hash) {
                if (qty == 0) qty = slot.qty;
                slot.hash = 0;
                slot.qty = 0;
                break;
            }
        }
        if (qty == 0) return;
        // Cancel/reject/failed only releases the reservation; rollback of a
        // previously matched trade arrives as explicit venue events and any
        // residue is repaired by REST reconciliation (counted as anomaly).
        if (ev.type == AccountEvent::Type::FAILED) ++anomalies_;
        release_reservation_for(ev.side, qty);
    }

    void release_reservation_for(uint8_t side, uint64_t qty) noexcept {
        if (qty == 0) return;
        if (side == 0) release_buy_core(qty);
        else release_sell_core(qty);
    }

    // Proportionally reduce the worst-cost reservation when only the quantity
    // of the released BUY is known (cancel/reject paths).  Fully-flat state
    // always collapses to zero: no cost residue can survive a flat book.
    void release_buy_core(uint64_t qty) noexcept {
        const uint64_t before = open_buy_;
        open_buy_ = qty >= before ? 0 : before - qty;
        open_buy_cost_ = open_buy_ == 0 ? 0 :
            static_cast<uint64_t>(
                static_cast<crowd_uint128_t>(open_buy_cost_) * open_buy_ /
                before);
    }

    void release_sell_core(uint64_t qty) noexcept {
        open_sell_ = qty >= open_sell_ ? 0 : open_sell_ - qty;
    }

    void apply_fill(uint8_t side, uint8_t asset, uint64_t price,
                    uint64_t qty, uint64_t order_hash) noexcept {
        if (qty == 0 || price == 0) { ++anomalies_; return; }
        // Consume the local reservation first (exposure moves, it is never
        // double counted), then update inventory/P&L.
        if (side == 0) release_buy_core(qty);
        else release_sell_core(qty);

        for (OpenOrder& slot : orders_) {
            if (slot.hash != 0 && slot.hash == order_hash) {
                if (qty >= slot.qty) { slot.hash = 0; slot.qty = 0; }
                else slot.qty -= qty;
                break;
            }
        }

        uint64_t& net = asset == 1 ? net_hedge_ : net_yes_;
        uint64_t& avg = asset == 1 ? hedge_avg_ : yes_avg_;
        if (side == 0) {  // BUY fill: grow inventory at VWAP
            const crowd_uint128_t total =
                static_cast<crowd_uint128_t>(avg) * net +
                static_cast<crowd_uint128_t>(price) * qty;
            const crowd_uint128_t new_qty =
                static_cast<crowd_uint128_t>(net) + qty;
            const uint64_t new_net = static_cast<uint64_t>(new_qty);
            avg = new_net ? static_cast<uint64_t>(total / new_qty) : 0;
            net = new_net;
        } else {          // SELL fill: realize P&L at the tracked average
            const uint64_t closing = qty > net ? net : qty;
            if (qty > net) ++anomalies_;  // sold more than tracked; reconcile
            const crowd_int128_t diff =
                static_cast<crowd_int128_t>(price) -
                static_cast<crowd_int128_t>(avg);
            const int64_t pnl_delta = static_cast<int64_t>(
                diff * static_cast<crowd_int128_t>(closing) /
                static_cast<crowd_int128_t>(1000000));
            if ((pnl_delta > 0 && realized_pnl_ > INT64_MAX - pnl_delta) ||
                (pnl_delta < 0 && realized_pnl_ < INT64_MIN - pnl_delta))
                ++anomalies_;  // saturated instead of wrapping
            else
                realized_pnl_ += pnl_delta;
            net -= closing;
            if (net == 0) avg = 0;
        }
    }

    void publish() noexcept {
        seq_.fetch_add(1, std::memory_order_seq_cst);
        net_yes_p_.store(net_yes_, std::memory_order_seq_cst);
        net_hedge_p_.store(net_hedge_, std::memory_order_seq_cst);
        yes_avg_p_.store(yes_avg_, std::memory_order_seq_cst);
        hedge_avg_p_.store(hedge_avg_, std::memory_order_seq_cst);
        realized_pnl_p_.store(realized_pnl_, std::memory_order_seq_cst);
        open_buy_p_.store(open_buy_, std::memory_order_seq_cst);
        open_sell_p_.store(open_sell_, std::memory_order_seq_cst);
        open_buy_cost_p_.store(open_buy_cost_, std::memory_order_seq_cst);
        fills_p_.store(fills_, std::memory_order_seq_cst);
        anomalies_p_.store(anomalies_, std::memory_order_seq_cst);
        orders_tracked_p_.store(orders_tracked_plain_, std::memory_order_seq_cst);
        updated_ns_p_.store(updated_ns_, std::memory_order_seq_cst);
        seq_.fetch_add(1, std::memory_order_seq_cst);
    }

    // Hot-owned plain state (single writer thread).
    uint64_t net_yes_ = 0;
    uint64_t net_hedge_ = 0;
    uint64_t yes_avg_ = 0;
    uint64_t hedge_avg_ = 0;
    int64_t realized_pnl_ = 0;
    uint64_t open_buy_ = 0;
    uint64_t open_sell_ = 0;
    uint64_t open_buy_cost_ = 0;
    uint64_t fills_ = 0;
    uint64_t anomalies_ = 0;
    uint64_t updated_ns_ = 0;
    uint64_t orders_tracked_plain_ = 0;
    uint64_t last_stale_scan_ns_ = 0;
    std::array<OpenOrder, OPEN_ORDER_SLOTS> orders_{};
    std::array<uint64_t, BLOOM_WORDS> bloom_current_{};
    std::array<uint64_t, BLOOM_WORDS> bloom_previous_{};
    uint64_t dedupe_epoch_ = 0;

    // Seqlock publication (atomics, seq_cst like OrderBookL2).
    alignas(64) std::atomic<uint64_t> seq_{0};
    std::atomic<uint64_t> net_yes_p_{0};
    std::atomic<uint64_t> net_hedge_p_{0};
    std::atomic<uint64_t> yes_avg_p_{0};
    std::atomic<uint64_t> hedge_avg_p_{0};
    std::atomic<int64_t> realized_pnl_p_{0};
    std::atomic<uint64_t> open_buy_p_{0};
    std::atomic<uint64_t> open_sell_p_{0};
    std::atomic<uint64_t> open_buy_cost_p_{0};
    std::atomic<uint64_t> fills_p_{0};
    std::atomic<uint64_t> anomalies_p_{0};
    std::atomic<uint64_t> orders_tracked_p_{0};
    std::atomic<uint64_t> updated_ns_p_{0};
};

#endif  // POSITION_TRACKER_HPP
