#pragma once
#include <cstdint>
#include <vector>

#include "core/input.h"
#include "gpu/texture.h"

namespace gpu {

// the linear font atlas that holds the game's inline button glyphs
bool IsPromptAtlas(const TextureDesc& d);

// redraw the atlas's button glyphs (decoded BGRA rows) in the given style;
// leaves any texture without the expected glyph layout untouched
void ReplaceControllerGlyphs(std::vector<uint8_t>& px, uint32_t pitch, input::PromptStyle style);

} // namespace gpu
