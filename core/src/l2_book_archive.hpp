// ─────────────────────────────────────────────────────────────────────────────
// l2_book_archive.hpp — P2: Change-only L2 book archive (CLOB V2)
//
// Ported from polywhales archive.ts (books/archive.ts) — C++20 implementation,
// NO TypeScript copied.
//
// Design:
//  - Writes ONLY when top-of-book changes (dedup per token by signature)
//  - Price moves always write; size-only churn throttled (default 5s/token)
//  - Daily files: books-YYYYMMDD.jsonl in configured dir
//  - Retention: deletes files older than retain_days (default 14)
//  - Synchronous escape hatch for crash-safe shutdown
//
// File format (compact JSON):
//   {"t":ts,"tok":"token","b":[[px,sz]],"a":[[px,sz]],"h":"hash"}
// ─────────────────────────────────────────────────────────────────────────────
#pragma once

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <string>
#include <sys/stat.h>
#include <dirent.h>
#include <unordered_map>

#include "../include/order_book.hpp"

// ── Configuration ────────────────────────────────────────────────────────────
struct L2BookArchiveConfig {
    const char* dir = "./books";
    bool dedupe_by_hash = false;
    uint32_t levels = 5;
    uint64_t throttle_ms = 5'000;
    uint32_t retain_days = 14;
};

// ── Stats ────────────────────────────────────────────────────────────────────
struct L2BookArchiveStats {
    std::atomic<uint64_t> lines{0};
    std::atomic<uint64_t> bytes{0};
    std::atomic<uint64_t> skipped{0};
    std::atomic<uint64_t> throttled{0};
    std::atomic<uint64_t> errors{0};
    std::atomic<uint64_t> rotations{0};
    std::atomic<uint64_t> tokens{0};
};

// ── Book top interface ───────────────────────────────────────────────────────
struct L2BookTop {
    Level2Entry bid;
    Level2Entry ask;
    uint64_t ts = 0;
    std::string token = "";
    std::string hash = "";
};

// ── Clock function pointer ───────────────────────────────────────────────────
using L2ClockFn = uint64_t (*)();

// ── L2 Book Archive ──────────────────────────────────────────────────────────
class L2BookArchive {
public:
    explicit L2BookArchive(const L2BookArchiveConfig& cfg, L2ClockFn clock = nullptr)
        : cfg_(cfg), clock_fn_(clock ? clock : &default_clock) {
        current_file_[0] = '\0';
        current_day_[0] = '\0';
    }

    // ── Write if changed (price moves always write; size-only churn throttled)
    bool write(const L2BookTop& top) {
        if (top.token.empty()) return false;

        // Signature for dedup: price:size pairs
        std::string sig = format_signature(top);

        auto& prev_sig = sigs_[top.token];
        if (prev_sig == sig) {
            stats_.skipped.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        // For throttle: only price matters (size-only churn)
        std::string px_sig = format_prices(top);
        uint64_t now = clock_fn_();

        if (prev_px_.count(top.token) > 0 &&
            prev_px_[top.token] == px_sig &&
            now - last_write_[top.token] < cfg_.throttle_ms) {
            stats_.throttled.fetch_add(1, std::memory_order_relaxed);
            return false;
        }

        prev_sig = sig;
        prev_px_[top.token] = px_sig;
        last_write_[top.token] = now;
        stats_.tokens.store(sigs_.size(), std::memory_order_relaxed);

        std::string payload = format_line(top);
        if (write_to_file(payload, now)) {
            stats_.lines.fetch_add(1, std::memory_order_relaxed);
            stats_.bytes.fetch_add((uint64_t)payload.size(), std::memory_order_relaxed);
            return true;
        }
        return false;
    }

    void write_sync(const L2BookTop& top) {
        if (top.token.empty()) return;
        std::string payload = format_line(top);
        FILE* f = fopen(current_file(), "a");
        if (!f) {
            stats_.errors.fetch_add(1, std::memory_order_relaxed);
            return;
        }
        fwrite(payload.c_str(), 1, payload.size(), f);
        fclose(f);
        stats_.lines.fetch_add(1, std::memory_order_relaxed);
    }

    const L2BookArchiveStats& stats() const { return stats_; }
    const char* current_file() const { return current_file_; }

private:
    static uint64_t default_clock() {
        struct timespec ts;
        clock_gettime(CLOCK_MONOTONIC, &ts);
        return (uint64_t)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }

    static std::string utc_day_str(uint64_t now_ms) {
        time_t t = (time_t)(now_ms / 1000);
        struct tm tm;
        gmtime_r(&t, &tm);
        char buf[16];
        snprintf(buf, sizeof(buf), "%04d%02d%02d",
            tm.tm_year + 1900, tm.tm_mon + 1, tm.tm_mday);
        return std::string(buf);
    }

    std::string format_signature(const L2BookTop& top) const {
        char buf[256];
        snprintf(buf, sizeof(buf), "%llu:%llu|%llu:%llu",
            (unsigned long long)top.bid.price, (unsigned long long)top.bid.size,
            (unsigned long long)top.ask.price, (unsigned long long)top.ask.size);
        return std::string(buf);
    }

    std::string format_prices(const L2BookTop& top) const {
        char buf[128];
        snprintf(buf, sizeof(buf), "%llu|%llu",
            (unsigned long long)top.bid.price, (unsigned long long)top.ask.price);
        return std::string(buf);
    }

    std::string format_line(const L2BookTop& top) const {
        char buf[512];
        snprintf(buf, sizeof(buf),
            "{\"t\":%llu,\"tok\":\"%s\",\"b\":[[%llu,%llu]],\"a\":[[%llu,%llu]],\"h\":\"%s\"}\n",
            (unsigned long long)top.ts,
            top.token.c_str(),
            (unsigned long long)top.bid.price, (unsigned long long)top.bid.size,
            (unsigned long long)top.ask.price, (unsigned long long)top.ask.size,
            top.hash.c_str());
        return std::string(buf);
    }

    bool write_to_file(const std::string& line, uint64_t now_ms) {
        std::string day = utc_day_str(now_ms);

        // Day rotation check
        if (current_day_[0] != '\0' && day != std::string(current_day_)) {
            stats_.rotations.fetch_add(1, std::memory_order_relaxed);
            run_retention(day);
        }
        strncpy(current_day_, day.c_str(), sizeof(current_day_) - 1);
        current_day_[sizeof(current_day_) - 1] = '\0';

        // Build file path
        std::string filepath = std::string(cfg_.dir) + "/books-" + day + ".jsonl";
        strncpy(current_file_, filepath.c_str(), sizeof(current_file_) - 1);
        current_file_[sizeof(current_file_) - 1] = '\0';

        FILE* f = fopen(filepath.c_str(), "a");
        if (!f) {
            stats_.errors.fetch_add(1, std::memory_order_relaxed);
            return false;
        }
        size_t written = fwrite(line.c_str(), 1, line.size(), f);
        fclose(f);
        return written == line.size();
    }

    void run_retention(const std::string& current_day) {
        int y, m, d;
        if (sscanf(current_day.c_str(), "%4d%2d%2d", &y, &m, &d) != 3) return;
        struct tm tm_cut = {0,0,0, d, m-1, y-1900, 0,0,0,0,0 };
        time_t cut = timegm(&tm_cut) - (time_t)cfg_.retain_days * 86400;

        struct tm* old = gmtime(&cut);
        char cut_buf[16];
        snprintf(cut_buf, sizeof(cut_buf), "%04d%02d%02d",
            old->tm_year + 1900, old->tm_mon + 1, old->tm_mday);
        std::string cutoff(cut_buf);

        DIR* dir = opendir(cfg_.dir);
        if (!dir) { stats_.errors.fetch_add(1, std::memory_order_relaxed); return; }

        struct dirent* ent;
        while ((ent = readdir(dir)) != nullptr) {
            std::string name(ent->d_name);
            if (name.size() >= 16 && name.substr(0, 6) == "books-") {
                std::string day_part = name.substr(6, 8);
                if (day_part.size() == 8 && day_part < cutoff) {
                    std::string fullpath = std::string(cfg_.dir) + "/" + name;
                    unlink(fullpath.c_str());
                }
            }
        }
        closedir(dir);
    }

    const L2BookArchiveConfig& cfg_;
    L2ClockFn clock_fn_;

    std::unordered_map<std::string, std::string> sigs_;
    std::unordered_map<std::string, std::string> prev_px_;
    std::unordered_map<std::string, uint64_t> last_write_;

    char current_file_[256];
    char current_day_[16];

    mutable L2BookArchiveStats stats_;
};
