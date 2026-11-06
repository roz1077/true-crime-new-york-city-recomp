// Chooses the native renderer. Vulkan is the default; TCNYC_RENDERER=software selects the
// CPU reference path for debugging.
#include <cstdlib>
#include <cstring>
#include <memory>

#include "core/log.h"
#include "gpu/backend.h"

namespace gpu {

std::unique_ptr<Backend> CreateSoftwareBackend();
std::unique_ptr<Backend> CreateVulkanBackend();

std::unique_ptr<Backend> CreateBackend() {
    const char* requested = std::getenv("TCNYC_RENDERER");
    if (requested && !std::strcmp(requested, "software")) {
        LOGR("renderer: reference software renderer");
        return CreateSoftwareBackend();
    }
    LOGR("renderer: Vulkan (set TCNYC_RENDERER=software for the reference renderer)");
    return CreateVulkanBackend();
}

} // namespace gpu
