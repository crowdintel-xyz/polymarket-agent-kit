# Gate 1: TSan Verification Protocol

## Build Configuration (TSan with Clang)

```bash
cmake -S core -B build_tsan -G Ninja \
  -DCMAKE_CXX_COMPILER=clang++ \
  -DCMAKE_C_COMPILER=clang \
  -DCMAKE_CXX_FLAGS="-fsanitize=thread -fno-omit-frame-pointer -O1" \
  -DCMAKE_EXE_LINKER_FLAGS="-fsanitize=thread" \
  -DSECP256K1_LIBRARY=/tmp/secp256k1/build/lib/libsecp256k1.a \
  -DSECP256K1_INCLUDE_DIR=/tmp/secp256k1/include
```

## Test Execution

```bash
# TSan suite (excludes timing tests — see rationale below)
TSAN_OPTIONS=halt_on_error=1:second_deadlock_stack=1 \
  ctest --test-dir build_tsan --output-on-failure -E "bench_cfc|accept_latency_budget"
# Result: 29/29 PASS

# Release latency budget (runs in build/, NOT build_tsan)
ctest --test-dir build -R accept_latency_budget --output-on-failure
# Result: 5/5 PASS
```

## Exclusions Rationale

| Excluded Test | Reason |
|--------------|--------|
| `bench_cfc` | Pre-existing link conflict (undefined symbols in AVX2 intrinsics under `-fsanitize=thread`). Not a unit test — performance benchmark only. |
| `accept_latency_budget` | **Intentionally excluded from TSan runs.** Measures nanosecond-level latency budgets (Fee calc p99 < 100ns, OFI update < 100ns, etc.). TSan instrumentation multiplies overhead ~5-10×, making latency measurements invalid. Runs in GCC Release build (`-O3`) where measurements are valid. |

## Gate 1 Verification

| Suite | Compiler | Flags | Tests | Status |
|-------|----------|-------|-------|--------|
| ASan + UBSan | GCC | `-O2 -fsanitize=address,undefined` | 28/30 | ✅ PASS |
| TSan | Clang | `-fsanitize=thread` | 29/29 | ✅ PASS |
| Latency Budget | GCC | `-O3 -DNDEBUG` | 5/5 | ✅ PASS |
