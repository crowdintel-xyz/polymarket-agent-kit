#ifndef KECCAK256_HPP
#define KECCAK256_HPP

// ─────────────────────────────────────────────────────────────────────────────
// Keccak-256 (FIPS 202 Keccak-f[1600], rate 1088 bits, suffix 0x01).
//
// Self-contained, zero-allocation, dependency-free. NOTE: this is Ethereum's
// Keccak-256 (padding byte 0x01), NOT SHA3-256 (padding byte 0x06).
//
// ρ+π is computed with the canonical rotation chain (XKCP reference) instead
// of a transcribed offset table: chain starts at (1,0), each step moves to
// (y, (2x+3y) mod 5) with rotation (t+1)(t+2)/2 mod 64. This makes table
// transcription errors impossible while producing identical output.
// ─────────────────────────────────────────────────────────────────────────────

#include <cstddef>
#include <cstdint>
#include <cstring>

inline void keccak_f1600(uint64_t A[25]) {
    static const uint64_t RC[24] = {
        0x0000000000000001ULL, 0x0000000000008082ULL, 0x800000000000808AULL,
        0x8000000080008000ULL, 0x000000000000808BULL, 0x0000000080000001ULL,
        0x8000000080008081ULL, 0x8000000000008009ULL, 0x000000000000008AULL,
        0x0000000000000088ULL, 0x0000000080008009ULL, 0x000000008000000AULL,
        0x000000008000808BULL, 0x800000000000008BULL, 0x8000000000008089ULL,
        0x8000000000008003ULL, 0x8000000000008002ULL, 0x8000000000000080ULL,
        0x000000000000800AULL, 0x800000008000000AULL, 0x8000000080008081ULL,
        0x8000000000008080ULL, 0x0000000080000001ULL, 0x8000000080008008ULL
    };

    for (int round = 0; round < 24; round++) {
        // θ (theta). Reverse traversal preserves the canonical permutation
        // while shortening a dependency chain on common compilers.
        uint64_t C[5], D[5];
        for (int x = 0; x < 5; x++)
            C[x] = A[x] ^ A[x + 5] ^ A[x + 10] ^ A[x + 15] ^ A[x + 20];
        for (int x = 4; x >= 0; x--) {
            const uint64_t c1 = C[(x + 1) % 5];
            D[x] = C[(x + 4) % 5] ^ ((c1 << 1) | (c1 >> 63));
        }
        for (int x = 0; x < 5; x++)
            for (int y = 0; y < 5; y++)
                A[x + y * 5] ^= D[x];

        // ρ (rho) + π (pi) — rotation chain
        {
            int x = 1, y = 0;
            uint64_t current = A[x + y * 5];
            for (int t = 0; t < 24; t++) {
                const int nx = y;
                const int ny = (2 * x + 3 * y) % 5;
                const uint64_t temp = A[nx + ny * 5];
                const int shift = (int)(((t + 1) * (t + 2)) / 2) & 63;
                A[nx + ny * 5] = (shift == 0)
                    ? current
                    : (current << shift) | (current >> (64 - shift));
                current = temp;
                x = nx;
                y = ny;
            }
        }

        // χ (chi)
        for (int y = 0; y < 5; y++) {
            uint64_t tmp[5];
            for (int x = 0; x < 5; x++) tmp[x] = A[x + y * 5];
            for (int x = 0; x < 5; x++)
                A[x + y * 5] = tmp[x] ^ ((~tmp[(x + 1) % 5]) & tmp[(x + 2) % 5]);
        }

        // ι (iota)
        A[0] ^= RC[round];
    }
}

inline void keccak256_absorb_block(uint64_t A[25], const uint8_t* block) {
    for (size_t i = 0; i < 17; i++) {  // 136 bytes = 17 lanes
        uint64_t word;
        std::memcpy(&word, block + i * 8, 8);   // little-endian host load
        A[i] ^= word;
    }
    keccak_f1600(A);
}

inline void keccak256_hash(const uint8_t* data, size_t len, uint8_t out[32]) {
    constexpr size_t RATE = 136;  // 1088-bit rate
    uint64_t A[25] = {0};

    size_t offset = 0;
    while (offset + RATE <= len) {
        keccak256_absorb_block(A, data + offset);
        offset += RATE;
    }

    // Final block + pad10*1 (suffix 0x01, 0x80 terminator)
    uint8_t block[RATE] = {0};
    const size_t remaining = len - offset;
    if (remaining) std::memcpy(block, data + offset, remaining);
    block[remaining] ^= 0x01;
    block[RATE - 1] ^= 0x80;
    keccak256_absorb_block(A, block);

    for (size_t i = 0; i < 4; i++)
        std::memcpy(out + i * 8, &A[i], 8);
}

#endif // KECCAK256_HPP
