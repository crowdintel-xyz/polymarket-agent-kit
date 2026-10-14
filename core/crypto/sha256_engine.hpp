#ifndef SHA256_ENGINE_HPP
#define SHA256_ENGINE_HPP

// ─────────────────────────────────────────────────────────────────────────────
// In-house SHA-256 + HMAC-SHA256 with precomputed key midstates + base64url.
//
// Rationale (hot path): the CLOB L2 auth HMAC runs on every order submission.
// Calling into a general-purpose crypto library per order costs call overhead
// and forbids midstate reuse. Here the secret key's ipad/opad SHA-256
// midstates are computed ONCE at startup; per-order cost is then a single
// SHA-256 block compression for the message plus one for the outer hash —
// a few hundred nanoseconds, zero heap allocation, zero library calls.
// ─────────────────────────────────────────────────────────────────────────────

#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>

// ── SHA-256 ──────────────────────────────────────────────────────────────────
struct Sha256Ctx {
    uint32_t h[8];
    uint64_t len;
    uint8_t  buf[64];
    size_t   buf_len;
};

inline void sha256_init(Sha256Ctx& c) {
    c.h[0]=0x6a09e667; c.h[1]=0xbb67ae85; c.h[2]=0x3c6ef372; c.h[3]=0xa54ff53a;
    c.h[4]=0x510e527f; c.h[5]=0x9b05688c; c.h[6]=0x1f83d9ab; c.h[7]=0x5be0cd19;
    c.len = 0; c.buf_len = 0;
}

inline uint32_t rotr32(uint32_t x, int n) { return (x >> n) | (x << (32 - n)); }

inline void sha256_block(Sha256Ctx& c, const uint8_t* p) {
    static const uint32_t K[64] = {
        0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,
        0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,
        0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,
        0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,
        0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,
        0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,
        0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,
        0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2
    };
    uint32_t w[64];
    for (int i = 0; i < 16; i++)
        w[i] = (uint32_t)p[i*4] << 24 | (uint32_t)p[i*4+1] << 16 |
               (uint32_t)p[i*4+2] << 8 | (uint32_t)p[i*4+3];
    for (int i = 16; i < 64; i++) {
        const uint32_t s0 = rotr32(w[i-15],7) ^ rotr32(w[i-15],18) ^ (w[i-15] >> 3);
        const uint32_t s1 = rotr32(w[i-2],17) ^ rotr32(w[i-2],19) ^ (w[i-2] >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }
    uint32_t a=c.h[0],b=c.h[1],d,e,f,g,h2,cc;
    cc=c.h[2]; d=c.h[3]; e=c.h[4]; f=c.h[5]; g=c.h[6]; h2=c.h[7];
    for (int i = 0; i < 64; i++) {
        const uint32_t S1 = rotr32(e,6) ^ rotr32(e,11) ^ rotr32(e,25);
        const uint32_t ch = (e & f) ^ (~e & g);
        const uint32_t t1 = h2 + S1 + ch + K[i] + w[i];
        const uint32_t S0 = rotr32(a,2) ^ rotr32(a,13) ^ rotr32(a,22);
        const uint32_t maj = (a & b) ^ (a & cc) ^ (b & cc);
        const uint32_t t2 = S0 + maj;
        h2=g; g=f; f=e; e=d+t1; d=cc; cc=b; b=a; a=t1+t2;
    }
    c.h[0]+=a; c.h[1]+=b; c.h[2]+=cc; c.h[3]+=d;
    c.h[4]+=e; c.h[5]+=f; c.h[6]+=g; c.h[7]+=h2;
    c.len += 64;
}

inline void sha256_update(Sha256Ctx& c, const uint8_t* data, size_t len) {
    if (c.buf_len) {
        const size_t take = (64 - c.buf_len) < len ? (64 - c.buf_len) : len;
        std::memcpy(c.buf + c.buf_len, data, take);
        c.buf_len += take; data += take; len -= take;
        if (c.buf_len == 64) { sha256_block(c, c.buf); c.buf_len = 0; }
    }
    while (len >= 64) { sha256_block(c, data); data += 64; len -= 64; }
    if (len) { std::memcpy(c.buf, data, len); c.buf_len = len; }
}

inline void sha256_final(Sha256Ctx& c, uint8_t out[32]) {
    const uint64_t bits = (c.len + c.buf_len) * 8;
    uint8_t pad = 0x80;
    sha256_update(c, &pad, 1);
    const uint8_t zero = 0;
    while (c.buf_len != 56) sha256_update(c, &zero, 1);
    uint8_t lenb[8];
    for (int i = 0; i < 8; i++) lenb[i] = (uint8_t)(bits >> (56 - i * 8));
    // lenb fits in the 56..63 window by construction
    std::memcpy(c.buf + c.buf_len, lenb, 8);
    sha256_block(c, c.buf);
    c.buf_len = 0;
    for (int i = 0; i < 8; i++) {
        out[i*4]   = (uint8_t)(c.h[i] >> 24);
        out[i*4+1] = (uint8_t)(c.h[i] >> 16);
        out[i*4+2] = (uint8_t)(c.h[i] >> 8);
        out[i*4+3] = (uint8_t)(c.h[i]);
    }
}

inline void sha256(const uint8_t* data, size_t len, uint8_t out[32]) {
    Sha256Ctx c;
    sha256_init(c);
    sha256_update(c, data, len);
    sha256_final(c, out);
}

// ── HMAC-SHA256 with precomputed key midstates ───────────────────────────────
class HmacSha256 {
public:
    // key: raw (already decoded) secret bytes. Call once at startup.
    void set_key(const uint8_t* key, size_t len) {
        uint8_t kblock[64] = {0};
        if (len > 64) {
            Sha256Ctx c; sha256_init(c);
            sha256_update(c, key, len);
            sha256_final(c, kblock);          // 32 bytes, rest zero
        } else {
            std::memcpy(kblock, key, len);
        }

        uint8_t ipad[64], opad[64];
        for (int i = 0; i < 64; i++) {
            ipad[i] = kblock[i] ^ 0x36;
            opad[i] = kblock[i] ^ 0x5c;
        }
        sha256_init(inner_base_);
        sha256_update(inner_base_, ipad, 64);
        sha256_init(outer_base_);
        sha256_update(outer_base_, opad, 64);
        std::memset(kblock, 0, 64);
        std::memset(ipad, 0, 64);
        std::memset(opad, 0, 64);
    }

    // HMAC(key, data) → 32 bytes. Hot path: one midstate copy + compression.
    void compute(const uint8_t* data, size_t len, uint8_t out[32]) const {
        Sha256Ctx c = inner_base_;              // copy midstate (fast struct copy)
        sha256_update(c, data, len);
        uint8_t inner[32];
        sha256_final(c, inner);
        Sha256Ctx o = outer_base_;
        sha256_update(o, inner, 32);
        sha256_final(o, out);
    }

private:
    Sha256Ctx inner_base_;
    Sha256Ctx outer_base_;
};

// ── base64url (RFC 4648 §5, WITH padding — as the CLOB requires) ─────────────
inline size_t base64url_encode(const uint8_t* in, size_t len, char* out) {
    static constexpr char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t o = 0;
    size_t i = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i+1] << 8 | in[i+2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];
        out[o++] = T[v & 63];
    }
    if (len - i == 1) {
        const uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = '=';
        out[o++] = '=';
    } else if (len - i == 2) {
        const uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i+1] << 8;
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];
        out[o++] = '=';
    }
    return o;
}

// Decode base64url (with or without padding). Returns bytes written or SIZE_MAX.
inline size_t base64url_decode(const char* in, size_t len,
                               uint8_t* out, size_t out_cap) {
    if (!in || !out) return SIZE_MAX;
    static auto val = [](char c) -> int {
        if (c >= 'A' && c <= 'Z') return c - 'A';
        if (c >= 'a' && c <= 'z') return c - 'a' + 26;
        if (c >= '0' && c <= '9') return c - '0' + 52;
        if (c == '-' || c == '+') return 62;
        if (c == '_' || c == '/') return 63;
        return -1;
    };
    size_t content_len = 0;
    while (content_len < len && in[content_len] != '=') ++content_len;
    const size_t padding = len - content_len;
    if (content_len % 4 == 1 || padding > 2) return SIZE_MAX;
    if (padding != 0) {
        if (len % 4 != 0 ||
            (padding == 1 && content_len % 4 != 3) ||
            (padding == 2 && content_len % 4 != 2))
            return SIZE_MAX;
        for (size_t i = content_len; i < len; ++i)
            if (in[i] != '=') return SIZE_MAX;
    }

    size_t written = 0;
    uint32_t acc = 0;
    unsigned bits = 0;
    for (size_t i = 0; i < content_len; ++i) {
        const int decoded = val(in[i]);
        if (decoded < 0) return SIZE_MAX;
        acc = (acc << 6) | static_cast<uint32_t>(decoded);
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            if (written == out_cap) return SIZE_MAX;
            out[written++] = static_cast<uint8_t>(acc >> bits);
        }
    }
    if (bits != 0 && (acc & ((1U << bits) - 1U)) != 0) return SIZE_MAX;
    return written;
}

// Standard base64 (for the WebSocket handshake key).
inline size_t base64_encode(const uint8_t* in, size_t len, char* out) {
    static constexpr char T[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    size_t o = 0, i = 0;
    for (; i + 3 <= len; i += 3) {
        const uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i+1] << 8 | in[i+2];
        out[o++] = T[(v >> 18) & 63];
        out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];
        out[o++] = T[v & 63];
    }
    if (len - i == 1) {
        const uint32_t v = (uint32_t)in[i] << 16;
        out[o++] = T[(v >> 18) & 63]; out[o++] = T[(v >> 12) & 63];
        out[o++] = '='; out[o++] = '=';
    } else if (len - i == 2) {
        const uint32_t v = (uint32_t)in[i] << 16 | (uint32_t)in[i+1] << 8;
        out[o++] = T[(v >> 18) & 63]; out[o++] = T[(v >> 12) & 63];
        out[o++] = T[(v >> 6) & 63];  out[o++] = '=';
    }
    return o;
}

#endif // SHA256_ENGINE_HPP
