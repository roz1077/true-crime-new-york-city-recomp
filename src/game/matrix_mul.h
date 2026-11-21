#pragma once

#include <cstdint>

#include "cpu/x86_context.h"

namespace game {
// 0x24C6E0's body: dst = a * b for 4x4 floats, bit-exact with the guest sequence (see matrix_mul.cpp).
// The wrapper in city_stream.cpp calls this in place of the recompiled body.
void MatrixMul4x4(X86Context& ctx, uint8_t* base);
}  // namespace game
