// Applies the city-cell grid remainder fix to the generated recompiled sources.
// Used by the CMake generation step so every build produces the corrected code.
#include <cstdio>
#include <cstdlib>

#include "xbe/grid_patch.h"

int main(int argc, char** argv) {
    if (argc != 3) {
        std::fprintf(stderr, "usage: grid_patch <generated-dir> <partitions>\n");
        return 2;
    }
    const uint32_t partitions = static_cast<uint32_t>(std::strtoul(argv[2], nullptr, 10));
    const xbe::GridPatchResult result = xbe::PatchGridSources(argv[1], partitions);
    if (!result.ok()) {
        std::fprintf(stderr, "grid_patch: %s\n", result.error.c_str());
        return 1;
    }
    std::printf("grid_patch: %u file(s), %u site(s)\n", result.files, result.occurrences);
    return 0;
}
