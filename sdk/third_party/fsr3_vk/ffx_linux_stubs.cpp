// Not part of the FidelityFX SDK: the Vulkan backend references the frame generation swapchain, which this build
// leaves out (upscaling only), so its one entry point the backend calls reports that it is unavailable.
#include <FidelityFX/host/backends/vk/ffx_vk.h>

FFX_API FfxErrorCode ffxSetFrameGenerationConfigToSwapchainVK(FfxFrameGenerationConfig const*) {
    return FFX_ERROR_INVALID_ARGUMENT;
}
