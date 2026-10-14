#!/usr/bin/env bash
set -euo pipefail

REPO_ROOT="${1:-$(cd "$(dirname "$0")/.." && pwd)}"
SECP_COMMIT="6e2c8bc4ecdc6e71dbe7a368f360d8d453ce435d"

echo "=== Building libsecp256k1 at ${SECP_COMMIT} ==="
if [ ! -d /tmp/secp256k1 ]; then
  git clone --filter=blob:none https://github.com/bitcoin-core/secp256k1.git /tmp/secp256k1
fi
git -C /tmp/secp256k1 checkout --quiet --detach "$SECP_COMMIT"
cmake -S /tmp/secp256k1 -B /tmp/secp256k1/build -G Ninja \
  -DCMAKE_BUILD_TYPE=Release -DBUILD_SHARED_LIBS=OFF \
  -DSECP256K1_ENABLE_MODULE_RECOVERY=ON \
  -DSECP256K1_BUILD_BENCHMARK=OFF -DSECP256K1_BUILD_TESTS=OFF \
  -DSECP256K1_BUILD_EXHAUSTIVE_TESTS=OFF
cmake --build /tmp/secp256k1/build -j"$(nproc)"

echo "=== Building Bot_Crowdintel ==="
cmake -S "$REPO_ROOT/core" -B "$REPO_ROOT/build" -G Ninja \
  -DCMAKE_BUILD_TYPE=Release \
  -DCROWDINTEL_NETWORK=OFF \
  -DCROWDINTEL_CPU_TARGET=portable \
  -DSECP256K1_ROOT=/tmp/secp256k1

cmake --build "$REPO_ROOT/build" -j"$(nproc)"

echo "=== Running tests ==="
ctest --test-dir "$REPO_ROOT/build" --output-on-failure

echo "=== Build complete ==="
