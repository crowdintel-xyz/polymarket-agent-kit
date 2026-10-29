#ifndef EIP712_SIGNER_HPP
#define EIP712_SIGNER_HPP

// ─────────────────────────────────────────────────────────────────────────────
// EIP712Signer: Polymarket CLOB **V2** order signer (Polygon, chainId 137).
//
// Implements exactly the wire scheme documented at docs.polymarket.com:
//
//   domain = EIP712Domain(string name,string version,uint256 chainId,
//                         address verifyingContract)
//            { name: "Polymarket CTF Exchange", version: "2", chainId: 137,
//              verifyingContract: 0xE11118... (standard)
//                                 0xe2222d... (neg-risk) }
//
//   structHash  = keccak256(typeHash ‖ ABI(Order))
//   digest      = keccak256(0x1901 ‖ domainSeparator ‖ structHash)
//   sig         = (r‖s‖v) via libsecp256k1 recoverable signing (RFC 6979 nonces,
//                 deterministic, low-S by construction — EIP-2 compliant)
//
// Signed struct (V2 — taker/nonce/feeRateBps/expiration REMOVED; timestamp,
// metadata, builder ADDED):
//   Order(uint256 salt,address maker,address signer,uint256 tokenId,
//         uint256 makerAmount,uint256 takerAmount,uint8 side,
//         uint8 signatureType,uint256 timestamp,bytes32 metadata,
//         bytes32 builder)
//
// Performance notes:
//   - secp256k1 context is created with SIGN, validated/randomized once at
//     startup, and uses the library's precomputed ecmult-gen table. Actual
//     timing is CPU/build dependent and is reported only by the benchmark.
//   - The domain separator AND the Order typehash are computed once and cached.
//   - All hashing buffers are stack arrays — zero heap allocation per sign.
//   - Private key memory is wiped on destruction (OPENSSL_cleanse-free, using
//     volatile zeroization so the compiler cannot elide it).
// ─────────────────────────────────────────────────────────────────────────────

#include <cstdint>
#include <cstring>

#include <secp256k1.h>
#include <secp256k1_preallocated.h>
#include <secp256k1_recovery.h>

#include <cstdlib>

#include "keccak256.hpp"
#include "fast_random.hpp"
#include "secure_zero.hpp"
#include "polymarket_order.hpp"

namespace crowdintel {

inline constexpr const char* K_DOMAIN_TYPE_STR =
    "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)";
inline constexpr const char* K_ORDER_TYPE_STR =
    "Order(uint256 salt,address maker,address signer,uint256 tokenId,"
    "uint256 makerAmount,uint256 takerAmount,uint8 side,uint8 signatureType,"
    "uint256 timestamp,bytes32 metadata,bytes32 builder)";

// Precompute the typehash once per signer (cold path).
inline void compute_typehash(const char* type_str, size_t len, uint8_t out[32]) {
    keccak256_hash(reinterpret_cast<const uint8_t*>(type_str), len, out);
}

// Canonical Polymarket V2 Exchange contracts (docs.polymarket.com/resources/contracts).
inline constexpr uint8_t K_STANDARD_EXCHANGE[20] = {
    0xE1,0x11,0x18,0x00,0x00,0xd2,0x66,0x3C,0x00,0x91,
    0xe4,0xf4,0x00,0x23,0x75,0x45,0xB8,0x7B,0x99,0x6B};
inline constexpr uint8_t K_NEGRISK_EXCHANGE[20] = {
    0xe2,0x22,0x2d,0x27,0x9d,0x74,0x40,0x50,0xd2,0x8e,
    0x00,0x52,0x00,0x10,0x52,0x00,0x00,0x31,0x0F,0x59};

// Domain parameters → 32-byte separator (cold path, called once).
inline void compute_domain_separator(bool neg_risk, uint8_t out[32]) {
    uint8_t buf[32 * 5];
    keccak256_hash(reinterpret_cast<const uint8_t*>(K_DOMAIN_TYPE_STR),
                   std::strlen(K_DOMAIN_TYPE_STR), buf);
    keccak256_hash(reinterpret_cast<const uint8_t*>("Polymarket CTF Exchange"), 23, buf + 32);
    keccak256_hash(reinterpret_cast<const uint8_t*>("2"), 1, buf + 64);
    std::memset(buf + 96, 0, 32);   // zero the full chainId slot
    const uint32_t chain = 137;
    buf[96+28] = (uint8_t)(chain >> 24); buf[96+29] = (uint8_t)(chain >> 16);
    buf[96+30] = (uint8_t)(chain >> 8);  buf[96+31] = (uint8_t)chain;
    std::memset(buf + 128, 0, 12);
    std::memcpy(buf + 128 + 12, neg_risk ? K_NEGRISK_EXCHANGE : K_STANDARD_EXCHANGE, 20);
    keccak256_hash(buf, sizeof(buf), out);
}

}  // namespace crowdintel

class EIP712Signer {
public:
    EIP712Signer() = default;

    bool init(const uint8_t private_key[32], bool neg_risk) noexcept {
        if (secp_ctx_) return true;  // already initialized
        if (!private_key) return false;

        // libsecp256k1: SIGN-only context, randomized before any key operation.
        const size_t ctx_size = secp256k1_context_preallocated_size(
            SECP256K1_CONTEXT_SIGN);
        ctx_mem_ = std::malloc(ctx_size);
        if (!ctx_mem_) return false;
        secp_ctx_ = secp256k1_context_preallocated_create(
            ctx_mem_, SECP256K1_CONTEXT_SIGN);
        if (!secp_ctx_ ||
            secp256k1_ec_seckey_verify(secp_ctx_, private_key) != 1) {
            release_context();
            return false;
        }

        uint8_t rand32[32]{};
        try {
            fill_entropy(rand32);
        } catch (...) {
            secure_zero(rand32, sizeof(rand32));
            release_context();
            return false;
        }
        const int randomized = secp256k1_context_randomize(secp_ctx_, rand32);
        secure_zero(rand32, sizeof(rand32));
        if (randomized != 1) {
            release_context();
            return false;
        }

        std::memcpy(privkey_, private_key, 32);
        crowdintel::compute_domain_separator(neg_risk, domain_sep_);
        keccak256_hash(reinterpret_cast<const uint8_t*>(crowdintel::K_ORDER_TYPE_STR),
                       std::strlen(crowdintel::K_ORDER_TYPE_STR), order_typehash_);
        if (!derive_signer_address()) {
            secure_zero(privkey_, sizeof(privkey_));
            release_context();
            return false;
        }
        return true;
    }

    ~EIP712Signer() {
        secure_zero(privkey_, sizeof(privkey_));
        release_context();
    }

    EIP712Signer(const EIP712Signer&) = delete;
    EIP712Signer& operator=(const EIP712Signer&) = delete;

    // Address of the signing key (20 bytes), derived at init.
    const uint8_t* signer_address() const { return signer_addr_; }

    // Sign a V2 order → 65-byte (r‖s‖v, v ∈ {27,28}). Returns false on failure
    // (never throws — hot path).
    bool sign_order(const OrderV2& o, uint8_t out_sig65[65]) const noexcept {
        if (!secp_ctx_ || !out_sig65) return false;
        // 1. structHash = keccak256(typeHash ‖ ABI-encode(order))
        uint8_t encoded[32 * 11];
        abi_encode_order(o, encoded);

        uint8_t buf[32 + 32 * 11];
        std::memcpy(buf, order_typehash_, 32);
        std::memcpy(buf + 32, encoded, sizeof(encoded));
        uint8_t struct_hash[32];
        keccak256_hash(buf, sizeof(buf), struct_hash);

        // 2. digest = keccak256(0x1901 ‖ domainSeparator ‖ structHash)
        uint8_t final_buf[2 + 32 + 32];
        final_buf[0] = 0x19; final_buf[1] = 0x01;
        std::memcpy(final_buf + 2, domain_sep_, 32);
        std::memcpy(final_buf + 34, struct_hash, 32);
        uint8_t digest[32];
        keccak256_hash(final_buf, sizeof(final_buf), digest);

        // 3. Recoverable ECDSA — recid (⇒ v) computed during signing at no
        //    extra cost; RFC 6979 deterministic nonce; low-S automatic.
        //    Failure is exceptionally rare after startup key validation.
        secp256k1_ecdsa_recoverable_signature sig;
        if (__builtin_expect(!secp256k1_ecdsa_sign_recoverable(secp_ctx_, &sig, digest,
                                              privkey_, nullptr, nullptr), 0))
            return false;

        int recid = 0;
        uint8_t compact[64];
        if (__builtin_expect(!secp256k1_ecdsa_recoverable_signature_serialize_compact(
                secp_ctx_, compact, &recid, &sig), 0))
            return false;

        std::memcpy(out_sig65, compact, 64);
        out_sig65[64] = static_cast<uint8_t>(27 + recid);
        return true;
    }

    const uint8_t* domain_separator() const { return domain_sep_; }
    const uint8_t* order_typehash() const { return order_typehash_; }

    static void hash_keccak256(const uint8_t* data, size_t len, uint8_t out[32]) {
        keccak256_hash(data, len, out);
    }

private:
    static void fill_entropy(uint8_t out[32]) {
        // One OS-seeded ChaCha20 instance supplies the context blinding seed;
        // no clock, address, or hardware-instruction fallback is accepted.
        FastRandom rng;
        for (int i = 0; i < 4; ++i) {
            const uint64_t word = rng.next_u64();
            std::memcpy(out + i * 8, &word, 8);
        }
    }

    bool derive_signer_address() noexcept {
        // address = last 20 bytes of keccak256(uncompressed pubkey[1..64])
        secp256k1_pubkey pub;
        if (secp256k1_ec_pubkey_create(secp_ctx_, &pub, privkey_) != 1)
            return false;
        uint8_t ser65[65];
        size_t out_len = sizeof(ser65);
        if (secp256k1_ec_pubkey_serialize(
                secp_ctx_, ser65, &out_len, &pub,
                SECP256K1_EC_UNCOMPRESSED) != 1 || out_len != sizeof(ser65))
            return false;
        uint8_t hash[32];
        keccak256_hash(ser65 + 1, 64, hash);
        std::memcpy(signer_addr_, hash + 12, 20);
        secure_zero(hash, sizeof(hash));
        secure_zero(ser65, sizeof(ser65));
        return true;
    }

    void release_context() noexcept {
        if (secp_ctx_) secp256k1_context_preallocated_destroy(secp_ctx_);
        if (ctx_mem_) std::free(ctx_mem_);
        secp_ctx_ = nullptr;
        ctx_mem_ = nullptr;
    }

    secp256k1_context* secp_ctx_ = nullptr;
    void* ctx_mem_ = nullptr;
    uint8_t privkey_[32] = {0};
    uint8_t domain_sep_[32] = {0};
    uint8_t order_typehash_[32] = {0};
    uint8_t signer_addr_[20] = {0};
};

#endif // EIP712_SIGNER_HPP
