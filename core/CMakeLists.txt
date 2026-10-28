cmake_minimum_required(VERSION 3.16)
project(CrowdIntelBot LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)

option(CROWDINTEL_NETWORK "Build live curl/OpenSSL transports" ON)
option(CROWDINTEL_LTO "Enable compiler-supported interprocedural optimization" ON)
option(CROWDINTEL_SANITIZERS "Backward-compatible alias for ASan + UBSan" OFF)
option(CROWDINTEL_ASAN "Enable AddressSanitizer" OFF)
option(CROWDINTEL_UBSAN "Enable UndefinedBehaviorSanitizer" OFF)
option(CROWDINTEL_TSAN "Enable ThreadSanitizer" OFF)
set(CROWDINTEL_CPU_TARGET "portable" CACHE STRING
    "CPU target: portable, x86-64-v2, x86-64-v3, or native (local-only)")
set_property(CACHE CROWDINTEL_CPU_TARGET PROPERTY STRINGS
    portable x86-64-v2 x86-64-v3 native)

if(NOT CMAKE_BUILD_TYPE)
    set(CMAKE_BUILD_TYPE Release CACHE STRING "Build type" FORCE)
endif()

find_package(Threads REQUIRED)

if(SECP256K1_ROOT OR DEFINED ENV{SECP256K1_ROOT})
    if(SECP256K1_ROOT)
        set(_secp_root "${SECP256K1_ROOT}")
    else()
        set(_secp_root "$ENV{SECP256K1_ROOT}")
    endif()
    # An explicit root is a supply-chain boundary, not a hint behind whatever
    # version happens to be installed globally or cached from an older build.
    unset(SECP256K1_LIBRARY CACHE)
    unset(SECP256K1_INCLUDE_DIR CACHE)
    find_library(SECP256K1_LIBRARY NAMES secp256k1
        PATHS "${_secp_root}/build/lib" "${_secp_root}/lib" "${_secp_root}/.libs"
        NO_DEFAULT_PATH)
    find_path(SECP256K1_INCLUDE_DIR secp256k1.h
        PATHS "${_secp_root}/include" NO_DEFAULT_PATH)
else()
    find_library(SECP256K1_LIBRARY NAMES secp256k1)
    find_path(SECP256K1_INCLUDE_DIR secp256k1.h)
endif()
if(NOT SECP256K1_LIBRARY OR NOT SECP256K1_INCLUDE_DIR)
    message(FATAL_ERROR "libsecp256k1 with recovery support not found; pass "
                        "-DSECP256K1_LIBRARY=... -DSECP256K1_INCLUDE_DIR=...")
endif()
message(STATUS "libsecp256k1: ${SECP256K1_LIBRARY}")

set(HAVE_NETWORK OFF)
if(CROWDINTEL_NETWORK)
    find_package(OpenSSL QUIET)
    find_package(CURL QUIET)
    if(OpenSSL_FOUND AND CURL_FOUND)
        set(HAVE_NETWORK ON)
        message(STATUS "network layer: ON")
    else()
        message(FATAL_ERROR
            "CROWDINTEL_NETWORK=ON requires libcurl and OpenSSL development packages; "
            "use -DCROWDINTEL_NETWORK=OFF for an explicit offline build")
    endif()
endif()

add_library(crowdintel_options INTERFACE)
target_compile_options(crowdintel_options INTERFACE
    $<$<CONFIG:Release>:-O3>
    -Wall -Wextra -Wpedantic -Wconversion -Wshadow
    -fno-plt -fvisibility=hidden)
if(CROWDINTEL_CPU_TARGET STREQUAL "native")
    target_compile_options(crowdintel_options INTERFACE -march=native)
    message(WARNING "-march=native binaries must not be copied to another CPU class")
elseif(CROWDINTEL_CPU_TARGET MATCHES "^x86-64-v[23]$")
    if(CMAKE_SYSTEM_PROCESSOR MATCHES "x86_64|AMD64|amd64")
        target_compile_options(crowdintel_options INTERFACE
            -march=${CROWDINTEL_CPU_TARGET})
    else()
        message(FATAL_ERROR "${CROWDINTEL_CPU_TARGET} requested on non-x86 target")
    endif()
elseif(NOT CROWDINTEL_CPU_TARGET STREQUAL "portable")
    message(FATAL_ERROR "Unknown CROWDINTEL_CPU_TARGET=${CROWDINTEL_CPU_TARGET}")
endif()

if(CROWDINTEL_SANITIZERS)
    set(CROWDINTEL_ASAN ON)
    set(CROWDINTEL_UBSAN ON)
endif()
if(CROWDINTEL_TSAN AND (CROWDINTEL_ASAN OR CROWDINTEL_UBSAN))
    message(FATAL_ERROR "TSan cannot be combined with ASan/UBSan in this build")
endif()
set(_sanitizers "")
if(CROWDINTEL_ASAN)
    list(APPEND _sanitizers address)
endif()
if(CROWDINTEL_UBSAN)
    list(APPEND _sanitizers undefined)
endif()
if(CROWDINTEL_TSAN)
    list(APPEND _sanitizers thread)
endif()
if(_sanitizers)
    list(JOIN _sanitizers "," _sanitizer_list)
    target_compile_options(crowdintel_options INTERFACE
        -fsanitize=${_sanitizer_list} -fno-omit-frame-pointer -O1)
    target_link_options(crowdintel_options INTERFACE
        -fsanitize=${_sanitizer_list})
    message(STATUS "sanitizers: ${_sanitizer_list}")
endif()

if(CROWDINTEL_LTO)
    include(CheckIPOSupported)
    check_ipo_supported(RESULT IPO_SUPPORTED OUTPUT IPO_ERROR)
    if(IPO_SUPPORTED)
        set(CMAKE_INTERPROCEDURAL_OPTIMIZATION TRUE)
        message(STATUS "IPO/LTO: ON")
    else()
        message(WARNING "IPO/LTO unsupported: ${IPO_ERROR}")
    endif()
endif()

set(CROWDINTEL_INCLUDES
    ${CMAKE_CURRENT_SOURCE_DIR}/include
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${CMAKE_CURRENT_SOURCE_DIR}/crypto
    ${CMAKE_CURRENT_SOURCE_DIR}/../alpha/crowdintel
    ${CMAKE_CURRENT_SOURCE_DIR}/../alpha/strategy
    ${SECP256K1_INCLUDE_DIR})

function(crowdintel_target target)
    target_include_directories(${target} PRIVATE ${CROWDINTEL_INCLUDES})
    target_link_libraries(${target} PRIVATE
        crowdintel_options ${SECP256K1_LIBRARY} Threads::Threads)
endfunction()

add_executable(crowdintel_bot src/main_hot_path.cpp)
crowdintel_target(crowdintel_bot)
if(HAVE_NETWORK)
    target_compile_definitions(crowdintel_bot PRIVATE CROWDINTEL_HAVE_NETWORK=1)
    target_link_libraries(crowdintel_bot PRIVATE CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
else()
    target_compile_definitions(crowdintel_bot PRIVATE CROWDINTEL_FORCE_MOCK=1)
endif()
set_target_properties(crowdintel_bot PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_signer crypto/test_signer.cpp)
crowdintel_target(test_signer)
set_target_properties(test_signer PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_core ../tests/unit/test_core.cpp)
crowdintel_target(test_core)
if(HAVE_NETWORK)
    target_compile_definitions(test_core PRIVATE CROWDINTEL_HAVE_NETWORK=1)
    target_link_libraries(test_core PRIVATE
        CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
endif()
set_target_properties(test_core PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(latency_bench ../tests/benchmarks/latency_bench.cpp)
crowdintel_target(latency_bench)
set_target_properties(latency_bench PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(l2_backtester ../tests/replay/l2_backtester.cpp)
crowdintel_target(l2_backtester)
set_target_properties(l2_backtester PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_layers ../tests/unit/test_layers.cpp)
crowdintel_target(test_layers)
if(HAVE_NETWORK)
    target_compile_definitions(test_layers PRIVATE CROWDINTEL_HAVE_NETWORK=1)
    target_link_libraries(test_layers PRIVATE
        CURL::libcurl OpenSSL::SSL OpenSSL::Crypto)
endif()
set_target_properties(test_layers PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── AVX2 detection ─────────────────────────────────────────────────────────────
# CI uses -DCROWDINTEL_CPU_TARGET=portable (no AVX2 by default). CfC network
# uses AVX2+FMA intrinsics. Detect availability before building those targets.
set(CMAKE_REQUIRED_FLAGS_SAVE "${CMAKE_REQUIRED_FLAGS}")
set(CMAKE_REQUIRED_FLAGS "-mavx2 -mfma ${CMAKE_REQUIRED_FLAGS}")
check_cxx_source_compiles("
    #include <immintrin.h>
    int main() {
        __m256 a = _mm256_set1_ps(1.0f);
        __m256 b = _mm256_fmadd_ps(a, a, a);
        (void)b; return 0;
    }
" HAVE_AVX2_INTRINSICS)
set(CMAKE_REQUIRED_FLAGS "${CMAKE_REQUIRED_FLAGS_SAVE}")

# ── Phase 1: OFI + Binance WS + Evidence Adapter tests ──────────────────────────
# OFI uses only SSE4.2 (portable across all x86-64). No AVX2 required.
add_executable(test_ofi ../tests/unit/test_ofi.cpp)
crowdintel_target(test_ofi)
set_target_properties(test_ofi PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Phase 4: Fee, Kelly, Ladder, Time strategy tests ───────────────────────────
# Pure-math components, no AVX2 or network deps.
add_executable(test_fees ../tests/unit/test_fees.cpp)
crowdintel_target(test_fees)
set_target_properties(test_fees PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_kelly ../tests/unit/test_kelly.cpp)
crowdintel_target(test_kelly)
set_target_properties(test_kelly PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_ladder ../tests/unit/test_ladder.cpp)
crowdintel_target(test_ladder)
set_target_properties(test_ladder PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Phase 2: CfC Network tests ─────────────────────────────────────────────────
# Only build when AVX2+FMA intrinsics are available.
if(HAVE_AVX2_INTRINSICS)
    # Enable AVX2+FMA for CfC test targets (intrinsics are used unconditionally
    # in cfc_network.hpp). check_cxx_source_compiles only tests compile ability;
    # we must also pass the flags to actual targets.
    add_executable(test_cfc_kat ../tests/unit/test_cfc_kat.cpp)
    crowdintel_target(test_cfc_kat)
    target_compile_options(test_cfc_kat PRIVATE -mavx2 -mfma)
    set_target_properties(test_cfc_kat PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

    # Benchmark: latency < 3μs p50, < 5μs p99
    add_executable(bench_cfc ../tests/benchmarks/bench_cfc.cpp)
    crowdintel_target(bench_cfc)
    target_compile_options(bench_cfc PRIVATE -mavx2 -mfma)
    set_target_properties(bench_cfc PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
    # bench_cfc must run from the repo root to find the model file
    set_target_properties(bench_cfc PROPERTIES
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..)
else()
    message(STATUS "AVX2 not available — skipping CfC tests/benchmarks")
endif()

# ── Phase 5: Integration tests (DSH, NO AVX2 required) ─────────────────────────
# Pure deterministic math pipeline: BB + Vol + OFI + WindowShield + Kelly + Ladder
add_executable(test_pipeline ../tests/integration/test_pipeline.cpp)
crowdintel_target(test_pipeline)
set_target_properties(test_pipeline PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_dsh_pipeline ../tests/integration/test_dsh_pipeline.cpp)
crowdintel_target(test_dsh_pipeline)
set_target_properties(test_dsh_pipeline PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_paper_trade ../tests/integration/test_paper_trade.cpp)
crowdintel_target(test_paper_trade)
set_target_properties(test_paper_trade PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Phase 1 & 2: Deterministic Math ─────────────────────────────────────────────
# Brownian Bridge TWAP probability, volatility estimator, OFI filter, WindowShield

add_executable(test_twap_bb ../tests/unit/test_twap_bb.cpp)
crowdintel_target(test_twap_bb)
target_compile_options(test_twap_bb PRIVATE -Werror)
set_target_properties(test_twap_bb PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_vol_estimator ../tests/unit/test_vol_estimator.cpp)
crowdintel_target(test_vol_estimator)
target_compile_options(test_vol_estimator PRIVATE -Werror)
set_target_properties(test_vol_estimator PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_ofi_filter ../tests/unit/test_ofi_filter.cpp)
crowdintel_target(test_ofi_filter)
target_compile_options(test_ofi_filter PRIVATE -Werror)
set_target_properties(test_ofi_filter PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_window_shield ../tests/unit/test_window_shield.cpp)
crowdintel_target(test_window_shield)
target_compile_options(test_window_shield PRIVATE -Werror)
set_target_properties(test_window_shield PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Phase 3: Network-layer primitives ──────────────────────────────────────────
# Rate limiter, circuit breaker, request prioritizer.
# -Werror enforced: these headers must be warning-clean under -Wall -Wextra
# -Wpedantic -Wconversion -Wshadow.
add_executable(test_rate_limiter ../tests/unit/test_rate_limiter.cpp)
crowdintel_target(test_rate_limiter)
target_compile_options(test_rate_limiter PRIVATE -Werror)
set_target_properties(test_rate_limiter PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_circuit_breaker ../tests/unit/test_circuit_breaker.cpp)
crowdintel_target(test_circuit_breaker)
target_compile_options(test_circuit_breaker PRIVATE -Werror)
set_target_properties(test_circuit_breaker PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_request_prioritizer ../tests/unit/test_request_prioritizer.cpp)
crowdintel_target(test_request_prioritizer)
target_compile_options(test_request_prioritizer PRIVATE -Werror)
set_target_properties(test_request_prioritizer PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# ── Phase 5: Acceptance tests (granular verification) ─────────────────────────
add_executable(test_accept_bb_conviction ../tests/acceptance/test_accept_bb_conviction.cpp)
crowdintel_target(test_accept_bb_conviction)
set_target_properties(test_accept_bb_conviction PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_accept_vol_regime ../tests/acceptance/test_accept_vol_regime.cpp)
crowdintel_target(test_accept_vol_regime)
set_target_properties(test_accept_vol_regime PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_accept_ofi_pressure ../tests/acceptance/test_accept_ofi_pressure.cpp)
crowdintel_target(test_accept_ofi_pressure)
set_target_properties(test_accept_ofi_pressure PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_accept_shield_lifecycle ../tests/acceptance/test_accept_shield_lifecycle.cpp)
crowdintel_target(test_accept_shield_lifecycle)
set_target_properties(test_accept_shield_lifecycle PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_accept_rate_circuit ../tests/acceptance/test_accept_rate_circuit.cpp)
crowdintel_target(test_accept_rate_circuit)
set_target_properties(test_accept_rate_circuit PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_accept_kelly_edge ../tests/acceptance/test_accept_kelly_edge.cpp)
crowdintel_target(test_accept_kelly_edge)
set_target_properties(test_accept_kelly_edge PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_accept_ladder_time ../tests/acceptance/test_accept_ladder_time.cpp)
crowdintel_target(test_accept_ladder_time)
set_target_properties(test_accept_ladder_time PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_executable(test_accept_latency_budget ../tests/acceptance/test_accept_latency_budget.cpp)
crowdintel_target(test_accept_latency_budget)
set_target_properties(test_accept_latency_budget PROPERTIES RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

# T1.1: Daily volume cap enforcement (CSAA critical fix ID-C02)
add_executable(test_daily_volume_cap ../tests/unit/test_daily_volume_cap.cpp)
crowdintel_target(test_daily_volume_cap)
set_target_properties(test_daily_volume_cap PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

enable_testing()
add_test(NAME crypto_kat COMMAND $<TARGET_FILE:test_signer>)
add_test(NAME core_units COMMAND $<TARGET_FILE:test_core>)
add_executable(test_daily_volume_persist ../tests/unit/test_daily_volume_persist.cpp)
crowdintel_target(test_daily_volume_persist)
set_target_properties(test_daily_volume_persist PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_test(NAME daily_vol_cap COMMAND $<TARGET_FILE:test_daily_volume_cap>)
add_test(NAME daily_volume_persist COMMAND $<TARGET_FILE:test_daily_volume_persist>)

# P2: L2 book archive
add_executable(test_l2_archive ../tests/unit/test_l2_archive.cpp)
crowdintel_target(test_l2_archive)
set_target_properties(test_l2_archive PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_test(NAME l2_archive COMMAND $<TARGET_FILE:test_l2_archive>)

# P3: Market discovery
add_executable(test_market_discovery ../tests/unit/test_market_discovery.cpp)
crowdintel_target(test_market_discovery)
set_target_properties(test_market_discovery PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_test(NAME market_discovery COMMAND $<TARGET_FILE:test_market_discovery>)

# P4: Paper trading broker
add_executable(test_paper_broker ../tests/unit/test_paper_broker.cpp)
crowdintel_target(test_paper_broker)
set_target_properties(test_paper_broker PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_test(NAME paper_broker COMMAND $<TARGET_FILE:test_paper_broker>)

# P5: Maker-bid fill-rate metric
add_executable(test_maker_bid_metrics ../tests/unit/test_maker_bid_metrics.cpp)
crowdintel_target(test_maker_bid_metrics)
set_target_properties(test_maker_bid_metrics PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_test(NAME maker_bid_metrics COMMAND $<TARGET_FILE:test_maker_bid_metrics>)

# P6: Nightly audit ledger
add_executable(test_nightly_audit ../tests/unit/test_nightly_audit.cpp)
crowdintel_target(test_nightly_audit)
set_target_properties(test_nightly_audit PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_test(NAME nightly_audit COMMAND $<TARGET_FILE:test_nightly_audit>)

# P1: WebSocket session manager
add_executable(test_ws_session ../tests/unit/test_ws_session.cpp)
crowdintel_target(test_ws_session)
set_target_properties(test_ws_session PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_test(NAME ws_session COMMAND $<TARGET_FILE:test_ws_session>)
add_test(NAME layer_units COMMAND $<TARGET_FILE:test_layers>)
add_test(NAME ofi_units COMMAND $<TARGET_FILE:test_ofi>)
add_executable(test_fee_consistency ../tests/unit/test_fee_consistency.cpp)
crowdintel_target(test_fee_consistency)
set_target_properties(test_fee_consistency PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

add_test(NAME fee_units COMMAND $<TARGET_FILE:test_fees>)
add_test(NAME fee_consistency COMMAND $<TARGET_FILE:test_fee_consistency>)
add_test(NAME kelly_units COMMAND $<TARGET_FILE:test_kelly>)
add_test(NAME ladder_units COMMAND $<TARGET_FILE:test_ladder>)
if(HAVE_AVX2_INTRINSICS)
    add_test(NAME cfc_kat COMMAND $<TARGET_FILE:test_cfc_kat>
        WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..)
endif()
add_test(NAME pipeline_integration COMMAND $<TARGET_FILE:test_pipeline>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..)
add_test(NAME dsh_pipeline COMMAND $<TARGET_FILE:test_dsh_pipeline>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..)
add_test(NAME paper_trade COMMAND $<TARGET_FILE:test_paper_trade>
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..)
# Phase 1 enhancement: Multi-feed integration tests
add_executable(test_multi_feed ../tests/integration/test_multi_feed.cpp)
crowdintel_target(test_multi_feed)
set_target_properties(test_multi_feed PROPERTIES
    RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)
add_test(NAME multi_feed COMMAND $<TARGET_FILE:test_multi_feed>
         WORKING_DIRECTORY ${CMAKE_SOURCE_DIR}/..)
add_test(NAME backtester_smoke COMMAND $<TARGET_FILE:l2_backtester>
         ${CMAKE_CURRENT_SOURCE_DIR}/../tests/replay/sample_ticks.csv)

# Phase 3: network-layer primitives
add_test(NAME rate_limiter_units COMMAND $<TARGET_FILE:test_rate_limiter>)
add_test(NAME circuit_breaker_units COMMAND $<TARGET_FILE:test_circuit_breaker>)
add_test(NAME request_prioritizer_units COMMAND $<TARGET_FILE:test_request_prioritizer>)

# Phase 1 & 2: deterministic math tests
add_test(NAME twap_bb_kat COMMAND $<TARGET_FILE:test_twap_bb>)
add_test(NAME vol_estimator COMMAND $<TARGET_FILE:test_vol_estimator>)
add_test(NAME ofi_filter COMMAND $<TARGET_FILE:test_ofi_filter>)
add_test(NAME window_shield COMMAND $<TARGET_FILE:test_window_shield>)

# Phase 5: acceptance tests
add_test(NAME accept_bb_conviction COMMAND $<TARGET_FILE:test_accept_bb_conviction>)
add_test(NAME accept_vol_regime COMMAND $<TARGET_FILE:test_accept_vol_regime>)
add_test(NAME accept_ofi_pressure COMMAND $<TARGET_FILE:test_accept_ofi_pressure>)
add_test(NAME accept_shield_lifecycle COMMAND $<TARGET_FILE:test_accept_shield_lifecycle>)
add_test(NAME accept_rate_circuit COMMAND $<TARGET_FILE:test_accept_rate_circuit>)
add_test(NAME accept_kelly_edge COMMAND $<TARGET_FILE:test_accept_kelly_edge>)
add_test(NAME accept_ladder_time COMMAND $<TARGET_FILE:test_accept_ladder_time>)
add_test(NAME accept_latency_budget COMMAND $<TARGET_FILE:test_accept_latency_budget>)
# bench_cfc and latency_bench are benchmarks, not correctness tests; CI invokes them explicitly.
