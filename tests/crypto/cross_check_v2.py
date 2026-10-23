#!/usr/bin/env python3
"""
cross_check_v2.py — Python reference cross-verification for the C++ signer.

Verifies that the C++ EIP-712 signer output (emitted by `test_signer --json`)
matches an independent Python implementation of the official Polymarket CLOB
V2 scheme (docs.polymarket.com/v2-migration), using pycryptodome (Keccak-256)
and coincurve (secp256k1, RFC 6979 — the same nonce derivation libsecp256k1
uses, so signatures must match byte-for-byte).

Usage:
    ./bin/test_signer --json > /tmp/signer_out.json
    python3 tests/crypto/cross_check_v2.py /tmp/signer_out.json
"""
import base64
import hashlib
import json
import sys

from Crypto.Hash import keccak
from coincurve import PrivateKey, PublicKey


def keccak256(data: bytes) -> bytes:
    return keccak.new(digest_bits=256, data=data).digest()


DOMAIN_TYPE = "EIP712Domain(string name,string version,uint256 chainId,address verifyingContract)"
ORDER_TYPE = (
    "Order(uint256 salt,address maker,address signer,uint256 tokenId,"
    "uint256 makerAmount,uint256 takerAmount,uint8 side,uint8 signatureType,"
    "uint256 timestamp,bytes32 metadata,bytes32 builder)"
)
STANDARD_EXCHANGE = "0xE111180000d2663C0091e4f400237545B87B996B"
NEG_RISK_EXCHANGE = "0xe2222d279d744050d28e00520010520000310F59"


def domain_separator(neg_risk: bool = False) -> bytes:
    enc = (
        keccak256(DOMAIN_TYPE.encode())
        + keccak256(b"Polymarket CTF Exchange")
        + keccak256(b"2")
        + (137).to_bytes(32, "big")
        + bytes.fromhex((NEG_RISK_EXCHANGE if neg_risk else STANDARD_EXCHANGE)[2:].lower())
            .rjust(32, b"\x00")
    )
    return keccak256(enc)


def order_digest(o: dict, neg_risk: bool = False) -> bytes:
    def a(addr): return bytes.fromhex(addr[2:].lower()).rjust(32, b"\x00")
    def u(n): return int(n).to_bytes(32, "big")
    struct_hash = keccak256(
        keccak256(ORDER_TYPE.encode())
        + u(o["salt"]) + a(o["maker"]) + a(o["signer"])
        + u(o["token_id"]) + u(o["maker_amount"]) + u(o["taker_amount"])
        + u(o["side"]) + u(o["signature_type"]) + u(o["timestamp_ms"])
        + bytes.fromhex(o["metadata"][2:]) + bytes.fromhex(o["builder"][2:])
    )
    return keccak256(b"\x19\x01" + domain_separator(neg_risk) + struct_hash)


def main() -> int:
    blob = json.load(open(sys.argv[1])) if len(sys.argv) > 1 else json.load(sys.stdin)
    priv = bytes.fromhex(blob["private_key_hex"])
    o = blob["order"]
    sig = bytes.fromhex(blob["signature"])     # r || s || v(27/28)
    got_digest = bytes.fromhex(blob["eip712_digest"])

    # 1. Digest must match the independently selected exchange domain.
    neg_risk = bool(blob.get("neg_risk", False))
    want_digest = order_digest(o, neg_risk)
    ok_digest = got_digest == want_digest

    # 2. Signature must verify against the digest and recover the signer
    #    (coincurve wants the raw recid 0/3 in the last byte, not EIP-155 v=27/28)
    recid = sig[64] - 27
    pub = PublicKey.from_signature_and_message(sig[:64] + bytes([recid]),
                                               want_digest, hasher=None)
    addr = "0x" + keccak256(pub.format(compressed=False)[1:])[-20:].hex()
    ok_recover = addr == o["signer"]

    # 3. RFC 6979 determinism: independent Python lib must produce identical r||s
    pk = PrivateKey(priv)
    ref_sig = pk.sign_recoverable(want_digest, hasher=None)
    ok_det = ref_sig[:64] == sig[:64] and 27 + ref_sig[64] == sig[64]

    print(f"domain                   : {'negative-risk' if neg_risk else 'standard'}")
    print(f"digest matches reference : {'PASS' if ok_digest else 'FAIL'}")
    print(f"signature recovers signer: {'PASS' if ok_recover else 'FAIL'}")
    print(f"rfc6979 sig matches      : {'PASS' if ok_det else 'FAIL'}")
    print(f"signer                   : {addr}")
    return 0 if (ok_digest and ok_recover and ok_det) else 1


if __name__ == "__main__":
    sys.exit(main())
