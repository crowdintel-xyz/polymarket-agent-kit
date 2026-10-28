#pragma once
// MEDIUM-1 FIX (NEMESIS): Shared ClockFn type alias — single source of truth
// Previously duplicated in rate_limiter.hpp:31 and circuit_breaker.hpp:31
// which creates an ODR hazard if definitions ever diverge.

using ClockFn = uint64_t (*)();
