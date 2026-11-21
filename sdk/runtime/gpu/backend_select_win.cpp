// Windows: both native backends are in the executable; TCNYC_RENDERER=vulkan selects the ported Vulkan
// renderer, anything else the default D3D12 renderer.
#ifdef _WIN32
#include <cstdlib>
#include <cstring>
#include <memory>

#include "core/log.h"
#include "gpu/backend.h"

namespace gpu {

std::unique_ptr<Backend> CreateD3D12Backend();
std::unique_ptr<Backend> CreateVulkanBackend();

std::unique_ptr<Backend> CreateBackend() {
    const char* requested = std::getenv("TCNYC_RENDERER");
    if (requested && !std::strcmp(requested, "vulkan")) {
        LOGR("renderer: Vulkan (Windows; set TCNYC_RENDERER=d3d12 for D3D12)");
        return CreateVulkanBackend();
    }
    LOGR("renderer: D3D12 (set TCNYC_RENDERER=vulkan for the Vulkan renderer)");
    return CreateD3D12Backend();
}

}  // namespace gpu
#endif
