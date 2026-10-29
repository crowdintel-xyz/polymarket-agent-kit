#ifndef BOUNDED_JSON_HPP
#define BOUNDED_JSON_HPP

#include <cstddef>
#include <cstdint>

// Allocation-free JSON syntax validator for already bounded transport frames.
// It intentionally validates syntax only; semantic field extraction remains in
// each protocol adapter so venue-specific constraints stay explicit.
namespace bounded_json {

class Parser {
public:
    Parser(const char* data, size_t length, unsigned max_depth) noexcept
        : cursor_(data), end_(data + length), max_depth_(max_depth) {}

    bool document() noexcept {
        whitespace();
        if (!value(0)) return false;
        whitespace();
        return cursor_ == end_;
    }

private:
    void whitespace() noexcept {
        while (cursor_ < end_ && (*cursor_ == ' ' || *cursor_ == '\t' ||
               *cursor_ == '\r' || *cursor_ == '\n')) ++cursor_;
    }

    bool value(unsigned depth) noexcept {
        if (cursor_ == end_) return false;
        switch (*cursor_) {
            case '{': return object(depth + 1);
            case '[': return array(depth + 1);
            case '"': return string();
            case 't': return literal("true", 4);
            case 'f': return literal("false", 5);
            case 'n': return literal("null", 4);
            default: return number();
        }
    }

    bool object(unsigned depth) noexcept {
        if (depth > max_depth_ || cursor_ == end_ || *cursor_++ != '{')
            return false;
        whitespace();
        if (cursor_ < end_ && *cursor_ == '}') { ++cursor_; return true; }
        for (;;) {
            if (!string()) return false;
            whitespace();
            if (cursor_ == end_ || *cursor_++ != ':') return false;
            whitespace();
            if (!value(depth)) return false;
            whitespace();
            if (cursor_ == end_) return false;
            if (*cursor_ == '}') { ++cursor_; return true; }
            if (*cursor_++ != ',') return false;
            whitespace();
        }
    }

    bool array(unsigned depth) noexcept {
        if (depth > max_depth_ || cursor_ == end_ || *cursor_++ != '[')
            return false;
        whitespace();
        if (cursor_ < end_ && *cursor_ == ']') { ++cursor_; return true; }
        for (;;) {
            if (!value(depth)) return false;
            whitespace();
            if (cursor_ == end_) return false;
            if (*cursor_ == ']') { ++cursor_; return true; }
            if (*cursor_++ != ',') return false;
            whitespace();
        }
    }

    static bool hex(char c) noexcept {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') ||
               (c >= 'A' && c <= 'F');
    }

    bool string() noexcept {
        if (cursor_ == end_ || *cursor_++ != '"') return false;
        while (cursor_ < end_) {
            const unsigned char c = static_cast<unsigned char>(*cursor_++);
            if (c == '"') return true;
            if (c < 0x20U) return false;
            if (c != '\\') continue;
            if (cursor_ == end_) return false;
            const char escaped = *cursor_++;
            if (escaped == '"' || escaped == '\\' || escaped == '/' ||
                escaped == 'b' || escaped == 'f' || escaped == 'n' ||
                escaped == 'r' || escaped == 't')
                continue;
            if (escaped != 'u' || end_ - cursor_ < 4) return false;
            for (int i = 0; i < 4; ++i)
                if (!hex(*cursor_++)) return false;
        }
        return false;
    }

    bool number() noexcept {
        if (cursor_ == end_) return false;
        if (*cursor_ == '-') {
            if (++cursor_ == end_) return false;
        }
        if (*cursor_ == '0') {
            ++cursor_;
            if (cursor_ < end_ && *cursor_ >= '0' && *cursor_ <= '9')
                return false;
        } else {
            if (*cursor_ < '1' || *cursor_ > '9') return false;
            do { ++cursor_; }
            while (cursor_ < end_ && *cursor_ >= '0' && *cursor_ <= '9');
        }
        if (cursor_ < end_ && *cursor_ == '.') {
            ++cursor_;
            if (cursor_ == end_ || *cursor_ < '0' || *cursor_ > '9')
                return false;
            do { ++cursor_; }
            while (cursor_ < end_ && *cursor_ >= '0' && *cursor_ <= '9');
        }
        if (cursor_ < end_ && (*cursor_ == 'e' || *cursor_ == 'E')) {
            ++cursor_;
            if (cursor_ < end_ && (*cursor_ == '+' || *cursor_ == '-'))
                ++cursor_;
            if (cursor_ == end_ || *cursor_ < '0' || *cursor_ > '9')
                return false;
            do { ++cursor_; }
            while (cursor_ < end_ && *cursor_ >= '0' && *cursor_ <= '9');
        }
        return true;
    }

    bool literal(const char* expected, size_t length) noexcept {
        if (static_cast<size_t>(end_ - cursor_) < length) return false;
        for (size_t i = 0; i < length; ++i)
            if (cursor_[i] != expected[i]) return false;
        cursor_ += length;
        return true;
    }

    const char* cursor_;
    const char* end_;
    unsigned max_depth_;
};

inline bool valid_document(const char* data, size_t length,
                           unsigned max_depth = 16) noexcept {
    return data && length != 0 && Parser(data, length, max_depth).document();
}

}  // namespace bounded_json

#endif  // BOUNDED_JSON_HPP
