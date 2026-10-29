#ifndef SECURE_ZERO_HPP
#define SECURE_ZERO_HPP

#include <cstddef>
#include <cstdint>

// Volatile-wipe memory so the compiler cannot elide it (drops temporaries of
// keys/secrets). For a hardened deployment pair with mlock() to avoid swap.
inline void secure_zero(void* p, size_t n) {
    volatile uint8_t* v = static_cast<volatile uint8_t*>(p);
    while (n--) *v++ = 0;
}

#endif // SECURE_ZERO_HPP
