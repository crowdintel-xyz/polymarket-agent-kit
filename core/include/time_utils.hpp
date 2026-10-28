#ifndef TIME_UTILS_HPP
#define TIME_UTILS_HPP

// Shared monotonic/realtime helpers in one place.  Inline, syscall-free
// wrappers (clock_gettime via vDSO on Linux), safe for the hot path.

#include <chrono>
#include <cstdint>
#include <ctime>

namespace crowdintel {

inline uint64_t mono_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::steady_clock::now().time_since_epoch()).count());
}

inline uint64_t realtime_ns() noexcept {
    return static_cast<uint64_t>(
        std::chrono::duration_cast<std::chrono::nanoseconds>(
            std::chrono::system_clock::now().time_since_epoch()).count());
}

inline uint64_t mono_ms() noexcept {
    return mono_ns() / 1000000ULL;
}

inline uint64_t realtime_ms() noexcept {
    timespec ts{};
    clock_gettime(CLOCK_REALTIME, &ts);
    return static_cast<uint64_t>(ts.tv_sec) * 1000ULL +
           static_cast<uint64_t>(ts.tv_nsec) / 1000000ULL;
}

}  // namespace crowdintel

#endif  // TIME_UTILS_HPP
