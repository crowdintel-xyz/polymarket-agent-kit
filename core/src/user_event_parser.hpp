#ifndef USER_EVENT_PARSER_HPP
#define USER_EVENT_PARSER_HPP

// ─────────────────────────────────────────────────────────────────────────────
// UserEventParser: pure, offline-testable normalizer of Polymarket's PRIVATE
// user-channel messages into AccountEvents.
//
// Recognized venue facts (docs.polymarket.com, user channel):
//   * order events: event_type="order", type=PLACEMENT|CANCELLATION|UPDATE
//   * trade events: event_type="trade", type=MATCHED|MINED|CONFIRMED|
//                   RETRYING|FAILED; `trader_side` tells whether the user was
//                   MAKER or TAKER; `maker_orders` carries the maker fills
//                   (relevant when our resting order was hit).
//
// The parser is deliberately strict, matching the repository's fail-closed
// posture: duplicate semantic keys, malformed numbers and unknown sides are
// dropped rather than guessed.  Parsing happens on the user-WS thread (cold);
// only normalized fixed-point events enter the hot queue.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstdio>
#include <cstring>

#include "../include/account_events.hpp"
#include "../include/bounded_json.hpp"
#include "../include/time_utils.hpp"
#include "json_fields.hpp"
#include "polymarket_order.hpp"  // parse_fixed1e6

class UserEventParser {
public:
    UserEventParser(const char* token_id_dec, const char* hedge_token_id_dec,
                    uint64_t market_hash)
        : market_hash_(market_hash) {
        if (token_id_dec) {
            std::snprintf(token_, sizeof(token_), "%s", token_id_dec);
            token_[sizeof(token_) - 1] = '\0';
        }
        if (hedge_token_id_dec && hedge_token_id_dec[0]) {
            std::snprintf(hedge_token_, sizeof(hedge_token_), "%s",
                          hedge_token_id_dec);
            hedge_token_[sizeof(hedge_token_) - 1] = '\0';
        }
    }

    // Parse one venue message.  Returns true and fills `out` when the message
    // is a normalized account fact for OUR configured asset(s).
    bool parse(const char* json, size_t len, AccountEvent& out) const {
        if (!json || len == 0 || !bounded_json::valid_document(json, len))
            return false;
        const char* end = json + len;

        char event[24]{};
        if (!extract_unique(json, end, "event_type", event, sizeof(event)) &&
            !extract_unique(json, end, "type", event, sizeof(event)))
            return false;

        if (std::strcmp(event, "order") == 0) return parse_order(json, end, out);
        if (std::strcmp(event, "trade") == 0) return parse_trade(json, end, out);
        return false;
    }

    static bool is_user_message(const char* json, size_t len) {
        if (!json || len == 0 || !bounded_json::valid_document(json, len))
            return false;
        const char* end = json + len;
        char event[24]{};
        return (extract_unique(json, end, "event_type", event, sizeof(event)) ||
                extract_unique(json, end, "type", event, sizeof(event))) &&
               (std::strcmp(event, "order") == 0 ||
                std::strcmp(event, "trade") == 0);
    }

private:
    // A field is used only when it occurs exactly once at the top level.
    static bool extract_unique(const char* begin, const char* end,
                               const char* key, char* out, size_t cap) {
        return json_fields::extract_string(begin, end, key, out, cap);
    }

    uint8_t asset_role(const char* asset) const {
        if (token_[0] && std::strcmp(asset, token_) == 0) return 0;
        if (hedge_token_[0] && std::strcmp(asset, hedge_token_) == 0) return 1;
        return 255;  // not our asset
    }

    static bool parse_side(const char* text, uint8_t& side) {
        if (text[0] == 'B' || text[0] == 'b') { side = 0; return true; }
        if (text[0] == 'S' || text[0] == 's') { side = 1; return true; }
        return false;
    }

    bool parse_order(const char* json, const char* end,
                     AccountEvent& out) const {
        char type[16]{}, id[96]{}, asset[96]{}, side_text[8]{};
        char price_text[24]{}, matched_text[32]{}, original_text[32]{};

        if (!extract_unique(json, end, "id", id, sizeof(id)) || !id[0])
            return false;
        // asset_id may be absent on order events for single-asset streams
        if (!extract_unique(json, end, "asset_id", asset, sizeof(asset)))
            return false;
        const uint8_t role = asset_role(asset);
        if (role == 255) return false;
        if (!extract_unique(json, end, "type", type, sizeof(type)) &&
            !extract_unique(json, end, "order_status", type, sizeof(type)))
            return false;

        AccountEvent ev{};
        ev.asset = role;
        ev.market_hash = market_hash_;
        ev.order_hash = json_fields::fnv_hash(id, std::strlen(id));
        ev.event_id = ev.order_hash ^
                      (json_fields::fnv_hash(type, std::strlen(type)) << 1);
        ev.timestamp_ns = crowdintel::realtime_ns();

        uint64_t price = 0;
        if (extract_unique(json, end, "price", price_text, sizeof(price_text)))
            (void)parse_fixed1e6(price_text, std::strlen(price_text), price);
        ev.price = price;

        uint64_t matched = 0, original = 0;
        if (extract_unique(json, end, "size_matched", matched_text,
                           sizeof(matched_text)))
            (void)parse_fixed1e6(matched_text, std::strlen(matched_text),
                                 matched);
        if (extract_unique(json, end, "original_size", original_text,
                           sizeof(original_text)))
            (void)parse_fixed1e6(original_text, std::strlen(original_text),
                                 original);

        if (!extract_unique(json, end, "side", side_text,
                            sizeof(side_text)) ||
            !parse_side(side_text, ev.side))
            return false;
        ev.remaining = original > matched ? original - matched : 0;

        if (std::strcmp(type, "PLACEMENT") == 0 ||
            std::strcmp(type, "LIVE") == 0) {
            ev.type = AccountEvent::Type::OPEN;
            ev.size = ev.remaining;
        } else if (std::strcmp(type, "CANCELLATION") == 0 ||
                   std::strcmp(type, "CANCELLED") == 0 ||
                   std::strcmp(type, "CANCEL") == 0) {
            ev.type = AccountEvent::Type::CANCEL;
            ev.size = ev.remaining;
        } else if (std::strcmp(type, "UPDATE") == 0) {
            ev.type = AccountEvent::Type::OPEN;  // restate remaining
            ev.size = ev.remaining;
        } else {
            return false;
        }
        out = ev;
        return true;
    }

    bool parse_trade(const char* json, const char* end,
                     AccountEvent& out) const {
        char type[16]{}, id[96]{}, asset[96]{}, side_text[8]{};
        char price_text[24]{}, size_text[32]{}, status[24]{};
        char taker_order[96]{};

        if (!extract_unique(json, end, "type", type, sizeof(type)))
            return false;
        const bool matched = std::strcmp(type, "MATCHED") == 0;
        const bool mined = std::strcmp(type, "MINED") == 0 ||
                           std::strcmp(type, "CONFIRMED") == 0;
        const bool failed = std::strcmp(type, "FAILED") == 0 ||
                            std::strcmp(type, "RETRYING") == 0;
        if (!matched && !mined && !failed) return false;

        if (!extract_unique(json, end, "id", id, sizeof(id)) || !id[0])
            return false;
        if (!extract_unique(json, end, "asset_id", asset, sizeof(asset)))
            return false;
        const uint8_t role = asset_role(asset);
        if (role == 255) return false;
        if (!extract_unique(json, end, "side", side_text, sizeof(side_text)) ||
            !extract_unique(json, end, "price", price_text,
                            sizeof(price_text)) ||
            !extract_unique(json, end, "size", size_text, sizeof(size_text)))
            return false;

        AccountEvent ev{};
        ev.asset = role;
        if (!parse_side(side_text, ev.side)) return false;
        if (!parse_fixed1e6(price_text, std::strlen(price_text), ev.price) ||
            !parse_fixed1e6(size_text, std::strlen(size_text), ev.size) ||
            ev.size == 0 || ev.price == 0)
            return false;
        ev.market_hash = market_hash_;
        ev.event_id = json_fields::fnv_hash(id, std::strlen(id));
        ev.timestamp_ns = crowdintel::realtime_ns();
        if (extract_unique(json, end, "taker_order_id", taker_order,
                           sizeof(taker_order)))
            ev.order_hash =
                json_fields::fnv_hash(taker_order, std::strlen(taker_order));
        if (extract_unique(json, end, "status", status, sizeof(status)) &&
            std::strcmp(status, "FAILED") == 0)
            ev.type = AccountEvent::Type::FAILED;
        else
            ev.type = matched ? AccountEvent::Type::FILL
                              : (mined ? AccountEvent::Type::FILL_MINED
                                       : AccountEvent::Type::FAILED);
        out = ev;
        return true;
    }

    uint64_t market_hash_ = 0;
    char token_[96]{};
    char hedge_token_[96]{};
};

#endif  // USER_EVENT_PARSER_HPP
