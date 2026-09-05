// Conversion between the half-floats the network works in and the bytes an
// image file and a screen work in.
//
// Written out rather than pulled from a library because it is twenty lines and
// a dependency for twenty lines is a dependency to install, version and explain.

#pragma once

#include <cstdint>
#include <cstring>

namespace enhancer {

inline uint16_t float_to_half(float value) {
    uint32_t bits;
    std::memcpy(&bits, &value, 4);
    const uint32_t sign = (bits >> 16) & 0x8000u;
    int exponent = (int)((bits >> 23) & 0xFF) - 127 + 15;
    uint32_t mantissa = bits & 0x7FFFFFu;

    if (exponent <= 0) {
        // Too small for a normal half: either zero or subnormal.
        if (exponent < -10) return (uint16_t)sign;
        mantissa |= 0x800000u;
        const uint32_t shift = (uint32_t)(14 - exponent);
        return (uint16_t)(sign | (mantissa >> shift));
    }
    if (exponent >= 0x1F) return (uint16_t)(sign | 0x7C00u); // overflow to infinity
    return (uint16_t)(sign | ((uint32_t)exponent << 10) | (mantissa >> 13));
}

inline float half_to_float(uint16_t half) {
    const uint32_t sign = (uint32_t)(half & 0x8000u) << 16;
    const uint32_t exponent = (half >> 10) & 0x1Fu;
    const uint32_t mantissa = half & 0x3FFu;

    uint32_t bits;
    if (exponent == 0) {
        if (mantissa == 0) {
            bits = sign;
        } else {
            // Subnormal: normalise it by hand.
            uint32_t e = 0, m = mantissa;
            while (!(m & 0x400u)) { m <<= 1; ++e; }
            m &= 0x3FFu;
            bits = sign | ((127 - 15 - e + 1) << 23) | (m << 13);
        }
    } else if (exponent == 0x1F) {
        bits = sign | 0x7F800000u | (mantissa << 13);
    } else {
        bits = sign | ((exponent - 15 + 127) << 23) | (mantissa << 13);
    }
    float value;
    std::memcpy(&value, &bits, 4);
    return value;
}

} // namespace enhancer
