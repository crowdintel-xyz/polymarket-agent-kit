#ifndef FAST_RANDOM_HPP
#define FAST_RANDOM_HPP

// Small ChaCha20 CSPRNG seeded from the operating system.  It performs no
// syscalls in the normal hot path and is portable across CPU architectures.
// Salts are public, but cryptographic seeding also makes this suitable for
// libsecp256k1 context randomization.  Failure to obtain OS entropy is fatal;
// silently falling back to clocks/addresses would provide a false guarantee.

#include <array>
#include <cerrno>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <stdexcept>

#if defined(__linux__)
#include <sys/random.h>
#endif

#include "secure_zero.hpp"

class FastRandom {
public:
    FastRandom() { reseed(); }
    ~FastRandom() {
        secure_zero(state_.data(), state_.size() * sizeof(state_[0]));
        secure_zero(block_.data(), block_.size());
    }

    FastRandom(const FastRandom&) = delete;
    FastRandom& operator=(const FastRandom&) = delete;

    uint64_t next_u64() {
        if (offset_ + sizeof(uint64_t) > block_.size()) generate_block();
        uint64_t value = 0;
        std::memcpy(&value, block_.data() + offset_, sizeof(value));
        offset_ += sizeof(value);
        return value;
    }

    // JSON encodes salt as a number.  Stay within JavaScript's exact integer
    // domain rather than the previously claimed-but-incorrect 56-bit range.
    uint64_t next_salt() {
        return next_u64() & ((1ULL << 53) - 1ULL);
    }

private:
    static uint32_t load32(const uint8_t* p) {
        return static_cast<uint32_t>(p[0]) |
               static_cast<uint32_t>(p[1]) << 8 |
               static_cast<uint32_t>(p[2]) << 16 |
               static_cast<uint32_t>(p[3]) << 24;
    }

    static void store32(uint8_t* p, uint32_t v) {
        p[0] = static_cast<uint8_t>(v);
        p[1] = static_cast<uint8_t>(v >> 8);
        p[2] = static_cast<uint8_t>(v >> 16);
        p[3] = static_cast<uint8_t>(v >> 24);
    }

    static uint32_t rotl(uint32_t x, int n) {
        return (x << n) | (x >> (32 - n));
    }

    static void quarter(uint32_t& a, uint32_t& b,
                        uint32_t& c, uint32_t& d) {
        a += b; d ^= a; d = rotl(d, 16);
        c += d; b ^= c; b = rotl(b, 12);
        a += b; d ^= a; d = rotl(d, 8);
        c += d; b ^= c; b = rotl(b, 7);
    }

    static void os_entropy(uint8_t* out, size_t len) {
#if defined(__linux__)
        size_t done = 0;
        while (done < len) {
            const ssize_t n = ::getrandom(out + done, len - done, 0);
            if (n > 0) { done += static_cast<size_t>(n); continue; }
            if (n < 0 && errno == EINTR) continue;
            throw std::runtime_error("getrandom failed");
        }
#elif defined(__APPLE__) || defined(__FreeBSD__) || defined(__OpenBSD__) || \
      defined(__NetBSD__)
        ::arc4random_buf(out, len);
#else
        (void)out;
        (void)len;
        throw std::runtime_error("no supported OS CSPRNG");
#endif
    }

    void reseed() {
        uint8_t seed[44]{};  // 256-bit key + 96-bit nonce
        try {
            os_entropy(seed, sizeof(seed));
        } catch (...) {
            secure_zero(seed, sizeof(seed));
            throw;
        }
        static constexpr char sigma[] = "expand 32-byte k";
        for (int i = 0; i < 4; ++i)
            state_[i] = load32(reinterpret_cast<const uint8_t*>(sigma) + i * 4);
        for (int i = 0; i < 8; ++i) state_[4 + i] = load32(seed + i * 4);
        state_[12] = 0;
        state_[13] = load32(seed + 32);
        state_[14] = load32(seed + 36);
        state_[15] = load32(seed + 40);
        secure_zero(seed, sizeof(seed));
        generated_blocks_ = 0;
        offset_ = block_.size();
    }

    void generate_block() {
        // Periodic reseeding limits state compromise and counter lifetime.
        if (generated_blocks_ >= (1ULL << 20)) reseed();
        auto x = state_;
        for (int i = 0; i < 10; ++i) {
            quarter(x[0], x[4], x[8],  x[12]);
            quarter(x[1], x[5], x[9],  x[13]);
            quarter(x[2], x[6], x[10], x[14]);
            quarter(x[3], x[7], x[11], x[15]);
            quarter(x[0], x[5], x[10], x[15]);
            quarter(x[1], x[6], x[11], x[12]);
            quarter(x[2], x[7], x[8],  x[13]);
            quarter(x[3], x[4], x[9],  x[14]);
        }
        for (size_t i = 0; i < x.size(); ++i)
            store32(block_.data() + i * 4, x[i] + state_[i]);
        if (++state_[12] == 0 && ++state_[13] == 0)
            throw std::runtime_error("ChaCha20 counter exhausted");
        ++generated_blocks_;
        offset_ = 0;
    }

    std::array<uint32_t, 16> state_{};
    std::array<uint8_t, 64> block_{};
    size_t offset_ = 64;
    uint64_t generated_blocks_ = 0;
};

#endif  // FAST_RANDOM_HPP
