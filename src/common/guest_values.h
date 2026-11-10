// Small helpers for reading and writing guest values from the game hooks and the dev tools.
#pragma once

#include <cstdint>
#include <cstring>

#include "kernel/kernel.h"

namespace guest {

// a float at a guest address
inline float ReadF32(uint32_t va) {
    const uint32_t bits = kernel::R32(va);
    float f;
    std::memcpy(&f, &bits, 4);
    return f;
}

inline void WriteF32(uint32_t va, float f) {
    uint32_t bits;
    std::memcpy(&bits, &f, 4);
    kernel::W32(va, bits);
}

// CRC-32 (IEEE, reflected) of a name: the game keys script states, items, levels and damage types
// by it
constexpr uint32_t Crc32(const char *s) {
    uint32_t crc = 0xFFFFFFFFu;
    for (; *s; ++s) {
        crc ^= uint8_t(*s);
        for (int i = 0; i < 8; ++i) crc = (crc >> 1) ^ (0xEDB88320u & (0u - (crc & 1u)));
    }
    return ~crc;
}
static_assert(Crc32("DAMAGETYPE_IMPACT") == 0x2B2D2DAF, "the game's damage type hash");

} // namespace guest
