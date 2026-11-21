// Windows: both native backends are in the executable; TCNYC_RENDERER=vulkan selects the ported Vulkan
// renderer, anything else the default D3D12 renderer.
#ifdef _WIN32
#include <cstdlib>
#include <cstring>
#include <memory>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"

namespace gpu {

std::unique_ptr<Backend> CreateD3D12Backend();
std::unique_ptr<Backend> CreateVulkanBackend();

std::unique_ptr<Backend> CreateBackend() {
    const char* requested = std::getenv("TCNYC_RENDERER");
    const bool vulkan = requested ? !std::strcmp(requested, "vulkan") : app::Get().renderer == 1;
    if (vulkan) {
        LOGR("renderer: Vulkan (Windows; choose Direct3D 12 in the launcher to switch back)");
        return CreateVulkanBackend();
    }
    LOGR("renderer: D3D12 (choose Vulkan in the launcher, or set TCNYC_RENDERER=vulkan)");
    return CreateD3D12Backend();
}

}  // namespace gpu
#endif
