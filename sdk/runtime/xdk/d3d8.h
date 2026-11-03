#pragma once
#include <mutex>

#include "gpu/state.h"

namespace d3d {

gpu::State& State();
std::recursive_mutex& Lock();
// pictures the title has presented (D3DDevice_Swap calls), from any thread
uint32_t PresentCount();

}  // namespace d3d
