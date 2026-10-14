// ─────────────────────────────────────────────────────────────────────────────
// test_signer: crypto known-answer tests + golden E2E vector.
//
// 1. Keccak-256 KATs (Ethereum vectors).
// 2. In-house SHA-256 KAT (FIPS 180-2 vectors — the HMAC engine depends on it).
// 3. HMAC-SHA256 KAT (RFC 4231 test case 2).
// 4. EIP-712 V2 end-to-end GOLDEN VECTOR: domain separator, struct hash,
//    digest and signature are compared byte-for-byte against an independent
//    Python reference implementation of the official Polymarket CLOB V2 scheme
//    (tests/crypto/cross_check_v2.py — pycryptodome Keccak + coincurve RFC
//    6979 ECDSA; libsecp256k1 uses the same deterministic nonce derivation,
//    so signatures must be identical).
// 5. Signature recovery: 20 random orders must recover to the signer address.
// 6. `--json` / `--json-neg-risk`: emit standard and negative-risk vectors
//    for the independent Python cross-check.
// ─────────────────────────────────────────────────────────────────────────────

#include "eip712_signer.hpp"
#include "sha256_engine.hpp"

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static int g_failures = 0;
#define CHECK(cond, name)                                                     \
    do {                                                                      \
        if (cond) { std::printf("  PASS %s\n", name); }                       \
        else { std::printf("  FAIL %s (line %d)\n", name, __LINE__);         \
               ++g_failures; }                                                \
    } while (0)

static bool hex2bin(const char* hex, uint8_t* out, size_t n) {
    return parse_hex_bytes(hex, std::strlen(hex), out, n);
}

static void print_hash(const uint8_t* h) {
    for (int i = 0; i < 32; ++i) std::printf("%02x", h[i]);
}

// ── 1. Keccak KATs ───────────────────────────────────────────────────────────
static void test_keccak() {
    std::printf("keccak256 known-answer vectors\n");
    uint8_t out[32];

    EIP712Signer::hash_keccak256(nullptr, 0, out);
    {
        const char* want = "c5d2460186f7233c927e7db2dcc703c0e500b653ca82273b7bfad8045d85a470";
        uint8_t w[32]; hex2bin(want, w, 32);
        CHECK(std::memcmp(out, w, 32) == 0, "keccak256(\"\")");
    }
    {
        const uint8_t abc[] = {0x61, 0x62, 0x63};
        EIP712Signer::hash_keccak256(abc, 3, out);
        const char* want = "4e03657aea45a94fc7d47ba826c8d667c0d1e6e33a64a036ec44f58fa12d6c45";
        uint8_t w[32]; hex2bin(want, w, 32);
        CHECK(std::memcmp(out, w, 32) == 0, "keccak256(\"abc\")");
    }
    {
        // Multi-block (>136 bytes) input — vector generated with pycryptodome:
        //   keccak256(bytes(i & 0xFF for i in range(300)))
        uint8_t big[300];
        for (size_t i = 0; i < sizeof(big); ++i) big[i] = (uint8_t)(i & 0xFF);
        EIP712Signer::hash_keccak256(big, sizeof(big), out);
        const char* want =
            "a679e749a6af300c36e7ff2255d220864eab27b382f9cfdc5aa4d13563ba36ff";
        uint8_t w[32]; hex2bin(want, w, 32);
        CHECK(std::memcmp(out, w, 32) == 0, "keccak256(300-byte multi-block)");
    }
}

// ── 2. SHA-256 KATs ──────────────────────────────────────────────────────────
static void test_sha256() {
    std::printf("sha256 known-answer vectors (in-house engine)\n");
    uint8_t out[32];
    sha256((const uint8_t*)"abc", 3, out);
    const char* want1 = "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad";
    uint8_t w1[32]; hex2bin(want1, w1, 32);
    CHECK(std::memcmp(out, w1, 32) == 0, "sha256(\"abc\")");

    sha256((const uint8_t*)"abcdbcdecdefdefgefghfghighijhijkijkljklmklmnlmnomnopnopq", 56, out);
    const char* want2 = "248d6a61d20638b8e5c026930c3e6039a33ce45964ff2167f6ecedd419db06c1";
    uint8_t w2[32]; hex2bin(want2, w2, 32);
    CHECK(std::memcmp(out, w2, 32) == 0, "sha256(56-byte vector)");
}

// ── 3. HMAC KAT (RFC 4231 case 2: key "Jefe", data "what do ya want...") ────
static void test_hmac() {
    std::printf("hmac-sha256 RFC 4231 vector\n");
    HmacSha256 h;
    h.set_key((const uint8_t*)"Jefe", 4);
    const char* data = "what do ya want for nothing?";
    uint8_t got[32];
    h.compute((const uint8_t*)data, std::strlen(data), got);
    const char* want = "5bdcc146bf60754e6a042426089575c75a003f089d2739839dec58b964ec3843";
    uint8_t w[32]; hex2bin(want, w, 32);
    CHECK(std::memcmp(got, w, 32) == 0, "rfc4231 case 2 (midstate path)");
}

// ── 4+5. EIP-712 V2 golden vector + recovery ────────────────────────────────
struct GoldenContext {
    EIP712Signer signer;
    OrderV2 order;
};

static void make_golden(GoldenContext& gc, bool neg_risk = false) {
    // MUST match tests/crypto/cross_check_v2.py's golden order.
    const char* KEY = "23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b";
    uint8_t key[32];
    hex2bin(KEY, key, 32);

    if (!gc.signer.init(key, neg_risk)) {
        std::fprintf(stderr, "FATAL: signer initialization failed\n");
        ++g_failures;
    }
    secure_zero(key, sizeof(key));

    OrderV2& o = gc.order;
    o.salt = 123456789012345678ULL;
    hex2bin("26972a79b73e93a0374afabd80302d19638051c9", o.maker, 20);
    hex2bin("26972a79b73e93a0374afabd80302d19638051c9", o.signer, 20);
    const char* tok =
        "71321045679252212594626395510336467040167069592778062791519851593659551227755";
    parse_uint256_dec(tok, std::strlen(tok), o.token_id);
    o.maker_amount = 5500000;
    o.taker_amount = 10000000;
    o.side = K_SIDE_BUY;
    o.signature_type = 0;
    o.timestamp_ms = 1758528000000ULL;
    // metadata/builder stay zeroed
}

static void test_golden_vector() {
    std::printf("EIP-712 V2 golden vector (vs Python reference)\n");
    GoldenContext gc;
    make_golden(gc);

    // Domain separator must match the reference.
    static const uint8_t WANT_DS[32] = {
        0x32,0x64,0xe1,0x59,0x34,0x62,0x53,0xe2,0x6a,0x64,0xe0,0x0b,0x69,0x03,0x2d,0xb0,
        0xe7,0xd3,0x2f,0x94,0x62,0x8d,0xe3,0xe6,0xee,0xcb,0x50,0x30,0x4d,0x7a,0xf3,0xd2};
    CHECK(std::memcmp(gc.signer.domain_separator(), WANT_DS, 32) == 0,
          "domain separator (Polymarket CTF Exchange v2)");

    // Digest must match the reference (struct hash is embedded in it).
    uint8_t sig[65];
    // Recompute digest via a scratch sign on a copy (sign_order returns only
    // the signature; re-derive digest from components to keep this readable).
    uint8_t encoded[32 * 11];
    abi_encode_order(gc.order, encoded);
    uint8_t buf[32 + 32 * 11];
    std::memcpy(buf, gc.signer.order_typehash(), 32);
    std::memcpy(buf + 32, encoded, sizeof(encoded));
    uint8_t struct_hash[32];
    keccak256_hash(buf, sizeof(buf), struct_hash);
    static const uint8_t WANT_SH[32] = {
        0x1a,0x45,0x72,0xf6,0x4f,0xa5,0x3e,0x60,0xc8,0x2a,0x3f,0x10,0x52,0x9a,0x53,0x11,
        0xf2,0x9b,0x19,0x5a,0xcd,0x81,0x51,0x8f,0x01,0xfa,0xcb,0x47,0x69,0x4b,0x23,0xde};
    CHECK(std::memcmp(struct_hash, WANT_SH, 32) == 0, "Order struct hash (V2 ABI)");

    uint8_t final_buf[66];
    final_buf[0] = 0x19; final_buf[1] = 0x01;
    std::memcpy(final_buf + 2, gc.signer.domain_separator(), 32);
    std::memcpy(final_buf + 34, struct_hash, 32);
    uint8_t digest[32];
    keccak256_hash(final_buf, 66, digest);
    static const uint8_t WANT_DIGEST[32] = {
        0x0d,0x0f,0x00,0x59,0x97,0xc8,0xcd,0x2a,0x29,0x1a,0x72,0x16,0xf4,0x19,0xca,0x6c,
        0x88,0x2b,0x1f,0x45,0x44,0xaf,0x55,0x8d,0xb9,0x4e,0x57,0x23,0x3c,0x43,0xef,0x41};
    CHECK(std::memcmp(digest, WANT_DIGEST, 32) == 0, "EIP-712 digest");

    // Signature must match the RFC 6979 reference byte-for-byte.
    CHECK(gc.signer.sign_order(gc.order, sig), "sign succeeds");
    static const uint8_t WANT_SIG[65] = {
        0x2d,0x1a,0x7e,0xdd,0x09,0x6f,0x20,0x73,0xb5,0x57,0x65,0xc3,0x8c,0xf5,0xf5,0x75,
        0x27,0x3f,0x81,0x63,0xa3,0xe1,0x6e,0xb2,0x09,0x95,0x24,0xc0,0x3a,0x62,0x61,0xdf,
        0x4d,0xdc,0x6f,0x54,0xa6,0xe8,0xe3,0x9d,0xf1,0xc6,0xbd,0xd3,0x11,0xbf,0x91,0x6a,
        0x7c,0x20,0xb3,0xaf,0xdc,0x7c,0x8d,0xbd,0x01,0xa4,0x0a,0x83,0x6b,0xad,0xed,0x82,0x1c};
    CHECK(std::memcmp(sig, WANT_SIG, 65) == 0, "signature == coincurve RFC6979 reference");
}

static void test_recovery() {
    std::printf("signature recovery (20 random orders)\n");
    GoldenContext gc;
    make_golden(gc);

    // VERIFY context for local recovery checks.
    secp256k1_context* vctx = secp256k1_context_create(SECP256K1_CONTEXT_VERIFY);

    FastRandom rng;
    bool all_ok = true;
    for (int i = 0; i < 20 && all_ok; ++i) {
        OrderV2 o = gc.order;
        o.salt = rng.next_salt();
        o.timestamp_ms = 1758528000000ULL + (uint64_t)i;
        o.maker_amount = 1000000ULL + (uint64_t)i;
        o.taker_amount = 2000000ULL + (uint64_t)i;

        uint8_t encoded[32 * 11];
        abi_encode_order(o, encoded);
        uint8_t buf[32 + 32 * 11];
        std::memcpy(buf, gc.signer.order_typehash(), 32);
        std::memcpy(buf + 32, encoded, sizeof(encoded));
        uint8_t struct_hash[32];
        keccak256_hash(buf, sizeof(buf), struct_hash);
        uint8_t final_buf[66] = {0x19, 0x01};
        std::memcpy(final_buf + 2, gc.signer.domain_separator(), 32);
        std::memcpy(final_buf + 34, struct_hash, 32);
        uint8_t digest[32];
        keccak256_hash(final_buf, 66, digest);

        uint8_t sig[65];
        if (!gc.signer.sign_order(o, sig)) { all_ok = false; break; }
        if (sig[64] != 27 && sig[64] != 28) { all_ok = false; break; }

        secp256k1_ecdsa_recoverable_signature rsig;
        if (!secp256k1_ecdsa_recoverable_signature_parse_compact(
                vctx, &rsig, sig, sig[64] - 27)) { all_ok = false; break; }
        secp256k1_pubkey pub;
        if (!secp256k1_ecdsa_recover(vctx, &pub, &rsig, digest)) { all_ok = false; break; }
        uint8_t ser65[65];
        size_t ser_len = 65;
        secp256k1_ec_pubkey_serialize(vctx, ser65, &ser_len, &pub,
                                      SECP256K1_EC_UNCOMPRESSED);
        uint8_t hash[32];
        keccak256_hash(ser65 + 1, 64, hash);
        if (std::memcmp(hash + 12, gc.signer.signer_address(), 20) != 0) {
            all_ok = false;
        }
    }
    secp256k1_context_destroy(vctx);
    CHECK(all_ok, "all 20 signatures recover to the signer address");
}

// ── 6. JSON emission for the Python cross-check ──────────────────────────────
static void emit_json(bool neg_risk = false) {
    GoldenContext gc;
    make_golden(gc, neg_risk);
    uint8_t sig[65];
    if (!gc.signer.sign_order(gc.order, sig)) { std::exit(1); }
    const char* KEY = "23dd72ba9070d7903cf60cad22700819abb7ae93c5788e15f038a0ece0a6697b";

    uint8_t encoded[32 * 11];
    abi_encode_order(gc.order, encoded);
    uint8_t buf[32 + 32 * 11];
    std::memcpy(buf, gc.signer.order_typehash(), 32);
    std::memcpy(buf + 32, encoded, sizeof(encoded));
    uint8_t struct_hash[32];
    keccak256_hash(buf, sizeof(buf), struct_hash);
    uint8_t final_buf[66] = {0x19, 0x01};
    std::memcpy(final_buf + 2, gc.signer.domain_separator(), 32);
    std::memcpy(final_buf + 34, struct_hash, 32);
    uint8_t digest[32];
    keccak256_hash(final_buf, 66, digest);

    std::printf("{\n");
    std::printf("  \"neg_risk\": %s,\n", neg_risk ? "true" : "false");
    std::printf("  \"private_key_hex\": \"%s\",\n", KEY);
    std::printf("  \"eip712_digest\": \"");
    print_hash(digest);
    std::printf("\",\n  \"signature\": \"");
    for (int i = 0; i < 65; ++i) std::printf("%02x", sig[i]);
    std::printf("\",\n  \"order\": {\n");
    std::printf("    \"salt\": %llu,\n", (unsigned long long)gc.order.salt);
    std::printf("    \"maker\": \"0x");
    for (int i = 0; i < 20; ++i) std::printf("%02x", gc.order.maker[i]);
    std::printf("\",\n    \"signer\": \"0x");
    for (int i = 0; i < 20; ++i) std::printf("%02x", gc.order.signer[i]);
    std::printf("\",\n    \"token_id\": \"%s\",\n",
        "71321045679252212594626395510336467040167069592778062791519851593659551227755");
    std::printf("    \"maker_amount\": %llu,\n", (unsigned long long)gc.order.maker_amount);
    std::printf("    \"taker_amount\": %llu,\n", (unsigned long long)gc.order.taker_amount);
    std::printf("    \"side\": %u,\n", gc.order.side);
    std::printf("    \"signature_type\": %u,\n", gc.order.signature_type);
    std::printf("    \"timestamp_ms\": %llu,\n", (unsigned long long)gc.order.timestamp_ms);
    std::printf("    \"metadata\": \"0x");
    for (int i = 0; i < 32; ++i) std::printf("%02x", gc.order.metadata[i]);
    std::printf("\",\n    \"builder\": \"0x");
    for (int i = 0; i < 32; ++i) std::printf("%02x", gc.order.builder[i]);
    std::printf("\"\n  }\n}\n");
}

int main(int argc, char** argv) {
    if (argc > 1 && (std::strcmp(argv[1], "--json") == 0 ||
                     std::strcmp(argv[1], "--json-neg-risk") == 0)) {
        emit_json(std::strcmp(argv[1], "--json-neg-risk") == 0);
        return 0;
    }
    std::printf("== CROWDINTEL crypto KATs ==\n");
    test_keccak();
    test_sha256();
    test_hmac();
    test_golden_vector();
    test_recovery();
    std::printf("== %s (%d failures) ==\n", g_failures ? "FAILED" : "ALL PASS", g_failures);
    return g_failures ? 1 : 0;
}
