#ifndef JSON_FIELDS_HPP
#define JSON_FIELDS_HPP

// Shared allocation-free, top-level JSON field helpers for protocol adapters
// added after the original hardening pass (user-channel events, simulation
// venue).  Existing adapters keep their audited in-file copies intentionally:
// converging them is possible but would churn already-pen-tested code for no
// behavioral gain.
//
// All functions are bounded by explicit (begin, end) ranges, count keys only
// at the immediate object level, and reject duplicates so field-like text in
// nested objects or inside strings can never forge semantics.

#include <cctype>
#include <cstddef>
#include <cstdint>
#include <cstring>

namespace json_fields {

inline size_t key_occurrences(const char* begin, const char* end,
                              const char* key,
                              const char** first = nullptr) {
    if (first) *first = nullptr;
    const size_t key_len = std::strlen(key);
    int depth = 0;
    size_t count = 0;
    for (const char* p = begin; p < end; ++p) {
        if (*p == '{' || *p == '[') { ++depth; continue; }
        if (*p == '}' || *p == ']') { --depth; continue; }
        if (*p != '"') continue;
        const char* start = p + 1;
        const char* cursor = start;
        bool escaped = false;
        while (cursor < end) {
            if (escaped) escaped = false;
            else if (*cursor == '\\') escaped = true;
            else if (*cursor == '"') break;
            ++cursor;
        }
        if (cursor == end) return count;
        const char* after = cursor + 1;
        while (after < end && std::isspace(static_cast<unsigned char>(*after)))
            ++after;
        if (depth == 1 &&
            static_cast<size_t>(cursor - start) == key_len &&
            std::memcmp(start, key, key_len) == 0 &&
            after < end && *after == ':') {
            if (count++ == 0 && first) *first = cursor + 1;
        }
        p = cursor;
    }
    return count;
}

inline const char* find_char(const char* begin, const char* end, char wanted) {
    return static_cast<const char*>(
        std::memchr(begin, wanted, static_cast<size_t>(end - begin)));
}

inline const char* find_matching(const char* open, const char* end,
                                 char open_char, char close_char) {
    int depth = 0;
    bool in_string = false, escaped = false;
    for (const char* p = open; p < end; ++p) {
        if (in_string) {
            if (escaped) escaped = false;
            else if (*p == '\\') escaped = true;
            else if (*p == '"') in_string = false;
            continue;
        }
        if (*p == '"') in_string = true;
        else if (*p == open_char) ++depth;
        else if (*p == close_char && --depth == 0) return p;
    }
    return nullptr;
}

inline bool extract_string(const char* begin, const char* end, const char* key,
                           char* out, size_t cap) {
    const char* hit = nullptr;
    if (key_occurrences(begin, end, key, &hit) != 1 || cap == 0)
        return false;
    while (hit < end && std::isspace(static_cast<unsigned char>(*hit))) ++hit;
    if (hit == end || *hit++ != ':') return false;
    while (hit < end && std::isspace(static_cast<unsigned char>(*hit))) ++hit;
    if (hit == end || *hit++ != '"') return false;
    size_t n = 0;
    while (hit < end && *hit != '"') {
        if (*hit == '\\' || n + 1 >= cap) return false;
        out[n++] = *hit++;
    }
    if (hit == end) return false;
    out[n] = '\0';
    return true;
}

inline bool find_array(const char* begin, const char* end, const char* key,
                       const char*& array_begin, const char*& array_end) {
    const char* hit = nullptr;
    if (key_occurrences(begin, end, key, &hit) != 1) return false;
    while (hit < end && std::isspace(static_cast<unsigned char>(*hit))) ++hit;
    if (hit == end || *hit++ != ':') return false;
    while (hit < end && std::isspace(static_cast<unsigned char>(*hit))) ++hit;
    if (hit == end || *hit != '[') return false;
    const char* close = find_matching(hit, end, '[', ']');
    if (!close) return false;
    array_begin = hit;
    array_end = close;
    return true;
}

inline uint64_t fnv_hash(const char* data, size_t len) noexcept {
    uint64_t h = 1469598103934665603ULL;
    for (size_t i = 0; i < len; ++i) {
        h ^= static_cast<uint8_t>(data[i]);
        h *= 1099511628211ULL;
    }
    return h;
}

}  // namespace json_fields

#endif  // JSON_FIELDS_HPP
