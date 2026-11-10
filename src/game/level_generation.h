#pragma once
// Counts world resets and level loads (scene_nodes.cpp hooks sub_00068300 and sub_000703D0), so hooks that
// keep per-object state across frames can drop it when the world they refer to is rebuilt.
#include <atomic>
#include <cstdint>

namespace game {
extern std::atomic<uint32_t> g_level_generation;
inline uint32_t LevelGeneration() { return g_level_generation.load(std::memory_order_relaxed); }
} // namespace game
