#ifndef MARKET_CONFIG_HPP
#define MARKET_CONFIG_HPP

#include <cerrno>
#include <climits>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <string>
#include <sys/stat.h>
#include <unistd.h>

#include "../crypto/secure_zero.hpp"
#include "../crypto/sha256_engine.hpp"
#include "polymarket_order.hpp"
#include "alpha_receiver.hpp"

struct MarketConfig {
    // Identity / credentials
    char private_key_hex[65]{};
    uint8_t maker[20]{};
    uint8_t signer[20]{};
    char maker_hex[43]{};
    char signer_hex[43]{};
    char api_address_hex[43]{};  // address to which the supplied API key belongs
    char owner_api_key[64]{};
    char api_secret_b64[128]{};
    char api_passphrase[128]{};

    // Market
    char token_id_dec[80]{};
    uint8_t token_id_be[32]{};
    char market_slug[96] = "mock-market";
    uint64_t market_hash = 0;
    uint64_t tick_size = 10000;
    bool neg_risk = false;
    uint8_t signature_type = 0;
    char order_type[8] = "FAK";
    uint64_t gtd_ttl_seconds = 0;
    char clob_host[128] = "https://clob.polymarket.com";
    char ws_host[192] = "wss://ws-subscriptions-clob.polymarket.com/ws/market";
    // Private user channel (order/fill reconciliation) and hedge complement.
    char ws_user_host[192] = "wss://ws-subscriptions-clob.polymarket.com/ws/user";
    char market_condition_id[80]{};  // optional 0x…64hex for subscription scoping
    char hedge_token_id_dec[80]{};   // optional complement token id (binary pair)
    uint8_t hedge_token_id_be[32]{};

    // Strategy / risk.  Values are deliberately conservative until account
    // reconciliation is implemented.
    double bankroll_usd = 1000.0;
    double kelly_fraction = 0.10;
    double min_edge = 0.02;
    double min_confidence = 0.85;
    double max_q_value = 0.05;
    double taker_fee_rate = 0.072;      // T1.2 FIX: match FeeCalculator::TAKER_FEE_RATE (7.2%)
    double max_order_usd = 100.0;
    double max_exposure_usd = 250.0;
    double max_daily_loss_usd = 50.0;
    uint64_t initial_position_shares = 0;
    double initial_position_avg_price = 0.50;  // VWAP of supplied inventory
    uint64_t min_size_shares = 5000000;
    uint64_t presign_ttl_ms = 3000;
    uint64_t signal_ttl_ms = 2000;
    uint64_t max_book_age_ms = 3000;

    // ── Brakes (P2) ─────────────────────────────────────────────────────────
    double stop_loss_pct = 0.15;      // mark drop vs VWAP entry; 0 disables
    double hedge_trigger_pct = 0.0;   // hedge trigger before stop; 0 disables
    double max_portfolio_exposure_usd = 250.0;
    uint64_t reservation_ttl_ms = 10000;   // unconfirmed reservation release
    uint64_t reconcile_interval_sec = 30;  // REST reconciliation; 0 disables
    double reconcile_max_drift_shares = 0.01;  // max tolerated |REST−local|

    // ── Adverse selection (P3) ──────────────────────────────────────────────
    double pool_max_dev_bps = 200.0;    // signed-price vs current mid guard
    uint64_t pool_vol_ttl_ms = 500;     // ladder TTL while volatility is high
    double vol_max_spread_bps = 1500.0; // spread regime threshold
    uint64_t vol_max_ticks_per_sec = 200;    // markdown tick-rate threshold
    double vol_mid_gap_bps = 500.0;     // EMA mid-gap regime threshold
    double vol_size_multiplier = 0.5;   // passive size scale in volatile regime

    // ── Brain (P4) ──────────────────────────────────────────────────────────
    double bayes_prior_strength = 24.0;   // prior pseudo-count N0
    double bayes_signal_threshold = 0.03; // posterior-vs-price edge gate
    double bayes_min_reliability = 0.35;  // source weight gate [0,1]
    char bayes_sources[512]{};            // "id:weight,id:weight,…" (0..1)
    char bayes_recal_file[192]{};         // optional cold recalibration file
    char evidence_file[192]{};            // NDJSON evidence replay/tail file
    bool bayes_enable = true;

    // Alpha HTTP receiver (designed to sit behind a TLS/auth reverse proxy).
    char alpha_bind[64] = "127.0.0.1";
    uint16_t alpha_port = 8088;
    char alpha_bearer_token[128]{};

    // Runtime
    int pin_cpu = -1;
    int cold_cpu = -1;
    long max_ticks = 0;
    bool mock_mode = false;
    bool live_armed = false;
    char tls_pin[128]{};
    char kill_switch_file[192] = "/tmp/crowdintel.kill";

    // ── Multi-feed configuration (PHASE-1 enhancement) ──────────────────────
    uint32_t strategy_window_seconds = 300;  // 300=5m, 900=15m
    char kline_interval[16] = "@kline_5m";   // Binance kline stream suffix
    double pyth_weight = 0.55;               // Pyth feed weight in fusion
    double binance_weight = 0.45;            // Binance feed weight in fusion
    bool enable_pyth_feed = true;            // enable/disable Pyth stream
    uint64_t feed_stale_ns = 500'000'000ULL;  // 500ms staleness threshold
    bool feed_failover_enabled = true;       // failover when primary drops

    ~MarketConfig() {
        secure_zero(private_key_hex, sizeof(private_key_hex));
        secure_zero(api_secret_b64, sizeof(api_secret_b64));
        secure_zero(api_passphrase, sizeof(api_passphrase));
        secure_zero(alpha_bearer_token, sizeof(alpha_bearer_token));
    }

    MarketConfig() = default;
    MarketConfig(const MarketConfig&) = delete;
    MarketConfig& operator=(const MarketConfig&) = delete;

    static const char* env(const char* key, const char* fallback) {
        const char* value = std::getenv(key);
        return value && *value ? value : fallback;
    }

    static bool env_d(const char* key, double fallback, double& out) {
        const char* value = std::getenv(key);
        if (!value || !*value) { out = fallback; return true; }
        errno = 0;
        char* end = nullptr;
        const double parsed = std::strtod(value, &end);
        if (errno == ERANGE || !end || *end != '\0' || !std::isfinite(parsed))
            return false;
        out = parsed;
        return true;
    }

    static bool env_l(const char* key, long fallback, long& out) {
        const char* value = std::getenv(key);
        if (!value || !*value) { out = fallback; return true; }
        errno = 0;
        char* end = nullptr;
        const long parsed = std::strtol(value, &end, 10);
        if (errno == ERANGE || !end || *end != '\0') return false;
        out = parsed;
        return true;
    }

    const char* load(bool need_trading_creds, bool force_mock = false) {
        if (!read_secret("BOT_PRIVATE_KEY_HEX", private_key_hex,
                         sizeof(private_key_hex)) ||
            std::strlen(private_key_hex) != 64)
            return "BOT_PRIVATE_KEY_HEX or BOT_PRIVATE_KEY_HEX_FILE must contain 64 hex chars";
        uint8_t key_check[32];
        const bool private_key_is_hex = parse_hex_bytes(
            private_key_hex, std::strlen(private_key_hex), key_check,
            sizeof(key_check));
        secure_zero(key_check, sizeof(key_check));
        if (!private_key_is_hex)
            return "BOT_PRIVATE_KEY_HEX contains non-hexadecimal characters";

        long neg_risk_l = 0, sig = 0, presign_l = 0, signal_l = 0;
        long book_age_l = 0, pin_l = -1, cold_l = -1, ticks_l = 0;
        long armed_l = 0, gtd_l = 0;
        long reservation_ttl_l = 10000, reconcile_l = 30, pool_vol_ttl_l = 500;
        long vol_ticks_l = 200, bayes_on_l = 1;
        double tick = 0, min_size = 0, initial_position = 0;
        double initial_avg = 0.50;
        if (!env_l("BOT_NEG_RISK", 0, neg_risk_l) ||
            !env_l("BOT_SIGNATURE_TYPE", 0, sig) ||
            !env_d("BOT_TICK_SIZE", 0.01, tick) ||
            !env_d("BOT_BANKROLL_USD", bankroll_usd, bankroll_usd) ||
            !env_d("BOT_KELLY_FRACTION", kelly_fraction, kelly_fraction) ||
            !env_d("BOT_MIN_EDGE", min_edge, min_edge) ||
            !env_d("BOT_MIN_CONFIDENCE", min_confidence, min_confidence) ||
            !env_d("BOT_MAX_Q_VALUE", max_q_value, max_q_value) ||
            !env_d("BOT_TAKER_FEE_RATE", taker_fee_rate, taker_fee_rate) ||
            !env_d("BOT_MAX_ORDER_USD", max_order_usd, max_order_usd) ||
            !env_d("BOT_MAX_EXPOSURE_USD", max_exposure_usd, max_exposure_usd) ||
            !env_d("BOT_MAX_DAILY_LOSS_USD", max_daily_loss_usd,
                   max_daily_loss_usd) ||
            !env_d("BOT_MIN_SIZE_SHARES", 5.0, min_size) ||
            !env_d("BOT_INITIAL_POSITION_SHARES", 0.0, initial_position) ||
            !env_d("BOT_INITIAL_POSITION_AVG_PRICE", 0.50, initial_avg) ||
            !env_d("BOT_STOP_LOSS_PCT", stop_loss_pct, stop_loss_pct) ||
            !env_d("BOT_HEDGE_TRIGGER_PCT", hedge_trigger_pct,
                   hedge_trigger_pct) ||
            !env_d("BOT_MAX_PORTFOLIO_EXPOSURE_USD",
                   max_portfolio_exposure_usd, max_portfolio_exposure_usd) ||
            !env_d("BOT_POOL_MAX_DEV_BPS", pool_max_dev_bps,
                   pool_max_dev_bps) ||
            !env_d("BOT_VOL_MAX_SPREAD_BPS", vol_max_spread_bps,
                   vol_max_spread_bps) ||
            !env_d("BOT_VOL_MID_GAP_BPS", vol_mid_gap_bps, vol_mid_gap_bps) ||
            !env_d("BOT_VOL_SIZE_MULTIPLIER", vol_size_multiplier,
                   vol_size_multiplier) ||
            !env_d("BOT_BAYES_PRIOR_STRENGTH", bayes_prior_strength,
                   bayes_prior_strength) ||
            !env_d("BOT_BAYES_SIGNAL_THRESHOLD", bayes_signal_threshold,
                   bayes_signal_threshold) ||
            !env_d("BOT_BAYES_MIN_RELIABILITY", bayes_min_reliability,
                   bayes_min_reliability) ||
            !env_d("BOT_RECONCILE_MAX_DRIFT_SHARES",
                   reconcile_max_drift_shares, reconcile_max_drift_shares) ||
            !env_l("BOT_PRESIGN_TTL_MS", static_cast<long>(presign_ttl_ms),
                   presign_l) ||
            !env_l("BOT_SIGNAL_TTL_MS", static_cast<long>(signal_ttl_ms),
                   signal_l) ||
            !env_l("BOT_MAX_BOOK_AGE_MS", static_cast<long>(max_book_age_ms),
                   book_age_l) ||
            !env_l("BOT_PIN_CPU", -1, pin_l) ||
            !env_l("BOT_COLD_CPU", -1, cold_l) ||
            !env_l("BOT_TICKS", 0, ticks_l) ||
            !env_l("BOT_ENABLE_LIVE_TRADING", 0, armed_l) ||
            !env_l("BOT_GTD_TTL_SECONDS", 0, gtd_l) ||
            !env_l("BOT_RESERVATION_TTL_MS", 10000, reservation_ttl_l) ||
            !env_l("BOT_RECONCILE_INTERVAL_SEC", 30, reconcile_l) ||
            !env_l("BOT_POOL_VOL_TTL_MS", 500, pool_vol_ttl_l) ||
            !env_l("BOT_VOL_MAX_TICKS_PER_SEC", 200, vol_ticks_l) ||
            !env_l("BOT_BAYES_ENABLE", 1, bayes_on_l))
            return "invalid numeric configuration value";

        if ((neg_risk_l != 0 && neg_risk_l != 1) ||
            (armed_l != 0 && armed_l != 1))
            return "BOT_NEG_RISK and BOT_ENABLE_LIVE_TRADING must be 0 or 1";
        if (sig < 0 || sig > 3)
            return "BOT_SIGNATURE_TYPE must be 0, 1, 2, or 3";
        if (!(tick > 0.0 && tick < 1.0) ||
            tick * 1000000.0 > static_cast<double>(UINT64_MAX))
            return "BOT_TICK_SIZE is out of range";
        const double min_size_scaled = min_size * 1000000.0;
        const double initial_position_scaled = initial_position * 1000000.0;
        if (min_size <= 0.0 || initial_position < 0.0 ||
            min_size >= static_cast<double>(UINT64_MAX) / 1000000.0 ||
            initial_position >= static_cast<double>(UINT64_MAX) / 1000000.0 ||
            std::fabs(min_size_scaled - std::round(min_size_scaled)) > 1e-6 ||
            std::fabs(initial_position_scaled -
                      std::round(initial_position_scaled)) > 1e-6 ||
            static_cast<uint64_t>(std::round(min_size_scaled)) %
                    K_SHARE_QUANTUM != 0 ||
            static_cast<uint64_t>(std::round(initial_position_scaled)) %
                    K_SHARE_QUANTUM != 0)
            return "share configuration must be non-negative and exact to 0.01 shares";
        constexpr uint64_t MAX_SAFE_MS = UINT64_MAX / 1000000ULL;
        if (presign_l <= 0 || signal_l <= 0 || book_age_l <= 0 ||
            static_cast<uint64_t>(presign_l) > MAX_SAFE_MS ||
            static_cast<uint64_t>(signal_l) > MAX_SAFE_MS ||
            static_cast<uint64_t>(book_age_l) > MAX_SAFE_MS ||
            ticks_l < 0 || gtd_l < 0 || gtd_l > 31536000 ||
            pin_l < -1 || cold_l < -1 || pin_l > INT_MAX || cold_l > INT_MAX)
            return "runtime duration/CPU configuration is out of range";

        if (!(initial_avg >= 0.0 && initial_avg <= 1.0))
            return "BOT_INITIAL_POSITION_AVG_PRICE must be within [0, 1]";
        constexpr uint64_t MAX_SAFE_MS2 = UINT64_MAX / 1000000ULL;
        if (reservation_ttl_l < 1000 ||
                static_cast<uint64_t>(reservation_ttl_l) > MAX_SAFE_MS2 ||
            reconcile_l < 0 || reconcile_l > 86400 ||
            pool_vol_ttl_l <= 0 ||
                static_cast<uint64_t>(pool_vol_ttl_l) > MAX_SAFE_MS2 ||
            vol_ticks_l < 0 || vol_ticks_l > 1000000 ||
            (bayes_on_l != 0 && bayes_on_l != 1))
            return "layer2+ duration/rate configuration is out of range";
        if (stop_loss_pct < 0.0 || stop_loss_pct > 1.0 ||
            hedge_trigger_pct < 0.0 || hedge_trigger_pct > 1.0 ||
            max_portfolio_exposure_usd <= 0.0 ||
            pool_max_dev_bps < 0.0 || pool_max_dev_bps > 5000.0 ||
            vol_max_spread_bps <= 0.0 || vol_max_spread_bps > 9000.0 ||
            vol_mid_gap_bps <= 0.0 || vol_mid_gap_bps > 5000.0 ||
            vol_size_multiplier < 0.0 || vol_size_multiplier > 1.0 ||
            bayes_prior_strength <= 0.0 || bayes_prior_strength > 100000.0 ||
            bayes_signal_threshold <= 0.0 || bayes_signal_threshold >= 1.0 ||
            bayes_min_reliability < 0.0 || bayes_min_reliability > 1.0 ||
            reconcile_max_drift_shares < 0.0 ||
            reconcile_max_drift_shares > 1000000.0)
            return "invalid brakes/adverse-selection/brain configuration value";
        if (hedge_trigger_pct > 0.0 && stop_loss_pct > 0.0 &&
            hedge_trigger_pct >= stop_loss_pct)
            return "BOT_HEDGE_TRIGGER_PCT must fire strictly before BOT_STOP_LOSS_PCT";
        reservation_ttl_ms = static_cast<uint64_t>(reservation_ttl_l);
        reconcile_interval_sec = static_cast<uint64_t>(reconcile_l);
        pool_vol_ttl_ms = static_cast<uint64_t>(pool_vol_ttl_l);
        vol_max_ticks_per_sec = static_cast<uint64_t>(vol_ticks_l);
        bayes_enable = bayes_on_l == 1;
        neg_risk = neg_risk_l == 1;
        signature_type = static_cast<uint8_t>(sig);
        if (signature_type == 3)
            return "POLY_1271 requires ERC-7739 wrapping; this build fails closed. Use type 0/1/2 or the official V2 signer sidecar";
        tick_size = static_cast<uint64_t>(std::round(tick * 1000000.0));
        min_size_shares = static_cast<uint64_t>(std::round(min_size_scaled));
        initial_position_shares =
            static_cast<uint64_t>(std::round(initial_position_scaled));
        initial_position_avg_price = initial_avg;
        presign_ttl_ms = static_cast<uint64_t>(presign_l);
        signal_ttl_ms = static_cast<uint64_t>(signal_l);
        max_book_age_ms = static_cast<uint64_t>(book_age_l);
        pin_cpu = static_cast<int>(pin_l);
        cold_cpu = static_cast<int>(cold_l);
        max_ticks = ticks_l;
        const char* mode = std::getenv("BOT_MODE");
        if (mode && *mode && std::strcmp(mode, "mock") != 0 &&
            std::strcmp(mode, "live") != 0)
            return "BOT_MODE must be mock or live";
        mock_mode = force_mock || (mode && std::strcmp(mode, "mock") == 0);
        live_armed = armed_l == 1;
        gtd_ttl_seconds = static_cast<uint64_t>(gtd_l);

        if (!copy_env(order_type, "BOT_ORDER_TYPE", order_type) ||
            !copy_env(clob_host, "CLOB_HOST", clob_host) ||
            !copy_env(ws_host, "WS_HOST", ws_host) ||
            !copy_env(ws_user_host, "BOT_WS_USER_HOST", ws_user_host) ||
            !copy_env(market_condition_id, "BOT_MARKET_CONDITION_ID", "") ||
            !copy_env(hedge_token_id_dec, "BOT_HEDGE_TOKEN_ID", "") ||
            !copy_env(tls_pin, "BOT_TLS_PIN", "") ||
            !copy_env(kill_switch_file, "BOT_KILL_SWITCH_FILE",
                      kill_switch_file) ||
            !copy_env(market_slug, "BOT_MARKET_SLUG",
                      mock_mode ? "mock-market" : "") ||
            !copy_env(bayes_sources, "BOT_BAYES_SOURCES", "") ||
            !copy_env(bayes_recal_file, "BOT_BAYES_RECAL_FILE", "") ||
            !copy_env(evidence_file, "BOT_EVIDENCE_FILE", "") ||
            !copy_env(alpha_bind, "BOT_ALPHA_BIND", alpha_bind))
            return "configuration string exceeds its bounded capacity";
        if (!valid_bayes_sources(bayes_sources))
            return "BOT_BAYES_SOURCES must be 'id:weight' pairs (weight in [0,1])";
        if (!safe_config_text(bayes_recal_file))
            return "BOT_BAYES_RECAL_FILE cannot contain control characters";
        if (!safe_config_text(order_type) || !safe_config_text(clob_host) ||
            !safe_config_text(ws_host) || !safe_config_text(tls_pin) ||
            !safe_config_text(kill_switch_file) ||
            !safe_config_text(market_slug) || !safe_config_text(alpha_bind))
            return "configuration strings cannot contain control characters";
        if (tls_pin[0] && !valid_tls_pins(tls_pin))
            return "BOT_TLS_PIN must contain one or two sha256// pins separated by a semicolon";
        long alpha_port_l = alpha_port;
        if (!env_l("BOT_ALPHA_PORT", alpha_port, alpha_port_l) ||
            alpha_port_l < 1 || alpha_port_l > 65535)
            return "BOT_ALPHA_PORT is invalid";
        alpha_port = static_cast<uint16_t>(alpha_port_l);
        market_hash = alpha_hash_bytes(market_slug, std::strlen(market_slug));

        if (std::strcmp(order_type, "GTC") != 0 &&
            std::strcmp(order_type, "GTD") != 0 &&
            std::strcmp(order_type, "FOK") != 0 &&
            std::strcmp(order_type, "FAK") != 0)
            return "BOT_ORDER_TYPE must be GTC, GTD, FOK, or FAK";
        if (std::strcmp(order_type, "GTD") == 0 && gtd_ttl_seconds < 120)
            return "GTD requires BOT_GTD_TTL_SECONDS >= 120 (plus the venue's 60-second buffer)";
        if (std::strcmp(order_type, "GTD") == 0 &&
            presign_ttl_ms >= gtd_ttl_seconds * 1000ULL)
            return "BOT_PRESIGN_TTL_MS must be shorter than the GTD lifetime";
        const double scaled_tick = tick * 1000000.0;
        if (std::fabs(scaled_tick - std::round(scaled_tick)) > 1e-6 ||
            tick_size == 0 || tick_size >= 1000000 ||
            1000000 % tick_size != 0 || amount_quantum_for_tick(tick_size) == 0)
            return "BOT_TICK_SIZE is not a supported exact V2 tick";
        if (bankroll_usd <= 0 || kelly_fraction <= 0 || kelly_fraction > 1 ||
            min_edge < 0 || min_edge >= 1 || min_confidence < 0 ||
            min_confidence > 1 || max_q_value < 0 || max_q_value > 1 ||
            taker_fee_rate < 0 || taker_fee_rate > 1 || max_order_usd <= 0 ||
            max_exposure_usd <= 0 || max_daily_loss_usd <= 0)
            return "invalid strategy/risk configuration";
        if (!valid_market_slug(market_slug))
            return "BOT_MARKET_SLUG must be a non-empty URL-safe slug";
        if (!mock_mode && !valid_secure_url(clob_host, "https://", false))
            return "CLOB_HOST must be an origin-only https:// URL in live mode";
        if (!mock_mode && !valid_secure_url(ws_host, "wss://", true))
            return "WS_HOST must be a valid wss:// URL in live mode";
        if (!mock_mode && !valid_secure_url(ws_user_host, "wss://", true))
            return "BOT_WS_USER_HOST must be a valid wss:// URL in live mode";
        if (market_condition_id[0] &&
            !valid_condition_id(market_condition_id))
            return "BOT_MARKET_CONDITION_ID must be 0x followed by 64 lowercase hex chars";

        if (need_trading_creds) {
            if (!live_armed)
                return "live mode is disarmed; set BOT_ENABLE_LIVE_TRADING=1 after completing the preflight checklist";
            if (!read_secret("CLOB_API_KEY", owner_api_key, sizeof(owner_api_key)) ||
                !read_secret("CLOB_SECRET", api_secret_b64, sizeof(api_secret_b64)) ||
                !read_secret("CLOB_PASSPHRASE", api_passphrase,
                             sizeof(api_passphrase)))
                return "CLOB_API_KEY/SECRET/PASSPHRASE (or *_FILE) are required";
            if (!printable_secret(owner_api_key) ||
                !printable_secret(api_secret_b64) ||
                !printable_secret(api_passphrase) ||
                std::strchr(owner_api_key, '"') ||
                std::strchr(owner_api_key, '\\'))
                return "CLOB credentials contain unsafe whitespace/control/JSON characters";
            uint8_t decoded_secret[64]{};
            const size_t decoded_secret_len = base64url_decode(
                api_secret_b64, std::strlen(api_secret_b64), decoded_secret,
                sizeof(decoded_secret));
            secure_zero(decoded_secret, sizeof(decoded_secret));
            if (decoded_secret_len == 0 || decoded_secret_len == SIZE_MAX)
                return "CLOB_SECRET is not valid bounded base64/base64url";
            if (!read_secret("BOT_ALPHA_BEARER_TOKEN", alpha_bearer_token,
                             sizeof(alpha_bearer_token)) ||
                std::strlen(alpha_bearer_token) < 16 ||
                !printable_secret(alpha_bearer_token))
                return "BOT_ALPHA_BEARER_TOKEN (or *_FILE) must contain at least 16 printable chars";
        } else {
            std::snprintf(owner_api_key, sizeof(owner_api_key),
                          "00000000-0000-0000-0000-000000000000");
        }

        const char* tok = env("BOT_TOKEN_ID",
            "71321045679252212594626395510336467040167069592778062791519851593659551227755");
        const size_t token_len = std::strlen(tok);
        if (token_len >= sizeof(token_id_dec) ||
            (token_len > 1 && tok[0] == '0') ||
            !parse_uint256_dec(tok, token_len, token_id_be))
            return "BOT_TOKEN_ID is not a canonical uint256 decimal";
        bool token_nonzero = false;
        for (const uint8_t byte : token_id_be) token_nonzero |= byte != 0;
        if (!token_nonzero) return "BOT_TOKEN_ID cannot be zero";
        std::memcpy(token_id_dec, tok, token_len + 1);
        if (hedge_token_id_dec[0]) {
            const size_t hedge_len = std::strlen(hedge_token_id_dec);
            if (hedge_len >= sizeof(hedge_token_id_dec) ||
                (hedge_len > 1 && hedge_token_id_dec[0] == '0') ||
                !parse_uint256_dec(hedge_token_id_dec, hedge_len,
                                   hedge_token_id_be) ||
                std::strcmp(hedge_token_id_dec, token_id_dec) == 0)
                return "BOT_HEDGE_TOKEN_ID is not a canonical nonzero uint256 distinct from BOT_TOKEN_ID";
            bool hedge_nonzero = false;
            for (const uint8_t byte : hedge_token_id_be)
                hedge_nonzero |= byte != 0;
            if (!hedge_nonzero) return "BOT_HEDGE_TOKEN_ID cannot be zero";
        }
        return nullptr;
    }

    // Called once after the only EIP712Signer has derived the EOA address.
    const char* finalize_identity(const uint8_t signer_address[20]) {
        std::memcpy(signer, signer_address, 20);
        const char* maker_env = std::getenv("BOT_MAKER_ADDRESS");
        if (maker_env) {
            if (!parse_hex_bytes(maker_env, std::strlen(maker_env), maker, 20))
                return "BOT_MAKER_ADDRESS is not a valid 20-byte hex address";
        } else {
            if (signature_type == 1 || signature_type == 2)
                return "BOT_MAKER_ADDRESS is required for proxy/safe signature types";
            std::memcpy(maker, signer, 20);
        }
        format_address(maker, maker_hex);
        format_address(signer, signer_hex);

        const char* api_address = std::getenv("BOT_API_ADDRESS");
        if (api_address) {
            uint8_t parsed[20];
            if (!parse_hex_bytes(api_address, std::strlen(api_address), parsed, 20))
                return "BOT_API_ADDRESS is not a valid 20-byte hex address";
            format_address(parsed, api_address_hex);
        } else {
            // L2 credentials for legacy types are normally bound to the EOA.
            std::memcpy(api_address_hex, signer_hex, sizeof(signer_hex));
        }
        return nullptr;
    }

    uint64_t wire_expiration(uint64_t now_seconds) const noexcept {
        if (std::strcmp(order_type, "GTD") != 0) return 0;
        const uint64_t offset = 60ULL + gtd_ttl_seconds;
        if (now_seconds > UINT64_MAX - offset) return 0;
        return now_seconds + offset;
    }

private:
    static bool valid_bayes_sources(const char* sources) noexcept {
        if (!sources) return false;
        if (!*sources) return true;  // empty = all sources at default weight
        const char* p = sources;
        while (*p) {
            const char* colon = std::strchr(p, ':');
            if (!colon || colon == p) return false;
            long id = 0;
            for (const char* c = p; c < colon; ++c) {
                if (*c < '0' || *c > '9') return false;
                id = id * 10 + (*c - '0');
                if (id > 255) return false;
            }
            const char* v = colon + 1;
            if (*v == '\0') return false;
            char* end = nullptr;
            errno = 0;
            const double w = std::strtod(v, &end);
            if (errno == ERANGE || !end || !std::isfinite(w) ||
                w < 0.0 || w > 1.0)
                return false;
            if (*end == '\0') return true;
            if (*end != ',') return false;
            p = end + 1;
            if (*p == '\0') return false;
        }
        return true;
    }

    static bool valid_condition_id(const char* id) noexcept {
        if (!id || id[0] != '0' || id[1] != 'x' || std::strlen(id) != 66)
            return false;
        for (size_t i = 2; i < 66; ++i)
            if (!(id[i] >= '0' && id[i] <= '9') &&
                !(id[i] >= 'a' && id[i] <= 'f'))
                return false;
        return true;
    }

    static bool valid_market_slug(const char* slug) noexcept {
        if (!slug || !*slug) return false;
        for (const unsigned char* p =
                 reinterpret_cast<const unsigned char*>(slug); *p; ++p) {
            const bool allowed = (*p >= 'a' && *p <= 'z') ||
                (*p >= 'A' && *p <= 'Z') || (*p >= '0' && *p <= '9') ||
                *p == '-' || *p == '_' || *p == '.';
            if (!allowed) return false;
        }
        return true;
    }

    static bool valid_secure_url(const char* url, const char* scheme,
                                 bool allow_path) noexcept {
        if (!url || !scheme) return false;
        const size_t scheme_len = std::strlen(scheme);
        if (std::strncmp(url, scheme, scheme_len) != 0) return false;
        const char* authority = url + scheme_len;
        const char* slash = std::strchr(authority, '/');
        const char* authority_end = slash ? slash : authority + std::strlen(authority);
        if (authority == authority_end || (slash && !allow_path)) return false;
        for (const char* p = authority; p < authority_end; ++p)
            if (*p == '@' || *p == '?' || *p == '#' || *p == ' ')
                return false;
        if (slash && (std::strchr(slash, '#') || std::strchr(slash, ' ')))
            return false;
        return true;
    }

    static bool valid_tls_pins(const char* pins) noexcept {
        size_t count = 0;
        const char* cursor = pins;
        while (*cursor) {
            const char* separator = std::strchr(cursor, ';');
            const size_t length = separator
                ? static_cast<size_t>(separator - cursor) : std::strlen(cursor);
            if (++count > 2 || length != 52 || cursor[51] != '=' ||
                std::memcmp(cursor, "sha256//", 8) != 0)
                return false;
            for (size_t i = 8; i < 52; ++i) {
                const char c = cursor[i];
                const bool base64 = (c >= 'A' && c <= 'Z') ||
                    (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') ||
                    c == '+' || c == '/' || (c == '=' && i == 51);
                if (!base64) return false;
            }
            if (!separator) break;
            cursor = separator + 1;
            if (!*cursor) return false;
        }
        return count != 0;
    }

    static bool safe_config_text(const char* value) noexcept {
        if (!value) return false;
        for (const unsigned char* p =
                 reinterpret_cast<const unsigned char*>(value); *p; ++p)
            if (*p < 0x20U || *p == 0x7fU) return false;
        return true;
    }

    static bool printable_secret(const char* value) noexcept {
        if (!value || !*value) return false;
        for (const unsigned char* p =
                 reinterpret_cast<const unsigned char*>(value); *p; ++p)
            if (*p <= 0x20U || *p == 0x7fU) return false;
        return true;
    }

    static bool read_secret(const char* key, char* out, size_t cap) {
        if (!out || cap < 2) return false;
        out[0] = '\0';
        char file_key[96];
        std::snprintf(file_key, sizeof(file_key), "%s_FILE", key);
        const char* value = std::getenv(key);
        const char* path = std::getenv(file_key);
        if (value && *value && path && *path) {
            unsetenv(key);
            return false;  // reject ambiguous secret sources
        }
        if (value && *value) {
            const size_t length = std::strlen(value);
            if (length >= cap) {
                unsetenv(key);
                return false;
            }
            std::memcpy(out, value, length + 1);
            unsetenv(key);
            return true;
        }
        if (!path || !*path) return false;
        const int fd = ::open(path, O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
        if (fd < 0) return false;
        struct stat metadata{};
        if (fstat(fd, &metadata) != 0 || !S_ISREG(metadata.st_mode) ||
            (metadata.st_mode & S_IRUSR) == 0 ||
            (metadata.st_mode & (S_IRWXG | S_IRWXO)) != 0) {
            ::close(fd);
            return false;
        }
        char buffer[512];
        size_t total = 0;
        bool io_ok = true;
        while (total < sizeof(buffer) - 1) {
            const ssize_t amount = ::read(fd, buffer + total,
                                          sizeof(buffer) - 1 - total);
            if (amount < 0) {
                if (errno == EINTR) continue;
                io_ok = false;
                break;
            }
            if (amount == 0) break;
            total += static_cast<size_t>(amount);
        }
        char extra = 0;
        ssize_t extra_read;
        do { extra_read = ::read(fd, &extra, 1); }
        while (extra_read < 0 && errno == EINTR);
        const bool fits = io_ok && extra_read == 0;
        ::close(fd);
        if (!fits || total == 0) {
            secure_zero(buffer, sizeof(buffer));
            return false;
        }
        size_t length = total;
        while (length && (buffer[length - 1] == '\n' ||
                          buffer[length - 1] == '\r'))
            --length;
        if (length == 0 || length >= cap) {
            secure_zero(buffer, sizeof(buffer));
            return false;
        }
        std::memcpy(out, buffer, length);
        out[length] = '\0';
        secure_zero(buffer, sizeof(buffer));
        unsetenv(file_key);
        return true;
    }

    template <size_t N>
    static bool copy_env(char (&dst)[N], const char* key, const char* fallback) {
        const char* value = std::getenv(key);
        const char* source = value && *value ? value : fallback;
        if (!source || source == dst) return true;
        if (std::strlen(source) >= N) return false;
        std::memcpy(dst, source, std::strlen(source) + 1);
        return true;
    }

    static void format_address(const uint8_t in[20], char out[43]) {
        out[0] = '0'; out[1] = 'x';
        bytes_to_hex(in, 20, out + 2);
        out[42] = '\0';
    }
};

#endif  // MARKET_CONFIG_HPP
