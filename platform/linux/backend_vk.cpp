// Native Linux renderer: Vulkan implementation of gpu::Backend. This is the
// default renderer; TCNYC_RENDERER=software keeps the CPU reference path for
// debugging and CI.
//
// Stage: real GPU draws - vertex/index/uniform upload, runtime HLSL -> SPIR-V with
// shaderc, the title's render targets as Vulkan images with guest-memory readback,
// decoded console textures, gamma-correct present and the ImGui Graphics overlay.
//
// Deliberate v1 limits:
//  - render scale 1 and no MSAA/SMAA/bloom/DOF (the shared settings still store them)
//  - points/lines use native topologies (no console pixel geometry shader)
//  - occlusion queries stay pending, as the headless backend leaves them
#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <mutex>
#include <source_location>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <backends/imgui_impl_sdl3.h>
#include <backends/imgui_impl_vulkan.h>
#include <imgui.h>
#include <shaderc/shaderc.h>
#include <unistd.h>
#include <vulkan/vulkan.h>

#include "app/app.h"
#include "common/graphics_menu.h"
#include "gpu/prompt_glyphs.h"
#include "core/log.h"
#include "gpu/ao.h"
#include "gpu/backend.h"
#include "gpu/post_fx.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"
#include "kernel/memory.h"
#include "reference_renderer.h"

// SMAA 1x: the reference shader and its lookup textures (third_party/smaa, MIT)
#include "../../sdk/third_party/smaa/AreaTex.h"
#include "../../sdk/third_party/smaa/SMAAShader.h"
#include "../../sdk/third_party/smaa/SearchTex.h"

#include "texturedumper_linux.h"

namespace app {
Mode WindowPixels();
}

namespace gpu {

namespace {

constexpr uint32_t kBaseWidth = 640;
constexpr uint32_t kBaseHeight = 480;
constexpr uint64_t kUploadSize = 64ull << 20;

std::vector<uint8_t> StagingBuffer(VkDevice dev, VkPhysicalDeviceMemoryProperties& props, VkDeviceSize size,
                                   VkBuffer& buffer, VkDeviceMemory& memory, void** map);

float Bits(uint32_t v) {
    float f;
    std::memcpy(&f, &v, 4);
    return f;
}
void Color(uint32_t argb, float* o) {
    o[0] = ((argb >> 16) & 255) / 255.0f;
    o[1] = ((argb >> 8) & 255) / 255.0f;
    o[2] = (argb & 255) / 255.0f;
    o[3] = (argb >> 24) / 255.0f;
}

VkCompareOp CompareOp(uint32_t v, bool reversed = false) {
    static const VkCompareOp ops[8] = {VK_COMPARE_OP_NEVER,      VK_COMPARE_OP_LESS,     VK_COMPARE_OP_EQUAL,
                                       VK_COMPARE_OP_LESS_OR_EQUAL, VK_COMPARE_OP_GREATER, VK_COMPARE_OP_NOT_EQUAL,
                                       VK_COMPARE_OP_GREATER_OR_EQUAL, VK_COMPARE_OP_ALWAYS};
    uint32_t n = v >= 0x200 && v <= 0x207 ? v - 0x200 : 7;
    if (reversed) n = uint32_t(7 - n);
    return ops[n];
}
VkBlendFactor BlendFactor(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return VK_BLEND_FACTOR_ZERO;
        case 1: return VK_BLEND_FACTOR_ONE;
        case 0x300: return alpha ? VK_BLEND_FACTOR_SRC_ALPHA : VK_BLEND_FACTOR_SRC_COLOR;
        case 0x301: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_SRC_COLOR;
        case 0x302: return VK_BLEND_FACTOR_SRC_ALPHA;
        case 0x303: return VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
        case 0x304: return VK_BLEND_FACTOR_DST_ALPHA;
        case 0x305: return VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA;
        case 0x306: return alpha ? VK_BLEND_FACTOR_DST_ALPHA : VK_BLEND_FACTOR_DST_COLOR;
        case 0x307: return alpha ? VK_BLEND_FACTOR_ONE_MINUS_DST_ALPHA : VK_BLEND_FACTOR_ONE_MINUS_DST_COLOR;
        case 0x308: return VK_BLEND_FACTOR_SRC_ALPHA_SATURATE;
        case 0x8001: return VK_BLEND_FACTOR_CONSTANT_COLOR;
        case 0x8002: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_COLOR;
        case 0x8003: return VK_BLEND_FACTOR_CONSTANT_ALPHA;
        case 0x8004: return VK_BLEND_FACTOR_ONE_MINUS_CONSTANT_ALPHA;
        default: return VK_BLEND_FACTOR_ONE;
    }
}
VkBlendOp BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return VK_BLEND_OP_SUBTRACT;
        case 0x800B: return VK_BLEND_OP_REVERSE_SUBTRACT;
        case 0x8007: return VK_BLEND_OP_MIN;
        case 0x8008: return VK_BLEND_OP_MAX;
        default: return VK_BLEND_OP_ADD;
    }
}
VkStencilOp StencilOp(uint32_t v) {
    switch (v) {
        case 0: return VK_STENCIL_OP_ZERO;
        case 0x1E01: return VK_STENCIL_OP_REPLACE;
        case 0x1E02: return VK_STENCIL_OP_INCREMENT_AND_CLAMP;
        case 0x1E03: return VK_STENCIL_OP_DECREMENT_AND_CLAMP;
        case 0x150A: return VK_STENCIL_OP_INVERT;
        case 0x8507: return VK_STENCIL_OP_INCREMENT_AND_WRAP;
        case 0x8508: return VK_STENCIL_OP_DECREMENT_AND_WRAP;
        default: return VK_STENCIL_OP_KEEP;
    }
}
VkSamplerAddressMode AddressMode(uint32_t v) {
    switch (v) {
        case 2: return VK_SAMPLER_ADDRESS_MODE_MIRRORED_REPEAT;
        case 3: case 5: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        case 4: return VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_BORDER;
        default: return VK_SAMPLER_ADDRESS_MODE_REPEAT;
    }
}

// TCNYC_PERF=1: per-frame CPU cost of drawing, the overlay and the present wait
struct ScopeTimer {
    bool on;
    std::chrono::steady_clock::time_point start;
    double* total;
    explicit ScopeTimer(bool enabled, double* out)
        : on(enabled), start(on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}), total(out) {}
    ~ScopeTimer() {
        if (on)
            *total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
    }
};

// one clock per segment: hand the current segment's total to the next one
struct LapTimer {
    bool on;
    std::chrono::steady_clock::time_point start;
    double* total;
    explicit LapTimer(bool enabled, double* out)
        : on(enabled), start(on ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{}), total(out) {}
    void Stop() {
        if (!on) return;
        *total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
        on = false;
    }
    void Lap(double* next) {
        if (!on) return;
        const auto now = std::chrono::steady_clock::now();
        *total += std::chrono::duration<double, std::milli>(now - start).count();
        total = next;
        start = now;
    }
};

constexpr uint32_t kCullCw = 0x900;
constexpr uint32_t kFillWireframe = 0x1B01;
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
constexpr uint32_t kZUseW = 2;
constexpr uint32_t kFogTableNone = 0;
constexpr float kFogTableLinear = 3.0f;
constexpr float kAlphaTestAlways = 7.0f;  // shader function index: 7 is always
constexpr uint32_t kOcclusionQueries = 16384;  // one per counted draw, a frame's worth

uint32_t CompareOpIndex(uint32_t v) { return v >= 0x200 && v <= 0x207 ? v - 0x200 : 7; }
// the console tests texel OP reference; SampleCmp tests reference OP texel
uint32_t ReverseCompare(uint32_t v) {
    static const uint8_t reverse[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    return 0x200 + reverse[CompareOpIndex(v)];
}

struct GuestExtent {
    uint32_t w = kBaseWidth, h = kBaseHeight;
};

// one console surface's GPU mirror; guest memory is the source of truth at Sync
struct Tex;

struct Target {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageView sample_view = VK_NULL_HANDLE;  // depth aspect only, for comparison sampling
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t data = 0, size = 0, guest_format = 0, header = 0;
    uint32_t gw = 0, gh = 0;  // console extent; w/h below are the host image extent
    float scale = 1.0f, stretch = 1.0f;
    uint32_t w = 0, h = 0, pitch = 0;
    uint32_t samples = 1;
    uint32_t fallback_samples = 0;  // a sample count whose allocation failed; retried on a setting/size change
    VkImage resolved_image = VK_NULL_HANDLE;
    VkDeviceMemory resolved_memory = VK_NULL_HANDLE;
    VkImageView resolved_view = VK_NULL_HANDLE;
    VkImageLayout resolved_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint8_t bytes = 4;
    bool depth = false, swizzled = false, linear = false;
    uint64_t epoch = 0;
    bool drawn = false;
    bool upload_pending = false;
    // guest pixels were copied into this target, and the frame a 3D draw last covered it
    bool cpu = false;
    uint64_t scene_frame = ~0ull;
    uint64_t last_use = 0;  // frame this target was last bound or sampled
    // a copy to sample while the target itself is bound (D3D12 Feedback)
    VkImage fb_image = VK_NULL_HANDLE;
    VkDeviceMemory fb_memory = VK_NULL_HANDLE;
    VkImageView fb_view = VK_NULL_HANDLE;
    VkImageLayout fb_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    Tex* fb_tex = nullptr;
};

struct Tex {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_B8G8R8A8_UNORM;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t w = 0, h = 0, levels = 1, layers = 1;
    bool cube = false;
    bool borrowed = false;  // points at a render target's image, not its own
    bool bc_fallback = false;  // decoded BC bytes uploaded as BGRA because the device has no BC
    uint32_t data = 0, guest_format = 0, depth = 1;
    uint64_t epoch = 0, palette = 0;
    uint64_t sig = 0, frame = 0;  // content signature and the frame it was checked on
    input::PromptStyle prompt_style = input::PromptStyle::Original;  // glyph set baked into the font atlas
    // texture pack replacement: the key whose image is being loaded, and the ready image
    uint64_t pack_key = 0;
    uint32_t pack_gen = 0;
    Tex* swap = nullptr;
};

// A renderer-owned image: the AO prepass depth/distance, the half/full AO factor and the
// full-size occlusion target. Unlike Target these are not guest surfaces.
struct AoImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0, samples = 1;
};

// One scene draw recorded into the CPU display list. Everything the replay needs is resolved at
// record time; replay only binds and draws. The depth pipeline is the ao = 3 variant used by the
// AO depth prepass.
struct DeferredDraw {
    VkPipeline pipe = VK_NULL_HANDLE;
    VkPipeline depth_pipe = VK_NULL_HANDLE;
    VkDescriptorSet set = VK_NULL_HANDLE;
    VkBuffer vbufs[16]{};
    VkDeviceSize voffs[16]{};
    uint32_t vbind[16]{};
    uint32_t vb_count = 0;
    VkBuffer ibuf = VK_NULL_HANDLE;
    VkDeviceSize ioff = 0;
    VkIndexType itype = VK_INDEX_TYPE_UINT16;
    uint32_t index_count = 0, vertex_count = 0, base_vertex = 0;
    VkViewport vp{};
    VkRect2D sc{};
    float blend[4]{};
    uint32_t stencil = 0;
    uint32_t zfunc = 0;
    bool receiver = false;  // the receiver pipeline reads t4
    int32_t plain = -1;     // receivers: index of the DeferredPlain to draw with when no AO set exists
    Target* rt = nullptr;
    Target* ds = nullptr;
};

// Pick the best sample count at or below what the user asked for that both attachments support.
static uint32_t PickSampleCount(uint32_t requested, uint32_t supported_mask) {
    for (uint32_t samples : {8u, 4u, 2u})
        if (requested >= samples && (supported_mask & samples)) return samples;
    return 1;
}

// the projection's near and far clip w, as D3DDevice_SetTransform finds them (mirrors d3d12.cpp)
struct WRange {
    float near_w;
    float far_w;
};

WRange ProjectionWRange(const float* m) {
    const float m33 = m[10];
    const float m34 = m[11];
    const float m43 = m[14];
    const float m44 = m[15];
    if (m33 == m34 || m33 == 0) return {0, 1};
    return {m44 - m43 / m33 * m34, (m44 - m43) / (m33 - m34) * m34 + m44};
}

constexpr uint32_t kMClipMin = 0x394;
constexpr float kFallbackNear = 1.0f / 65536;  // of the far w, when no range is known

// Fixed function clip w is the host's view depth over the projection's range; a program's is in
// buffer units up to zscale, its near end the clip minimum the title sent.
WRange DrawWRange(const State& s, bool program) {
    WRange r{0, s.zscale};
    if (!program) {
        r = ProjectionWRange(s.transforms[3].data());  // D3DTS_PROJECTION
    } else if (SimpleState(s, kMClipMin)) {
        r.near_w = Bits(SimpleState(s, kMClipMin));
    }
    if (r.near_w <= 0 || r.near_w >= r.far_w) r.near_w = r.far_w * kFallbackNear;
    return r;
}

// One single-sample SMAA image (edge/weight/output or a reference lookup).
struct SmaaImage {
    VkImage image = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    VkImageView view = VK_NULL_HANDLE;
    VkFramebuffer framebuffer = VK_NULL_HANDLE;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t w = 0, h = 0;
};

struct PsoKey {
    uint64_t vs = 0, ps = 0;
    uint32_t regs = 0;
    uint32_t elements[16] = {};
    uint32_t strides[16] = {};
    uint32_t topo = 0, strip = 0;
    uint32_t blend = 0, src = 0, dst = 0, op = 0, wmask = 0;
    uint32_t zen = 0, zwrite = 0, zfunc = 0;
    uint32_t sten = 0, sfunc = 0, smask = 0, swmask = 0, sfail = 0, szfail = 0, spass = 0;
    uint32_t cull = 0, ccw = 0, fill = 0;
    float zslope = 0.0f;  // solid polygon offset, as the D3D12 backend applies it
    uint32_t rtfmt = 0, dsfmt = 0;
    uint32_t samples = 1;
    uint32_t sprite = 0;  // a program's point list drawn as point sprites
    bool operator==(const PsoKey&) const = default;
};

struct PsoKeyHash {
    size_t operator()(const PsoKey& k) const { return size_t(Hash(&k, sizeof(k))); }
};

// what a deferred AO receiver needs to build its ordinary pipeline if no occlusion set can be bound
struct DeferredPlain {
    PsoKey key;
    PixelKey pk;  // with ao = 0
    VkShaderModule vs = VK_NULL_HANDLE;
};

// A target is identified by the surface's address and shape: the guest reuses one address for
// surfaces of different sizes (a scratch pool), and both must keep their own contents.
struct TargetKey {
    uint32_t data = 0, gw = 0, gh = 0, format = 0, pitch = 0;
    uint8_t depth = 0, linear = 0;
    bool operator==(const TargetKey&) const = default;
};

struct TargetKeyHash {
    size_t operator()(const TargetKey& k) const {
        uint64_t h = Hash(&k.data, sizeof(k.data));
        h = Hash(&k.gw, sizeof(k.gw), h);
        h = Hash(&k.gh, sizeof(k.gh), h);
        h = Hash(&k.format, sizeof(k.format), h);
        h = Hash(&k.pitch, sizeof(k.pitch), h);
        h = Hash(&k.depth, sizeof(k.depth), h);
        h = Hash(&k.linear, sizeof(k.linear), h);
        return size_t(h);
    }
};

// A borrowed view belongs to one live target. The guest reuses a data address for
// surfaces of different sizes, so an address+format key aliases a 128x128 reflection
// target with the 320x240 light buffer that shares its address.
struct BorrowKey {
    Target* target = nullptr;
    uint32_t format = 0;
    uint8_t depth = 0;
    bool operator==(const BorrowKey&) const = default;
};
struct BorrowKeyHash {
    size_t operator()(const BorrowKey& k) const {
        uint64_t h = Hash(&k.target, sizeof(k.target));
        h = Hash(&k.format, sizeof(k.format), h);
        h = Hash(&k.depth, sizeof(k.depth), h);
        return size_t(h);
    }
};

// the depth target the main scene was drawn with, for the distance blur at present
struct SceneDepth {
    Target* depth = nullptr;
    float near_w = 0.1f;
    float far_w = 1000.0f;
};

// texture cache key: a collision here shows the wrong texture, so keep the fields
// explicit instead of packing them into one XOR
struct TexKey {
    uint32_t data = 0, format = 0, width = 0, height = 0, depth = 0, levels = 0, pitch = 0;
    uint32_t palette_va = 0;
    uint64_t palette = 0;
    uint32_t flags = 0;  // cube bit 0, linear bit 1
    bool operator==(const TexKey&) const = default;
};

struct TexKeyHash {
    size_t operator()(const TexKey& k) const {
        uint64_t h = Hash(&k.data, sizeof(k.data));
        h = Hash(&k.format, sizeof(k.format), h);
        h = Hash(&k.width, sizeof(k.width), h);
        h = Hash(&k.height, sizeof(k.height), h);
        h = Hash(&k.depth, sizeof(k.depth), h);
        h = Hash(&k.levels, sizeof(k.levels), h);
        h = Hash(&k.pitch, sizeof(k.pitch), h);
        h = Hash(&k.palette_va, sizeof(k.palette_va), h);
        h = Hash(&k.palette, sizeof(k.palette), h);
        h = Hash(&k.flags, sizeof(k.flags), h);
        return size_t(h);
    }
};

struct SamplerKey {
    uint32_t mag = 0, min = 0, mip = 0, addr_u = 0, addr_v = 0, addr_w = 0;
    uint32_t maxmip = 0, aniso = 0, shadow = 0, clamp = 0, lodbias = 0;
    uint32_t border = 0, shadow_func = 0;
    bool operator==(const SamplerKey&) const = default;
};

struct SamplerKeyHash {
    size_t operator()(const SamplerKey& k) const {
        uint64_t h = Hash(&k.mag, sizeof(k.mag));
        h = Hash(&k.min, sizeof(k.min), h);
        h = Hash(&k.mip, sizeof(k.mip), h);
        h = Hash(&k.addr_u, sizeof(k.addr_u), h);
        h = Hash(&k.addr_v, sizeof(k.addr_v), h);
        h = Hash(&k.addr_w, sizeof(k.addr_w), h);
        h = Hash(&k.maxmip, sizeof(k.maxmip), h);
        h = Hash(&k.aniso, sizeof(k.aniso), h);
        h = Hash(&k.shadow, sizeof(k.shadow), h);
        h = Hash(&k.clamp, sizeof(k.clamp), h);
        h = Hash(&k.lodbias, sizeof(k.lodbias), h);
        h = Hash(&k.border, sizeof(k.border), h);
        h = Hash(&k.shadow_func, sizeof(k.shadow_func), h);
        return size_t(h);
    }
};

uint32_t SwizzleOffset(uint32_t x, uint32_t y, uint32_t w, uint32_t h) {
    uint32_t xb = 0, yb = 0;
    while ((1u << xb) < w) xb++;
    while ((1u << yb) < h) yb++;
    uint32_t out = 0, at = 0;
    for (uint32_t i = 0; i < xb || i < yb; i++) {
        if (i < xb) out |= ((x >> i) & 1) << at++;
        if (i < yb) out |= ((y >> i) & 1) << at++;
    }
    return out;
}

VkFormat HostFormatVk(Host h) {
    switch (h) {
        case Host::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case Host::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
        case Host::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
        case Host::R32F: return VK_FORMAT_R32_SFLOAT;
        case Host::R16Unorm: return VK_FORMAT_R16_UNORM;
        default: return VK_FORMAT_B8G8R8A8_UNORM;
    }
}

// BC1/2/3 to BGRA8 for devices without textureCompressionBC. The pack and guest paths
// share this, so a dumped texture can also be replaced on such a device.
void Expand565(uint16_t c, uint8_t& r, uint8_t& g, uint8_t& b) {
    const uint32_t r5 = (c >> 11) & 31, g6 = (c >> 5) & 63, b5 = c & 31;
    r = uint8_t((r5 << 3) | (r5 >> 2));
    g = uint8_t((g6 << 2) | (g6 >> 4));
    b = uint8_t((b5 << 3) | (b5 >> 2));
}

void DecodeColourBlock(const uint8_t* src, bool bc1, uint8_t px[16][4]) {
    const uint16_t c0 = uint16_t(src[0] | src[1] << 8);
    const uint16_t c1 = uint16_t(src[2] | src[3] << 8);
    uint8_t r[4], g[4], b[4], a[4] = {255, 255, 255, 255};
    Expand565(c0, r[0], g[0], b[0]);
    Expand565(c1, r[1], g[1], b[1]);
    if (!bc1 || c0 > c1) {
        r[2] = uint8_t((2 * r[0] + r[1]) / 3);
        g[2] = uint8_t((2 * g[0] + g[1]) / 3);
        b[2] = uint8_t((2 * b[0] + b[1]) / 3);
        r[3] = uint8_t((r[0] + 2 * r[1]) / 3);
        g[3] = uint8_t((g[0] + 2 * g[1]) / 3);
        b[3] = uint8_t((b[0] + 2 * b[1]) / 3);
    } else {
        r[2] = uint8_t((r[0] + r[1]) / 2);
        g[2] = uint8_t((g[0] + g[1]) / 2);
        b[2] = uint8_t((b[0] + b[1]) / 2);
        a[3] = 0;
    }
    for (int i = 0; i < 16; i++) {
        const int index = (src[4 + i / 4] >> ((i % 4) * 2)) & 3;
        px[i][0] = r[index];
        px[i][1] = g[index];
        px[i][2] = b[index];
        px[i][3] = a[index];
    }
}

bool DecodeBcLevel(const uint8_t* src, uint32_t w, uint32_t h, Host host, std::vector<uint8_t>& out,
                   uint32_t& pitch) {
    if (!src || !w || !h || (host != Host::BC1 && host != Host::BC2 && host != Host::BC3)) return false;
    pitch = w * 4;
    out.assign(size_t(pitch) * h, 0);
    const uint32_t blocks_x = (w + 3) / 4, blocks_y = (h + 3) / 4;
    const bool bc1 = host == Host::BC1;
    const uint32_t stride = bc1 ? 8 : 16;
    for (uint32_t by = 0; by < blocks_y; by++)
        for (uint32_t bx = 0; bx < blocks_x; bx++) {
            const uint8_t* block = src + (size_t(by) * blocks_x + bx) * stride;
            uint8_t px[16][4];
            if (bc1) {
                DecodeColourBlock(block, true, px);
            } else {
                uint8_t alpha[16];
                if (host == Host::BC2) {
                    for (int i = 0; i < 16; i++) alpha[i] = uint8_t((block[i / 2] >> ((i & 1) * 4)) & 0xF) * 17;
                } else {
                    uint8_t pal[8] = {block[0], block[1], 0, 0, 0, 0, 0, 255};
                    if (pal[0] > pal[1]) {
                        for (int i = 1; i < 7; i++)
                            pal[i + 1] = uint8_t(((7 - i) * pal[0] + i * pal[1]) / 7);
                    } else {
                        for (int i = 1; i < 5; i++)
                            pal[i + 1] = uint8_t(((5 - i) * pal[0] + i * pal[1]) / 5);
                        pal[6] = 0;
                        pal[7] = 255;
                    }
                    uint64_t bits = 0;
                    for (int i = 0; i < 6; i++) bits |= uint64_t(block[2 + i]) << (8 * i);
                    for (int i = 0; i < 16; i++) alpha[i] = pal[(bits >> (3 * i)) & 7];
                }
                DecodeColourBlock(block + 8, false, px);
                for (int i = 0; i < 16; i++) px[i][3] = alpha[i];
            }
            for (int i = 0; i < 16; i++) {
                const uint32_t x = bx * 4 + (i & 3), y = by * 4 + (i >> 2);
                if (x >= w || y >= h) continue;
                std::memcpy(&out[(size_t(y) * w + x) * 4], px[i], 4);
            }
        }
    return true;
}

VkFormat ElementFormat(uint32_t dxgi) {
    switch (dxgi) {
        case 2: return VK_FORMAT_R32G32B32A32_SFLOAT;
        case 6: return VK_FORMAT_R32G32B32_SFLOAT;
        case 13: return VK_FORMAT_R16G16B16A16_SNORM;
        case 14: return VK_FORMAT_R16G16B16A16_SINT;
        case 16: return VK_FORMAT_R32G32_SFLOAT;
        case 28: return VK_FORMAT_R8G8B8A8_UNORM;
        case 37: return VK_FORMAT_R16G16_SNORM;
        case 38: return VK_FORMAT_R16G16_SINT;
        case 41: return VK_FORMAT_R32_SFLOAT;
        case 42: return VK_FORMAT_R32_UINT;
        case 49: return VK_FORMAT_R8G8_UNORM;
        case 58: return VK_FORMAT_R16_SNORM;
        case 59: return VK_FORMAT_R16_SINT;
        case 61: return VK_FORMAT_R8_UNORM;
        case 87: return VK_FORMAT_B8G8R8A8_UNORM;
        default:
            LOG_FIRST(32, "vulkan: unknown vertex element format %u", dxgi);
            return VK_FORMAT_R32G32B32A32_SFLOAT;
    }
}

// bytes one vertex element fetches, matching ElementFormat's fallback
uint32_t ElementBytes(uint32_t dxgi) {
    switch (dxgi) {
        case 2: return 16;
        case 6: return 12;
        case 13: case 14: case 16: return 8;
        case 28: case 37: case 38: case 41: case 42: case 87: return 4;
        case 49: case 58: case 59: return 2;
        case 61: return 1;
        default: return 16;
    }
}

} // namespace

class VulkanBackend final : public Backend {
public:
    ~VulkanBackend() override { Shutdown(); }
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t rects, uint32_t points) override;
    void Present(const Surface& front) override;
    void ClearReport() override { counted_.clear(); }
    void GetReport(uint32_t report) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override;
    void SetGamma(const uint8_t* ramp) override {
        if (!ramp) return;
        std::memcpy(gamma_ramp_, ramp, sizeof(gamma_ramp_));
        gamma_dirty_ = true;
    }

private:
    // device and swapchain
    bool CreateInstance();
    bool CreateSurface();
    bool PickDevice();
    bool CreateDevice();
    bool CreateSwapchain();
    bool SwapchainFailTest();
    bool CreateCommands();
    bool CreateUpload();
    bool CreateUploadBuffer(uint64_t size, VkBuffer& buffer, VkDeviceMemory& memory, uint8_t*& ptr);
    bool CreatePresent();
    bool CreateUi();
    void DestroyUi();
    bool CreateUiTarget();
    void DestroyUiTarget();
    bool CreateUiComposite();
    void RenderUi();
    void Shutdown();
    bool EnsureFrameImage(uint32_t w, uint32_t h);
    bool EnsureTraceBuffer(VkDeviceSize size);
    void ResetFrame();

    // recording
    void BeginRecord();
    void EndRecord(bool wait);
    void Fail(const char* what, VkResult r);
    void Transition(VkImage image, VkImageLayout& layout, VkImageLayout want, VkFormat format, uint32_t levels = 1,
                    uint32_t layers = 1);
    VkRenderPass RenderPassFor(VkFormat color, VkFormat depth);
    VkRenderPass RenderPassForMs(VkFormat color, VkFormat depth, uint32_t samples);
    VkFramebuffer FramebufferFor(Target* rt, Target* ds);
    void BindTargets(const State& s, Target*& rt, Target*& ds);
    void BeginScenePass(Target* rt, Target* ds);
    void FlushPass();
    VkPipeline ClearPipeline(VkFormat format, VkFormat depth, uint32_t mask, uint32_t samples);

    // deferred scene display list and receiver AO
    bool DeferredPending() const { return !deferred_.empty(); }
    // the pending scene draws render into t: work that reads or rewrites t must replay them first
    bool DeferredWrites(const Target* t) const {
        return t && DeferredPending() &&
               (t == defer_rt_ || t == defer_ds_ || deferred_write_targets_.count(const_cast<Target*>(t)));
    }
    // where is the caller, counted by line with TCNYC_PERF so the frequent flush reasons show
    void FlushDeferredScene(std::source_location where = std::source_location::current());
    void ReplayDeferred(const std::vector<DeferredDraw>& draws, const std::vector<DeferredPlain>& plain,
                        VkDescriptorSet occlusion);
    VkPipeline PlainPipeline(DeferredPlain p);
    void ResetDeferred(uint32_t ao_wanted);  // ao_wanted: the AO setting for the next frame
    bool CreateAoImage(AoImage& image, uint32_t w, uint32_t h, VkFormat format, uint32_t samples,
                       VkImageUsageFlags usage);
    void DestroyAoImage(AoImage& image);
    bool EnsureAoDepth(uint32_t w, uint32_t h, uint32_t samples);
    bool EnsureAoHalf(uint32_t w, uint32_t h);
    bool EnsureAoOcc(uint32_t w, uint32_t h);
    bool EnsureAoZero(uint32_t w, uint32_t h);
    bool EnsureAoPipelines(bool depth_ms);
    VkRenderPass AoPrepassPass(uint32_t samples);
    VkRenderPass AoColorPass();
    VkFramebuffer AoFramebuffer(VkRenderPass pass, AoImage* color, AoImage* depth);
    VkPipeline CreateAoPipeline(VkShaderModule vs, VkShaderModule ps, VkRenderPass pass);
    VkDescriptorSet AcquireAoSet(VkImageView ao, VkImageView depth, bool occlusion_set);
    VkDescriptorSet AcquireAoOcclusionSet(VkImageView view);
    void DumpAoProbe(AoImage& image);

    // SMAA 1x on the finished scene before present
    bool EnsureSmaa(uint32_t w, uint32_t h, VkFormat format);
    void DestroySmaaTargets();
    bool CreateSmaaImage(SmaaImage& image, uint32_t w, uint32_t h, VkFormat format, bool framebuffer);
    void DrawSmaaPass(uint32_t pass, VkDescriptorSet set, SmaaImage& dst, uint32_t w, uint32_t h);

    // upload slices: keep the buffer with the offset, because the ring can grow between allocations
    struct UploadAlloc {
        VkBuffer buffer = VK_NULL_HANDLE;
        uint64_t offset = 0;
    };
    UploadAlloc AllocUpload(uint64_t size, uint64_t align, void** cpu);
    bool Slice(VkBufferUsageFlags usage, const void* data, uint64_t size, VkBuffer& buffer, VkDeviceSize& offset);

    // shaders and pipelines
    VkShaderModule CompileShader(const std::string& src, shaderc_shader_kind kind, const char* entry);
    VkShaderModule GeometryShaderFor(bool points, bool sprite);
    VkShaderModule VertexShaderFor(const State& s, const Assembled& a, bool prog, bool sprite, uint64_t& key);
    VkShaderModule PixelShaderFor(const PixelKey& pk, uint64_t& key);
    bool CreatePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps, VkPipeline& out,
                        VkRenderPass pass_override = VK_NULL_HANDLE);

    // targets and textures
    Target* EnsureTarget(const Surface& s, bool depth);
    Target* TargetAt(uint32_t data, bool depth, bool newest);
    bool ReadTarget(Target* t);
    void DumpTexture(const Tex* t);
    bool UploadTarget(Target* t);
    bool CreateTargetImages(Target& t);
    void DestroyTargetImages(Target& t);
    bool TargetAllocFailTest();
    bool RebuildTargetSingleSample(Target* t);
    bool CreateTargetImage(Target* t);
    void DestroyTargets();
    // a surface reused at another size/format: move its GPU objects to the retire lists instead of
    // leaking them (the guest reuses scratch surfaces between sizes every frame)
    void RetireTargetResources(Target* t);
    struct RetiredGpu {
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkImage resolved_image = VK_NULL_HANDLE;
        VkDeviceMemory resolved_memory = VK_NULL_HANDLE;
        VkImageView resolved_view = VK_NULL_HANDLE;
        VkImageView sample_view = VK_NULL_HANDLE;
        VkImage fb_image = VK_NULL_HANDLE;
        VkDeviceMemory fb_memory = VK_NULL_HANDLE;
        VkImageView fb_view = VK_NULL_HANDLE;
        Tex* fb_tex = nullptr;
    };
    std::vector<RetiredGpu> retired_gpu_;
    std::vector<VkFramebuffer> retired_framebuffers_;
    std::vector<Tex*> retired_borrowed_;
    Tex* EnsureTexture(const State& s, int stage, bool* is_target = nullptr);
    Tex* FeedbackTexture(const State& s, Target* t, const TextureDesc& d);
    void UploadTexture(Tex* t, const TextureDesc& d, const uint32_t* palette, bool depth_tex);
    bool MakeTexImage(Tex& t);
    // drops textures idle for a while (or, forced, everything not used this frame) when device
    // memory is tight; the Windows backend's ReleaseOldTextures equivalent
    size_t ReleaseOldTextures(bool force = false);
    // texture packs: the bytes' key, the loaded image, and the shared GPU copies
    void LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint32_t* colours);
    void LatchPackImage(Tex& tx);
    bool CreatePackTexture(Tex& r, texturedumper::Image& img);
    void ReleasePackTextures();
    VkSampler EnsureSampler(const State& s, int stage, const TexInfo& info, bool clamp, uint32_t levels);
    void WriteGuestPixels(Target* t, const uint8_t* src, uint32_t src_w, uint32_t src_h, uint32_t pitch);

    // constants
    void FillConstants(const State& s, const PixelKey& pk, const std::array<TexInfo, 4>& info, bool prog,
                       bool fixed_transform, const GuestExtent& size, DrawConstants& c);
    float UiSqueeze(const State& s, const Assembled& a, const std::array<TexInfo, 4>& info,
                    const std::array<Tex*, 4>& tex, const Target* rt) const;

    uint32_t width_ = 0, height_ = 0;
    float scale_ = 1.0f, stretch_ = 1.0f;
    uint32_t max_image_side_ = 16384;
    uint32_t msaa_ = 1;
    uint32_t color_samples_ = 1, depth_samples_ = 1;
    VkSampleCountFlags ao_color_samples_ = VK_SAMPLE_COUNT_1_BIT;
    uint32_t instance_version_ = VK_API_VERSION_1_0;
    bool depth_resolve_ = false;
    VkResolveModeFlagBits depth_resolve_mode_ = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    PFN_vkCreateRenderPass2 create_render_pass2_ = nullptr;
    bool api12_ = false;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
    // Pipelines kept on disk between runs ($XDG_CACHE_HOME/TrueCrimeNYC/pipelines.bin), so a pipeline the game
    // needed before does not stall a frame the first time again. Every pipeline is made through it.
    VkPipelineCache pipeline_cache_ = VK_NULL_HANDLE;
    uint32_t pipeline_cache_new_ = 0;          // pipelines made since the last save
    uint64_t pipeline_cache_quiet_since_ = 0;  // frames_ at the last one
    void OpenPipelineCache();
    void SavePipelineCache(bool force);
    VkResult CreatePipelines(uint32_t count, const VkGraphicsPipelineCreateInfo* info,
                             const VkAllocationCallbacks* alloc, VkPipeline* out);
    VkQueue queue_ = VK_NULL_HANDLE, present_queue_ = VK_NULL_HANDLE;
    uint32_t graphics_family_ = ~0u, present_family_ = ~0u;
    std::string device_name_;
    VkPhysicalDeviceMemoryProperties mem_props_{};
    bool bc_supported_ = false;
    bool fill_non_solid_ = false;
    bool geometry_ = false;  // geometry shaders expand console points and lines
    bool border_custom_ = false;  // VK_EXT_custom_border_color can use the guest's border colour
    bool sampler_anisotropy_ = false;
    float max_anisotropy_ = 1.0f;
    uint32_t aniso_setting_ = 0;  // latched once a frame: this is sampled per texture stage
    float aspect_ = 16.0f / 9.0f; // likewise sampled per draw by UiSqueeze

    VkSwapchainKHR swapchain_ = VK_NULL_HANDLE;
    std::vector<VkImage> swapchain_images_;
    VkExtent2D extent_{};
    VkFormat swapchain_format_ = VK_FORMAT_B8G8R8A8_UNORM;
    bool vsync_ = true;
    // HDR chosen at swapchain creation, the setting it was built for, and display detection.
    HdrOutput hdr_output_ = HdrOutput::Off;
    bool hdr_setting_ = false;
    uint32_t display_peak_nits_ = 1000;
    bool swapchain_colorspace_ = false;  // VK_EXT_swapchain_colorspace is enabled
    bool hdr_metadata_ = false;           // VK_EXT_hdr_metadata is enabled
    PFN_vkSetHdrMetadataEXT set_hdr_metadata_ = nullptr;

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkSemaphore image_available_ = VK_NULL_HANDLE;
    std::vector<VkSemaphore> render_done_;  // one per swapchain image: a present may still own the previous one
    VkFormat depth_format_ = VK_FORMAT_D24_UNORM_S8_UINT;
    bool depth_has_stencil_ = true;
    // the aspects of depth_format_: a device without a depth/stencil format has no stencil to view
    VkImageAspectFlags DepthAspect() const {
        return VK_IMAGE_ASPECT_DEPTH_BIT | (depth_has_stencil_ ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
    }
    VkFence fence_ = VK_NULL_HANDLE;
    bool failed_ = false;  // a submit or acquire failure stops presenting instead of waiting on a dead fence
    bool recording_ = false;
    // dynamic state caching: these persist across render passes in one command buffer
    VkPipeline bound_pipe_ = VK_NULL_HANDLE;
    bool viewport_valid_ = false;
    VkViewport bound_vp_{};
    VkRect2D bound_sc_{};
    
    VkBuffer upload_ = VK_NULL_HANDLE;
    VkDeviceMemory upload_memory_ = VK_NULL_HANDLE;
    uint8_t* upload_ptr_ = nullptr;
    uint64_t upload_size_ = 0, upload_used_ = 0;
    // an oversized frame grows the ring; the previous buffer stays alive until the frame's GPU work is done
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> upload_retired_;
    // staging buffers for uploads recorded into the current command buffer; freed after its fence
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> staging_pending_;

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> draw_sets_;
    size_t draw_set_next_ = 0;

    // receiver AO: set 1 is the occlusion texture; the full-screen AO passes have their own
    // layout (images at 0/1, samplers at 2/3) and a 32 byte push constant block.
    VkDescriptorSetLayout ao_occ_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool ao_occ_pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> ao_occ_sets_;
    size_t ao_occ_set_next_ = 0;
    VkDescriptorSetLayout ao_set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout ao_pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool ao_pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> ao_sets_;
    bool ao_pools_tried_ = false;
    bool EnsureAoPools();
    size_t ao_set_next_ = 0;

    // deferred scene display list
    bool defer_forced_ = false;
    bool defer_scene_ = false;
    // STORE multisample attachments for the whole frame while the deferred display list is on (AO,
    // TCNYC_DEFER_SCENE): its replay closes and reopens the scene pass. Only the render passes are
    // keyed by it. Store ops do not affect render pass compatibility, so pipelines and framebuffers
    // made for either variant work with both, and toggling AO does not rebuild them.
    bool msaa_store_ = false;
    bool defer_abandoned_ = false;
    bool defer_ao_applied_ = false;
    bool flushing_deferred_ = false;
    bool skip_deferred_flush_ = false;
    bool ao_depth_clear_ = true;
    bool ao_probed_ = false;
    bool ao_dist_probed_ = false;
    bool ao_failed_ = false;  // no occlusion image could be bound: AO is paused (see ResetFrame)
    uint32_t ao_failed_setting_ = 0;
    bool ao_fail_test_ = false;
    uint32_t ao_failed_frames_ = 0;
    static constexpr uint32_t kAoRetryFrames = 600;  // about ten seconds
    // occlusion sets written this frame, by view. Every replay binds one of at most two views (the
    // occlusion target or the zero image), so a frame needs two sets however often it flushes. The
    // cache is dropped with the frame and whenever an AO image (and so a view handle) is destroyed.
    std::pair<VkImageView, VkDescriptorSet> ao_occ_cached_[2]{};
    uint32_t ao_mode_ = 0;  // app::Snapshot().ao latched for the frame
    Target* defer_rt_ = nullptr;
    Target* defer_ds_ = nullptr;
    std::vector<DeferredDraw> deferred_;
    std::vector<DeferredPlain> deferred_plain_;  // fallback pipelines for the receivers in deferred_
    std::unordered_set<Target*> deferred_read_targets_, deferred_write_targets_;
    std::unordered_set<VkImage> deferred_read_images_;

    // AO targets: prepass depth and view distance, the half/full factor, full-size occlusion,
    // and a full-size zero occlusion image for flushes that do not run AO.
    AoImage ao_depth_, ao_dist_, ao_half_, ao_occ_, ao_zero_;
    std::unordered_map<uint64_t, VkRenderPass> ao_passes_;
    VkRenderPass ao_color_pass_ = VK_NULL_HANDLE;
    VkFramebuffer ao_prepass_fb_ = VK_NULL_HANDLE;
    VkFramebuffer ao_half_fb_ = VK_NULL_HANDLE;
    VkFramebuffer ao_occ_fb_ = VK_NULL_HANDLE;
    VkPipeline ao_gtao_pipe_ = VK_NULL_HANDLE;
    VkPipeline ao_occ_pipe_ = VK_NULL_HANDLE;
    std::vector<VkPipeline> retired_ao_pipelines_;
    VkShaderModule ao_gtao_vs_ = VK_NULL_HANDLE, ao_gtao_ps_ = VK_NULL_HANDLE;
    VkShaderModule ao_occ_vs_ = VK_NULL_HANDLE, ao_occ_ps_ = VK_NULL_HANDLE;
    bool ao_pipe_depth_ms_ = false;
    uint32_t ao_pipe_mode_ = 0;

    std::unordered_map<uint64_t, VkShaderModule> vs_cache_, ps_cache_;
    VkShaderModule geo_point_ = VK_NULL_HANDLE, geo_line_ = VK_NULL_HANDLE, geo_sprite_ = VK_NULL_HANDLE;
    std::unordered_map<PsoKey, VkPipeline, PsoKeyHash> pipelines_;
    std::unordered_map<uint64_t, VkRenderPass> passes_;
    std::unordered_map<uint64_t, VkFramebuffer> framebuffers_;
    std::unordered_map<uint64_t, std::pair<Target*, Target*>> framebuffer_targets_;
    std::unordered_map<TargetKey, Target*, TargetKeyHash> targets_;
    std::unordered_map<Target*, SceneDepth> scene_depth_;
    bool scene_dof_ = false;

    // NOOVERWRITE locks: a small ring of finished copies, delivered without waiting for the GPU
    struct LateSlot {
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        void* map = nullptr;
        uint32_t w = 0, h = 0, pitch = 0;
        uint64_t serial = 0;    // the draw submission that will carry the copy
        uint64_t presents = 0;  // presents_seen_ when recorded
        bool used = false;
    };
    struct LateTarget {
        LateSlot slot[3];
        uint32_t next = 0;
        uint32_t data = 0, size = 0, gw = 0, gh = 0, pitch = 0;
        bool swizzled = false;
        uint8_t bytes = 4;
        uint64_t delivered = 0;
    };
    std::unordered_map<uint32_t, LateTarget> late_;
    std::mutex late_lock_;
    std::vector<std::pair<VkBuffer, VkDeviceMemory>> late_retired_;
    std::atomic<uint64_t> completed_serial_{0};
    std::atomic<uint64_t> presents_seen_{0};
    uint64_t submit_serial_ = 0;
    std::unordered_map<TexKey, Tex*, TexKeyHash> textures_;
    std::unordered_map<BorrowKey, Tex*, BorrowKeyHash> borrowed_;
    std::unordered_map<uint64_t, Tex*> pack_textures_;  // pack images by key, kept for the run
    std::vector<Tex*> pack_textures_retired_;           // released after the frame's GPU work
    std::unordered_map<SamplerKey, VkSampler, SamplerKeyHash> samplers_;
    VkImage white_image_ = VK_NULL_HANDLE;
    VkDeviceMemory white_memory_ = VK_NULL_HANDLE;
    VkImageView white_view_ = VK_NULL_HANDLE;
    VkSampler white_sampler_ = VK_NULL_HANDLE;

    VkShaderModule clear_vs_ = VK_NULL_HANDLE, clear_ps_ = VK_NULL_HANDLE;
    std::unordered_map<uint64_t, VkPipeline> clear_pipelines_;

    // SMAA 1x: lookups plus the edge/weight/output images for the current frame size
    SmaaImage smaa_edges_, smaa_weights_, smaa_output_, smaa_area_, smaa_search_;
    VkSampler smaa_linear_ = VK_NULL_HANDLE, smaa_point_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout smaa_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool smaa_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet smaa_sets_[3] = {};
    VkPipelineLayout smaa_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline smaa_pipelines_[3] = {};
    VkShaderModule smaa_vs_[3] = {}, smaa_ps_[3] = {};
    uint32_t smaa_w_ = 0, smaa_h_ = 0;
    VkFormat smaa_format_ = VK_FORMAT_UNDEFINED;
    VkFormat smaa_pipeline_format_ = VK_FORMAT_UNDEFINED;
    uint64_t smaa_area_off_ = 0, smaa_search_off_ = 0;
    VkBuffer smaa_area_buffer_ = VK_NULL_HANDLE, smaa_search_buffer_ = VK_NULL_HANDLE;
    bool smaa_lookups_pending_ = false;
    bool smaa_failed_ = false;
    // the shared layout of the full-screen passes on the finished frame (SMAA, bloom; smaa_set_layout_ and
    // smaa_pipeline_layout_ are it)
    bool EnsurePostLayout();
    VkDescriptorPool CreatePostPool(uint32_t count);
    bool AllocatePostSets(VkDescriptorPool pool, uint32_t count, VkDescriptorSet* out);
    void WritePostSet(VkDescriptorSet set, VkImageView t0, VkImageView t1, VkImageView t2);
    VkPipeline MakePostPipeline(VkShaderModule vs, VkShaderModule ps, const char* entry, VkFormat format);
    void DrawPostPass(VkPipeline pipeline, VkDescriptorSet set, SmaaImage& dst, const float p[8]);
    // the bloom chain (post_fx.h): down[0] is half the source size, each next level half again; up[i] is level i
    // finished, with every smaller level folded in
    static constexpr VkFormat kBloomFormat = VK_FORMAT_R16G16B16A16_SFLOAT;
    static constexpr uint32_t kBloomSets = 2 * kBloomLevels - 1;
    struct BloomChain {
        SmaaImage down[kBloomLevels], up[kBloomLevels - 1];
        uint32_t src_w = 0, src_h = 0;
        VkShaderModule vs = VK_NULL_HANDLE, ps[3] = {};
        VkPipeline pipelines[3] = {};  // bright, down, up
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet sets[kBloomSets] = {};
        bool failed = false;
    } bloom_;
    bool EnsureBloom(uint32_t src_w, uint32_t src_h);
    void DestroyBloomTargets();
    VkImageView BuildBloom(VkImageView src, uint32_t w, uint32_t h);
    // FSR 1 (post_fx.h): easu is output sized, rcas whichever size it sharpened; both in the scene's format
    struct FsrState {
        SmaaImage easu, rcas;
        VkShaderModule vs = VK_NULL_HANDLE, ps[2] = {};
        VkPipeline pipelines[2] = {};  // easu, rcas
        VkFormat pipeline_format = VK_FORMAT_UNDEFINED;
        VkDescriptorPool pool = VK_NULL_HANDLE;
        VkDescriptorSet sets[2] = {};
        bool failed = false;
    } fsr_;
    void DestroyFsrImage(SmaaImage& image);
    // EASU to out_w x out_h when the scene is smaller and the Upscaler setting is FSR, then RCAS; the view to
    // present, src when neither applies
    VkImageView UpscaleAndSharpen(VkImageView src, VkFormat format, uint32_t w, uint32_t h, uint32_t out_w,
                                  uint32_t out_h, const app::Settings& s);
    bool dof_active_ = false;
    bool bloom_active_ = false;

    // the open pass
    Target* pass_rt_ = nullptr;
    Target* pass_ds_ = nullptr;
    VkRenderPass pass_ = VK_NULL_HANDLE;
    VkFramebuffer pass_fb_ = VK_NULL_HANDLE;

    // the frame's front buffer, for present and read-back
    Target* front_ = nullptr;
    std::vector<uint8_t> frame_;
    VkImage frame_image_ = VK_NULL_HANDLE;
    VkDeviceMemory frame_memory_ = VK_NULL_HANDLE;
    VkImageView frame_view_ = VK_NULL_HANDLE;
    VkImageLayout frame_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t frame_w_ = 0, frame_h_ = 0;

    // present: fullscreen pass sampling the frame and the title's gamma ramp
    VkRenderPass swap_pass_ = VK_NULL_HANDLE;
    std::vector<VkImageView> swap_views_;
    std::vector<VkFramebuffer> swap_framebuffers_;
    VkImage gamma_image_ = VK_NULL_HANDLE;
    VkDeviceMemory gamma_memory_ = VK_NULL_HANDLE;
    VkImageView gamma_view_ = VK_NULL_HANDLE;
    VkImageLayout gamma_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    VkSampler present_sampler_ = VK_NULL_HANDLE;
    VkSampler present_point_sampler_ = VK_NULL_HANDLE;
    VkDescriptorSetLayout present_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool present_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet present_set_ = VK_NULL_HANDLE;
    VkPipelineLayout present_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline present_pipeline_ = VK_NULL_HANDLE;
    VkFormat present_format_ = VK_FORMAT_UNDEFINED;
    VkShaderModule present_vs_ = VK_NULL_HANDLE, present_ps_ = VK_NULL_HANDLE;
    uint8_t gamma_ramp_[3 * 256] = {};
    bool gamma_dirty_ = true;

    // Overlays: ImGui renders into a BGRA8 image (its pipeline is built for that pass), then a
    // composite pass puts it over the swapchain, unchanged in SDR and at paper white in HDR. Both
    // are skipped while nothing is on screen.
    VkRenderPass ui_pass_ = VK_NULL_HANDLE;
    VkImage ui_image_ = VK_NULL_HANDLE;
    VkDeviceMemory ui_memory_ = VK_NULL_HANDLE;
    VkImageView ui_view_ = VK_NULL_HANDLE;
    VkFramebuffer ui_fb_ = VK_NULL_HANDLE;
    bool ui_direct_ = false;  // ImGui was set up for swap_pass_ (SDR) rather than ui_pass_ (HDR)
    VkImageLayout ui_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    uint32_t ui_w_ = 0, ui_h_ = 0;
    VkDescriptorSetLayout ui_set_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool ui_pool_ = VK_NULL_HANDLE;
    VkDescriptorSet ui_set_ = VK_NULL_HANDLE;
    VkPipelineLayout ui_pipeline_layout_ = VK_NULL_HANDLE;
    VkPipeline ui_pipeline_ = VK_NULL_HANDLE;
    VkFormat ui_pipeline_format_ = VK_FORMAT_UNDEFINED;
    VkShaderModule ui_vs_ = VK_NULL_HANDLE, ui_ps_ = VK_NULL_HANDLE;
    VkSampler ui_sampler_ = VK_NULL_HANDLE;
    bool ui_composite_logged_ = false;  // one error line if the overlay pipeline cannot be made

    // F3 diagnostic read-back of the finished swapchain image
    VkBuffer trace_buffer_ = VK_NULL_HANDLE;
    VkDeviceMemory trace_memory_ = VK_NULL_HANDLE;
    uint8_t* trace_ptr_ = nullptr;
    VkDeviceSize trace_size_ = 0;
    bool swap_capture_supported_ = false;

    // visibility tests: an occlusion query per counted draw, read at GetReport
    VkQueryPool query_pool_ = VK_NULL_HANDLE;
    uint32_t query_next_ = 0;
    std::vector<uint32_t> counted_;
    bool query_reset_ = true;

    // Graphics menu overlay (Dear ImGui), recorded inside the swapchain pass
    bool ui_initialized_ = false;
    double ui_fps_ = 0.0;
    uint64_t ui_fps_frames_ = 0;
    std::chrono::steady_clock::time_point ui_fps_time_{};
    unsigned long long frames_ = 0, draws_ = 0, tex_uploads_ = 0, shader_compiles_ = 0;
    bool perf_ = false;
    FILE* draw_log_ = nullptr;
    unsigned long long draw_log_n_ = 0;
    bool trace_ = false;
    bool trace_present_ = true;
    bool dump_targets_ = false;
    bool dump_now_ = false;
    uint32_t dump_frame_ = 0;
    std::string dump_prefix_ = "target";  // TEMP: mid-frame dumps name themselves
    std::vector<uint32_t> dump_tex_list_;
    std::unordered_set<uint32_t> frame_tex_;
    std::string frame_trace_;
    const char* tex_reason_ = "";
    Target* sampled_target_ = nullptr;  // the target EnsureTexture last borrowed, for kTexWide
    double perf_draw_ms_ = 0.0, perf_ui_ms_ = 0.0, perf_wait_ms_ = 0.0;
    double perf_assemble_ms_ = 0.0, perf_vertex_ms_ = 0.0, perf_tex_ms_ = 0.0, perf_pixel_ms_ = 0.0;
    double perf_target_ms_ = 0.0, perf_pipe_ms_ = 0.0, perf_const_ms_ = 0.0, perf_geom_ms_ = 0.0;
    double perf_record_ms_ = 0.0;
    double perf_sync_ms_ = 0.0, perf_copy_ms_ = 0.0, perf_clear_ms_ = 0.0, perf_present_ms_ = 0.0;
    unsigned long long perf_direct_ = 0, perf_stale_ = 0, perf_readback_ = 0;
    unsigned long long perf_bind_ = 0, perf_pass_new_ = 0, perf_fb_new_ = 0, perf_target_new_ = 0;
    unsigned long long perf_target_upload_ = 0, perf_feedback_ = 0;
    unsigned long long perf_defer_flushes_ = 0, perf_ao_runs_ = 0;  // deferred replays and AO passes
    std::map<uint32_t, unsigned long long> perf_flush_lines_;        // deferred replays by caller line
    // reused across draws: the assembled vertex/index buffers keep their capacity
    Assembled scratch_;
    std::chrono::steady_clock::time_point perf_log_at_{};
    };

// PART2

namespace {
const char* VkError(VkResult r) {
    switch (r) {
        case VK_SUCCESS: return "success";
        case VK_NOT_READY: return "not ready";
        case VK_TIMEOUT: return "timeout";
        case VK_ERROR_OUT_OF_HOST_MEMORY: return "out of host memory";
        case VK_ERROR_OUT_OF_DEVICE_MEMORY: return "out of device memory";
        case VK_ERROR_INITIALIZATION_FAILED: return "initialization failed";
        case VK_ERROR_DEVICE_LOST: return "device lost";
        case VK_ERROR_LAYER_NOT_PRESENT: return "layer not present";
        case VK_ERROR_EXTENSION_NOT_PRESENT: return "extension not present";
        case VK_ERROR_FEATURE_NOT_PRESENT: return "feature not present";
        case VK_ERROR_INCOMPATIBLE_DRIVER: return "incompatible driver";
        case VK_ERROR_FORMAT_NOT_SUPPORTED: return "format not supported";
        case VK_ERROR_SURFACE_LOST_KHR: return "surface lost";
        case VK_ERROR_OUT_OF_DATE_KHR: return "out of date";
        case VK_SUBOPTIMAL_KHR: return "suboptimal";
        default: return "unknown";
    }
}

// F3 diagnostic: write the raw swapchain read-back, i.e. the finished present
// (gamma ramp, PC contrast, scaling and the overlay all included). Guest-frame
// TGAs only show the front buffer; this one shows what the display received.
// HDR swapchain formats are decoded and tonemapped back to SDR so the capture stays readable.
uint32_t SwapBytesPerPixel(VkFormat fmt) {
    return fmt == VK_FORMAT_R16G16B16A16_SFLOAT ? 8 : 4;
}

float HalfToFloat(uint16_t h) {
    const uint32_t sign = (h >> 15) & 1u;
    const uint32_t exp = (h >> 10) & 0x1fu;
    const uint32_t mant = h & 0x3ffu;
    float v = 0.0f;
    if (exp == 0)
        v = std::ldexp(float(mant), -24);
    else if (exp == 31)
        v = 65504.0f;
    else
        v = std::ldexp(float(mant + 1024), int(exp) - 25);
    return sign ? -v : v;
}

float PqDecode(float e) {
    const float m1 = 2610.0f / 16384.0f;
    const float m2 = 2523.0f / 4096.0f * 128.0f;
    const float c1 = 3424.0f / 4096.0f;
    const float c2 = 2413.0f / 4096.0f * 32.0f;
    const float c3 = 2392.0f / 4096.0f * 32.0f;
    const float p = std::pow(std::max(e, 0.0f), 1.0f / m2);
    return std::pow(std::max((p - c1) / (c2 - c3 * p), 0.0f), 1.0f / m1);
}

uint8_t SrgbByte(float linear) {
    linear = std::clamp(linear, 0.0f, 1.0f);
    const float encoded = linear <= 0.0031308f ? 12.92f * linear : 1.055f * std::pow(linear, 1.0f / 2.4f) - 0.055f;
    return uint8_t(std::lround(std::clamp(encoded, 0.0f, 1.0f) * 255.0f));
}

bool WriteSwapTga(const char* path, const uint8_t* px, uint32_t w, uint32_t h, VkFormat fmt, HdrOutput mode,
                  float paper_white) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    uint8_t head[18] = {0, 0, 2};  // uncompressed true colour
    head[12] = uint8_t(w), head[13] = uint8_t(w >> 8), head[14] = uint8_t(h), head[15] = uint8_t(h >> 8);
    head[16] = 24;
    head[17] = 0x20;  // top row first
    std::fwrite(head, 1, 18, f);
    const bool bgr = fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB;
    const bool fp16 = mode != HdrOutput::Off && fmt == VK_FORMAT_R16G16B16A16_SFLOAT;
    const bool pq = mode != HdrOutput::Off && fmt == VK_FORMAT_A2B10G10R10_UNORM_PACK32;
    const uint32_t bpp = mode != HdrOutput::Off ? SwapBytesPerPixel(fmt) : 4;
    const float white = std::max(paper_white, 1.0f);
    // BT.2020 to BT.709, for reading PQ captures back on an SDR monitor
    constexpr float k2020to709[3][3] = {
        {1.6605f, -0.5876f, -0.0728f},
        {-0.1246f, 1.1329f, -0.0083f},
        {-0.0182f, -0.1006f, 1.1187f},
    };
    std::vector<uint8_t> row(size_t(w) * 3);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* p = px + (size_t(y) * w + x) * bpp;
            if (fp16) {
                uint16_t hpx[3];
                std::memcpy(hpx, p, sizeof(hpx));
                const float r = HalfToFloat(hpx[0]) * 80.0f / white;
                const float g = HalfToFloat(hpx[1]) * 80.0f / white;
                const float b = HalfToFloat(hpx[2]) * 80.0f / white;
                row[size_t(x) * 3 + 0] = SrgbByte(b);
                row[size_t(x) * 3 + 1] = SrgbByte(g);
                row[size_t(x) * 3 + 2] = SrgbByte(r);
            } else if (pq) {
                uint32_t packed = 0;
                std::memcpy(&packed, p, sizeof(packed));
                float rgb[3] = {float(packed & 0x3ffu) / 1023.0f, float((packed >> 10) & 0x3ffu) / 1023.0f,
                                float((packed >> 20) & 0x3ffu) / 1023.0f};
                for (float& c : rgb) c = PqDecode(c) * 10000.0f;
                float out[3];
                for (int r = 0; r < 3; r++)
                    out[r] = (k2020to709[r][0] * rgb[0] + k2020to709[r][1] * rgb[1] + k2020to709[r][2] * rgb[2]) / white;
                row[size_t(x) * 3 + 0] = SrgbByte(out[2]);
                row[size_t(x) * 3 + 1] = SrgbByte(out[1]);
                row[size_t(x) * 3 + 2] = SrgbByte(out[0]);
            } else {
                row[size_t(x) * 3 + 0] = bgr ? p[0] : p[2];
                row[size_t(x) * 3 + 1] = p[1];
                row[size_t(x) * 3 + 2] = bgr ? p[2] : p[0];
            }
        }
        std::fwrite(row.data(), 1, row.size(), f);
    }
    std::fclose(f);
    return true;
}

// 32-bit TGA, BGRA rows with alpha, for texture inspection
bool WriteRgbaTga(const char* path, const uint8_t* px, uint32_t w, uint32_t h, uint32_t pitch) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    uint8_t head[18] = {0, 0, 2};
    head[12] = uint8_t(w), head[13] = uint8_t(w >> 8), head[14] = uint8_t(h), head[15] = uint8_t(h >> 8);
    head[16] = 32;
    head[17] = 0x28;  // top row first, 8 alpha bits
    std::fwrite(head, 1, 18, f);
    for (uint32_t y = 0; y < h; y++) std::fwrite(px + size_t(y) * pitch, 1, size_t(w) * 4, f);
    std::fclose(f);
    return true;
}
} // namespace

namespace {
// $XDG_CACHE_HOME/TrueCrimeNYC, or ~/.cache/TrueCrimeNYC; empty when neither is set
std::filesystem::path CacheRoot() {
    if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
        return std::filesystem::path(xdg) / "TrueCrimeNYC";
    if (const char* home = std::getenv("HOME"); home && *home)
        return std::filesystem::path(home) / ".cache" / "TrueCrimeNYC";
    return {};
}

constexpr uint64_t kPipelineCacheMagic = 0x3142494C4F535058ull;  // "XPSOLIB1"
constexpr uint64_t kPipelineCacheMaxBytes = 512ull << 20;
struct PipelineFileHeader {
    uint64_t magic, size, checksum;
};
}  // namespace

VkResult VulkanBackend::CreatePipelines(uint32_t count, const VkGraphicsPipelineCreateInfo* info,
                                        const VkAllocationCallbacks* alloc, VkPipeline* out) {
    const VkResult r = vkCreateGraphicsPipelines(device_, pipeline_cache_, count, info, alloc, out);
    if (r == VK_SUCCESS && pipeline_cache_) {
        // Vulkan does not say whether the cache already had it; a save with nothing new is only a rewrite
        ++pipeline_cache_new_;
        pipeline_cache_quiet_since_ = frames_;
    }
    return r;
}

// The cache from the last run, or an empty one. Its own header names the GPU and driver build; data from
// another is not handed to the driver (some drivers do not reject it safely), so that starts an empty cache.
void VulkanBackend::OpenPipelineCache() {
    if (const char* e = std::getenv("TCNYC_PIPELINE_CACHE"); e && std::strcmp(e, "0") == 0) return;
    std::vector<uint8_t> data;
    const std::filesystem::path root = CacheRoot();
    if (!root.empty()) {
        std::ifstream in(root / "pipelines.bin", std::ios::binary | std::ios::ate);
        const std::streamoff length = in ? std::streamoff(in.tellg()) : std::streamoff(-1);
        PipelineFileHeader header{};
        if (length > std::streamoff(sizeof(header)) &&
            length <= std::streamoff(sizeof(header) + kPipelineCacheMaxBytes)) {
            in.seekg(0);
            if (in.read(reinterpret_cast<char*>(&header), sizeof(header)) && header.magic == kPipelineCacheMagic &&
                header.size == uint64_t(length) - sizeof(header)) {
                data.resize(size_t(header.size));
                if (!in.read(reinterpret_cast<char*>(data.data()), std::streamsize(data.size())) ||
                    Hash(data.data(), data.size()) != header.checksum)
                    data.clear();
            }
        }
    }
    // VkPipelineCacheHeaderVersionOne: length, version, vendor, device, UUID
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(physical_, &props);
    if (data.size() >= 32) {
        uint32_t words[4];
        std::memcpy(words, data.data(), 16);
        if (words[1] != VK_PIPELINE_CACHE_HEADER_VERSION_ONE || words[2] != props.vendorID ||
            words[3] != props.deviceID || std::memcmp(data.data() + 16, props.pipelineCacheUUID, VK_UUID_SIZE)) {
            LOGR("vulkan: saved pipelines are from another GPU or driver, starting again");
            data.clear();
            pipeline_cache_new_ = 1;  // replace the stale file at the next save
        }
    } else {
        data.clear();
    }
    VkPipelineCacheCreateInfo info{VK_STRUCTURE_TYPE_PIPELINE_CACHE_CREATE_INFO};
    info.initialDataSize = data.size();
    info.pInitialData = data.empty() ? nullptr : data.data();
    if (vkCreatePipelineCache(device_, &info, nullptr, &pipeline_cache_) != VK_SUCCESS) {
        info.initialDataSize = 0;
        info.pInitialData = nullptr;
        if (vkCreatePipelineCache(device_, &info, nullptr, &pipeline_cache_) != VK_SUCCESS) {
            pipeline_cache_ = VK_NULL_HANDLE;
            LOGR("vulkan: pipeline cache unavailable, pipelines are not kept between runs");
            return;
        }
    }
    LOGR("vulkan: pipeline cache opened, %zu KB saved", data.size() / 1024);
}

// Writes the cache once a few seconds pass with no new pipeline (they come in bursts, at level loads and new
// effects), so the copy does not land in the middle of one. force writes any change (at shutdown).
void VulkanBackend::SavePipelineCache(bool force) {
    if (!pipeline_cache_ || !pipeline_cache_new_) return;
    if (!force && frames_ - pipeline_cache_quiet_since_ < 300) return;  // 5 s at 60 FPS
    const std::filesystem::path root = CacheRoot();
    if (root.empty()) return;
    size_t size = 0;
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, nullptr) != VK_SUCCESS || !size ||
        size > kPipelineCacheMaxBytes)
        return;
    std::vector<uint8_t> bytes(size);
    if (vkGetPipelineCacheData(device_, pipeline_cache_, &size, bytes.data()) != VK_SUCCESS) return;
    bytes.resize(size);
    pipeline_cache_new_ = 0;
    std::error_code ec;
    std::filesystem::create_directories(root, ec);
    const std::string file = (root / "pipelines.bin").string();
    const std::string temp = file + ".tmp." + std::to_string(::getpid());
    const PipelineFileHeader header{kPipelineCacheMagic, bytes.size(), Hash(bytes.data(), bytes.size())};
    FILE* f = std::fopen(temp.c_str(), "wb");
    if (!f) return;
    const bool ok = std::fwrite(&header, sizeof(header), 1, f) == 1 &&
                    std::fwrite(bytes.data(), 1, bytes.size(), f) == bytes.size();
    if (std::fclose(f) == 0 && ok && std::rename(temp.c_str(), file.c_str()) == 0) {
        LOGI("vulkan: saved %zu KB of pipelines", bytes.size() / 1024);
        return;
    }
    std::remove(temp.c_str());
}

bool VulkanBackend::Init(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    scale_ = app::RenderScale(height_);
    stretch_ = app::RenderStretch(width_, height_);
    aniso_setting_ = app::Snapshot().aniso;
    aspect_ = app::AspectRatio();
    perf_ = std::getenv("TCNYC_PERF") != nullptr;
    // testing only: no occlusion set can be bound, to exercise the non-AO receiver fallback
    ao_fail_test_ = std::getenv("TCNYC_AO_FAIL_TEST") != nullptr;
    trace_ = std::getenv("TCNYC_TRACE") != nullptr;
    if (const char* e = std::getenv("TCNYC_DEFER_SCENE")) defer_forced_ = *e && std::atoi(e) != 0;
    ao_mode_ = app::Snapshot().ao;
    defer_scene_ = defer_forced_ || ao_mode_ != 0;
    msaa_store_ = defer_scene_;
    if (defer_scene_) LOGR("vulkan: deferred scene display list enabled (%s)", defer_forced_ ? "TCNYC_DEFER_SCENE" : "ambient occlusion setting");
    if (const char* e = std::getenv("TCNYC_TRACE_PRESENT")) trace_present_ = std::atoi(e) != 0;
    if (const char* e = std::getenv("TCNYC_DUMP_TEX")) {
        const char* p = e;
        while (*p) {
            char* end = nullptr;
            const unsigned long v = std::strtoul(p, &end, 16);
            if (end == p) break;
            dump_tex_list_.push_back(uint32_t(v));
            p = end;
            while (*p == ',' || *p == ' ') p++;
        }
    }
    dump_targets_ = std::getenv("TCNYC_DUMP_TARGETS") != nullptr;
    if (std::getenv("TCNYC_DRAWLOG")) {
        std::error_code ec;
        std::filesystem::create_directories(app::ExeDir() / "logs", ec);
        draw_log_ = std::fopen((app::ExeDir() / "logs/draws_vk.txt").string().c_str(), "w");
    }
    perf_log_at_ = std::chrono::steady_clock::now();
    if (!CreateInstance() || !CreateSurface() || !PickDevice() || !CreateDevice()) return false;
    OpenPipelineCache();
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical_, &props);
        max_image_side_ = props.limits.maxImageDimension2D;
        api12_ = instance_version_ >= VK_API_VERSION_1_2 && props.apiVersion >= VK_API_VERSION_1_2;
        VkPhysicalDeviceDepthStencilResolveProperties dsr{
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DEPTH_STENCIL_RESOLVE_PROPERTIES};
        VkPhysicalDeviceProperties2 props2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
        props2.pNext = &dsr;
        if (api12_) vkGetPhysicalDeviceProperties2(physical_, &props2);
        create_render_pass2_ =
            reinterpret_cast<PFN_vkCreateRenderPass2>(vkGetDeviceProcAddr(device_, "vkCreateRenderPass2"));
        if (!create_render_pass2_)
            create_render_pass2_ =
                reinterpret_cast<PFN_vkCreateRenderPass2>(vkGetDeviceProcAddr(device_, "vkCreateRenderPass2KHR"));
        if (!create_render_pass2_)
            create_render_pass2_ =
                reinterpret_cast<PFN_vkCreateRenderPass2>(vkGetInstanceProcAddr(instance_, "vkCreateRenderPass2"));
        if (!create_render_pass2_)
            create_render_pass2_ =
                reinterpret_cast<PFN_vkCreateRenderPass2>(vkGetInstanceProcAddr(instance_, "vkCreateRenderPass2KHR"));
        depth_resolve_ = api12_ && create_render_pass2_ && (dsr.supportedDepthResolveModes != 0);
        // D3D12 resolves depth by reading sample 0; MAX would resolve the farthest sample instead.
        if (dsr.supportedDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)
            depth_resolve_mode_ = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
        else if (dsr.supportedDepthResolveModes & VK_RESOLVE_MODE_MAX_BIT)
            depth_resolve_mode_ = VK_RESOLVE_MODE_MAX_BIT;
        else
            depth_resolve_ = false;
        color_samples_ = props.limits.framebufferColorSampleCounts;
        depth_samples_ = props.limits.framebufferDepthSampleCounts;
        max_anisotropy_ = props.limits.maxSamplerAnisotropy;
        // RADV (and some other drivers) do not support D24S8 as an optimal-tiling sampled
        // depth format; pick the first depth/stencil format the driver actually lists.
        constexpr VkFormat kDepthCandidates[] = {VK_FORMAT_D32_SFLOAT_S8_UINT, VK_FORMAT_D24_UNORM_S8_UINT,
                                                 VK_FORMAT_D32_SFLOAT, VK_FORMAT_D16_UNORM};
        constexpr VkFormatFeatureFlags kDepthNeeded =
            VK_FORMAT_FEATURE_DEPTH_STENCIL_ATTACHMENT_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_BIT;
        for (VkFormat f : kDepthCandidates) {
            VkFormatProperties fp{};
            vkGetPhysicalDeviceFormatProperties(physical_, f, &fp);
            if ((fp.optimalTilingFeatures & kDepthNeeded) != kDepthNeeded) continue;
            depth_format_ = f;
            depth_has_stencil_ = f == VK_FORMAT_D32_SFLOAT_S8_UINT || f == VK_FORMAT_D24_UNORM_S8_UINT;
            break;
        }
        LOGR("vulkan: depth format %d%s", int(depth_format_), depth_has_stencil_ ? " (stencil)" : "");
        const uint32_t requested = app::MsaaSamples();
        msaa_ = api12_ && depth_resolve_ ? PickSampleCount(requested, color_samples_ & depth_samples_) : 1;
        if (requested > 1)
            LOGR("vulkan: %ux MSAA requested, using %ux%s", requested, msaa_,
                 msaa_ > 1 ? "" : " (unsupported here)");
    }
    // AO's distance target is R32F. Its supported sample count is independent of the scene
    // colour/depth target, so cap the prepass samples to it and fall back to one sample.
    {
        VkImageFormatProperties props{};
        if (vkGetPhysicalDeviceImageFormatProperties(physical_, VK_FORMAT_R32_SFLOAT, VK_IMAGE_TYPE_2D,
                                                     VK_IMAGE_TILING_OPTIMAL,
                                                     VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                                                     0, &props) == VK_SUCCESS)
            ao_color_samples_ = props.sampleCounts;
        // the prepass also needs a depth target of depth_format_ at the same sample count
        VkImageFormatProperties depth_props{};
        if (vkGetPhysicalDeviceImageFormatProperties(physical_, depth_format_, VK_IMAGE_TYPE_2D,
                                                     VK_IMAGE_TILING_OPTIMAL,
                                                     VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT, 0,
                                                     &depth_props) == VK_SUCCESS)
            ao_color_samples_ &= depth_props.sampleCounts;
        if ((ao_color_samples_ & VK_SAMPLE_COUNT_1_BIT) == 0) ao_color_samples_ = VK_SAMPLE_COUNT_1_BIT;
    }
    if (!CreateSwapchain() || !CreateCommands() || !CreateUpload()) return false;

    // 1x1 white for stages with no texture
    VkImageCreateInfo wi{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    wi.imageType = VK_IMAGE_TYPE_2D;
    wi.format = VK_FORMAT_B8G8R8A8_UNORM;
    wi.extent = {1, 1, 1};
    wi.mipLevels = 1;
    wi.arrayLayers = 1;
    wi.samples = VK_SAMPLE_COUNT_1_BIT;
    wi.tiling = VK_IMAGE_TILING_OPTIMAL;
    wi.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(device_, &wi, nullptr, &white_image_) != VK_SUCCESS) return false;
    VkMemoryRequirements wr{};
    vkGetImageMemoryRequirements(device_, white_image_, &wr);
    VkMemoryAllocateInfo wa{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    wa.allocationSize = wr.size;
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; i++)
        if ((wr.memoryTypeBits & (1u << i)) &&
            (mem_props_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            wa.memoryTypeIndex = i;
            break;
        }
    if (vkAllocateMemory(device_, &wa, nullptr, &white_memory_) != VK_SUCCESS ||
        vkBindImageMemory(device_, white_image_, white_memory_, 0) != VK_SUCCESS)
        return false;
    VkImageViewCreateInfo wv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    wv.image = white_image_;
    wv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    wv.format = VK_FORMAT_B8G8R8A8_UNORM;
    wv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &wv, nullptr, &white_view_) != VK_SUCCESS) return false;
    // clear the white image once and leave it sampleable
    BeginRecord();
    VkImageLayout white_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    Transition(white_image_, white_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_FORMAT_B8G8R8A8_UNORM);
    VkClearColorValue white{};
    white.float32[0] = white.float32[1] = white.float32[2] = white.float32[3] = 1.0f;
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd_, white_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &white, 1, &range);
    Transition(white_image_, white_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_FORMAT_B8G8R8A8_UNORM);
    EndRecord(true);
    VkSamplerCreateInfo ws{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    ws.magFilter = ws.minFilter = VK_FILTER_NEAREST;
    ws.addressModeU = ws.addressModeV = ws.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &ws, nullptr, &white_sampler_) != VK_SUCCESS) return false;

    // descriptor set layout: b0 VC, b1 DC, t0-t3, s0-s3
    VkDescriptorSetLayoutBinding bind[10]{};
    bind[0].binding = 0;
    bind[0].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    bind[0].descriptorCount = 1;
    bind[0].stageFlags = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT |
                         VK_SHADER_STAGE_GEOMETRY_BIT;
    bind[1] = bind[0];
    bind[1].binding = 1;
    for (int i = 0; i < 4; i++) {
        bind[2 + i].binding = uint32_t(2 + i);
        bind[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bind[2 + i].descriptorCount = 1;
        bind[2 + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bind[6 + i].binding = uint32_t(6 + i);
        bind[6 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bind[6 + i].descriptorCount = 1;
        bind[6 + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 10;
    li.pBindings = bind;
    if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &set_layout_) != VK_SUCCESS) return false;

    // set 1: the receiver AO occlusion image (t4 in the generated pixel shader)
    VkDescriptorSetLayoutBinding occBind{};
    occBind.binding = 0;
    occBind.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    occBind.descriptorCount = 1;
    occBind.stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo oli{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    oli.bindingCount = 1;
    oli.pBindings = &occBind;
    if (vkCreateDescriptorSetLayout(device_, &oli, nullptr, &ao_occ_set_layout_) != VK_SUCCESS) return false;

    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    const VkDescriptorSetLayout scene_sets[2] = {set_layout_, ao_occ_set_layout_};
    pl.setLayoutCount = 2;
    pl.pSetLayouts = scene_sets;
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};  // the masked-clear colour
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device_, &pl, nullptr, &pipeline_layout_) != VK_SUCCESS) return false;

    // the full-screen AO passes: t0/t1 sampled images, s0/s1 samplers, 32 byte push constants
    VkDescriptorSetLayoutBinding aoBind[4]{};
    for (int i = 0; i < 2; i++) {
        aoBind[i].binding = uint32_t(i);
        aoBind[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        aoBind[i].descriptorCount = 1;
        aoBind[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    for (int i = 0; i < 2; i++) {
        aoBind[2 + i].binding = uint32_t(2 + i);
        aoBind[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        aoBind[2 + i].descriptorCount = 1;
        aoBind[2 + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    VkDescriptorSetLayoutCreateInfo ali{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    ali.bindingCount = 4;
    ali.pBindings = aoBind;
    if (vkCreateDescriptorSetLayout(device_, &ali, nullptr, &ao_set_layout_) != VK_SUCCESS) return false;
    VkPipelineLayoutCreateInfo apl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    apl.setLayoutCount = 1;
    apl.pSetLayouts = &ao_set_layout_;
    VkPushConstantRange aoPush{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 32};  // src + ao/planes
    apl.pushConstantRangeCount = 1;
    apl.pPushConstantRanges = &aoPush;
    if (vkCreatePipelineLayout(device_, &apl, nullptr, &ao_pipeline_layout_) != VK_SUCCESS) return false;

    VkDescriptorPoolSize sizes[3] = {{VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 2 * 8192},
                                     {VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4 * 8192},
                                     {VK_DESCRIPTOR_TYPE_SAMPLER, 4 * 8192}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 8192;
    pi.poolSizeCount = 3;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device_, &pi, nullptr, &descriptor_pool_) != VK_SUCCESS) return false;
    // all per-draw descriptor sets up front: allocating one per draw is a driver call
    // 600-2000 times a frame. They are rewritten and reused in a ring each frame.
    std::vector<VkDescriptorSetLayout> layouts(8192, set_layout_);
    VkDescriptorSetAllocateInfo dai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    dai.descriptorPool = descriptor_pool_;
    dai.descriptorSetCount = uint32_t(layouts.size());
    dai.pSetLayouts = layouts.data();
    draw_sets_.resize(layouts.size());
    if (vkAllocateDescriptorSets(device_, &dai, draw_sets_.data()) != VK_SUCCESS) return false;

    // the AO pools are made on first AO use (EnsureAoPools): with AO off they are never needed
    if (!CreatePresent()) return false;

    LOGR("vulkan: %s, %ux%u swapchain, GPU draws", device_name_.c_str(), extent_.width, extent_.height);
    return true;
}

// The AO descriptor pools, made the first time AO runs. One try: a failure leaves the set lists
// empty, and the AO set accessors then report no set, which draws the receivers without AO.
bool VulkanBackend::EnsureAoPools() {
    if (!ao_sets_.empty()) return true;
    if (ao_pools_tried_) return false;
    ao_pools_tried_ = true;
    auto fail = [&] {
        LOGE("vulkan: AO descriptor pools could not be created");
        ao_occ_sets_.clear();
        ao_sets_.clear();
        if (ao_occ_pool_) vkDestroyDescriptorPool(device_, ao_occ_pool_, nullptr);
        if (ao_pool_) vkDestroyDescriptorPool(device_, ao_pool_, nullptr);
        ao_occ_pool_ = ao_pool_ = VK_NULL_HANDLE;
        return false;
    };
    // one occlusion set per flush, and two full-screen AO sets (GTAO then the blur) per flush
    VkDescriptorPoolSize occSizes{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 2048};
    VkDescriptorPoolCreateInfo occPi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    occPi.maxSets = 2048;
    occPi.poolSizeCount = 1;
    occPi.pPoolSizes = &occSizes;
    if (vkCreateDescriptorPool(device_, &occPi, nullptr, &ao_occ_pool_) != VK_SUCCESS) return fail();
    std::vector<VkDescriptorSetLayout> occLayouts(2048, ao_occ_set_layout_);
    VkDescriptorSetAllocateInfo occAi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    occAi.descriptorPool = ao_occ_pool_;
    occAi.descriptorSetCount = uint32_t(occLayouts.size());
    occAi.pSetLayouts = occLayouts.data();
    ao_occ_sets_.resize(occLayouts.size());
    if (vkAllocateDescriptorSets(device_, &occAi, ao_occ_sets_.data()) != VK_SUCCESS) return fail();

    VkDescriptorPoolSize aoSizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 8192},
                                       {VK_DESCRIPTOR_TYPE_SAMPLER, 8192}};
    VkDescriptorPoolCreateInfo aoPi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    aoPi.maxSets = 4096;
    aoPi.poolSizeCount = 2;
    aoPi.pPoolSizes = aoSizes;
    if (vkCreateDescriptorPool(device_, &aoPi, nullptr, &ao_pool_) != VK_SUCCESS) return fail();
    std::vector<VkDescriptorSetLayout> aoLayouts(4096, ao_set_layout_);
    VkDescriptorSetAllocateInfo aoAi{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    aoAi.descriptorPool = ao_pool_;
    aoAi.descriptorSetCount = uint32_t(aoLayouts.size());
    aoAi.pSetLayouts = aoLayouts.data();
    ao_sets_.resize(aoLayouts.size());
    if (vkAllocateDescriptorSets(device_, &aoAi, ao_sets_.data()) != VK_SUCCESS) return fail();
    LOGR("vulkan: AO descriptor pools created");
    return true;
}

bool VulkanBackend::CreateInstance() {
    if (!SDL_Vulkan_LoadLibrary(nullptr)) {
        LOGE("vulkan: SDL could not load the Vulkan loader: %s", SDL_GetError());
        return false;
    }
    uint32_t count = 0;
    const char* const* extensions = SDL_Vulkan_GetInstanceExtensions(&count);
    if (!extensions || !count) {
        LOGE("vulkan: SDL listed no instance extensions: %s", SDL_GetError());
        return false;
    }
    std::vector<const char*> enabled(extensions, extensions + count);
    // The color-space extension is optional: without it the surface only offers SDR formats.
    uint32_t extension_count = 0;
    if (vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, nullptr) == VK_SUCCESS && extension_count) {
        std::vector<VkExtensionProperties> properties(extension_count);
        if (vkEnumerateInstanceExtensionProperties(nullptr, &extension_count, properties.data()) == VK_SUCCESS) {
            for (const VkExtensionProperties& property : properties) {
                if (!std::strcmp(property.extensionName, VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME)) {
                    enabled.push_back(VK_EXT_SWAPCHAIN_COLOR_SPACE_EXTENSION_NAME);
                    swapchain_colorspace_ = true;
                    break;
                }
            }
        }
    }
    if (!swapchain_colorspace_) LOGR("vulkan: VK_EXT_swapchain_colorspace unavailable, HDR output off");
    // Ask for the newest instance version the loader offers (capped at 1.3). The loader hides
    // 1.2+ device entry points from vkGetDeviceProcAddr when the instance asked for 1.1, which
    // made vkCreateRenderPass2 resolve to a null trampoline.
    uint32_t loader_version = VK_API_VERSION_1_0;
    if (auto enumerate_version = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
            vkGetInstanceProcAddr(nullptr, "vkEnumerateInstanceVersion")))
        enumerate_version(&loader_version);
    instance_version_ = std::min(loader_version, uint32_t(VK_API_VERSION_1_3));
    VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
    app.pApplicationName = "TrueCrimeNYC";
    app.apiVersion = instance_version_;
    VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
    info.pApplicationInfo = &app;
    info.enabledExtensionCount = uint32_t(enabled.size());
    info.ppEnabledExtensionNames = enabled.data();
    VkResult r = vkCreateInstance(&info, nullptr, &instance_);
    if (r != VK_SUCCESS) {
        LOGE("vulkan: vkCreateInstance failed: %s", VkError(r));
        return false;
    }
    return true;
}

bool VulkanBackend::CreateSurface() {
    if (!SDL_Vulkan_CreateSurface(static_cast<SDL_Window*>(app::Window()), instance_, nullptr, &surface_)) {
        LOGE("vulkan: SDL_Vulkan_CreateSurface failed: %s", SDL_GetError());
        return false;
    }
    return true;
}

bool VulkanBackend::PickDevice() {
    uint32_t count = 0;
    vkEnumeratePhysicalDevices(instance_, &count, nullptr);
    if (!count) {
        LOGE("vulkan: no Vulkan device");
        return false;
    }
    std::vector<VkPhysicalDevice> devices(count);
    vkEnumeratePhysicalDevices(instance_, &count, devices.data());
    for (VkPhysicalDevice device : devices) {
        uint32_t families = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(device, &families, nullptr);
        std::vector<VkQueueFamilyProperties> props(families);
        vkGetPhysicalDeviceQueueFamilyProperties(device, &families, props.data());
        uint32_t graphics = ~0u, present = ~0u;
        for (uint32_t i = 0; i < families; i++) {
            if ((props[i].queueFlags & VK_QUEUE_GRAPHICS_BIT) && graphics == ~0u) graphics = i;
            VkBool32 supported = VK_FALSE;
            vkGetPhysicalDeviceSurfaceSupportKHR(device, i, surface_, &supported);
            if (supported && present == ~0u) present = i;
        }
        if (graphics == ~0u || present == ~0u) continue;
        uint32_t formats = 0, modes = 0;
        vkGetPhysicalDeviceSurfaceFormatsKHR(device, surface_, &formats, nullptr);
        vkGetPhysicalDeviceSurfacePresentModesKHR(device, surface_, &modes, nullptr);
        if (!formats || !modes) continue;
        VkPhysicalDeviceProperties p{};
        vkGetPhysicalDeviceProperties(device, &p);
        physical_ = device;
        graphics_family_ = graphics;
        present_family_ = present;
        device_name_ = p.deviceName;
        vkGetPhysicalDeviceMemoryProperties(device, &mem_props_);
        VkPhysicalDeviceFeatures f{};
        vkGetPhysicalDeviceFeatures(device, &f);
        bc_supported_ = f.textureCompressionBC == VK_TRUE;
        return true;
    }
    LOGE("vulkan: no device can present to the window");
    return false;
}

bool VulkanBackend::CreateDevice() {
    const float priority = 1.0f;
    std::vector<VkDeviceQueueCreateInfo> queues;
    for (uint32_t family : {graphics_family_, present_family_}) {
        bool seen = false;
        for (const auto& q : queues) seen = seen || q.queueFamilyIndex == family;
        if (seen) continue;
        VkDeviceQueueCreateInfo q{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        q.queueFamilyIndex = family;
        q.queueCount = 1;
        q.pQueuePriorities = &priority;
        queues.push_back(q);
    }
    std::vector<const char*> extensions = {VK_KHR_SWAPCHAIN_EXTENSION_NAME};
    VkPhysicalDeviceFeatures supported{};
    vkGetPhysicalDeviceFeatures(physical_, &supported);
    // the guest picks a border colour per texture stage; Vulkan needs VK_EXT_custom_border_color for
    // anything but the four fixed colours, and it needs the without-format variant because one
    // sampler is shared by every texture format
    VkPhysicalDeviceCustomBorderColorFeaturesEXT border_features{
        VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_CUSTOM_BORDER_COLOR_FEATURES_EXT};
    {
        uint32_t count = 0;
        vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, nullptr);
        std::vector<VkExtensionProperties> available(count);
        vkEnumerateDeviceExtensionProperties(physical_, nullptr, &count, available.data());
        bool has_border = false;
        bool has_hdr_metadata = false;
        for (const VkExtensionProperties& e : available) {
            has_border = has_border || !std::strcmp(e.extensionName, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
            has_hdr_metadata = has_hdr_metadata || !std::strcmp(e.extensionName, VK_EXT_HDR_METADATA_EXTENSION_NAME);
        }
        if (has_border) {
            VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features2.pNext = &border_features;
            vkGetPhysicalDeviceFeatures2(physical_, &features2);
            border_custom_ = border_features.customBorderColors && border_features.customBorderColorWithoutFormat;
            border_features.customBorderColors = border_custom_ ? VK_TRUE : VK_FALSE;
            border_features.customBorderColorWithoutFormat = border_custom_ ? VK_TRUE : VK_FALSE;
            if (border_custom_) extensions.push_back(VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
        }
        if (has_hdr_metadata) {
            extensions.push_back(VK_EXT_HDR_METADATA_EXTENSION_NAME);
            hdr_metadata_ = true;
        }
    }
    LOGR("vulkan: custom sampler border colours %s", border_custom_ ? "on" : "off (fixed colours)");
    VkPhysicalDeviceFeatures features{};
    features.robustBufferAccess = supported.robustBufferAccess;
    features.textureCompressionBC = supported.textureCompressionBC;
    features.samplerAnisotropy = supported.samplerAnisotropy;
    sampler_anisotropy_ = supported.samplerAnisotropy == VK_TRUE;
    features.shaderClipDistance = supported.shaderClipDistance;
    features.fillModeNonSolid = supported.fillModeNonSolid;
    features.geometryShader = supported.geometryShader;
    geometry_ = supported.geometryShader == VK_TRUE;
    LOGR("vulkan: console point/line expansion %s", geometry_ ? "on (geometry shaders)" : "off (unsupported)");
    fill_non_solid_ = supported.fillModeNonSolid == VK_TRUE;
    VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
    info.queueCreateInfoCount = uint32_t(queues.size());
    info.pQueueCreateInfos = queues.data();
    info.enabledExtensionCount = uint32_t(extensions.size());
    info.ppEnabledExtensionNames = extensions.data();
    info.pEnabledFeatures = &features;
    info.pNext = border_custom_ ? &border_features : nullptr;
    VkResult r = vkCreateDevice(physical_, &info, nullptr, &device_);
    if (r != VK_SUCCESS) {
        LOGE("vulkan: vkCreateDevice failed: %s", VkError(r));
        return false;
    }
    vkGetDeviceQueue(device_, graphics_family_, 0, &queue_);
    vkGetDeviceQueue(device_, present_family_, 0, &present_queue_);
    if (hdr_metadata_) {
        set_hdr_metadata_ =
            reinterpret_cast<PFN_vkSetHdrMetadataEXT>(vkGetDeviceProcAddr(device_, "vkSetHdrMetadataEXT"));
        hdr_metadata_ = set_hdr_metadata_ != nullptr;
        if (!hdr_metadata_) LOGR("vulkan: VK_EXT_hdr_metadata enabled but its entry point is missing");
    }
    return true;
}

// TCNYC_SWAPCHAIN_FAIL_TEST=N fails the first N swapchain rebuilds (not the first creation), to
// exercise the retry in Present
bool VulkanBackend::SwapchainFailTest() {
    static uint32_t left = [] {
        const char* v = std::getenv("TCNYC_SWAPCHAIN_FAIL_TEST");
        return v ? uint32_t(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    if (!left || !extent_.width) return false;  // never the first creation
    left--;
    LOGR("vulkan: TCNYC_SWAPCHAIN_FAIL_TEST failing a swapchain rebuild");
    return true;
}

bool VulkanBackend::CreateSwapchain() {
    VkSurfaceCapabilitiesKHR caps{};
    if (vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical_, surface_, &caps) != VK_SUCCESS) {
        LOGE("vulkan: could not read surface capabilities");
        return false;
    }
    uint32_t format_count = 0;
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &format_count, nullptr);
    if (!format_count) {
        LOGE("vulkan: the surface lists no formats");
        return false;
    }
    std::vector<VkSurfaceFormatKHR> formats(format_count);
    vkGetPhysicalDeviceSurfaceFormatsKHR(physical_, surface_, &format_count, formats.data());
    // SDR selection is unchanged: the first BGRA8/RGBA8 UNORM entry the surface lists.
    VkSurfaceFormatKHR chosen = formats[0];
    VkSurfaceFormatKHR sdr = formats[0], hdr10{}, linear{};
    bool have_sdr = false, have_hdr10 = false, have_linear = false;
    for (const auto& f : formats) {
        if (!have_sdr && (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM)) {
            sdr = f;
            have_sdr = true;
        }
        if (f.format == VK_FORMAT_A2B10G10R10_UNORM_PACK32 && f.colorSpace == VK_COLOR_SPACE_HDR10_ST2084_EXT) {
            hdr10 = f;
            have_hdr10 = true;
        }
        if (f.format == VK_FORMAT_R16G16B16A16_SFLOAT && f.colorSpace == VK_COLOR_SPACE_EXTENDED_SRGB_LINEAR_EXT) {
            linear = f;
            have_linear = true;
        }
    }
    if (have_sdr) chosen = sdr;
    const app::Settings settings = app::Snapshot();
    hdr_setting_ = settings.hdr;
    const bool hdr_available = swapchain_colorspace_ && (have_hdr10 || have_linear);
    g_hdr_available.store(hdr_available, std::memory_order_relaxed);
    // Vulkan has no standard panel-peak query; HDR1000 is the metadata default.
    display_peak_nits_ = 1000;
    g_hdr_display_peak_nits.store(hdr_available ? display_peak_nits_ : 0, std::memory_order_relaxed);
    if (settings.hdr && have_hdr10) {
        chosen = hdr10;
        hdr_output_ = HdrOutput::Hdr10;
    } else if (settings.hdr && have_linear) {
        chosen = linear;
        hdr_output_ = HdrOutput::ScRgb;
    } else {
        hdr_output_ = HdrOutput::Off;
        if (settings.hdr)
            LOGR("vulkan: HDR requested but %s; using SDR output",
                 swapchain_colorspace_ ? "the surface offers no HDR format" : "VK_EXT_swapchain_colorspace is missing");
    }
    uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    if (mode_count)
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &mode_count, modes.data());
    const bool vsync = settings.vsync;
    VkPresentModeKHR present_mode = VK_PRESENT_MODE_FIFO_KHR;
    const VkPresentModeKHR wanted = vsync ? VK_PRESENT_MODE_FIFO_KHR : VK_PRESENT_MODE_IMMEDIATE_KHR;
    bool found = false;
    for (VkPresentModeKHR m : modes)
        if (m == wanted) {
            present_mode = m;
            found = true;
            break;
        }
    if (!vsync && !found)
        for (VkPresentModeKHR m : modes)
            if (m == VK_PRESENT_MODE_MAILBOX_KHR) {
                present_mode = m;
                break;
            }
    LOGI("vulkan: %s present mode", present_mode == VK_PRESENT_MODE_FIFO_KHR    ? "FIFO"
                                     : present_mode == VK_PRESENT_MODE_MAILBOX_KHR ? "mailbox"
                                                                                   : "immediate");

    VkExtent2D extent = caps.currentExtent;
    if (extent.width == UINT32_MAX) {
        const app::Mode pixels = app::WindowPixels();
        extent.width = std::clamp(pixels.width ? pixels.width : width_, caps.minImageExtent.width,
                                  caps.maxImageExtent.width);
        extent.height = std::clamp(pixels.height ? pixels.height : height_, caps.minImageExtent.height,
                                   caps.maxImageExtent.height);
    }
    // A minimised surface can report 0x0. Keep the old chain (if any) and retry next frame rather
    // than creating an invalid swapchain.
    if (!extent.width || !extent.height) {
        LOG_FIRST(8, "vulkan: surface extent %ux%u, keeping the current swapchain", extent.width, extent.height);
        return false;
    }
    // F3 can read the finished swapchain image back when the surface allows it
    swap_capture_supported_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (swapchain_) {
        vkDeviceWaitIdle(device_);
        DestroyUi();
        DestroyUiTarget();
        if (ui_pipeline_) vkDestroyPipeline(device_, ui_pipeline_, nullptr);
        ui_pipeline_ = VK_NULL_HANDLE;
        ui_pipeline_format_ = VK_FORMAT_UNDEFINED;
        for (VkFramebuffer fb : swap_framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
        swap_framebuffers_.clear();
        for (VkImageView view : swap_views_) vkDestroyImageView(device_, view, nullptr);
        swap_views_.clear();
        if (swap_pass_) vkDestroyRenderPass(device_, swap_pass_, nullptr);
        swap_pass_ = VK_NULL_HANDLE;
        if (present_pipeline_) vkDestroyPipeline(device_, present_pipeline_, nullptr);
        present_pipeline_ = VK_NULL_HANDLE;
        present_format_ = VK_FORMAT_UNDEFINED;
    }
    // Handed to the driver as oldSwapchain, so it can move the surface over without a gap, and
    // destroyed once the replacement exists (or failed). A failed rebuild leaves swapchain_ empty
    // and Present tries again on the next frame.
    const VkSwapchainKHR old_swapchain = swapchain_;
    swapchain_ = VK_NULL_HANDLE;
    uint32_t images = caps.minImageCount + 1;
    if (caps.maxImageCount && images > caps.maxImageCount) images = caps.maxImageCount;

    VkSwapchainCreateInfoKHR info{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
    info.surface = surface_;
    info.minImageCount = images;
    info.imageFormat = chosen.format;
    info.imageColorSpace = chosen.colorSpace;
    info.imageExtent = extent;
    info.imageArrayLayers = 1;
    info.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
    if (swap_capture_supported_) info.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    uint32_t families[] = {graphics_family_, present_family_};
    if (graphics_family_ != present_family_) {
        info.imageSharingMode = VK_SHARING_MODE_CONCURRENT;
        info.queueFamilyIndexCount = 2;
        info.pQueueFamilyIndices = families;
    } else {
        info.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
    }
    info.preTransform = caps.currentTransform;
    info.compositeAlpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
    constexpr VkCompositeAlphaFlagBitsKHR kAlphas[] = {
        VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
        VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR};
    for (VkCompositeAlphaFlagBitsKHR a : kAlphas)
        if (caps.supportedCompositeAlpha & a) {
            info.compositeAlpha = a;
            break;
        }
    info.presentMode = present_mode;
    info.clipped = VK_TRUE;
    info.oldSwapchain = old_swapchain;
    VkResult r = SwapchainFailTest() ? VK_ERROR_INITIALIZATION_FAILED
                                     : vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_);
    if (r != VK_SUCCESS && hdr_output_ != HdrOutput::Off) {
        LOGR("vulkan: %s swapchain rejected: %s; using SDR output",
             hdr_output_ == HdrOutput::Hdr10 ? "HDR10" : "extended sRGB linear", VkError(r));
        hdr_output_ = HdrOutput::Off;
        chosen = sdr;
        info.imageFormat = sdr.format;
        info.imageColorSpace = sdr.colorSpace;
        r = vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_);
    }
    if (old_swapchain) vkDestroySwapchainKHR(device_, old_swapchain, nullptr);
    if (r != VK_SUCCESS) {
        swapchain_ = VK_NULL_HANDLE;
        LOG_FIRST(8, "vulkan: vkCreateSwapchainKHR failed: %s; retrying next frame", VkError(r));
        return false;
    }
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    swapchain_images_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapchain_images_.data());
    extent_ = extent;
    swapchain_format_ = chosen.format;
    vsync_ = vsync;
    if (hdr_output_ != HdrOutput::Off) {
        LOGR("vulkan: %s output, %ux%u %s, peak %u nits",
             hdr_output_ == HdrOutput::Hdr10 ? "HDR10" : "extended sRGB linear", extent.width, extent.height,
             hdr_output_ == HdrOutput::Hdr10 ? "A2B10G10R10" : "FP16", display_peak_nits_);
        if (hdr_output_ == HdrOutput::Hdr10 && hdr_metadata_ && set_hdr_metadata_) {
            VkHdrMetadataEXT metadata{VK_STRUCTURE_TYPE_HDR_METADATA_EXT};
            metadata.displayPrimaryRed = {0.708f, 0.292f};
            metadata.displayPrimaryGreen = {0.170f, 0.797f};
            metadata.displayPrimaryBlue = {0.131f, 0.046f};
            metadata.whitePoint = {0.3127f, 0.3290f};
            metadata.maxLuminance = float(display_peak_nits_);
            metadata.minLuminance = 0.0f;
            metadata.maxContentLightLevel = float(display_peak_nits_);
            metadata.maxFrameAverageLightLevel = float(std::max(settings.hdr_paper_white, 80u));
            set_hdr_metadata_(device_, 1, &swapchain_, &metadata);
        }
    } else {
        LOGR("vulkan: SDR output, %ux%u format %d", extent.width, extent.height, int(swapchain_format_));
    }
    for (VkSemaphore s : render_done_)
        if (s) vkDestroySemaphore(device_, s, nullptr);
    render_done_.assign(swapchain_images_.size(), VK_NULL_HANDLE);
    for (VkSemaphore& s : render_done_) {
        VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
        if (vkCreateSemaphore(device_, &sem, nullptr, &s) != VK_SUCCESS) return false;
    }

    // gamma/sampling present pass and one framebuffer per back buffer
    VkAttachmentDescription color{};
    color.format = swapchain_format_;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &color;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = deps;
    if (vkCreateRenderPass(device_, &rp, nullptr, &swap_pass_) != VK_SUCCESS) return false;

    swap_views_.resize(count);
    swap_framebuffers_.resize(count);
    for (uint32_t i = 0; i < count; i++) {
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = swapchain_images_[i];
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = swapchain_format_;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &view, nullptr, &swap_views_[i]) != VK_SUCCESS) return false;
        VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
        fb.renderPass = swap_pass_;
        fb.attachmentCount = 1;
        fb.pAttachments = &swap_views_[i];
        fb.width = extent.width;
        fb.height = extent.height;
        fb.layers = 1;
        if (vkCreateFramebuffer(device_, &fb, nullptr, &swap_framebuffers_[i]) != VK_SUCCESS) return false;
    }
    // HDR composites the overlay from its own BGRA8 image; SDR draws it straight into the swapchain
    // pass, so the full-size image exists only with HDR on
    if (hdr_output_ != HdrOutput::Off)
        CreateUiTarget();
    else
        DestroyUiTarget();
    CreateUi();  // the Graphics overlay is optional; rendering continues without it
    return true;
}

bool VulkanBackend::CreateUi() {
    // its pipeline is built for one render pass: the swapchain's in SDR, the overlay image's in HDR
    const bool direct = hdr_output_ == HdrOutput::Off;
    const VkRenderPass pass = direct ? swap_pass_ : ui_pass_;
    if (ui_initialized_ || !pass || !ImGui::GetCurrentContext()) return true;
    std::lock_guard<std::recursive_mutex> guard(app::UiLock());
    const uint32_t images = uint32_t(swapchain_images_.size());
    ImGui_ImplVulkan_InitInfo info{};
    info.ApiVersion = VK_API_VERSION_1_1;
    info.Instance = instance_;
    info.PhysicalDevice = physical_;
    info.Device = device_;
    info.QueueFamily = graphics_family_;
    info.Queue = queue_;
    info.DescriptorPoolSize = 64;  // the backend creates its own image descriptor pool
    info.MinImageCount = images >= 2 ? 2 : images;
    info.ImageCount = images;
    info.PipelineInfoMain.RenderPass = pass;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&info)) {
        LOGE("vulkan: ImGui Vulkan backend failed to start");
        return false;
    }
    ui_initialized_ = true;
    ui_direct_ = direct;
    LOGR("vulkan: ImGui overlay ready (%s)", direct ? "drawn into the swapchain pass" : "HDR composite");
    return true;
}

void VulkanBackend::DestroyUi() {
    if (!ui_initialized_) return;
    std::lock_guard<std::recursive_mutex> guard(app::UiLock());
    ImGui_ImplVulkan_Shutdown();
    ui_initialized_ = false;
}

// The BGRA8 image ImGui renders into, and the pass that clears it and hands it to the HDR
// composite as a sampled image.
bool VulkanBackend::CreateUiTarget() {
    if (!extent_.width || !extent_.height) return false;
    // ImGui always draws into this BGRA8 image, in SDR too: its pipeline is built for this pass,
    // and SDR surfaces are not guaranteed to be BGRA8. The composite pass then puts it over the
    // swapchain image unchanged in SDR, or at paper white in HDR.
    if (ui_image_ && ui_w_ == extent_.width && ui_h_ == extent_.height && ui_pass_ && ui_fb_) return true;
    DestroyUiTarget();
    ui_w_ = extent_.width;
    ui_h_ = extent_.height;

    VkAttachmentDescription color{};
    color.format = VK_FORMAT_B8G8R8A8_UNORM;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &ref;
    VkSubpassDependency deps[2]{};
    deps[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    deps[0].dstSubpass = 0;
    deps[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].srcSubpass = 0;
    deps[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    deps[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    deps[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    deps[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
    deps[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    VkRenderPassCreateInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rp.attachmentCount = 1;
    rp.pAttachments = &color;
    rp.subpassCount = 1;
    rp.pSubpasses = &sub;
    rp.dependencyCount = 2;
    rp.pDependencies = deps;
    if (vkCreateRenderPass(device_, &rp, nullptr, &ui_pass_) != VK_SUCCESS) {
        LOGE("vulkan: HDR overlay render pass failed");
        return false;
    }

    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_B8G8R8A8_UNORM;
    info.extent = {ui_w_, ui_h_, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vkCreateImage(device_, &info, nullptr, &ui_image_) != VK_SUCCESS) {
        LOGE("vulkan: HDR overlay image %ux%u could not be created", ui_w_, ui_h_);
        DestroyUiTarget();
        return false;
    }
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, ui_image_, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mem_props_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            alloc.memoryTypeIndex = i;
            break;
        }
    if (vkAllocateMemory(device_, &alloc, nullptr, &ui_memory_) != VK_SUCCESS ||
        vkBindImageMemory(device_, ui_image_, ui_memory_, 0) != VK_SUCCESS) {
        DestroyUiTarget();
        return false;
    }
    ui_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = ui_image_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_B8G8R8A8_UNORM;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &view, nullptr, &ui_view_) != VK_SUCCESS) {
        DestroyUiTarget();
        return false;
    }
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = ui_pass_;
    fb.attachmentCount = 1;
    fb.pAttachments = &ui_view_;
    fb.width = ui_w_;
    fb.height = ui_h_;
    fb.layers = 1;
    if (vkCreateFramebuffer(device_, &fb, nullptr, &ui_fb_) != VK_SUCCESS) {
        DestroyUiTarget();
        return false;
    }
    return true;
}

void VulkanBackend::DestroyUiTarget() {
    if (ui_fb_) vkDestroyFramebuffer(device_, ui_fb_, nullptr);
    ui_fb_ = VK_NULL_HANDLE;
    if (ui_view_) vkDestroyImageView(device_, ui_view_, nullptr);
    ui_view_ = VK_NULL_HANDLE;
    if (ui_image_) vkDestroyImage(device_, ui_image_, nullptr);
    ui_image_ = VK_NULL_HANDLE;
    if (ui_memory_) vkFreeMemory(device_, ui_memory_, nullptr);
    ui_memory_ = VK_NULL_HANDLE;
    if (ui_pass_) vkDestroyRenderPass(device_, ui_pass_, nullptr);
    ui_pass_ = VK_NULL_HANDLE;
    ui_w_ = ui_h_ = 0;
    ui_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
}

void VulkanBackend::RenderUi() {
    if (!ui_initialized_) return;
    ScopeTimer timer(perf_, &perf_ui_ms_);
    const auto now = std::chrono::steady_clock::now();
    if (!ui_fps_time_.time_since_epoch().count()) {
        ui_fps_time_ = now;
        ui_fps_frames_ = 0;
    }
    ui_fps_frames_++;
    const double elapsed = std::chrono::duration<double>(now - ui_fps_time_).count();
    if (elapsed >= 0.5) {
        const double fps = double(ui_fps_frames_) / elapsed;
        ui_fps_ = ui_fps_ > 0.0 ? ui_fps_ * 0.5 + fps * 0.5 : fps;
        ui_fps_frames_ = 0;
        ui_fps_time_ = now;
    }

    std::lock_guard<std::recursive_mutex> guard(app::UiLock());
    ImGui_ImplVulkan_NewFrame();
    ImGui_ImplSDL3_NewFrame();
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(ui_fps_);
    ImGui::Render();
}

// The HDR overlay composite: ImGui's sRGB values are put over the encoded swapchain at paper
// white, with premultiplied-alpha over blending.
static const char* kHdrUiHlsl = R"(
[[vk::binding(0, 0)]] Texture2D<float4> uiTex;
[[vk::binding(1, 0)]] SamplerState uiSampler;
struct PC { float4 hdr; };
[[vk::push_constant]] ConstantBuffer<PC> pc;
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t;
    o.pos = float4(t * float2(2, 2) + float2(-1, -1), 0, 1);
    return o;
}
float3 SrgbToLinear(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);
    return lerp(lo, hi, step(0.04045, c));
}
float3 PqEncode(float3 nits) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    float3 y = pow(max(nits / 10000.0, 0.0), m1);
    return pow(max((c1 + c2 * y) / (1.0 + c3 * y), 0.0), m2);
}
static const float3x3 Rec709to2020 = float3x3(
    0.6274040, 0.3292820, 0.0433136,
    0.0690970, 0.9195400, 0.0113612,
    0.0163916, 0.0880132, 0.8955950);
float4 psmain(O i) : SV_Target {
    float4 c = uiTex.Sample(uiSampler, i.uv);
    // SDR: the over-blend reproduces ImGui's direct blend exactly.
    if (pc.hdr.x < 0.5) return c;
    // The target holds premultiplied sRGB (ImGui's over-blend into a cleared target). Undo the
    // premultiply before the non-linear decode, then premultiply again in the output encoding:
    // decoding premultiplied values made half-covered text edges and panels far too dark.
    const float a = saturate(c.a);
    const float3 straight = a > 1e-5 ? saturate(c.rgb / a) : float3(0, 0, 0);
    float3 lin = SrgbToLinear(straight) * pc.hdr.y;
    if (pc.hdr.x < 1.5) return float4(lin / 80.0 * a, a);
    // HDR10 blends in the PQ domain: an approximation for partial coverage, exact at 0 and 1.
    float3 rgb2020 = mul(Rec709to2020, lin);
    return float4(PqEncode(clamp(rgb2020, 0.0, 10000.0)) * a, a);
}
)";

bool VulkanBackend::CreateUiComposite() {
    if (!swap_pass_) return false;
    if (ui_pipeline_ && ui_pipeline_format_ == swapchain_format_) return true;
    if (ui_pipeline_) vkDestroyPipeline(device_, ui_pipeline_, nullptr);
    ui_pipeline_ = VK_NULL_HANDLE;
    ui_pipeline_format_ = VK_FORMAT_UNDEFINED;

    if (!ui_set_layout_) {
        VkDescriptorSetLayoutBinding bind[2]{};
        bind[0].binding = 0;
        bind[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bind[0].descriptorCount = 1;
        bind[0].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        bind[1].binding = 1;
        bind[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bind[1].descriptorCount = 1;
        bind[1].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 2;
        li.pBindings = bind;
        if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &ui_set_layout_) != VK_SUCCESS) return false;
        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 1}, {VK_DESCRIPTOR_TYPE_SAMPLER, 1}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 1;
        pi.poolSizeCount = 2;
        pi.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device_, &pi, nullptr, &ui_pool_) != VK_SUCCESS) return false;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = ui_pool_;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &ui_set_layout_;
        if (vkAllocateDescriptorSets(device_, &ai, &ui_set_) != VK_SUCCESS) return false;
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &ui_set_layout_;
        VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 4};
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(device_, &pl, nullptr, &ui_pipeline_layout_) != VK_SUCCESS) return false;
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        if (vkCreateSampler(device_, &si, nullptr, &ui_sampler_) != VK_SUCCESS) return false;
    }
    if (!ui_vs_) {
        ui_vs_ = CompileShader(kHdrUiHlsl, shaderc_vertex_shader, "vs");
        ui_ps_ = CompileShader(kHdrUiHlsl, shaderc_fragment_shader, "psmain");
        if (!ui_vs_ || !ui_ps_) return false;
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = 0xF;
    cb.blendEnable = VK_TRUE;
    cb.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cb.dstColorBlendFactor = VK_BLEND_FACTOR_ONE_MINUS_SRC_ALPHA;
    cb.colorBlendOp = VK_BLEND_OP_ADD;
    cb.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cb.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cb.alphaBlendOp = VK_BLEND_OP_ADD;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &cb;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dyn;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = ui_vs_;
    stages[0].pName = "vs";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ui_ps_;
    stages[1].pName = "psmain";
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = ui_pipeline_layout_;
    info.renderPass = swap_pass_;
    if (CreatePipelines(1, &info, nullptr, &ui_pipeline_) != VK_SUCCESS)
        return false;
    ui_pipeline_format_ = swapchain_format_;
    return true;
}

bool VulkanBackend::CreateCommands() {    VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
    pool.queueFamilyIndex = graphics_family_;
    pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
    if (vkCreateCommandPool(device_, &pool, nullptr, &command_pool_) != VK_SUCCESS) return false;
    VkCommandBufferAllocateInfo alloc{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    alloc.commandPool = command_pool_;
    alloc.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    alloc.commandBufferCount = 1;
    if (vkAllocateCommandBuffers(device_, &alloc, &cmd_) != VK_SUCCESS) return false;
    VkSemaphoreCreateInfo sem{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
    VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
    fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
    if (vkCreateSemaphore(device_, &sem, nullptr, &image_available_) != VK_SUCCESS) return false;
    if (vkCreateFence(device_, &fence, nullptr, &fence_) != VK_SUCCESS) return false;
    VkQueryPoolCreateInfo q{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
    q.queryType = VK_QUERY_TYPE_OCCLUSION;
    q.queryCount = kOcclusionQueries;
    return vkCreateQueryPool(device_, &q, nullptr, &query_pool_) == VK_SUCCESS;
}

bool VulkanBackend::CreateUploadBuffer(uint64_t size, VkBuffer& buffer, VkDeviceMemory& memory, uint8_t*& ptr) {
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_VERTEX_BUFFER_BIT | VK_BUFFER_USAGE_INDEX_BUFFER_BIT |
                 VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    if (vkCreateBuffer(device_, &info, nullptr, &buffer) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, buffer, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    bool found = false;
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; i++) {
        const auto flags = mem_props_.memoryTypes[i].propertyFlags;
        if ((req.memoryTypeBits & (1u << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            alloc.memoryTypeIndex = i;
            found = true;
            break;
        }
    }
    if (!found || vkAllocateMemory(device_, &alloc, nullptr, &memory) != VK_SUCCESS) return false;
    vkBindBufferMemory(device_, buffer, memory, 0);
    return vkMapMemory(device_, memory, 0, VK_WHOLE_SIZE, 0, reinterpret_cast<void**>(&ptr)) == VK_SUCCESS;
}

bool VulkanBackend::CreateUpload() {
    if (!CreateUploadBuffer(kUploadSize, upload_, upload_memory_, upload_ptr_)) return false;
    upload_size_ = kUploadSize;
    return true;
}

bool VulkanBackend::CreatePresent() {
    for (int i = 0; i < 256; i++) {
        gamma_ramp_[i] = uint8_t(i);
        gamma_ramp_[256 + i] = uint8_t(i);
        gamma_ramp_[512 + i] = uint8_t(i);
    }
    gamma_dirty_ = true;

    VkImageCreateInfo gi{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    gi.imageType = VK_IMAGE_TYPE_2D;
    gi.format = VK_FORMAT_R8G8B8A8_UNORM;
    gi.extent = {256, 1, 1};
    gi.mipLevels = 1;
    gi.arrayLayers = 1;
    gi.samples = VK_SAMPLE_COUNT_1_BIT;
    gi.tiling = VK_IMAGE_TILING_OPTIMAL;
    gi.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(device_, &gi, nullptr, &gamma_image_) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, gamma_image_, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mem_props_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            alloc.memoryTypeIndex = i;
            break;
        }
    if (vkAllocateMemory(device_, &alloc, nullptr, &gamma_memory_) != VK_SUCCESS) return false;
    vkBindImageMemory(device_, gamma_image_, gamma_memory_, 0);
    VkImageViewCreateInfo gv{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    gv.image = gamma_image_;
    gv.viewType = VK_IMAGE_VIEW_TYPE_2D;
    gv.format = VK_FORMAT_R8G8B8A8_UNORM;
    gv.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &gv, nullptr, &gamma_view_) != VK_SUCCESS) return false;

    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    if (vkCreateSampler(device_, &si, nullptr, &present_sampler_) != VK_SUCCESS) return false;
    // depth reads for the distance blur: depth formats are not required to support linear filtering
    si.magFilter = si.minFilter = VK_FILTER_NEAREST;
    if (vkCreateSampler(device_, &si, nullptr, &present_point_sampler_) != VK_SUCCESS) return false;

    // 0 frame, 1 gamma ramp, 2 depth, 3 and 4 samplers, 5 the bloom chain
    VkDescriptorSetLayoutBinding bind[6]{};
    for (int i = 0; i < 3; i++) {
        bind[i].binding = uint32_t(i);
        bind[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        bind[i].descriptorCount = 1;
        bind[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    for (int i = 0; i < 2; i++) {
        bind[3 + i].binding = uint32_t(3 + i);
        bind[3 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        bind[3 + i].descriptorCount = 1;
        bind[3 + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    }
    bind[5].binding = 5;
    bind[5].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    bind[5].descriptorCount = 1;
    bind[5].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 6;
    li.pBindings = bind;
    if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &present_set_layout_) != VK_SUCCESS) return false;

    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 4},
                                     {VK_DESCRIPTOR_TYPE_SAMPLER, 2}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = 1;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    if (vkCreateDescriptorPool(device_, &pi, nullptr, &present_pool_) != VK_SUCCESS) return false;
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = present_pool_;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &present_set_layout_;
    if (vkAllocateDescriptorSets(device_, &ai, &present_set_) != VK_SUCCESS) return false;

    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &present_set_layout_;
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 16};  // contrast, bloom, dof, source size, near/far, HDR, source rect
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device_, &pl, nullptr, &present_pipeline_layout_) != VK_SUCCESS) return false;

    static const char* kPresentHlsl = R"(
[[vk::binding(0, 0)]] Texture2D<float4> frameTex;
[[vk::binding(1, 0)]] Texture2D<float4> gammaTex;
[[vk::binding(2, 0)]] Texture2D<float4> depthTex;
[[vk::binding(3, 0)]] SamplerState frameSampler;
[[vk::binding(4, 0)]] SamplerState depthSampler;
[[vk::binding(5, 0)]] Texture2D<float4> bloomTex;
// chain: the bloom chain is bound (bloomTex); without it BloomAt taps the frame
struct PC { float pc; float bloom; float dof; float chain; float2 src; float2 zrange; float4 hdr; float4 rect; };
[[vk::push_constant]] ConstantBuffer<PC> pc;
struct O { float4 pos : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.uv = t;
    o.pos = float4(t * float2(2, 2) + float2(-1, -1), 0, 1);
    return o;
}
float3 ToGamma(float3 c) {
    return float3(gammaTex.Sample(frameSampler, float2(c.r, 0.5)).r,
                  gammaTex.Sample(frameSampler, float2(c.g, 0.5)).g,
                  gammaTex.Sample(frameSampler, float2(c.b, 0.5)).b);
}
// Exact piecewise sRGB EOTF, for expanding the finished SDR frame into linear light.
float3 SrgbToLinear(float3 c) {
    float3 lo = c / 12.92;
    float3 hi = pow(max((c + 0.055) / 1.055, 0.0), 2.4);
    return lerp(lo, hi, step(0.04045, c));
}
// ST.2084 output transfer function: linear nits to PQ code values.
float3 PqEncode(float3 nits) {
    const float m1 = 2610.0 / 16384.0;
    const float m2 = 2523.0 / 4096.0 * 128.0;
    const float c1 = 3424.0 / 4096.0;
    const float c2 = 2413.0 / 4096.0 * 32.0;
    const float c3 = 2392.0 / 4096.0 * 32.0;
    float3 y = pow(max(nits / 10000.0, 0.0), m1);
    return pow(max((c1 + c2 * y) / (1.0 + c3 * y), 0.0), m2);
}
static const float3x3 Rec709to2020 = float3x3(
    0.6274040, 0.3292820, 0.0433136,
    0.0690970, 0.9195400, 0.0113612,
    0.0163916, 0.0880132, 0.8955950);
// Inverse tone map the highlights only: below the knee the SDR colour is unchanged, above it
// the luminance expands towards P while the hue is preserved.
// l2 = l + t*t*(P - 1), so l2(knee) = knee with slope 1, l2(1) = P, and P = 1 is identity.
float3 HdrHighlights(float3 lin, float paper_white, float peak, float strength) {
    const float knee = 0.75;
    const float3 luma = float3(0.2126, 0.7152, 0.0722);
    const float l = dot(lin, luma);
    const float P = lerp(1.0, max(peak, paper_white) / max(paper_white, 1.0), saturate(strength));
    if (l <= knee) return lin;  // exactly unchanged, down to black
    const float t = (l - knee) / (1.0 - knee);
    const float l2 = l + t * t * (P - 1.0);
    return lin * (l2 / l);
}
// A single-pass approximation of the PC release's bright-pass bloom. Each tap first averages a
// small box, so thin chrome/specular edges are diluted before the threshold while lamps and neon
// still pass.
float3 BrightAt(float2 uv) {
    float2 q = pc.src * 0.5;
    float3 c = (frameTex.Sample(frameSampler, uv + float2(-q.x, -q.y)).rgb +
                frameTex.Sample(frameSampler, uv + float2( q.x, -q.y)).rgb +
                frameTex.Sample(frameSampler, uv + float2(-q.x,  q.y)).rgb +
                frameTex.Sample(frameSampler, uv + float2( q.x,  q.y)).rgb) * 0.25;
    c = ToGamma(c);
    return max(c - 0.94, 0.0);
}
float3 BloomAt(float2 uv) {
    if (pc.chain > 0.0) return bloomTex.SampleLevel(frameSampler, uv, 0).rgb;
    static const float2 dirs[8] = {
        float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1),
        float2(0.7071, 0.7071), float2(-0.7071, 0.7071),
        float2(0.7071, -0.7071), float2(-0.7071, -0.7071)
    };
    float2 t = pc.src * 8.0;
    float3 b = 0;
    [unroll] for (int k = 0; k < 8; k++) b += BrightAt(uv + dirs[k] * t);
    b += BrightAt(uv);
    return b * (1.0 / 9.0);
}
// PC distance blur: 13-tap hex blur weighted by the linearised scene depth, matching the D3D12
// present shader. The Vulkan depth buffer keeps the guest's 0..1 convention (near 0), so no
// reversed-Z undo is needed here.
float3 DistanceBlur(float2 uv, float3 color) {
    static const float2 hex[13] = {
        float2(0, 0), float2(0.5, 0.8660), float2(-0.5, 0.8660), float2(-1, 0),
        float2(-0.5, -0.8660), float2(0.5, -0.8660), float2(1, 0),
        float2(1, 1.7320), float2(-1, 1.7320), float2(-2, 0),
        float2(-1, -1.7320), float2(1, -1.7320), float2(2, 0)
    };
    float raw = saturate(depthTex.SampleLevel(depthSampler, saturate(uv), 0).r);
    float world = pc.zrange.x * pc.zrange.y /
                  max(pc.zrange.y - raw * (pc.zrange.y - pc.zrange.x), 1e-4);
    float coef = saturate((world - 40.0) / (65.0 - 40.0));
    if (coef <= 0.0) return color;
    float3 sum = 0;
    float count = 0;
    [unroll] for (int i = 0; i < 13; i++) {
        float wgt = exp(-dot(hex[i], hex[i]) * 0.5);
        sum += frameTex.SampleLevel(frameSampler, uv + hex[i] * pc.src * coef, 0).rgb * wgt;
        count += wgt;
    }
    return sum / count;
}

// Match the PC release's tone on the finished frame (same curve as the D3D12 backend).
float3 PcContrast(float3 c) {
    const float3 L = float3(0.2126, 0.7152, 0.0722);
    float l = dot(c, L);
    const float pivot = 0.114;
    float l2 = pivot + (l - pivot) * 1.35;
    // After the Xbox gamma ramp the intro steps are already close to black. Lift that range while
    // anchoring black and the original contrast pivot. The toe caps the lift at about 2x.
    if (l < pivot) {
        const float t = saturate(l / pivot);
        const float k = 1.5936;  // k / (1 - exp(-k)) = 2
        l2 = pivot * (1.0 - exp(-k * t)) / (1.0 - exp(-k));
    }
    float over = max(l2 - 0.80, 0.0);
    l2 = l2 > 0.80 ? 0.80 + over / (1.0 + over * 2.0) : l2;
    l2 = saturate(l2);
    c *= l2 / max(l, 1e-4);
    return saturate(c);
}
float4 psmain(O i) : SV_Target {
    i.uv = pc.rect.xy + i.uv * pc.rect.zw;  // the part of the frame shown: all of it, or a movie's picture
    float4 c = frameTex.Sample(frameSampler, i.uv);
    if (pc.dof > 0.0) c.rgb = DistanceBlur(i.uv, c.rgb);
    c.rgb = ToGamma(c.rgb);
    if (pc.hdr.x > 0.5) {
        // Match the SDR order through PC contrast: gamma, then bloom, then contrast. The part of
        // the bloom above white is carried separately so it can exceed white, scaled by highlights.
        float3 g = c.rgb;
        float3 b = pc.bloom > 0.0 ? BloomAt(i.uv) * (1.2 * pc.bloom) : 0;
        g += b;
        if (pc.pc > 0.0) g = lerp(g, PcContrast(g), pc.pc);
        float3 over = max(c.rgb + b - 1.0, 0.0);
        float3 lin = SrgbToLinear(saturate(g));
        lin = HdrHighlights(lin, pc.hdr.y, pc.hdr.z, pc.hdr.w) + over * (2.4 * pc.hdr.w);
        if (pc.hdr.x < 1.5) return float4(lin * (pc.hdr.y / 80.0), c.a);
        float3 rgb2020 = mul(Rec709to2020, lin);
        float3 nits = clamp(rgb2020 * pc.hdr.y, 0.0, 10000.0);
        return float4(PqEncode(nits), c.a);
    }
    if (pc.bloom > 0.0) c.rgb += BloomAt(i.uv) * (1.2 * pc.bloom);
    if (pc.pc > 0.0) {
        c.rgb = PcContrast(c.rgb);
        static const float kDither[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
        int2 dp = int2(i.pos.xy) % 4;
        float d = (kDither[dp.y * 4 + dp.x] + 0.5) / 16.0 - 0.5;
        c.rgb = saturate(c.rgb + d / 255.0);
    }
    return c;
}
)";
    present_vs_ = CompileShader(kPresentHlsl, shaderc_vertex_shader, "vs");
    present_ps_ = CompileShader(kPresentHlsl, shaderc_fragment_shader, "psmain");
    return present_vs_ != VK_NULL_HANDLE && present_ps_ != VK_NULL_HANDLE;
}

void VulkanBackend::Shutdown() {
    if (device_) vkDeviceWaitIdle(device_);
    DestroyUi();
    // no explicit teardown of every cache: the process is exiting, and freeing in
    // dependency order here would be much longer than the objects are worth.
    if (device_) {
        if (ui_pipeline_) vkDestroyPipeline(device_, ui_pipeline_, nullptr);
        if (ui_pipeline_layout_) vkDestroyPipelineLayout(device_, ui_pipeline_layout_, nullptr);
        if (ui_sampler_) vkDestroySampler(device_, ui_sampler_, nullptr);
        if (ui_pool_) vkDestroyDescriptorPool(device_, ui_pool_, nullptr);
        if (ui_set_layout_) vkDestroyDescriptorSetLayout(device_, ui_set_layout_, nullptr);
        for (VkShaderModule m : {ui_vs_, ui_ps_})
            if (m) vkDestroyShaderModule(device_, m, nullptr);
        DestroyUiTarget();
        if (upload_) vkDestroyBuffer(device_, upload_, nullptr);
        if (upload_memory_) vkFreeMemory(device_, upload_memory_, nullptr);
        if (frame_image_) vkDestroyImage(device_, frame_image_, nullptr);
        if (frame_memory_) vkFreeMemory(device_, frame_memory_, nullptr);
        if (fence_) vkDestroyFence(device_, fence_, nullptr);
        if (query_pool_) vkDestroyQueryPool(device_, query_pool_, nullptr);
        if (image_available_) vkDestroySemaphore(device_, image_available_, nullptr);
        for (VkSemaphore s : render_done_)
            if (s) vkDestroySemaphore(device_, s, nullptr);
        render_done_.clear();
        if (command_pool_) vkDestroyCommandPool(device_, command_pool_, nullptr);
        if (swapchain_) vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        for (VkPipeline p : retired_ao_pipelines_)
            if (p) vkDestroyPipeline(device_, p, nullptr);
        retired_ao_pipelines_.clear();
        if (ao_gtao_pipe_) vkDestroyPipeline(device_, ao_gtao_pipe_, nullptr);
        if (ao_occ_pipe_) vkDestroyPipeline(device_, ao_occ_pipe_, nullptr);
        for (VkShaderModule m : {ao_gtao_vs_, ao_gtao_ps_, ao_occ_vs_, ao_occ_ps_})
            if (m) vkDestroyShaderModule(device_, m, nullptr);
        for (VkFramebuffer fb : {ao_prepass_fb_, ao_half_fb_, ao_occ_fb_})
            if (fb) vkDestroyFramebuffer(device_, fb, nullptr);
        DestroyAoImage(ao_depth_);
        DestroyAoImage(ao_dist_);
        DestroyAoImage(ao_half_);
        DestroyAoImage(ao_occ_);
        DestroyAoImage(ao_zero_);
        for (auto& [key, pass] : ao_passes_)
            if (pass) vkDestroyRenderPass(device_, pass, nullptr);
        ao_passes_.clear();
        if (ao_color_pass_) vkDestroyRenderPass(device_, ao_color_pass_, nullptr);
        // Pools own the sets; destroy them before their set layouts. The scene pipeline layout
        // still names ao_occ_set_layout_, so that layout intentionally lives to process exit.
        if (ao_pool_) vkDestroyDescriptorPool(device_, ao_pool_, nullptr);
        if (ao_occ_pool_) vkDestroyDescriptorPool(device_, ao_occ_pool_, nullptr);
        if (ao_pipeline_layout_) vkDestroyPipelineLayout(device_, ao_pipeline_layout_, nullptr);
        if (ao_set_layout_) vkDestroyDescriptorSetLayout(device_, ao_set_layout_, nullptr);
        SavePipelineCache(true);
        if (pipeline_cache_) vkDestroyPipelineCache(device_, pipeline_cache_, nullptr);
        vkDestroyDevice(device_, nullptr);
    }
    if (surface_) vkDestroySurfaceKHR(instance_, surface_, nullptr);
    if (instance_) vkDestroyInstance(instance_, nullptr);
}

void VulkanBackend::BeginRecord() {
    if (recording_) return;
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo info{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    info.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &info);
    if (query_reset_ && query_pool_) {
        vkCmdResetQueryPool(cmd_, query_pool_, 0, kOcclusionQueries);
        query_reset_ = false;
    }
    bound_pipe_ = VK_NULL_HANDLE;
    viewport_valid_ = false;
    recording_ = true;
}

void VulkanBackend::Fail(const char* what, VkResult r) {
    if (failed_) return;
    failed_ = true;
    LOGE("vulkan: %s failed: %s; stopping presentation", what, VkError(r));
    if (r == VK_ERROR_DEVICE_LOST || r == VK_ERROR_SURFACE_LOST_KHR) {
        LOGE("vulkan: %s is unrecoverable", what);
        kernel::Shutdown(1);
    }
}

void VulkanBackend::EndRecord(bool wait) {
    if (!recording_) return;
    if (failed_) {
        // A prior failure left the fence unsignalled: finish the buffer but never wait on it.
        deferred_.clear();
        deferred_plain_.clear();
        FlushPass();
        vkEndCommandBuffer(cmd_);
        recording_ = false;
        return;
    }
    // Any submit must happen after the deferred scene commands: a texture upload or a copy
    // recorded before the replay would otherwise change what the pending draws read.
    if (!flushing_deferred_ && !skip_deferred_flush_ && DeferredPending()) FlushDeferredScene();
    FlushPass();
    vkEndCommandBuffer(cmd_);
    recording_ = false;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_;
    VkResult r = vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    if (r != VK_SUCCESS) {
        Fail("wait for fence", r);
        return;
    }
    r = vkResetFences(device_, 1, &fence_);
    if (r != VK_SUCCESS) {
        Fail("reset fence", r);
        return;
    }
    const uint64_t serial = ++submit_serial_;
    r = vkQueueSubmit(queue_, 1, &submit, fence_);
    if (r != VK_SUCCESS) {
        Fail("submit", r);
        return;
    }
    if (wait) {
        r = vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
        if (r != VK_SUCCESS) {
            Fail("wait for submitted frame", r);
            return;
        }
        completed_serial_.store(serial);
    }
}

void VulkanBackend::ResetFrame() {
    upload_used_ = 0;
    draw_set_next_ = 0;
    const app::Settings settings = app::Snapshot();  // one copy for the whole reset
    ResetDeferred(settings.ao);
    scene_dof_ = settings.dof;
    scene_depth_.clear();
    // the last frame's GPU work is done (present waited on the fence): pack images and late-lock
    // buffers released earlier can go now
    for (Tex* t : pack_textures_retired_) {
        if (t->view) vkDestroyImageView(device_, t->view, nullptr);
        if (t->image) vkDestroyImage(device_, t->image, nullptr);
        if (t->memory) vkFreeMemory(device_, t->memory, nullptr);
        delete t;
    }
    pack_textures_retired_.clear();
    ReleaseOldTextures();
    // framebuffers reference the views below: destroy them first, then the views and images
    for (VkFramebuffer fb : retired_framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    retired_framebuffers_.clear();
    for (Tex* tx : retired_borrowed_) delete tx;
    retired_borrowed_.clear();
    for (RetiredGpu& r : retired_gpu_) {
        if (r.sample_view && r.sample_view != r.resolved_view) vkDestroyImageView(device_, r.sample_view, nullptr);
        if (r.resolved_view) vkDestroyImageView(device_, r.resolved_view, nullptr);
        if (r.view) vkDestroyImageView(device_, r.view, nullptr);
        if (r.fb_view) vkDestroyImageView(device_, r.fb_view, nullptr);
        if (r.fb_tex) delete r.fb_tex;
        if (r.fb_image) vkDestroyImage(device_, r.fb_image, nullptr);
        if (r.fb_memory) vkFreeMemory(device_, r.fb_memory, nullptr);
        if (r.resolved_image) vkDestroyImage(device_, r.resolved_image, nullptr);
        if (r.resolved_memory) vkFreeMemory(device_, r.resolved_memory, nullptr);
        if (r.image) vkDestroyImage(device_, r.image, nullptr);
        if (r.memory) vkFreeMemory(device_, r.memory, nullptr);
    }
    retired_gpu_.clear();
    for (auto& [buffer, memory] : late_retired_) {
        if (buffer) vkDestroyBuffer(device_, buffer, nullptr);
        if (memory) vkFreeMemory(device_, memory, nullptr);
    }
    late_retired_.clear();
    if (trace_) frame_tex_.clear();
    query_next_ = 0;
    counted_.clear();
    query_reset_ = true;
    // present already waited for the fence, so these frame buffers are no longer read
    for (auto& [buffer, memory] : upload_retired_) {
        vkDestroyBuffer(device_, buffer, nullptr);
        vkFreeMemory(device_, memory, nullptr);
    }
    upload_retired_.clear();
    for (auto& [buffer, memory] : staging_pending_) {
        vkDestroyBuffer(device_, buffer, nullptr);
        vkFreeMemory(device_, memory, nullptr);
    }
    staging_pending_.clear();
}

void VulkanBackend::GetReport(uint32_t report) {
    if (!report) return;
    LOGR("linux: GetReport %08X recording %d pass %d queries %zu", report, int(recording_), int(pass_ != VK_NULL_HANDLE),
         counted_.size());
    if (failed_) return;  // no submission will complete the queries
    // the console's GPU fills the report after the counted draws; finish them first
    if (recording_)
        EndRecord(true);
    else {
        const VkResult r = vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
        if (r != VK_SUCCESS) Fail("wait for report fence", r);
    }
    if (failed_) return;
    double pixels = 0.0;
    const double samples_per_pixel =
        double(msaa_) * double(scale_) * double(scale_) * std::max(1.0, double(stretch_));
    for (uint32_t q : counted_) {
        uint64_t n = 0;
        if (vkGetQueryPoolResults(device_, query_pool_, q, 1, sizeof(n), &n, sizeof(n),
                                  VK_QUERY_RESULT_64_BIT | VK_QUERY_RESULT_WAIT_BIT) == VK_SUCCESS)
            pixels += double(n) / samples_per_pixel;
    }
    // the console's 16 byte visibility report: time, high time, count, then 0 for done
    kernel::W32(report + 0, uint32_t(frames_));
    kernel::W32(report + 4, 0);
    kernel::W32(report + 8, uint32_t(std::lround(pixels)));
    std::atomic_thread_fence(std::memory_order_release);
    kernel::W32(report + 12, 0);
}

void VulkanBackend::Transition(VkImage image, VkImageLayout& layout, VkImageLayout want, VkFormat format,
                               uint32_t levels, uint32_t layers) {
    if (layout == want) return;
    VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT;
    const bool stencil_format = format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    const bool depth_format = stencil_format || format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM;
    if (stencil_format)
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
    else if (depth_format)
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    else if (format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM)
        aspect = VK_IMAGE_ASPECT_DEPTH_BIT;
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.srcAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    b.oldLayout = layout;
    b.newLayout = want;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.image = image;
    b.subresourceRange = {aspect, 0, levels, 0, layers};
    vkCmdPipelineBarrier(cmd_, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
    layout = want;
}

VulkanBackend::UploadAlloc VulkanBackend::AllocUpload(uint64_t size, uint64_t align, void** cpu) {
    if (size > upload_size_) {
        // Grow the ring instead of ending the frame's command buffer: the pass may be open.
        uint64_t next = std::max<uint64_t>(kUploadSize, upload_size_ * 2);
        while (next < size + align) next *= 2;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint8_t* ptr = nullptr;
        if (!CreateUploadBuffer(next, buffer, memory, ptr)) return {};
        if (upload_) upload_retired_.emplace_back(upload_, upload_memory_);
        upload_ = buffer;
        upload_memory_ = memory;
        upload_ptr_ = ptr;
        upload_size_ = next;
        upload_used_ = 0;
        return AllocUpload(size, align, cpu);
    }
    const uint64_t off = (upload_used_ + align - 1) & ~(align - 1);
    if (off + size > upload_size_) {
        uint64_t next = upload_size_ * 2;
        while (next < off + size) next *= 2;
        VkBuffer buffer = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        uint8_t* ptr = nullptr;
        if (!CreateUploadBuffer(next, buffer, memory, ptr)) return {};
        upload_retired_.emplace_back(upload_, upload_memory_);
        upload_ = buffer;
        upload_memory_ = memory;
        upload_ptr_ = ptr;
        upload_size_ = next;
        upload_used_ = 0;
        return AllocUpload(size, align, cpu);
    }
    upload_used_ = off + size;
    if (cpu) *cpu = upload_ptr_ + off;
    return {upload_, off};
}

bool VulkanBackend::EnsureFrameImage(uint32_t w, uint32_t h) {
    if (frame_image_ && frame_w_ == w && frame_h_ == h) return true;
    if (frame_image_) {
        if (frame_view_) vkDestroyImageView(device_, frame_view_, nullptr);
        frame_view_ = VK_NULL_HANDLE;
        vkDestroyImage(device_, frame_image_, nullptr);
        vkFreeMemory(device_, frame_memory_, nullptr);
        frame_image_ = VK_NULL_HANDLE;
    }
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = VK_FORMAT_R8G8B8A8_UNORM;
    info.extent = {w, h, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    if (vkCreateImage(device_, &info, nullptr, &frame_image_) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, frame_image_, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mem_props_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            alloc.memoryTypeIndex = i;
            break;
        }
    if (vkAllocateMemory(device_, &alloc, nullptr, &frame_memory_) != VK_SUCCESS) return false;
    vkBindImageMemory(device_, frame_image_, frame_memory_, 0);
    frame_layout_ = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = frame_image_;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = VK_FORMAT_R8G8B8A8_UNORM;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &view, nullptr, &frame_view_) != VK_SUCCESS) return false;
    frame_w_ = w;
    frame_h_ = h;
    return true;
}

bool VulkanBackend::EnsureTraceBuffer(VkDeviceSize size) {
    if (trace_buffer_ && trace_size_ >= size) return true;
    if (trace_buffer_) {
        vkUnmapMemory(device_, trace_memory_);
        vkDestroyBuffer(device_, trace_buffer_, nullptr);
        vkFreeMemory(device_, trace_memory_, nullptr);
        trace_buffer_ = VK_NULL_HANDLE;
        trace_memory_ = VK_NULL_HANDLE;
        trace_ptr_ = nullptr;
        trace_size_ = 0;
    }
    VkBufferCreateInfo bi{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bi.size = size;
    bi.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bi.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateBuffer(device_, &bi, nullptr, &trace_buffer_) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(device_, trace_buffer_, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    bool found = false;
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount && !found; i++) {
        const VkMemoryPropertyFlags flags = mem_props_.memoryTypes[i].propertyFlags;
        if ((req.memoryTypeBits & (1u << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            alloc.memoryTypeIndex = i;
            found = true;
        }
    }
    if (!found || vkAllocateMemory(device_, &alloc, nullptr, &trace_memory_) != VK_SUCCESS) return false;
    vkBindBufferMemory(device_, trace_buffer_, trace_memory_, 0);
    if (vkMapMemory(device_, trace_memory_, 0, size, 0, reinterpret_cast<void**>(&trace_ptr_)) != VK_SUCCESS)
        return false;
    trace_size_ = size;
    return true;
}

// PART3

VkRenderPass VulkanBackend::RenderPassFor(VkFormat color, VkFormat depth) {
    uint64_t key = uint64_t(color) | (uint64_t(depth) << 32);
    auto it = passes_.find(key);
    if (it != passes_.end()) return it->second;
    VkAttachmentDescription att[2]{};
    VkAttachmentReference refs[2]{};
    uint32_t n = 0;
    if (color != VK_FORMAT_UNDEFINED) {
        att[n].format = color;
        att[n].samples = VK_SAMPLE_COUNT_1_BIT;
        att[n].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[n].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[n].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        refs[0] = {n++, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    }
    uint32_t depth_ref = VK_ATTACHMENT_UNUSED;
    if (depth != VK_FORMAT_UNDEFINED) {
        att[n].format = depth;
        att[n].samples = VK_SAMPLE_COUNT_1_BIT;
        att[n].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[n].stencilLoadOp = depth_has_stencil_ ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[n].stencilStoreOp = depth_has_stencil_ ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[n].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[n].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_ref = n;
        refs[1] = {n++, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    }
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = color != VK_FORMAT_UNDEFINED ? 1u : 0u;
    sub.pColorAttachments = color != VK_FORMAT_UNDEFINED ? &refs[0] : nullptr;
    sub.pDepthStencilAttachment = depth != VK_FORMAT_UNDEFINED ? &refs[1] : nullptr;
    VkSubpassDependency dep[2]{};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep[0].dstStageMask = dep[0].srcStageMask | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[0].dstAccessMask = dep[0].srcAccessMask | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = dep[0].dstStageMask;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[1].srcAccessMask = dep[0].dstAccessMask;
    dep[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = n;
    info.pAttachments = att;
    info.subpassCount = 1;
    info.pSubpasses = &sub;
    info.dependencyCount = 2;
    info.pDependencies = dep;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(device_, &info, nullptr, &pass) != VK_SUCCESS) return VK_NULL_HANDLE;
    passes_[key] = pass;
    return pass;
}

VkRenderPass VulkanBackend::RenderPassForMs(VkFormat color, VkFormat depth, uint32_t samples) {
    uint64_t key = uint64_t(color) | (uint64_t(depth) << 32) | (uint64_t(samples) << 48) | (1ull << 63) |
                   (msaa_store_ ? (1ull << 62) : 0);
    auto it = passes_.find(key);
    if (it != passes_.end()) return it->second;

    VkAttachmentDescription2 att[4]{};
    uint32_t n = 0;
    uint32_t color_ref = VK_ATTACHMENT_UNUSED, color_res = VK_ATTACHMENT_UNUSED;
    uint32_t depth_ref = VK_ATTACHMENT_UNUSED, depth_res = VK_ATTACHMENT_UNUSED;
    if (color != VK_FORMAT_UNDEFINED) {
        att[n] = {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
        att[n].format = color;
        att[n].samples = VkSampleCountFlagBits(samples);
        att[n].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        // Deferred replay closes and reopens the scene pass; per-attachment DONT_CARE discarded
        // the multisample colour/depth that the reopened pass needs.
        att[n].storeOp = msaa_store_ ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[n].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        att[n].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
        color_ref = n++;
        att[n] = {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
        att[n].format = color;
        att[n].samples = VK_SAMPLE_COUNT_1_BIT;
        att[n].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[n].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[n].finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        color_res = n++;
    }
    if (depth != VK_FORMAT_UNDEFINED) {
        att[n] = {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
        att[n].format = depth;
        att[n].samples = VkSampleCountFlagBits(samples);
        att[n].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att[n].storeOp = msaa_store_ ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[n].stencilLoadOp = depth_has_stencil_ ? VK_ATTACHMENT_LOAD_OP_LOAD : VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[n].stencilStoreOp = msaa_store_ && depth_has_stencil_ ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[n].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[n].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_ref = n++;
        att[n] = {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
        att[n].format = depth;
        att[n].samples = VK_SAMPLE_COUNT_1_BIT;
        att[n].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[n].stencilStoreOp = depth_has_stencil_ ? VK_ATTACHMENT_STORE_OP_STORE : VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[n].initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        att[n].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        depth_res = n++;
    }

    VkAttachmentReference2 cref{}, cres{}, dref{}, dres{};
    cref = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
    cref.attachment = color_ref;
    cref.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    cres = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
    cres.attachment = color_res;
    cres.layout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    dref = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
    dref.attachment = depth_ref;
    dref.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    dres = {VK_STRUCTURE_TYPE_ATTACHMENT_REFERENCE_2};
    dres.attachment = depth_res;
    dres.layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;

    VkSubpassDescriptionDepthStencilResolve dsr{
        VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_DEPTH_STENCIL_RESOLVE};
    dsr.pDepthStencilResolveAttachment = depth != VK_FORMAT_UNDEFINED ? &dres : nullptr;
    dsr.depthResolveMode = depth_resolve_mode_;
    dsr.stencilResolveMode = depth_has_stencil_ ? VK_RESOLVE_MODE_SAMPLE_ZERO_BIT : VK_RESOLVE_MODE_NONE;

    VkSubpassDescription2 sub{VK_STRUCTURE_TYPE_SUBPASS_DESCRIPTION_2};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = color != VK_FORMAT_UNDEFINED ? 1u : 0u;
    sub.pColorAttachments = color != VK_FORMAT_UNDEFINED ? &cref : nullptr;
    sub.pResolveAttachments = color != VK_FORMAT_UNDEFINED ? &cres : nullptr;
    sub.pDepthStencilAttachment = depth != VK_FORMAT_UNDEFINED ? &dref : nullptr;
    sub.pNext = depth != VK_FORMAT_UNDEFINED ? &dsr : nullptr;

    VkSubpassDependency2 dep[2]{};
    for (auto& d : dep) d.sType = VK_STRUCTURE_TYPE_SUBPASS_DEPENDENCY_2;
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep[0].dstStageMask = dep[0].srcStageMask | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[0].dstAccessMask = dep[0].srcAccessMask | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = dep[0].dstStageMask;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[1].srcAccessMask = dep[0].dstAccessMask;
    dep[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;

    VkRenderPassCreateInfo2 info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO_2};
    info.attachmentCount = n;
    info.pAttachments = att;
    info.subpassCount = 1;
    info.pSubpasses = &sub;
    info.dependencyCount = 2;
    info.pDependencies = dep;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (!create_render_pass2_ || create_render_pass2_(device_, &info, nullptr, &pass) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    passes_[key] = pass;
    return pass;
}

VkFramebuffer VulkanBackend::FramebufferFor(Target* rt, Target* ds) {
    VkImageView attach[4]{};
    uint32_t n = 0;
    if (rt) {
        attach[n++] = rt->view;
        if (rt->samples > 1) attach[n++] = rt->resolved_view;
    }
    if (ds) {
        attach[n++] = ds->view;
        if (ds->samples > 1) attach[n++] = ds->resolved_view;
    }
    uint64_t key = Hash(attach, sizeof(VkImageView) * n);
    auto it = framebuffers_.find(key);
    if (it != framebuffers_.end()) return it->second;
    const uint32_t samples = rt ? rt->samples : ds ? ds->samples : 1;
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = samples > 1
                          ? RenderPassForMs(rt ? rt->format : VK_FORMAT_UNDEFINED,
                                            ds ? ds->format : VK_FORMAT_UNDEFINED, samples)
                          : RenderPassFor(rt ? rt->format : VK_FORMAT_UNDEFINED,
                                          ds ? ds->format : VK_FORMAT_UNDEFINED);
    info.attachmentCount = n;
    info.pAttachments = attach;
    info.width = rt ? rt->w : ds->w;
    info.height = rt ? rt->h : ds->h;
    info.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(device_, &info, nullptr, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    LOG_FIRST(12, "vulkan: fb key %016llx rt %08X ds %08X views %llx/%llx %ux%u",
              static_cast<unsigned long long>(key), rt ? rt->data : 0, ds ? ds->data : 0,
              static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(rt ? rt->view : VK_NULL_HANDLE)),
              static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ds ? ds->view : VK_NULL_HANDLE)),
              info.width, info.height);
    if (perf_) perf_fb_new_++;
    framebuffers_[key] = fb;
    framebuffer_targets_[key] = {rt, ds};
    return fb;
}

void VulkanBackend::FlushPass() {
    if (pass_ != VK_NULL_HANDLE) {
        vkCmdEndRenderPass(cmd_);
        if (pass_rt_ && pass_rt_->samples > 1) pass_rt_->resolved_layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        if (pass_ds_ && pass_ds_->samples > 1)
            pass_ds_->resolved_layout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL;
        pass_ = VK_NULL_HANDLE;
        pass_fb_ = VK_NULL_HANDLE;
        pass_rt_ = pass_ds_ = nullptr;
    }
}

// Open a render pass on an already resolved RT/DS pair. The deferred replay uses this to
// re-open the scene pass after the AO passes without going through a guest State.
void VulkanBackend::BeginScenePass(Target* rt, Target* ds) {
    FlushPass();
    if (!rt && !ds) return;
    pass_rt_ = rt;
    pass_ds_ = ds;
    BeginRecord();
    if (rt) Transition(rt->image, rt->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, rt->format);
    if (ds) Transition(ds->image, ds->layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL, ds->format);
    pass_ = (rt && rt->samples > 1) || (ds && ds->samples > 1)
                ? RenderPassForMs(rt ? rt->format : VK_FORMAT_UNDEFINED,
                                  ds ? ds->format : VK_FORMAT_UNDEFINED,
                                  rt ? rt->samples : ds->samples)
                : RenderPassFor(rt ? rt->format : VK_FORMAT_UNDEFINED, ds ? ds->format : VK_FORMAT_UNDEFINED);
    pass_fb_ = FramebufferFor(rt, ds);
    if (pass_ == VK_NULL_HANDLE || pass_fb_ == VK_NULL_HANDLE) {
        pass_rt_ = pass_ds_ = nullptr;
        pass_ = VK_NULL_HANDLE;
        pass_fb_ = VK_NULL_HANDLE;
        return;
    }
    VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    info.renderPass = pass_;
    info.framebuffer = pass_fb_;
    info.renderArea = {{0, 0}, {rt ? rt->w : ds->w, rt ? rt->h : ds->h}};
    vkCmdBeginRenderPass(cmd_, &info, VK_SUBPASS_CONTENTS_INLINE);
    bound_pipe_ = VK_NULL_HANDLE;
    viewport_valid_ = false;
}

void VulkanBackend::BindTargets(const State& s, Target*& rt, Target*& ds) {
    if (perf_) perf_bind_++;
    rt = nullptr;
    ds = nullptr;
    TextureDesc rd, dd;
    // Swizzled surfaces encode their dimensions in Format and have no Size field value.
    const bool want_rt = DescribeSurface(s.rt, rd);
    const bool want_ds = DescribeSurface(s.zs, dd);
    const auto same_target = [](Target* old, const TextureDesc& desc, bool depth) {
        return old && old->data == desc.data && old->depth == depth && old->gw == desc.width && old->gh == desc.height &&
               old->guest_format == desc.format && old->pitch == desc.pitch && old->linear == desc.linear;
    };
    if (pass_ != VK_NULL_HANDLE &&
        ((want_rt && same_target(pass_rt_, rd, false)) || (!want_rt && !pass_rt_)) &&
        ((want_ds && same_target(pass_ds_, dd, true)) || (!want_ds && !pass_ds_))) {
        rt = pass_rt_;
        ds = pass_ds_;
        if (DeferredPending() &&
            ((rt && deferred_read_targets_.count(rt)) || (ds && deferred_read_targets_.count(ds)))) {
            FlushDeferredScene();
            BeginScenePass(rt, ds);
        }
        return;
    }
    FlushPass();
    if (perf_) perf_pass_new_++;
    if (want_rt) rt = EnsureTarget(s.rt, false);
    if (want_ds) ds = EnsureTarget(s.zs, true);
    // A framebuffer needs one extent and sample count: a stale or differently sized depth
    // attachment is not usable with this colour target.
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h || rt->samples != ds->samples)) ds = nullptr;
    if (!rt && !ds) return;
    // Do not transition a target out of a sampled layout while pending draws still read it.
    if (DeferredPending() &&
        ((rt && deferred_read_targets_.count(rt)) || (ds && deferred_read_targets_.count(ds)))) {
        FlushDeferredScene();
        if (!rt && !ds) return;
    }
    BeginScenePass(rt, ds);
}

// ---------------------------------------------------------------------------------------------
// AO targets and full-screen passes

bool VulkanBackend::CreateAoImage(AoImage& image, uint32_t w, uint32_t h, VkFormat format, uint32_t samples,
                                  VkImageUsageFlags usage) {
    DestroyAoImage(image);
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {w, h, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VkSampleCountFlagBits(std::max(1u, samples));
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = usage;
    info.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (vkCreateImage(device_, &info, nullptr, &image.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, image.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    bool found = false;
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mem_props_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            alloc.memoryTypeIndex = i;
            found = true;
            break;
        }
    if (!found || vkAllocateMemory(device_, &alloc, nullptr, &image.memory) != VK_SUCCESS) {
        vkDestroyImage(device_, image.image, nullptr);
        image.image = VK_NULL_HANDLE;
        return false;
    }
    if (vkBindImageMemory(device_, image.image, image.memory, 0) != VK_SUCCESS) {
        DestroyAoImage(image);
        return false;
    }
    const bool depth = format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D16_UNORM ||
                       format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    const bool stencil = format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D32_SFLOAT_S8_UINT;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT |
                                                        (stencil ? VK_IMAGE_ASPECT_STENCIL_BIT : 0))
                                  : VK_IMAGE_ASPECT_COLOR_BIT,
                             0, 1, 0, 1};
    if (vkCreateImageView(device_, &view, nullptr, &image.view) != VK_SUCCESS) {
        DestroyAoImage(image);
        return false;
    }
    image.format = format;
    image.w = w;
    image.h = h;
    image.samples = std::max(1u, samples);
    image.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    return true;
}

void VulkanBackend::DestroyAoImage(AoImage& image) {
    for (auto& cached : ao_occ_cached_) cached = {};
    if (image.view) vkDestroyImageView(device_, image.view, nullptr);
    if (image.image) vkDestroyImage(device_, image.image, nullptr);
    if (image.memory) vkFreeMemory(device_, image.memory, nullptr);
    image = {};
}

bool VulkanBackend::EnsureAoDepth(uint32_t w, uint32_t h, uint32_t samples) {
    if (ao_depth_.image && ao_dist_.image && ao_depth_.w == w && ao_depth_.h == h &&
        ao_depth_.samples == samples)
        return true;
    if (ao_depth_.image || ao_dist_.image) {
        EndRecord(true);
        if (ao_prepass_fb_) vkDestroyFramebuffer(device_, ao_prepass_fb_, nullptr);
        ao_prepass_fb_ = VK_NULL_HANDLE;
        DestroyAoImage(ao_depth_);
        DestroyAoImage(ao_dist_);
    }
    const bool ok = CreateAoImage(ao_depth_, w, h, depth_format_, samples, VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT) &&
                    CreateAoImage(ao_dist_, w, h, VK_FORMAT_R32_SFLOAT, samples,
                                  VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                                      VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
    if (!ok) {
        DestroyAoImage(ao_depth_);
        DestroyAoImage(ao_dist_);
        return false;
    }
    ao_depth_clear_ = true;
    LOGR("vulkan: AO prepass targets %ux%u, %u sample(s)", w, h, samples);
    return true;
}

bool VulkanBackend::EnsureAoHalf(uint32_t w, uint32_t h) {
    const uint32_t aw = ao_mode_ == 1 ? std::max(1u, w / 2) : std::max(1u, w);
    const uint32_t ah = ao_mode_ == 1 ? std::max(1u, h / 2) : std::max(1u, h);
    if (ao_half_.image && ao_half_.w == aw && ao_half_.h == ah) return true;
    if (ao_half_.image) {
        EndRecord(true);
        if (ao_half_fb_) vkDestroyFramebuffer(device_, ao_half_fb_, nullptr);
        ao_half_fb_ = VK_NULL_HANDLE;
        DestroyAoImage(ao_half_);
    }
    if (!CreateAoImage(ao_half_, aw, ah, VK_FORMAT_R8_UNORM, 1,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return false;
    LOGR("vulkan: AO factor target %ux%u", aw, ah);
    return true;
}

bool VulkanBackend::EnsureAoOcc(uint32_t w, uint32_t h) {
    if (ao_occ_.image && ao_occ_.w == w && ao_occ_.h == h) return true;
    if (ao_occ_.image) {
        EndRecord(true);
        if (ao_occ_fb_) vkDestroyFramebuffer(device_, ao_occ_fb_, nullptr);
        ao_occ_fb_ = VK_NULL_HANDLE;
        DestroyAoImage(ao_occ_);
    }
    if (!CreateAoImage(ao_occ_, w, h, VK_FORMAT_R8_UNORM, 1,
                       VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
                           VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
        return false;
    LOGR("vulkan: AO occlusion target %ux%u", w, h);
    return true;
}

bool VulkanBackend::EnsureAoZero(uint32_t w, uint32_t h) {
    if (ao_zero_.image && ao_zero_.w == w && ao_zero_.h == h) return true;
    if (ao_zero_.image) {
        EndRecord(true);
        DestroyAoImage(ao_zero_);
    }
    if (!CreateAoImage(ao_zero_, w, h, VK_FORMAT_R8_UNORM, 1,
                       VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT))
        return false;
    // A receiver recorded with ao = 1 must still have a real, full-size t4. A 1x1 image would
    // be out of bounds for Load() and Vulkan does not define that without robustImageAccess.
    FlushPass();
    BeginRecord();
    Transition(ao_zero_.image, ao_zero_.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, ao_zero_.format);
    VkClearColorValue zero{};
    VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd_, ao_zero_.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &zero, 1, &range);
    Transition(ao_zero_.image, ao_zero_.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, ao_zero_.format);
    return true;
}

VkRenderPass VulkanBackend::AoPrepassPass(uint32_t samples) {
    const uint64_t key = uint64_t(depth_format_) | (uint64_t(samples) << 32) | (1ull << 63);
    if (auto it = ao_passes_.find(key); it != ao_passes_.end()) return it->second;
    VkAttachmentDescription att[2]{};
    att[0].format = VK_FORMAT_R32_SFLOAT;
    att[0].samples = VkSampleCountFlagBits(std::max(1u, samples));
    att[0].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[0].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[0].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[0].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[0].initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[0].finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att[1].format = depth_format_;
    att[1].samples = VkSampleCountFlagBits(std::max(1u, samples));
    att[1].loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
    att[1].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att[1].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att[1].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att[1].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    att[1].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkAttachmentReference dref{1, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    sub.pDepthStencilAttachment = &dref;
    VkSubpassDependency dep[2]{};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT |
                          VK_PIPELINE_STAGE_EARLY_FRAGMENT_TESTS_BIT;
    dep[0].dstStageMask = dep[0].srcStageMask | VK_PIPELINE_STAGE_LATE_FRAGMENT_TESTS_BIT;
    dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_WRITE_BIT;
    dep[0].dstAccessMask = dep[0].srcAccessMask | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT |
                           VK_ACCESS_DEPTH_STENCIL_ATTACHMENT_READ_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = dep[0].dstStageMask;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[1].srcAccessMask = dep[0].dstAccessMask;
    dep[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 2;
    info.pAttachments = att;
    info.subpassCount = 1;
    info.pSubpasses = &sub;
    info.dependencyCount = 2;
    info.pDependencies = dep;
    VkRenderPass pass = VK_NULL_HANDLE;
    if (vkCreateRenderPass(device_, &info, nullptr, &pass) != VK_SUCCESS) return VK_NULL_HANDLE;
    ao_passes_[key] = pass;
    return pass;
}

VkRenderPass VulkanBackend::AoColorPass() {
    if (ao_color_pass_) return ao_color_pass_;
    VkAttachmentDescription att{};
    att.format = VK_FORMAT_R8_UNORM;
    att.samples = VK_SAMPLE_COUNT_1_BIT;
    att.loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    att.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    att.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    att.initialLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    att.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
    VkAttachmentReference cref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &cref;
    VkSubpassDependency dep[2]{};
    dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
    dep[0].dstSubpass = 0;
    dep[0].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    dep[0].dstStageMask = dep[0].srcStageMask;
    dep[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
    dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_READ_BIT;
    dep[1].srcSubpass = 0;
    dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
    dep[1].srcStageMask = dep[0].dstStageMask;
    dep[1].dstStageMask = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
    dep[1].srcAccessMask = dep[0].dstAccessMask;
    dep[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    VkRenderPassCreateInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    info.attachmentCount = 1;
    info.pAttachments = &att;
    info.subpassCount = 1;
    info.pSubpasses = &sub;
    info.dependencyCount = 2;
    info.pDependencies = dep;
    if (vkCreateRenderPass(device_, &info, nullptr, &ao_color_pass_) != VK_SUCCESS) return VK_NULL_HANDLE;
    return ao_color_pass_;
}

VkFramebuffer VulkanBackend::AoFramebuffer(VkRenderPass pass, AoImage* color, AoImage* depth) {
    if (!pass || (!color && !depth)) return VK_NULL_HANDLE;
    VkImageView views[2]{};
    uint32_t n = 0;
    if (color) views[n++] = color->view;
    if (depth) views[n++] = depth->view;
    VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    info.renderPass = pass;
    info.attachmentCount = n;
    info.pAttachments = views;
    info.width = color ? color->w : depth->w;
    info.height = color ? color->h : depth->h;
    info.layers = 1;
    VkFramebuffer fb = VK_NULL_HANDLE;
    if (vkCreateFramebuffer(device_, &info, nullptr, &fb) != VK_SUCCESS) return VK_NULL_HANDLE;
    return fb;
}

VkPipeline VulkanBackend::CreateAoPipeline(VkShaderModule vs, VkShaderModule ps, VkRenderPass pass) {
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "vs";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = "psmain";
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &cb;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dyn;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = ao_pipeline_layout_;
    info.renderPass = pass;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (CreatePipelines(1, &info, nullptr, &pipeline) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    return pipeline;
}

bool VulkanBackend::EnsureAoPipelines(bool depth_ms) {
    if (ao_gtao_pipe_ && ao_occ_pipe_ && ao_pipe_depth_ms_ == depth_ms && ao_pipe_mode_ == ao_mode_)
        return true;
    const VkRenderPass pass = AoColorPass();
    if (!pass) return false;
    const std::string ms = depth_ms ? "1\n" : "0\n";
    std::string gtao = std::string("#define DEPTH_MS ") + ms + "#define GTAO_SLICES 3\n#define GTAO_STEPS " +
                       (ao_mode_ == 1 ? "6\n" : "8\n") + kAoHlsl;
    const std::string occ = std::string("#define DEPTH_MS ") + ms + kAoOcclusionHlsl;
    VkShaderModule gvs = CompileShader(gtao, shaderc_vertex_shader, "vs");
    VkShaderModule gps = CompileShader(gtao, shaderc_fragment_shader, "psmain");
    VkShaderModule ovs = CompileShader(occ, shaderc_vertex_shader, "vs");
    VkShaderModule ops = CompileShader(occ, shaderc_fragment_shader, "psmain");
    VkPipeline gp = gvs && gps ? CreateAoPipeline(gvs, gps, pass) : VK_NULL_HANDLE;
    VkPipeline op = ovs && ops ? CreateAoPipeline(ovs, ops, pass) : VK_NULL_HANDLE;
    if (!gp || !op) {
        LOG_FIRST(4, "vulkan: AO pipeline creation failed (GTAO %s, occlusion %s)", gp ? "ok" : "failed",
                  op ? "ok" : "failed");
        if (gp) vkDestroyPipeline(device_, gp, nullptr);
        if (op) vkDestroyPipeline(device_, op, nullptr);
        for (VkShaderModule m : {gvs, gps, ovs, ops})
            if (m) vkDestroyShaderModule(device_, m, nullptr);
        return false;
    }
    if (ao_gtao_pipe_) retired_ao_pipelines_.push_back(ao_gtao_pipe_);
    if (ao_occ_pipe_) retired_ao_pipelines_.push_back(ao_occ_pipe_);
    for (VkShaderModule m : {ao_gtao_vs_, ao_gtao_ps_, ao_occ_vs_, ao_occ_ps_})
        if (m) vkDestroyShaderModule(device_, m, nullptr);
    ao_gtao_vs_ = gvs;
    ao_gtao_ps_ = gps;
    ao_occ_vs_ = ovs;
    ao_occ_ps_ = ops;
    ao_gtao_pipe_ = gp;
    ao_occ_pipe_ = op;
    ao_pipe_depth_ms_ = depth_ms;
    ao_pipe_mode_ = ao_mode_;
    return true;
}

VkDescriptorSet VulkanBackend::AcquireAoSet(VkImageView ao, VkImageView depth, bool occlusion_set) {
    if (!EnsureAoPools() || ao_set_next_ >= ao_sets_.size()) return VK_NULL_HANDLE;
    VkDescriptorSet set = ao_sets_[ao_set_next_++];
    VkDescriptorImageInfo image[2]{};
    image[0].imageView = ao ? ao : white_view_;
    image[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    image[1].imageView = depth ? depth : white_view_;
    image[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo sampler[2]{};
    sampler[0].sampler = occlusion_set ? present_sampler_ : present_point_sampler_;
    sampler[1].sampler = present_point_sampler_;
    VkWriteDescriptorSet writes[4]{};
    for (int i = 0; i < 2; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = uint32_t(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[i].pImageInfo = &image[i];
    }
    for (int i = 0; i < 2; i++) {
        writes[2 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2 + i].dstSet = set;
        writes[2 + i].dstBinding = uint32_t(2 + i);
        writes[2 + i].descriptorCount = 1;
        writes[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[2 + i].pImageInfo = &sampler[i];
    }
    vkUpdateDescriptorSets(device_, 4, writes, 0, nullptr);
    return set;
}

// TCNYC_AO_PROBE=1: dump the first full-size occlusion image as PGM, for a same-scene
// comparison against D3D12. Diagnostic only; it submits the AO work and restarts the command
// buffer, so never enable it during normal play.
void VulkanBackend::DumpAoProbe(AoImage& image) {
    const bool dist = image.format == VK_FORMAT_R32_SFLOAT;
    if (!std::getenv("TCNYC_AO_PROBE") || !image.image || image.samples > 1 ||
        (dist ? ao_dist_probed_ : ao_probed_))
        return;
    if (dist)
        ao_dist_probed_ = true;
    else
        ao_probed_ = true;
    const uint64_t bytes = uint64_t(image.w) * image.h * (dist ? 4 : 1);
    EndRecord(true);
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* map = nullptr;
    StagingBuffer(device_, mem_props_, bytes, buffer, memory, &map);
    if (!map) {
        BeginRecord();
        return;
    }
    BeginRecord();
    Transition(image.image, image.layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, image.format);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {image.w, image.h, 1};
    vkCmdCopyImageToBuffer(cmd_, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    EndRecord(true);
    std::error_code ec;
    std::filesystem::create_directories(app::ExeDir() / "logs", ec);
    const std::string path = (app::ExeDir() / "logs" / (dist ? "ao_dist.pgm" : "ao_probe.pgm")).string();
    if (FILE* f = std::fopen(path.c_str(), "wb")) {
        std::fprintf(f, "P5\n%u %u\n255\n", image.w, image.h);
        if (dist) {
            const float* src = static_cast<const float*>(map);
            std::vector<uint8_t> px(size_t(image.w) * image.h);
            for (size_t i = 0; i < px.size(); i++) {
                // Near geometry white, the 1e30 clear black; enough to see the prepass coverage.
                px[i] = uint8_t(std::clamp(1.0f - src[i] / 50.0f, 0.0f, 1.0f) * 255.0f);
            }
            std::fwrite(px.data(), 1, px.size(), f);
        } else {
            std::fwrite(map, 1, size_t(image.w) * image.h, f);
        }
        std::fclose(f);
        LOGR("vulkan: AO probe saved to %s", path.c_str());
    }
    vkUnmapMemory(device_, memory);
    vkDestroyBuffer(device_, buffer, nullptr);
    vkFreeMemory(device_, memory, nullptr);
    BeginRecord();
    Transition(image.image, image.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, image.format);
}

VkDescriptorSet VulkanBackend::AcquireAoOcclusionSet(VkImageView view) {
    if (!view || ao_fail_test_) return VK_NULL_HANDLE;
    for (const auto& [cached_view, cached_set] : ao_occ_cached_)
        if (cached_view == view) return cached_set;
    if (!EnsureAoPools() || ao_occ_set_next_ >= ao_occ_sets_.size()) return VK_NULL_HANDLE;
    VkDescriptorSet set = ao_occ_sets_[ao_occ_set_next_++];
    auto& slot = ao_occ_cached_[0].first ? ao_occ_cached_[1] : ao_occ_cached_[0];
    slot = {view, set};
    VkDescriptorImageInfo image{};
    image.imageView = view;
    image.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    write.dstSet = set;
    write.dstBinding = 0;
    write.descriptorCount = 1;
    write.descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    write.pImageInfo = &image;
    vkUpdateDescriptorSets(device_, 1, &write, 0, nullptr);
    return set;
}

// the game's own pipeline for a receiver; built on first use, which only happens when AO failed
VkPipeline VulkanBackend::PlainPipeline(DeferredPlain p) {
    uint64_t pkey = 0;
    VkShaderModule ps = PixelShaderFor(p.pk, pkey);
    if (!ps || !p.vs) return VK_NULL_HANDLE;
    p.key.ps = pkey;
    if (auto it = pipelines_.find(p.key); it != pipelines_.end()) return it->second;
    VkPipeline pipe = VK_NULL_HANDLE;
    if (!CreatePipeline(p.key, p.vs, ps, pipe)) return VK_NULL_HANDLE;
    pipelines_[p.key] = pipe;
    return pipe;
}

void VulkanBackend::ReplayDeferred(const std::vector<DeferredDraw>& draws, const std::vector<DeferredPlain>& plain,
                                   VkDescriptorSet occlusion) {
    size_t fallback = 0, skipped = 0;
    // dynamic state as last set in this replay; consecutive draws mostly share it
    const DeferredDraw* last = nullptr;
    for (const DeferredDraw& d : draws) {
        if (!d.pipe) continue;
        VkPipeline pipe = d.pipe;
        // A receiver's AO pipeline reads set 1. Without an occlusion set (no AO image could be
        // made) it is drawn with the game's own pipeline instead, without AO.
        if (d.receiver && !occlusion) {
            pipe = d.plain >= 0 && size_t(d.plain) < plain.size() ? PlainPipeline(plain[size_t(d.plain)])
                                                                  : VK_NULL_HANDLE;
            if (!pipe) {
                ++skipped;
                continue;
            }
            ++fallback;
        }
        if (pipe != bound_pipe_) {
            vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
            bound_pipe_ = pipe;
        }
        for (uint32_t i = 0; i < d.vb_count; i++)
            vkCmdBindVertexBuffers(cmd_, d.vbind[i], 1, &d.vbufs[i], &d.voffs[i]);
        if (d.ibuf) vkCmdBindIndexBuffer(cmd_, d.ibuf, d.ioff, d.itype);
        VkDescriptorSet sets[2] = {d.set, occlusion};
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0,
                                occlusion ? 2u : 1u, sets, 0, nullptr);
        if (!last || std::memcmp(&last->vp, &d.vp, sizeof(d.vp))) vkCmdSetViewport(cmd_, 0, 1, &d.vp);
        if (!last || std::memcmp(&last->sc, &d.sc, sizeof(d.sc))) vkCmdSetScissor(cmd_, 0, 1, &d.sc);
        if (!last || std::memcmp(last->blend, d.blend, sizeof(d.blend))) vkCmdSetBlendConstants(cmd_, d.blend);
        if (!last || last->stencil != d.stencil)
            vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, d.stencil);
        last = &d;
        if (d.index_count)
            vkCmdDrawIndexed(cmd_, d.index_count, 1, 0, d.base_vertex, 0);
        else
            vkCmdDraw(cmd_, d.vertex_count, 1, 0, 0);
        draws_++;
        if (d.rt) d.rt->drawn = true;
        if (d.ds) d.ds->drawn = true;
    }
    if (fallback) LOG_FIRST(4, "vulkan: %zu AO receiver draw(s) drawn without AO: no occlusion image", fallback);
    if (skipped) LOGE("vulkan: %zu AO receiver draw(s) skipped: no occlusion image or ordinary pipeline", skipped);
    bound_pipe_ = VK_NULL_HANDLE;
    viewport_valid_ = false;
}

void VulkanBackend::FlushDeferredScene(std::source_location where) {
    if (deferred_.empty() || flushing_deferred_) return;
    flushing_deferred_ = true;
    if (perf_) {
        perf_defer_flushes_++;
        perf_flush_lines_[where.line()]++;
    }
    struct Guard {
        bool& f;
        ~Guard() { f = false; }
    } guard{flushing_deferred_};

    Target* rt = defer_rt_;
    Target* ds = defer_ds_;
    std::vector<DeferredDraw> draws;
    draws.swap(deferred_);
    std::vector<DeferredPlain> plain;
    plain.swap(deferred_plain_);
    deferred_read_targets_.clear();
    deferred_write_targets_.clear();
    deferred_read_images_.clear();

    FlushPass();
    VkDescriptorSet occlusion = VK_NULL_HANDLE;
    bool ao_ran = false;
    bool receivers = false, receiver_draws = false;
    for (const DeferredDraw& d : draws) {
        receivers = receivers || d.depth_pipe != VK_NULL_HANDLE;
        receiver_draws = receiver_draws || d.receiver;
    }
    if (ao_mode_ != 0 && rt && ds && receivers) {
        const uint32_t ao_samples = PickSampleCount(rt->samples, ao_color_samples_);
        const bool depth_ms = ao_samples > 1;
        const bool zero_ok = EnsureAoZero(rt->w, rt->h);
        const bool ready = zero_ok && EnsureAoDepth(rt->w, rt->h, ao_samples) && EnsureAoHalf(rt->w, rt->h) &&
                           EnsureAoOcc(rt->w, rt->h) && EnsureAoPipelines(depth_ms);
        if (ready) {
            BeginRecord();
            const VkRenderPass prepass = AoPrepassPass(ao_samples);
            if (!ao_prepass_fb_) ao_prepass_fb_ = AoFramebuffer(prepass, &ao_dist_, &ao_depth_);
            if (prepass && ao_prepass_fb_) {
                Transition(ao_depth_.image, ao_depth_.layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL,
                           ao_depth_.format);
                Transition(ao_dist_.image, ao_dist_.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                           ao_dist_.format);
                VkRenderPassBeginInfo bi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                bi.renderPass = prepass;
                bi.framebuffer = ao_prepass_fb_;
                bi.renderArea = {{0, 0}, {ao_dist_.w, ao_dist_.h}};
                vkCmdBeginRenderPass(cmd_, &bi, VK_SUBPASS_CONTENTS_INLINE);
                if (ao_depth_clear_) {
                    bool wants_zero = false, wants_one = false;
                    for (const DeferredDraw& d : draws) {
                        if (!d.depth_pipe) continue;
                        const uint32_t n = d.zfunc >= 0x200 && d.zfunc <= 0x207 ? d.zfunc - 0x200 : 7;
                        if (n == 4 || n == 6) wants_zero = true;
                        else if (n == 1 || n == 3) wants_one = true;
                    }
                    // One depth target can only have one clear value. World draws normally share
                    // the compare op; if they do not, keep the D3D12-compatible far-side default.
                    const float depth_clear = wants_zero && !wants_one ? 0.0f : 1.0f;
                    static std::atomic<int> logged_clear{0};
                    if (logged_clear.fetch_add(1, std::memory_order_relaxed) < 4)
                        LOGR("vulkan: AO prepass clear %.1f (%s)", depth_clear,
                             wants_zero && wants_one ? "mixed compare ops" : "scene compare op");
                    VkClearAttachment clear[2]{};
                    clear[0].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                    clear[0].colorAttachment = 0;
                    clear[0].clearValue.color.float32[0] = kAoDistClear;
                    clear[1].aspectMask = VK_IMAGE_ASPECT_DEPTH_BIT;
                    clear[1].clearValue.depthStencil.depth = depth_clear;
                    clear[1].clearValue.depthStencil.stencil = 0;
                    VkClearRect rect{};
                    rect.rect = {{0, 0}, {ao_dist_.w, ao_dist_.h}};
                    rect.baseArrayLayer = 0;
                    rect.layerCount = 1;
                    vkCmdClearAttachments(cmd_, 2, clear, 1, &rect);
                    ao_depth_clear_ = false;
                }
                for (const DeferredDraw& d : draws) {
                    if (!d.depth_pipe) continue;
                    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, d.depth_pipe);
                    for (uint32_t i = 0; i < d.vb_count; i++)
                        vkCmdBindVertexBuffers(cmd_, d.vbind[i], 1, &d.vbufs[i], &d.voffs[i]);
                    if (d.ibuf) vkCmdBindIndexBuffer(cmd_, d.ibuf, d.ioff, d.itype);
                    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &d.set,
                                            0, nullptr);
                    vkCmdSetViewport(cmd_, 0, 1, &d.vp);
                    vkCmdSetScissor(cmd_, 0, 1, &d.sc);
                    vkCmdSetBlendConstants(cmd_, d.blend);
                    vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, d.stencil);
                    if (d.index_count)
                        vkCmdDrawIndexed(cmd_, d.index_count, 1, 0, d.base_vertex, 0);
                    else
                        vkCmdDraw(cmd_, d.vertex_count, 1, 0, 0);
                }
                vkCmdEndRenderPass(cmd_);
                bound_pipe_ = VK_NULL_HANDLE;
                viewport_valid_ = false;
                Transition(ao_dist_.image, ao_dist_.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                           ao_dist_.format);
                DumpAoProbe(ao_dist_);

                // GTAO from the distance target, sample 0 of the MSAA image.
                if (!ao_half_fb_) ao_half_fb_ = AoFramebuffer(AoColorPass(), &ao_half_, nullptr);
                VkDescriptorSet gtao_set = ao_half_fb_ && ao_gtao_pipe_
                                               ? AcquireAoSet(ao_dist_.view, VK_NULL_HANDLE, false)
                                               : VK_NULL_HANDLE;
                bool gtao_ok = false;
                if (gtao_set) {
                    Transition(ao_half_.image, ao_half_.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               ao_half_.format);
                    VkRenderPassBeginInfo gb{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                    gb.renderPass = AoColorPass();
                    gb.framebuffer = ao_half_fb_;
                    gb.renderArea = {{0, 0}, {ao_half_.w, ao_half_.h}};
                    vkCmdBeginRenderPass(cmd_, &gb, VK_SUBPASS_CONTENTS_INLINE);
                    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, ao_gtao_pipe_);
                    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, ao_pipeline_layout_, 0, 1,
                                            &gtao_set, 0, nullptr);
                    float aspect = app::Widescreen() ? app::AspectRatio() : 4.0f / 3.0f;
                    float tan_half_fov = 0.577f;
                    const float fov = game::g_camera_fov_degrees.load(std::memory_order_relaxed);
                    if (fov > 1.0f && fov < 179.0f && aspect > 0.1f)
                        tan_half_fov = std::tan(fov * 3.14159265f / 360.0f) / aspect;
                    const float push[8] = {1.0f / float(std::max(1u, ao_dist_.w)),
                                           1.0f / float(std::max(1u, ao_dist_.h)), 0.0f, kAoDistClear,
                                           kAoStrength, kAoRadius, tan_half_fov, aspect};
                    vkCmdPushConstants(cmd_, ao_pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
                    VkViewport vp{0, 0, float(ao_half_.w), float(ao_half_.h), 0, 1};
                    VkRect2D sc{{0, 0}, {ao_half_.w, ao_half_.h}};
                    vkCmdSetViewport(cmd_, 0, 1, &vp);
                    vkCmdSetScissor(cmd_, 0, 1, &sc);
                    vkCmdDraw(cmd_, 3, 1, 0, 0);
                    vkCmdEndRenderPass(cmd_);
                    bound_pipe_ = VK_NULL_HANDLE;
                    viewport_valid_ = false;
                    Transition(ao_half_.image, ao_half_.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               ao_half_.format);
                    gtao_ok = true;
                } else {
                    LOG_FIRST(4, "vulkan: AO GTAO skipped: descriptor or framebuffer unavailable");
                }

                // depth-aware blur and upsample into the full-size occlusion target
                if (!ao_occ_fb_) ao_occ_fb_ = AoFramebuffer(AoColorPass(), &ao_occ_, nullptr);
                VkDescriptorSet occ_pass_set =
                    ao_occ_fb_ && ao_occ_pipe_
                        ? AcquireAoSet(ao_half_.view, ao_dist_.view, true)
                        : VK_NULL_HANDLE;
                if (gtao_ok && occ_pass_set) {
                    Transition(ao_occ_.image, ao_occ_.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
                               ao_occ_.format);
                    VkRenderPassBeginInfo ob{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
                    ob.renderPass = AoColorPass();
                    ob.framebuffer = ao_occ_fb_;
                    ob.renderArea = {{0, 0}, {ao_occ_.w, ao_occ_.h}};
                    vkCmdBeginRenderPass(cmd_, &ob, VK_SUBPASS_CONTENTS_INLINE);
                    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, ao_occ_pipe_);
                    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, ao_pipeline_layout_, 0, 1,
                                            &occ_pass_set, 0, nullptr);
                    const float push[8] = {1.0f / float(std::max(1u, ao_half_.w)),
                                           1.0f / float(std::max(1u, ao_half_.h)), 0.0f, 0.0f,
                                           0.0f, kAoDistClear, 0.0f, 0.0f};
                    vkCmdPushConstants(cmd_, ao_pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(push), push);
                    VkViewport vp{0, 0, float(ao_occ_.w), float(ao_occ_.h), 0, 1};
                    VkRect2D sc{{0, 0}, {ao_occ_.w, ao_occ_.h}};
                    vkCmdSetViewport(cmd_, 0, 1, &vp);
                    vkCmdSetScissor(cmd_, 0, 1, &sc);
                    vkCmdDraw(cmd_, 3, 1, 0, 0);
                    vkCmdEndRenderPass(cmd_);
                    bound_pipe_ = VK_NULL_HANDLE;
                    viewport_valid_ = false;
                    Transition(ao_occ_.image, ao_occ_.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                               ao_occ_.format);
                    DumpAoProbe(ao_occ_);
                    ao_ran = true;
                    if (perf_) perf_ao_runs_++;
                } else {
                    LOG_FIRST(4, "vulkan: AO occlusion skipped: descriptor or framebuffer unavailable");
                }
            }
        }
        if (!ready) LOG_FIRST(4, "vulkan: deferred AO skipped this flush");
    }
    if (ao_ran)
        occlusion = AcquireAoOcclusionSet(ao_occ_.view);
    if (!occlusion && ao_mode_ != 0 && rt && receiver_draws) {
        bool zero_ok = true;
        if (!ao_zero_.image || ao_zero_.w != rt->w || ao_zero_.h != rt->h)
            zero_ok = EnsureAoZero(rt->w, rt->h);
        if (zero_ok && ao_zero_.image)
            occlusion = AcquireAoOcclusionSet(ao_zero_.view);
        // the occlusion target from an earlier flush is a full-size image too: stale AO for one
        // replay is better than an unbound set 1
        if (!occlusion && ao_occ_.image && ao_occ_.w == rt->w && ao_occ_.h == rt->h &&
            ao_occ_.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL)
            occlusion = AcquireAoOcclusionSet(ao_occ_.view);
        if (!occlusion && !ao_failed_) {
            // this replay draws the receivers with their ordinary pipelines; AO stays off until the
            // setting changes or kAoRetryFrames pass, then it is tried again
            ao_failed_ = true;
            ao_failed_setting_ = ao_mode_;
            ao_failed_frames_ = 0;
            LOGE("vulkan: no AO occlusion image could be bound; ambient occlusion is paused");
        }
    }

    BeginScenePass(rt, ds);
    if (pass_ != VK_NULL_HANDLE) {
        ReplayDeferred(draws, plain, occlusion);
    } else {
        defer_abandoned_ = true;
        LOGE("vulkan: deferred replay could not reopen the scene pass; dropping %zu draw(s)", draws.size());
    }
    defer_ao_applied_ = ao_ran;
    if (ao_ran) {
        static std::atomic<bool> logged_ao{false};
        if (!logged_ao.exchange(true, std::memory_order_relaxed))
            LOGR("vulkan: receiver GTAO active (%s)", ao_mode_ == 1 ? "Half" : "Full");
    }
    static std::atomic<int> logged_flushes{0};
    if (logged_flushes.fetch_add(1, std::memory_order_relaxed) < 8)
        LOGR("vulkan: deferred flush %zu draw(s), AO %s", draws.size(), ao_ran ? "applied" : "off");
    deferred_.swap(draws);  // keep the display list's allocations for the next scene
    deferred_.clear();
    deferred_plain_.swap(plain);
    deferred_plain_.clear();
}

void VulkanBackend::ResetDeferred(uint32_t ao_wanted) {
    deferred_.clear();
    deferred_plain_.clear();
    deferred_read_targets_.clear();
    deferred_write_targets_.clear();
    deferred_read_images_.clear();
    defer_rt_ = defer_ds_ = nullptr;
    defer_ao_applied_ = false;
    defer_abandoned_ = false;
    ao_depth_clear_ = true;
    ao_set_next_ = 0;
    ao_occ_set_next_ = 0;
    for (auto& cached : ao_occ_cached_) cached = {};
    if (ao_failed_ && (ao_wanted != ao_failed_setting_ || ++ao_failed_frames_ >= kAoRetryFrames)) {
        ao_failed_ = false;  // a new setting, size or memory situation may work now
        LOGR("vulkan: trying ambient occlusion again");
    }
    ao_mode_ = ao_failed_ ? 0 : ao_wanted;
    defer_scene_ = defer_forced_ || ao_mode_ != 0;
    msaa_store_ = defer_scene_;
    for (VkPipeline p : retired_ao_pipelines_)
        if (p) vkDestroyPipeline(device_, p, nullptr);
    retired_ao_pipelines_.clear();
}

namespace {
// or the generated HLSL: vk::binding attributes the SPIR-V compiler needs
std::string InjectBindings(std::string src) {
    std::string out;
    out.reserve(src.size() + 512);
    size_t start = 0;
    while (start < src.size()) {
        size_t end = src.find('\n', start);
        if (end == std::string::npos) end = src.size();
        std::string line = src.substr(start, end - start);
        const size_t text = line.find_first_not_of(" \t");
        std::string trimmed = text == std::string::npos ? std::string() : line.substr(text);
        std::string prefix;
        if (trimmed.rfind("cbuffer VC", 0) == 0) prefix = "[[vk::binding(0, 0)]] ";
        else if (trimmed.rfind("cbuffer DC", 0) == 0) prefix = "[[vk::binding(1, 0)]] ";
        else if (trimmed.find(" : register(t") != std::string::npos) {
            size_t p = trimmed.find(" : register(t") + 13;
            if (p < trimmed.size() && trimmed[p] >= '0' && trimmed[p] <= '3')
                prefix = "[[vk::binding(" + std::to_string(2 + (trimmed[p] - '0')) + ", 0)]] ";
            else if (p < trimmed.size() && trimmed[p] == '4')
                prefix = "[[vk::binding(0, 1)]] ";  // the AO occlusion target
        } else if (trimmed.find(" : register(s") != std::string::npos) {
            size_t p = trimmed.find(" : register(s") + 13;
            if (p < trimmed.size() && trimmed[p] >= '0' && trimmed[p] <= '3')
                prefix = "[[vk::binding(" + std::to_string(6 + (trimmed[p] - '0')) + ", 0)]] ";
        }
        if (!prefix.empty()) out += prefix;
        out += trimmed;
        if (end < src.size()) out += '\n';
        start = end + (end < src.size() ? 1 : 0);
    }
    return out;
}
} // namespace

VkShaderModule VulkanBackend::CompileShader(const std::string& src, shaderc_shader_kind kind, const char* entry) {
    static shaderc_compiler_t compiler = shaderc_compiler_initialize();    static shaderc_compile_options_t options = [] {
        shaderc_compile_options_t o = shaderc_compile_options_initialize();
        shaderc_compile_options_set_source_language(o, shaderc_source_language_hlsl);
        shaderc_compile_options_set_target_env(o, shaderc_target_env_vulkan, shaderc_env_version_vulkan_1_1);
        shaderc_compile_options_set_auto_map_locations(o, true);
        return o;
    }();
    std::string injected0 = InjectBindings(src);
    // The shared HLSL uses this to pick Vulkan bindings/push constants and the full-screen
    // NDC direction. D3D12 leaves it undefined.
    injected0 = "#define TCNYC_VULKAN 1\n" + injected0;
    // D3D's viewport maps NDC +y to the top; Vulkan maps it to the bottom. The
    // generated vertex epilogue flips for D3D, so undo that flip for Vulkan.
    std::string injected = injected0;
    const std::string yflip = "q.y = 1 - p.y * 2 / surf.y;";
    const size_t at = injected.find(yflip);
    if (at != std::string::npos) injected.replace(at, yflip.size(), "q.y = p.y * 2 / surf.y - 1;");
    const shaderc_shader_kind shader_kind = kind;

    // compiled SPIR-V persisted between runs, keyed by the final source and stage
    const uint64_t cache_key = Hash(injected.data(), injected.size()) ^ (uint64_t(kind) << 56);
    static const std::filesystem::path cache_dir = [] {
        if (const char* xdg = std::getenv("XDG_CACHE_HOME"); xdg && *xdg)
            return std::filesystem::path(xdg) / "TrueCrimeNYC" / "shaders";
        if (const char* home = std::getenv("HOME"); home && *home)
            return std::filesystem::path(home) / ".cache" / "TrueCrimeNYC" / "shaders";
        return std::filesystem::path();
    }();
    char cache_name[32];
    std::snprintf(cache_name, sizeof(cache_name), "%016llx.spv", static_cast<unsigned long long>(cache_key));
    const std::filesystem::path cache_path =
        cache_dir.empty() ? std::filesystem::path() : cache_dir / cache_name;
    if (!cache_path.empty()) {
        std::ifstream in(cache_path, std::ios::binary);
        std::vector<uint8_t> bytes((std::istreambuf_iterator<char>(in)), {});
        if (bytes.size() >= 4 && bytes.size() % 4 == 0) {
            VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
            info.codeSize = bytes.size();
            info.pCode = reinterpret_cast<const uint32_t*>(bytes.data());
            VkShaderModule module = VK_NULL_HANDLE;
            if (vkCreateShaderModule(device_, &info, nullptr, &module) == VK_SUCCESS) return module;
        }
    }

    shaderc_compilation_result_t result = shaderc_compile_into_spv(
        compiler, injected.data(), injected.size(), shader_kind, "tcnyc", entry, options);
    const shaderc_compilation_status status = shaderc_result_get_compilation_status(result);
    if (status != shaderc_compilation_status_success) {
        static int logged = 0;
        if (logged++ < 4)
            LOGE("vulkan: %s shader failed: %s",
                 kind == shaderc_vertex_shader ? "vertex"
                 : kind == shaderc_geometry_shader ? "geometry"
                                                   : "pixel",
                 shaderc_result_get_error_message(result));
        shaderc_result_release(result);
        return VK_NULL_HANDLE;
    }
    VkShaderModuleCreateInfo info{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
    info.codeSize = shaderc_result_get_length(result);
    info.pCode = reinterpret_cast<const uint32_t*>(shaderc_result_get_bytes(result));
    VkShaderModule module = VK_NULL_HANDLE;
    VkResult r = vkCreateShaderModule(device_, &info, nullptr, &module);
    if (r == VK_SUCCESS && !cache_path.empty()) {
        std::error_code ec;
        std::filesystem::create_directories(cache_dir, ec);
        const std::string temp = cache_path.string() + ".tmp." + std::to_string(::getpid());
        if (FILE* f = std::fopen(temp.c_str(), "wb")) {
            const size_t bytes_written =
                std::fwrite(shaderc_result_get_bytes(result), 1, shaderc_result_get_length(result), f);
            const bool ok = bytes_written == shaderc_result_get_length(result);
            std::fclose(f);
            if (ok) std::rename(temp.c_str(), cache_path.c_str());
            else std::remove(temp.c_str());
        }
    }
    shaderc_result_release(result);
    if (r != VK_SUCCESS) {
        LOGE("vulkan: vkCreateShaderModule failed: %s", VkError(r));
        return VK_NULL_HANDLE;
    }
    return module;
}

// The console draws a point as its whole pixel and a line as a one-pixel quad; the shared reference
// HLSL does that in a geometry shader (PointHlsl/LineHlsl), so the same source is compiled here with
// the geometry stage and attached to point/line pipelines.
std::string SpriteHlsl();

VkShaderModule VulkanBackend::GeometryShaderFor(bool points, bool sprite) {
    VkShaderModule& cache = sprite ? geo_sprite_ : points ? geo_point_ : geo_line_;
    if (!cache) {
        std::string src = sprite ? SpriteHlsl() : points ? PointHlsl() : LineHlsl();
        // glslang's HLSL parser crashes on a geometry entry point that copies SV_ClipDistance0
        // (the struct copy in PointHlsl/LineHlsl/SpriteHlsl). The stage does not consume it, so
        // drop the member from the geometry stage only; the vertex shader still writes it.
        constexpr char kClip[] = "    float2 zclip : SV_ClipDistance0;\n";
        if (const size_t at = src.find(kClip); at != std::string::npos) src.erase(at, sizeof(kClip) - 1);
        cache = CompileShader(src, shaderc_geometry_shader, "main");
    }
    return cache;
}

// A point sprite is a square of the program's point size in console pixels (oPts, which the
// sprite vertex shader passes in tc3.x) with the sprite's coordinates across it, like d3d12.cpp.
std::string SpriteHlsl() {
    std::string s = PointHlsl();
    const size_t at = s.find("[maxvertexcount");
    if (at != std::string::npos) s.erase(at);
    return s + R"([maxvertexcount(4)]
void main(point VOut v[1], inout TriangleStream<VOut> strip) {
    float2 half_size = v[0].tc3.x * v[0].pos.w / surf.xy;
    for (int i = 0; i < 4; i++) {
        VOut o = v[0];
        float2 corner = float2(i & 1, i >> 1);
        o.pos.x += (corner.x * 2 - 1) * half_size.x;
        o.pos.y -= (corner.y * 2 - 1) * half_size.y;
        o.tc0 = float4(corner, 0, 1);
        o.tc3 = o.tc0;
        strip.Append(o);
    }
}
)";
}

VkShaderModule VulkanBackend::VertexShaderFor(const State& s, const Assembled& a, bool prog, bool sprite,
                                              uint64_t& key) {
    // key by what the source is built from, not by the source text: generating and hashing the HLSL for every draw
    // is far too slow with ~1500 draws a frame. The string is only built on a cache miss.
    uint64_t program_hash = 0;
    if (prog && !s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
    if (prog) program_hash = s.vs->hash;
    const FixedKey fk = prog ? FixedKey{} : MakeFixedKey(s);
    const uint8_t tag = prog ? 1 : 0;
    key = Hash(&tag, sizeof(tag));
    if (prog) {
        key = Hash(&program_hash, sizeof(program_hash), key);
    } else {
        key = Hash(&fk, sizeof(fk), key);
    }
    key = Hash(&a.regs, sizeof(a.regs), key);
    key = Hash(&a.decode, sizeof(a.decode), key);
    key = Hash(&a.pretransformed, sizeof(a.pretransformed), key);
    key = Hash(&sprite, sizeof(sprite), key);
    if (auto it = vs_cache_.find(key); it != vs_cache_.end()) return it->second;
    std::string src = prog ? VertexProgramHlsl(s.vs->program, a.regs, a.decode)
                           : FixedVertexHlsl(a.regs, a.pretransformed, a.decode, fk);
    if (sprite) {
        // a point sprite's program hands its point size to SpriteHlsl in tc3
        constexpr char kTc3[] = "    o.tc3 = oT3;\n";
        const size_t at = src.find(kTc3);
        if (at != std::string::npos) src.replace(at, sizeof(kTc3) - 1, "    o.tc3 = oPts;\n");
    }
    // The restricted distant-city pass is pushed behind the detailed geometry by camera.cpp's
    // building_seam_fix. Windows scales host Z in its reversed-depth shader; Vulkan keeps the
    // guest's 0..1 depth, so scale the guest clip z by the same factor. Every other draw has 1.
    constexpr char kDepthScale[] = "    o.pos.z *= zparams.w;\n";
    for (const char* tail : {"    o.pos = q;\n", "    o.pos = cp;\n"}) {
        const size_t at = src.find(tail);
        if (at != std::string::npos) src.insert(at + std::strlen(tail), kDepthScale);
    }
    VkShaderModule module = CompileShader(src, shaderc_vertex_shader, "main");
    if (module) {
        vs_cache_[key] = module;
        shader_compiles_++;
    }
    return module;
}

VkShaderModule VulkanBackend::PixelShaderFor(const PixelKey& pk, uint64_t& key) {
    key = Hash(&pk, sizeof(pk));
    if (auto it = ps_cache_.find(key); it != ps_cache_.end()) return it->second;
    const std::string src = PixelHlsl(pk);
    VkShaderModule module = CompileShader(src, shaderc_fragment_shader, "main");
    if (module) {
        ps_cache_[key] = module;
        shader_compiles_++;
    }
    return module;
}

bool VulkanBackend::CreatePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps, VkPipeline& out,
                                   VkRenderPass pass_override) {
    VkVertexInputBindingDescription bindings[16]{};
    VkVertexInputAttributeDescription attrs[16]{};
    uint32_t nb = 0, na = 0;
    int bind_of[256];
    std::fill(std::begin(bind_of), std::end(bind_of), -1);
    for (int r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        const uint32_t e = k.elements[r];
        const uint32_t buffer = (e >> 8) & 0xFF;
        if (buffer >= 16) continue;
        const uint32_t format = e & 0xFF;
        const uint32_t offset = (e >> 16) & 0xFFFF;
        const uint32_t stride = k.strides[buffer];
        const uint32_t bytes = ElementBytes(format);
        if (!stride || offset > stride || bytes > stride - offset) {
            LOG_FIRST(16, "vulkan: vertex element %d fmt %u offset %u outside stride %u; rejecting pipeline", r,
                      format, offset, stride);
            return false;
        }
        if (bind_of[buffer] < 0) {
            bind_of[buffer] = int(nb);
            bindings[nb].binding = buffer;
            bindings[nb].stride = stride;
            bindings[nb].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            nb++;
        }
        attrs[na].location = na;
        attrs[na].binding = buffer;
        attrs[na].format = ElementFormat(format);
        attrs[na].offset = offset;
        na++;
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    vi.vertexBindingDescriptionCount = nb;
    vi.pVertexBindingDescriptions = bindings;
    vi.vertexAttributeDescriptionCount = na;
    vi.pVertexAttributeDescriptions = attrs;

    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    switch (k.topo) {
        case 0: ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST; break;
        case 1: ia.topology = k.strip ? VK_PRIMITIVE_TOPOLOGY_LINE_STRIP : VK_PRIMITIVE_TOPOLOGY_LINE_LIST; break;
        default: ia.topology = k.strip ? VK_PRIMITIVE_TOPOLOGY_TRIANGLE_STRIP : VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST; break;
    }
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = k.fill == kFillWireframe && fill_non_solid_ ? VK_POLYGON_MODE_LINE : VK_POLYGON_MODE_FILL;
    rs.cullMode = k.cull ? VK_CULL_MODE_BACK_BIT : VK_CULL_MODE_NONE;
    rs.frontFace = k.ccw ? VK_FRONT_FACE_COUNTER_CLOCKWISE : VK_FRONT_FACE_CLOCKWISE;
    rs.depthBiasEnable = k.zslope != 0.0f;
    rs.depthBiasConstantFactor = 0.0f;
    rs.depthBiasSlopeFactor = k.zslope;
    rs.depthBiasClamp = 0.0f;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VkSampleCountFlagBits(std::max(1u, k.samples));

    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = k.zen && k.dsfmt != 0;
    ds.depthWriteEnable = k.zwrite && ds.depthTestEnable;
    ds.depthCompareOp = CompareOp(k.zfunc);
    ds.stencilTestEnable = k.sten && ds.depthTestEnable;
    ds.front = {StencilOp(k.sfail), StencilOp(k.spass), StencilOp(k.szfail), CompareOp(k.sfunc),
                k.smask, k.swmask, 0};
    ds.back = ds.front;

    VkPipelineColorBlendAttachmentState cb{};
    cb.blendEnable = k.blend;
    cb.srcColorBlendFactor = BlendFactor(k.src, false);
    cb.dstColorBlendFactor = BlendFactor(k.dst, false);
    cb.colorBlendOp = BlendOp(k.op);
    cb.srcAlphaBlendFactor = BlendFactor(k.src, true);
    cb.dstAlphaBlendFactor = BlendFactor(k.dst, true);
    cb.alphaBlendOp = BlendOp(k.op);
    cb.colorWriteMask = k.wmask & 0xF;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = k.rtfmt != 0 ? 1u : 0u;
    blend.pAttachments = &cb;

    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR,
                            VK_DYNAMIC_STATE_BLEND_CONSTANTS, VK_DYNAMIC_STATE_STENCIL_REFERENCE};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 4;
    dynamic.pDynamicStates = dyn;

    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    VkPipelineShaderStageCreateInfo stages[3]{};
    uint32_t stage_count = 0;
    stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stage_count].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[stage_count].module = vs;
    stages[stage_count].pName = "main";
    stage_count++;
    // console points and lines cover console pixels: a geometry shader expands each one into the
    // quad the D3D12 backend's PointHlsl/LineHlsl draw, so an upscaled target keeps the same coverage
    VkShaderModule gs = VK_NULL_HANDLE;
    if (geometry_ && (k.topo == 0 || k.topo == 1)) gs = GeometryShaderFor(k.topo == 0, k.sprite != 0);
    if (gs) {
        stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[stage_count].stage = VK_SHADER_STAGE_GEOMETRY_BIT;
        stages[stage_count].module = gs;
        stages[stage_count].pName = "main";
        stage_count++;
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_CLOCKWISE;
    }
    stages[stage_count].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[stage_count].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[stage_count].module = ps;
    stages[stage_count].pName = "main";
    stage_count++;
    info.stageCount = stage_count;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipeline_layout_;
    info.renderPass = pass_override != VK_NULL_HANDLE
                          ? pass_override
                          : k.samples > 1 ? RenderPassForMs(VkFormat(k.rtfmt), VkFormat(k.dsfmt), k.samples)
                                          : RenderPassFor(VkFormat(k.rtfmt), VkFormat(k.dsfmt));
    if (info.renderPass == VK_NULL_HANDLE) return false;
    VkResult r = CreatePipelines(1, &info, nullptr, &out);
    if (r != VK_SUCCESS) {
        LOGE("vulkan: pipeline failed: %s", r == VK_ERROR_OUT_OF_DEVICE_MEMORY ? "out of memory" : "error");
        return false;
    }
    return true;
}

// PART4

namespace {

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++)
            o[r * 4 + c] = x[r * 4] * y[c] + x[r * 4 + 1] * y[4 + c] + x[r * 4 + 2] * y[8 + c] +
                           x[r * 4 + 3] * y[12 + c];
}
void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++)
        for (int c = 0; c < 4; c++) o[c * 4 + r] = m[r * 4 + c];
}
void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++)
        for (int c = 0; c < 3; c++) {
            int r1 = (r + 1) % 3, r2 = (r + 2) % 3, c1 = (c + 1) % 3, c2 = (c + 2) % 3;
            n[r * 4 + c] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
}

std::vector<uint8_t> StagingBuffer(VkDevice dev, VkPhysicalDeviceMemoryProperties& props, VkDeviceSize size,
                                    VkBuffer& buffer, VkDeviceMemory& memory, void** map) {
    *map = nullptr;
    VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    info.size = size;
    info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    if (vkCreateBuffer(dev, &info, nullptr, &buffer) != VK_SUCCESS) return {};
    VkMemoryRequirements req{};
    vkGetBufferMemoryRequirements(dev, buffer, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    bool found = false;
    for (uint32_t i = 0; i < props.memoryTypeCount; i++) {
        const auto flags = props.memoryTypes[i].propertyFlags;
        if ((req.memoryTypeBits & (1u << i)) && (flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
            (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) {
            alloc.memoryTypeIndex = i;
            found = true;
            break;
        }
    }
    if (!found || vkAllocateMemory(dev, &alloc, nullptr, &memory) != VK_SUCCESS) {
        vkDestroyBuffer(dev, buffer, nullptr);
        buffer = VK_NULL_HANDLE;
        return {};
    }
    vkBindBufferMemory(dev, buffer, memory, 0);
    if (vkMapMemory(dev, memory, 0, VK_WHOLE_SIZE, 0, map) != VK_SUCCESS) {
        *map = nullptr;
        vkDestroyBuffer(dev, buffer, nullptr);
        vkFreeMemory(dev, memory, nullptr);
        buffer = VK_NULL_HANDLE;
        memory = VK_NULL_HANDLE;
    }
    return {};
}

void ReadStagePalette(const State& s, int stage, const TextureDesc& d, uint32_t colours[256], uint32_t& va,
                      uint64_t& hash) {
    constexpr uint32_t kFmtP8 = 0x0B;
    va = 0;
    hash = 0;
    std::memset(colours, 0, 256 * 4);
    uint32_t size_code = 0;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    const uint32_t header = s.palettes[size_t(stage)];
    va = mem::VirtualFromPhysical(kernel::R32(header + 4));
    if (!va || !mem::IsCommitted(va)) {
        va = 0;
        return;
    }
    size_code = kernel::R32(header) >> 30;
    const size_t bytes = std::min<size_t>((256u >> std::min(size_code, 2u)) * sizeof(uint32_t), 1024);
    std::memcpy(colours, kernel::HostPtr(va), bytes);
    hash = Hash(colours, sizeof(uint32_t) * 256);
}

} // namespace

// A surface reused at another size or format: its old images, views and framebuffers may still be
// referenced by the frame being recorded, so they are moved to the retire lists (freed after the
// next fence wait) instead of being leaked or destroyed under the command buffer.
void VulkanBackend::RetireTargetResources(Target* t) {
    if (DeferredPending() &&
        (deferred_write_targets_.count(t) || deferred_read_targets_.count(t))) {
        FlushDeferredScene();
        FlushPass();
    }
    if (defer_rt_ == t) defer_rt_ = nullptr;
    if (defer_ds_ == t) defer_ds_ = nullptr;
    const VkImageView old_view = t->view;
    const VkImageView old_resolved_view = t->resolved_view;
    const VkImageView old_sample_view = t->sample_view;
    RetiredGpu r;
    r.image = t->image;
    r.memory = t->memory;
    r.view = t->view;
    r.resolved_image = t->resolved_image;
    r.resolved_memory = t->resolved_memory;
    r.resolved_view = t->resolved_view;
    r.sample_view = t->sample_view;
    r.fb_image = t->fb_image;
    r.fb_memory = t->fb_memory;
    r.fb_view = t->fb_view;
    r.fb_tex = t->fb_tex;
    retired_gpu_.push_back(r);
    t->image = VK_NULL_HANDLE;
    t->memory = VK_NULL_HANDLE;
    t->view = VK_NULL_HANDLE;
    t->resolved_image = VK_NULL_HANDLE;
    t->resolved_memory = VK_NULL_HANDLE;
    t->resolved_view = VK_NULL_HANDLE;
    t->sample_view = VK_NULL_HANDLE;
    t->fb_image = VK_NULL_HANDLE;
    t->fb_memory = VK_NULL_HANDLE;
    t->fb_view = VK_NULL_HANDLE;
    t->fb_tex = nullptr;
    t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
    t->resolved_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    t->fb_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (pass_rt_ == t) pass_rt_ = nullptr;
    if (pass_ds_ == t) pass_ds_ = nullptr;
    if (sampled_target_ == t) sampled_target_ = nullptr;

    for (auto it = framebuffer_targets_.begin(); it != framebuffer_targets_.end();) {
        if (it->second.first == t || it->second.second == t) {
            if (auto fb = framebuffers_.find(it->first); fb != framebuffers_.end()) {
                retired_framebuffers_.push_back(fb->second);
                framebuffers_.erase(fb);
            }
            it = framebuffer_targets_.erase(it);
        } else {
            ++it;
        }
    }
    // Tex objects borrowed from this target's images point at views that are going away; the draw
    // being recorded may still hold one, so they are deleted after the frame's fence wait
    for (auto it = borrowed_.begin(); it != borrowed_.end();) {
        Tex* tx = it->second;
        if (tx && (tx->view == old_view || tx->view == old_resolved_view || tx->view == old_sample_view)) {
            retired_borrowed_.push_back(tx);
            it = borrowed_.erase(it);
        } else {
            ++it;
        }
    }
    for (auto it = scene_depth_.begin(); it != scene_depth_.end();) {
        if (it->first == t || it->second.depth == t)
            it = scene_depth_.erase(it);
        else
            ++it;
    }
}

void VulkanBackend::DestroyTargets() {
    pass_ = VK_NULL_HANDLE;
    pass_fb_ = VK_NULL_HANDLE;
    pass_rt_ = pass_ds_ = nullptr;
    // Framebuffers reference target views: destroy them before the views and images they point at.
    for (auto& [key, fb] : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    framebuffers_.clear();
    framebuffer_targets_.clear();
    for (auto& [key, t] : borrowed_) delete t;
    borrowed_.clear();
    sampled_target_ = nullptr;
    scene_depth_.clear();
    for (auto& [key, t] : targets_) {
        if (!t) continue;
        if (t->sample_view && t->sample_view != t->resolved_view)
            vkDestroyImageView(device_, t->sample_view, nullptr);
        if (t->resolved_view) vkDestroyImageView(device_, t->resolved_view, nullptr);
        if (t->view) vkDestroyImageView(device_, t->view, nullptr);
        if (t->fb_view) vkDestroyImageView(device_, t->fb_view, nullptr);
        if (t->fb_image) vkDestroyImage(device_, t->fb_image, nullptr);
        if (t->fb_memory) vkFreeMemory(device_, t->fb_memory, nullptr);
        delete t->fb_tex;
        if (t->resolved_image) vkDestroyImage(device_, t->resolved_image, nullptr);
        if (t->resolved_memory) vkFreeMemory(device_, t->resolved_memory, nullptr);
        if (t->image) vkDestroyImage(device_, t->image, nullptr);
        if (t->memory) vkFreeMemory(device_, t->memory, nullptr);
        delete t;
    }
    targets_.clear();
    {
        std::lock_guard lock(late_lock_);
        for (auto& [address, l] : late_)
            for (LateSlot& s : l.slot)
                if (s.buffer) late_retired_.push_back({s.buffer, s.memory});
        late_.clear();
    }
}

Target* VulkanBackend::TargetAt(uint32_t data, bool depth, bool newest) {
    Target* best = nullptr;
    for (auto& [key, t] : targets_) {
        if (!t || t->depth != depth || t->data != data) continue;
        if (!best || (newest ? t->last_use > best->last_use : t->last_use < best->last_use)) best = t;
    }
    return best;
}

bool VulkanBackend::CreateTargetImages(Target& t) {
    auto create_image = [&](uint32_t samples, bool resolved, VkImage& image, VkDeviceMemory& memory,
                            VkImageView& out_view) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = t.format;
        info.extent = {t.w, t.h, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VkSampleCountFlagBits(samples);
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        const VkImageUsageFlags attachment =
            t.depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.usage = resolved ? (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | attachment)
                              : attachment;
        // build into locals and hand them over only when all three exist; nothing has used a
        // partial set yet, so it can be destroyed at once
        VkImage new_image = VK_NULL_HANDLE;
        VkDeviceMemory new_memory = VK_NULL_HANDLE;
        VkImageView new_view = VK_NULL_HANDLE;
        auto fail = [&] {
            if (new_view) vkDestroyImageView(device_, new_view, nullptr);
            if (new_image) vkDestroyImage(device_, new_image, nullptr);
            if (new_memory) vkFreeMemory(device_, new_memory, nullptr);
            return false;
        };
        if (TargetAllocFailTest() || vkCreateImage(device_, &info, nullptr, &new_image) != VK_SUCCESS) return fail();
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device_, new_image, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        bool found = false;
        for (uint32_t i = 0; i < mem_props_.memoryTypeCount; i++)
            if ((req.memoryTypeBits & (1u << i)) &&
                (mem_props_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                alloc.memoryTypeIndex = i;
                found = true;
                break;
            }
        if (!found || vkAllocateMemory(device_, &alloc, nullptr, &new_memory) != VK_SUCCESS) return fail();
        if (vkBindImageMemory(device_, new_image, new_memory, 0) != VK_SUCCESS) return fail();
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = new_image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = t.format;
        view.subresourceRange = {t.depth ? DepthAspect()
                                         : VK_IMAGE_ASPECT_COLOR_BIT,
                                 0, 1, 0, 1};
        if (vkCreateImageView(device_, &view, nullptr, &new_view) != VK_SUCCESS) return fail();
        image = new_image;
        memory = new_memory;
        out_view = new_view;
        return true;
    };
    t.fallback_samples = 0;
    if (t.samples > 1) {
        // the multisampled image and its resolve image succeed or fail together
        if (create_image(t.samples, false, t.image, t.memory, t.view)) {
            if (create_image(1, true, t.resolved_image, t.resolved_memory, t.resolved_view)) {
                t.resolved_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            } else {
                DestroyTargetImages(t);
            }
        }
        if (!t.image) {
            LOG_FIRST(8, "vulkan: %s target %ux%u %ux allocation failed, using one sample",
                      t.depth ? "depth" : "color", t.w, t.h, t.samples);
            // remembered so an unchanged bind does not retry the failed count every time
            t.fallback_samples = t.samples;
            t.samples = 1;
        }
    }
    if (t.samples == 1 && !create_image(1, true, t.image, t.memory, t.view)) return false;
    if (t.depth) {
        // shaders read the depth aspect only; the resolved image's combined view stays for present
        VkImageViewCreateInfo sample{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        sample.image = t.samples > 1 ? t.resolved_image : t.image;
        sample.viewType = VK_IMAGE_VIEW_TYPE_2D;
        sample.format = t.format;
        sample.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &sample, nullptr, &t.sample_view) != VK_SUCCESS) {
            DestroyTargetImages(t);
            return false;
        }
    }
    return true;
}

// Frees a target's own images straight away. Only for images no command buffer has used yet
// (a failed CreateTargetImages); anything recorded goes through RetireTargetResources.
void VulkanBackend::DestroyTargetImages(Target& t) {
    for (VkImageView* v : {&t.view, &t.resolved_view, &t.sample_view})
        if (*v) vkDestroyImageView(device_, *v, nullptr), *v = VK_NULL_HANDLE;
    for (VkImage* i : {&t.image, &t.resolved_image})
        if (*i) vkDestroyImage(device_, *i, nullptr), *i = VK_NULL_HANDLE;
    for (VkDeviceMemory* m : {&t.memory, &t.resolved_memory})
        if (*m) vkFreeMemory(device_, *m, nullptr), *m = VK_NULL_HANDLE;
}

// TCNYC_TARGET_FAIL_TEST=N fails every Nth target image creation, to exercise the cleanup paths
bool VulkanBackend::TargetAllocFailTest() {
    static const uint32_t every = [] {
        const char* v = std::getenv("TCNYC_TARGET_FAIL_TEST");
        return v ? uint32_t(std::strtoul(v, nullptr, 10)) : 0u;
    }();
    if (!every) return false;
    static uint32_t n = 0;
    if (++n % every) return false;
    LOG_FIRST(8, "vulkan: TCNYC_TARGET_FAIL_TEST failing a target image");
    return true;
}

Target* VulkanBackend::EnsureTarget(const Surface& s, bool depth) {
    TextureDesc d;
    if (!DescribeSurface(s, d) || !d.width || !d.height || d.width > 8192 || d.height > 8192) return nullptr;
    // Surface::data is physical; the renderer and guest memory use the virtual address Describe
    // resolves. The shape is part of the key: one address can host several live surface sizes (a
    // scratch pool), and each has to keep its own contents instead of being rebuilt every frame.
    TargetKey key{};
    key.data = d.data;
    key.gw = d.width;
    key.gh = d.height;
    key.format = d.format;
    key.pitch = d.pitch;
    key.depth = depth ? 1 : 0;
    key.linear = d.linear ? 1 : 0;
    auto it = targets_.find(key);
    if (it != targets_.end()) {
        Target* t = it->second;
        // A CPU-written movie/front buffer is rebuilt single-sample so the guest bytes can be
        // uploaded. When it is bound as a render target again, restore the configured sample
        // count; its old contents are gone anyway and the title draws/clears it next. A target
        // whose guest bytes are newer this frame skips that restore: UploadTarget needs the
        // single-sample image anyway, so rebuilding to MSAA first is a wasted allocate/free pair.
        const bool needs_upload = !depth && (DirtyEpoch(t->data) != t->epoch || t->upload_pending);
        // a target that fell back to one sample after a failed allocation waits for an MSAA
        // setting change (or a resize, below) instead of retrying on every bind
        const bool restores_samples = t->samples != msaa_ && t->fallback_samples != msaa_;
        const bool resized = t->scale != scale_ || t->stretch != stretch_;
        if (resized || (restores_samples && !needs_upload)) {
            EndRecord(true);
            RetireTargetResources(t);
            if (resized) {
                t->scale = scale_;
                t->stretch = stretch_;
                t->w = std::max(1u, uint32_t(std::lround(double(d.width) * t->scale * t->stretch)));
                t->h = std::max(1u, uint32_t(std::lround(double(d.height) * t->scale)));
                if (max_image_side_ && (t->w > max_image_side_ || t->h > max_image_side_)) {
                    t->w = std::min(t->w, max_image_side_);
                    t->h = std::min(t->h, max_image_side_);
                }
            }
            t->samples = msaa_;
            t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
            t->resolved_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            t->fb_layout = VK_IMAGE_LAYOUT_UNDEFINED;
            t->drawn = false;
            t->cpu = false;
            t->upload_pending = false;
            t->scene_frame = ~0ull;
            t->epoch = DirtyEpoch(t->data);
            if (!CreateTargetImages(*t)) {
                targets_.erase(it);
                delete t;
                return nullptr;
            }
        }
        t->header = s.header;
        t->size = s.size;
        t->last_use = frames_ + 1;
        if (needs_upload) {
            EndRecord(true);
            UploadTarget(t);
            t->upload_pending = false;
            if (restores_samples) {
                t->drawn = false;
                t->scene_frame = ~0ull;
            }
            if (perf_) perf_target_upload_++;
        }
        return t;
    }
    Target* t = new Target();
    if (perf_) perf_target_new_++;
    t->last_use = frames_ + 1;
    t->data = d.data;
    t->size = s.size;
    t->header = s.header;
    t->guest_format = d.format;
    t->gw = d.width;
    t->gh = d.height;
    t->scale = scale_;
    t->stretch = stretch_;
    t->w = std::max(1u, uint32_t(std::lround(double(d.width) * t->scale * t->stretch)));
    t->h = std::max(1u, uint32_t(std::lround(double(d.height) * t->scale)));
    if (max_image_side_ && (t->w > max_image_side_ || t->h > max_image_side_)) {
        LOGR("vulkan: host target %ux%u exceeds the device limit %u, clamping", t->w, t->h, max_image_side_);
        t->w = std::min(t->w, max_image_side_);
        t->h = std::min(t->h, max_image_side_);
    }
    t->pitch = d.pitch;
    t->linear = d.linear;
    t->swizzled = !d.linear;
    t->depth = depth;
    t->bytes = uint8_t(std::max<uint32_t>(1u, Format(d.format).bits / 8));
    t->format = depth ? depth_format_ : VK_FORMAT_B8G8R8A8_UNORM;

    t->samples = msaa_;
    if (!CreateTargetImages(*t)) {
        delete t;
        return nullptr;
    }
    targets_[key] = t;
    // a reused address can collect a few shapes; drop the ones idle for a long time. Small targets
    // stay for a minute, as on D3D12 (76e299a): the top-right HUD icon is a 104x60 target the game
    // redraws on weapon changes but samples only when the HUD layer redraws, and once dropped the
    // game falls back to the static FS texture in guest memory.
    if (targets_.size() > 64) {
        constexpr uint64_t kSmallTargetPixels = 128 * 128;
        for (auto e = targets_.begin(); e != targets_.end();) {
            Target* c = e->second;
            const uint64_t keep = uint64_t(c->gw) * c->gh <= kSmallTargetPixels ? 3600 : 600;
            if (c != t && c->last_use + keep < frames_) {
                RetireTargetResources(c);
                delete c;
                e = targets_.erase(e);
            } else {
                ++e;
            }
        }
    }
    t->epoch = DirtyEpoch(t->data);
    // A multisampled image cannot be a transfer destination. Scene targets are cleared or drawn
    // before they are read; if the title later writes one through guest memory, the reuse path
    // rebuilds it single-sample and uploads the bytes. Uploading here would write only the resolve
    // image, which the render pass then overwrites with undefined samples.
    if (!depth && t->samples == 1) {
        EndRecord(true);
        UploadTarget(t);
        if (perf_) perf_target_upload_++;
    }
    return t;
}

// Uploaded guest pixels cannot be copied into a multisampled image. Recreate this target single
// sample so the guest bytes are actually what the next draw and read-back see. Rare in practice:
// only targets the CPU writes through a lock need this.
bool VulkanBackend::RebuildTargetSingleSample(Target* t) {
    if (!t || t->samples <= 1) return true;
    EndRecord(true);
    // Retire, do not destroy: framebuffers, borrowed Tex wrappers and the scene-depth map may
    // still name these views until the frame's fence wait.
    RetireTargetResources(t);
    t->samples = 1;

    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = t->format;
    info.extent = {t->w, t->h, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                 (t->depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT);
    bool ok = vkCreateImage(device_, &info, nullptr, &t->image) == VK_SUCCESS;
    if (ok) {
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device_, t->image, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        bool found = false;
        for (uint32_t i = 0; i < mem_props_.memoryTypeCount; i++)
            if ((req.memoryTypeBits & (1u << i)) &&
                (mem_props_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                alloc.memoryTypeIndex = i;
                found = true;
                break;
            }
        ok = found && vkAllocateMemory(device_, &alloc, nullptr, &t->memory) == VK_SUCCESS;
        if (ok) {
            ok = vkBindImageMemory(device_, t->image, t->memory, 0) == VK_SUCCESS;
        }
        if (ok) {
            t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = t->image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = t->format;
            view.subresourceRange = {t->depth ? DepthAspect()
                                              : VK_IMAGE_ASPECT_COLOR_BIT,
                                     0, 1, 0, 1};
            ok = vkCreateImageView(device_, &view, nullptr, &t->view) == VK_SUCCESS;
            if (ok && t->depth) {
                view.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
                ok = vkCreateImageView(device_, &view, nullptr, &t->sample_view) == VK_SUCCESS;
            }
        }
    }
    if (!ok) {
        if (t->sample_view) vkDestroyImageView(device_, t->sample_view, nullptr);
        if (t->view) vkDestroyImageView(device_, t->view, nullptr);
        if (t->image) vkDestroyImage(device_, t->image, nullptr);
        if (t->memory) vkFreeMemory(device_, t->memory, nullptr);
        t->image = VK_NULL_HANDLE;
        t->memory = VK_NULL_HANDLE;
        t->view = VK_NULL_HANDLE;
        t->sample_view = VK_NULL_HANDLE;
    }
    return ok;
}

bool VulkanBackend::UploadTarget(Target* t) {
    if (!t || t->depth || !t->w || !t->h) return false;
    if (t->w > 16384 || t->h > 16384) return false;
    if (t->samples > 1 && !RebuildTargetSingleSample(t)) return false;
    const uint32_t row = t->w * 4;
    std::vector<uint8_t> px(size_t(row) * t->h, 0);
    const bool valid = t->data && t->size && mem::IsCommitted(t->data);
    const uint64_t span =
        valid && (t->swizzled ? uint64_t(t->gw) * t->gh * t->bytes : uint64_t(t->gh) * t->pitch);
    const bool in_range = valid && span <= uint64_t(t->size) + 4096;
    const uint8_t* guest = valid ? static_cast<const uint8_t*>(kernel::HostPtr(t->data)) : nullptr;
    if (guest && in_range) {
        std::vector<uint8_t> src(size_t(t->gw) * 4 * t->gh, 0);
        for (uint32_t y = 0; y < t->gh; y++)
            for (uint32_t x = 0; x < t->gw; x++) {
                const uint32_t off = t->swizzled ? SwizzleOffset(x, y, t->gw, t->gh) * t->bytes
                                                 : y * t->pitch + x * t->bytes;
                uint8_t* o = &src[(size_t(y) * t->gw + x) * 4];
                if (t->bytes >= 4) std::memcpy(o, guest + off, 4);
                else if (t->bytes == 2) {
                    uint16_t c;
                    std::memcpy(&c, guest + off, 2);
                    o[0] = uint8_t((c & 31) * 255 / 31);
                    o[1] = uint8_t(((c >> 5) & 63) * 255 / 63);
                    o[2] = uint8_t(((c >> 11) & 31) * 255 / 31);
                    o[3] = 255;
                }
            }
        for (uint32_t y = 0; y < t->h; y++) {
            const uint32_t sy = std::min(t->gh - 1, uint32_t(uint64_t(y) * t->gh / t->h));
            for (uint32_t x = 0; x < t->w; x++) {
                const uint32_t sx = std::min(t->gw - 1, uint32_t(uint64_t(x) * t->gw / t->w));
                std::memcpy(&px[(size_t(y) * t->w + x) * 4], &src[(size_t(sy) * t->gw + sx) * 4], 4);
            }
        }
    }
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* map = nullptr;
    StagingBuffer(device_, mem_props_, row * t->h, buffer, memory, &map);
    if (!map) return false;
    std::memcpy(map, px.data(), px.size());
    if (!recording_) BeginRecord();
    VkImage upload_image = t->image;
    VkImageLayout& upload_layout = t->layout;
    Transition(upload_image, upload_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->format);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {t->w, t->h, 1};
    vkCmdCopyBufferToImage(cmd_, buffer, upload_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    // The copy is ordered with the draws already recorded. Keep the staging
    // buffer until the next fence wait; do not submit and stall per upload.
    vkUnmapMemory(device_, memory);
    staging_pending_.emplace_back(buffer, memory);
    t->epoch = DirtyEpoch(t->data);
    t->cpu = true;
    return true;
}

bool VulkanBackend::ReadTarget(Target* t) {
    if (!t || !t->drawn) return false;
    if (t->depth) {
        t->drawn = false;
        t->epoch = DirtyEpoch(t->data);
        return false;
    }
    const uint32_t row = t->w * 4;
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* map = nullptr;
    StagingBuffer(device_, mem_props_, row * t->h, buffer, memory, &map);
    if (!map) return false;
    EndRecord(true);
    BeginRecord();
    VkImage read_image = t->samples > 1 ? t->resolved_image : t->image;
    VkImageLayout& read_layout = t->samples > 1 ? t->resolved_layout : t->layout;
    Transition(read_image, read_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->format);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {t->w, t->h, 1};
    vkCmdCopyImageToBuffer(cmd_, read_image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
    EndRecord(true);
    if (dump_targets_ && dump_now_ && !t->depth) {
        std::error_code ec;
        std::filesystem::create_directories(app::ExeDir() / "logs", ec);
        char name[64];
        std::snprintf(name, sizeof(name), "%s_%u_%08X.tga", dump_prefix_.c_str(), dump_frame_, t->data);
        // keep alpha: the light/reflection composites blend by the scene's destination alpha
        if (t->format == VK_FORMAT_B8G8R8A8_UNORM)
            WriteRgbaTga((app::ExeDir() / "logs" / name).string().c_str(), static_cast<const uint8_t*>(map), t->w,
                         t->h, t->w * 4);
        else
            WriteSwapTga((app::ExeDir() / "logs" / name).string().c_str(), static_cast<const uint8_t*>(map), t->w,
                         t->h, t->format, HdrOutput::Off, 200.0f);
    }
    WriteGuestPixels(t, static_cast<const uint8_t*>(map), t->w, t->h, row);
    vkUnmapMemory(device_, memory);
    vkDestroyBuffer(device_, buffer, nullptr);
    vkFreeMemory(device_, memory, nullptr);
    t->drawn = false;
    t->epoch = DirtyEpoch(t->data);
    return true;
}

void VulkanBackend::DumpTexture(const Tex* t) {
    if (!t || !t->data || !t->w || !t->h) return;
    if (!mem::IsCommitted(t->data)) {
        LOGR("linux: dump tex %08X not committed", t->data);
        return;
    }
    TextureDesc d;
    d.data = t->data;
    d.format = t->guest_format;
    d.width = t->w;
    d.height = t->h;
    d.depth = t->depth ? t->depth : 1;
    d.levels = t->levels;
    d.cube = t->cube;
    const FormatInfo& f = Format(d.format);
    d.linear = !f.swizzled && !f.compressed;
    d.pitch = d.linear ? d.width * f.bits / 8 : 0;
    std::vector<uint8_t> px;
    uint32_t pitch = 0;
    DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    if (px.empty()) {
        LOGR("linux: dump tex %08X empty fmt %02X", t->data, d.format);
        return;
    }
    std::error_code ec;
    std::filesystem::create_directories(app::ExeDir() / "logs", ec);
    char name[64];
    std::snprintf(name, sizeof(name), "tex_%u_%08X", dump_frame_, t->data);
    const std::string base = (app::ExeDir() / "logs" / name).string();
    if (f.compressed) {
        if (FILE* file = std::fopen((base + ".bin").c_str(), "wb")) {
            std::fwrite(px.data(), 1, px.size(), file);
            std::fclose(file);
        }
        if (FILE* file = std::fopen((base + ".txt").c_str(), "w")) {
            std::fprintf(file, "fmt %02X %ux%u levels %u pitch %u\n", d.format, d.width, d.height, d.levels, pitch);
            std::fclose(file);
        }
    } else {
        WriteRgbaTga((base + ".tga").c_str(), px.data(), t->w, t->h, pitch);
    }
}

// BGRA host rows into the guest surface, nearest-scaled; swizzled and 16-bit as the target is
void WriteGuestSurface(uint32_t data, uint32_t size, const uint8_t* src, uint32_t src_w, uint32_t src_h,
                       uint32_t src_pitch, uint32_t gw, uint32_t gh, uint32_t guest_pitch, bool swizzled,
                       uint32_t bytes) {
    if (!data || !size || !src || !gw || !gh || !mem::IsCommitted(data)) return;
    const uint64_t span = swizzled ? uint64_t(gw) * gh * bytes : uint64_t(gh) * guest_pitch;
    if (span > uint64_t(size) + 4096) return;
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    if (!guest) return;
    for (uint32_t y = 0; y < gh; y++)
        for (uint32_t x = 0; x < gw; x++) {
            const uint32_t sy = std::min(src_h - 1, uint32_t(uint64_t(y) * src_h / gh));
            const uint32_t sx = std::min(src_w - 1, uint32_t(uint64_t(x) * src_w / gw));
            const uint32_t off = swizzled ? SwizzleOffset(x, y, gw, gh) * bytes : y * guest_pitch + x * bytes;
            const uint8_t* s = src + size_t(sy) * src_pitch + sx * 4;
            if (bytes >= 4) {
                std::memcpy(guest + off, s, 4);
            } else if (bytes == 2) {
                uint16_t c = uint16_t((s[2] >> 3) << 11 | (s[1] >> 2) << 5 | (s[0] >> 3));
                std::memcpy(guest + off, &c, 2);
            }
        }
}

void VulkanBackend::WriteGuestPixels(Target* t, const uint8_t* src, uint32_t src_w, uint32_t src_h, uint32_t pitch) {
    WriteGuestSurface(t->data, t->size, src, src_w, src_h, pitch, t->gw, t->gh, t->pitch, t->swizzled, t->bytes);
}

// a content signature: small surfaces whole, large ones as evenly spaced samples
uint64_t MemSig(uint32_t data, uint32_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    if (!p || !total) return 0;
    constexpr uint32_t kWhole = 65536, kSamples = 64, kSampleBytes = 64;
    uint64_t sig = Hash(&total, sizeof(total));
    if (total <= kWhole) return Hash(p, total, sig);
    for (uint32_t i = 0; i < kSamples; i++)
        sig = Hash(p + uint64_t(total - kSampleBytes) * i / (kSamples - 1), kSampleBytes, sig);
    return sig;
}

Tex* VulkanBackend::EnsureTexture(const State& s, int stage, bool* is_target) {
    if (is_target) *is_target = false;
    sampled_target_ = nullptr;
    tex_reason_ = "";
    TextureDesc d;
    if (!Describe(s.textures[size_t(stage)], d) || !d.width || !d.height) {
        tex_reason_ = "describe";
        return nullptr;
    }
    if (!mem::IsCommitted(d.data)) {
        tex_reason_ = "uncommitted";
        return nullptr;
    }
    if (d.width > 8192 || d.height > 8192) {
        tex_reason_ = "too big";
        return nullptr;
    }
    const uint32_t tex_bytes = TotalSize(d);
    if (tex_bytes && !mem::IsCommitted(d.data + tex_bytes - 1)) {
        tex_reason_ = "bytes uncommitted";
        return nullptr;
    }
    const bool depth_tex = Format(d.format).depth;
    // D3D12 only samples a target for a 2D, one-level, uncompressed texture; a cube, volume or
    // compressed texture that happens to sit at a target's address is guest data, not the target.
    if (!d.cube && d.depth == 1 && !Format(d.format).compressed) {
        Target* target = nullptr;
        for (auto& [tk, cand] : targets_) {
            if (cand->data != d.data || cand->depth != depth_tex) continue;
            if (cand->gw == d.width && cand->gh == d.height) {
                target = cand;
                break;
            }
            // D3D12 BindTexture: only a target of the texture's size is sampled; any other size reads guest memory
        }
        if (target) {
            target->last_use = frames_ + 1;
            if (target->data == s.rt.data || target->data == s.zs.data) {
                if (perf_) perf_feedback_++;
                LOG_FIRST(24, "vulkan: feedback texture %08X fmt %X rt %08X depth %d", d.data, d.format, s.rt.data,
                          int(depth_tex));
                if (is_target) *is_target = true;
                sampled_target_ = target;
                if (DeferredPending()) FlushDeferredScene();
                return FeedbackTexture(s, target, d);
            }
            const BorrowKey bkey{target, d.format, uint8_t(depth_tex)};
            auto it = borrowed_.find(bkey);
            if (it == borrowed_.end()) {
                Tex* tx = new Tex();
                tx->borrowed = true;
                tx->image = target->samples > 1 ? target->resolved_image : target->image;
                tx->view = target->samples > 1
                               ? target->resolved_view
                               : (depth_tex && target->sample_view ? target->sample_view : target->view);
                tx->format = target->format;
                tx->w = target->w;
                tx->h = target->h;
                tx->data = d.data;
                tx->guest_format = d.format;
                borrowed_[bkey] = tx;
                it = borrowed_.find(bkey);
            }
            // sampling a target the pending draws render into needs their output first; one they
            // only read can be sampled now (it stays in a read layout)
            if (DeferredWrites(target)) FlushDeferredScene();
            VkImage target_image = target->samples > 1 ? target->resolved_image : target->image;
            VkImageLayout& target_layout = target->samples > 1 ? target->resolved_layout : target->layout;
            const VkImageLayout read_layout =
                depth_tex ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            // A barrier cannot run inside a render pass, so only a target that still has to change layout
            // ends the open pass. One already readable is sampled in place: ending the scene pass for every
            // draw that samples a small target (the garage light pools) restarted it ~140 times a frame,
            // reloading and resolving the whole multisampled frame each time.
            if (target_layout != read_layout) {
                FlushPass();
                BeginRecord();
                Transition(target_image, target_layout, read_layout, target->format);
            }
            it->second->layout = target_layout;  // the descriptor must name the image's real layout
            if (is_target) *is_target = true;
            sampled_target_ = target;
            return it->second;
        }
    }
    uint32_t colours[256];
    uint32_t palette_va = 0;
    uint64_t palette_hash = 0;
    ReadStagePalette(s, stage, d, colours, palette_va, palette_hash);
    TexKey key{};
    key.data = d.data;
    key.format = d.format;
    key.width = d.width;
    key.height = d.height;
    key.depth = d.depth;
    key.levels = d.levels;
    key.pitch = d.pitch;
    key.palette_va = palette_va;
    key.palette = palette_hash;
    key.flags = (d.cube ? 1u : 0u) | (d.linear ? 2u : 0u);

    const FormatInfo& fi = Format(d.format);
    const bool bc_fallback = fi.compressed && !bc_supported_;
    auto it = textures_.find(key);
    Tex* t = it != textures_.end() ? it->second : nullptr;
    const uint32_t layers = d.cube ? 6 : 1;
    uint32_t image_w = d.width, image_h = d.height, levels = std::max(1u, d.levels);
    // BC block copies need block-aligned extents; round the image up and drop its
    // mip chain, as the Windows renderer does
    if (fi.compressed && bc_supported_ && ((image_w & 3) || (image_h & 3))) {
        image_w = (image_w + 3) & ~3u;
        image_h = (image_h + 3) & ~3u;
        levels = 1;
    }
    const bool same = t && t->w == image_w && t->h == image_h && t->levels == levels &&
                      t->guest_format == d.format && t->layers == layers && t->depth == d.depth &&
                      t->bc_fallback == bc_fallback;
    if (!same) {
        if (!t) t = new Tex();
        if (t->image) {
            if (deferred_read_images_.count(t->image)) {
                FlushDeferredScene();
                EndRecord(true);  // the replay commands still name the image being replaced
            }
            vkDestroyImageView(device_, t->view, nullptr);
            vkDestroyImage(device_, t->image, nullptr);
            vkFreeMemory(device_, t->memory, nullptr);
            t->view = VK_NULL_HANDLE;
            t->image = VK_NULL_HANDLE;
            t->memory = VK_NULL_HANDLE;
        }
        t->w = image_w;
        t->h = image_h;
        t->levels = levels;
        t->layers = layers;
        t->cube = d.cube;
        t->format = depth_tex ? VK_FORMAT_D32_SFLOAT
                             : bc_fallback ? VK_FORMAT_B8G8R8A8_UNORM : HostFormatVk(fi.host);
        t->bc_fallback = bc_fallback;
        t->data = d.data;
        t->guest_format = d.format;
        t->depth = d.depth;
        t->palette = palette_hash;
        t->sig = 0;
        t->frame = frames_ + 1;  // in use now: forced eviction has to keep it
        if (!MakeTexImage(*t)) {
            // device memory is tight: drop idle textures and try once more before the white fallback
            ReleaseOldTextures(true);
            if (!MakeTexImage(*t)) {
                tex_reason_ = "out of device memory";
                LOGE("vulkan: out of device memory for a %ux%u texture, drawing it white", image_w, image_h);
                if (it != textures_.end()) textures_.erase(it);
                if (t->view) vkDestroyImageView(device_, t->view, nullptr);
                if (t->image) vkDestroyImage(device_, t->image, nullptr);
                if (t->memory) vkFreeMemory(device_, t->memory, nullptr);
                delete t;
                return nullptr;
            }
        }
        textures_[key] = t;
    }

    // cheap change check once per frame: lock epochs plus a sample of the bytes, so
    // a texture rewritten in place without a lock mark is still uploaded
    // the button-glyph atlas is uploaded again when the active input device changes
    const auto prompt_style = IsPromptAtlas(d) ? input::ControllerPromptStyle() : input::PromptStyle::Original;
    if (!same || t->frame != frames_ + 1 || t->palette != palette_hash || t->prompt_style != prompt_style) {
        t->frame = frames_ + 1;
        t->palette = palette_hash;
        const uint64_t sig = MemSig(d.data, TotalSize(d)) ^ DirtyEpoch(d.data) ^ palette_hash;
        if (!same || sig != t->sig || t->prompt_style != prompt_style) {
            t->sig = sig;
            t->prompt_style = prompt_style;

            UploadTexture(t, d, palette_va ? colours : nullptr, depth_tex);
            t->pack_gen = 0;  // new bytes: the pack and dump key is taken again
            t->swap = nullptr;
        }
    }
    // texture packs: a pack image is drawn in place of the guest bytes once it has loaded
    if (t->pack_gen != texturedumper::Generation())
        LatchPackKey(*t, d, fi, palette_va ? colours : nullptr);
    if (t->pack_key) LatchPackImage(*t);
    return t;
}

// after new bytes or a texture setting change: the name the pack and the dumps know the bytes by
void VulkanBackend::LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi,
                                 const uint32_t* colours) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
    tx.pack_gen = texturedumper::Generation();
    tx.swap = nullptr;
    const uint64_t key = texturedumper::Key(d, fi, p, colours);
    texturedumper::Dump(key, d, p, colours);
    // dumping alone keys the bytes too: the pack image is not drawn then
    tx.pack_key = texturedumper::Replacing() ? key : 0;
}

// the pack image for tx's bytes once it has loaded; textures with the same bytes share its GPU copy
void VulkanBackend::LatchPackImage(Tex& tx) {
    auto it = pack_textures_.find(tx.pack_key);
    if (it == pack_textures_.end()) {
        texturedumper::Image img;
        const texturedumper::Lookup got = texturedumper::Take(tx.pack_key, img);
        if (got == texturedumper::Lookup::Loading) return;
        if (got == texturedumper::Lookup::None) {
            tx.pack_key = 0;
            return;
        }
        Tex* pack = new Tex();
        if (!CreatePackTexture(*pack, img)) {
            delete pack;
            tx.pack_key = 0;
            return;
        }
        it = pack_textures_.emplace(tx.pack_key, pack).first;
    }
    if (it->second->image) tx.swap = it->second;
    tx.pack_key = 0;
}

// a pack image with all its levels, sampled like a guest texture at any resolution
bool VulkanBackend::CreatePackTexture(Tex& r, texturedumper::Image& img) {
    if (img.levels.empty() || !img.width || !img.height) return false;
    if (!bc_supported_) {
        Host host = img.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK ? Host::BC1
                  : img.format == VK_FORMAT_BC2_UNORM_BLOCK ? Host::BC2
                  : img.format == VK_FORMAT_BC3_UNORM_BLOCK ? Host::BC3
                                                            : Host::None;
        if (host != Host::None) {
            for (uint32_t l = 0; l < img.levels.size(); l++) {
                std::vector<uint8_t> decoded;
                uint32_t decoded_pitch = 0;
                if (!DecodeBcLevel(img.levels[l].px.data(), std::max(1u, img.width >> l),
                                   std::max(1u, img.height >> l), host, decoded, decoded_pitch))
                    return false;
                img.levels[l].px = std::move(decoded);
                img.levels[l].pitch = decoded_pitch;
            }
            img.format = VK_FORMAT_B8G8R8A8_UNORM;
        }
    }
    r.format = img.format;
    r.w = img.width;
    r.h = img.height;
    r.levels = uint32_t(img.levels.size());
    r.layers = 1;
    r.depth = 1;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = r.format;
    info.extent = {r.w, r.h, 1};
    info.mipLevels = r.levels;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(device_, &info, nullptr, &r.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, r.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    bool found = false;
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mem_props_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            alloc.memoryTypeIndex = i;
            found = true;
            break;
        }
    if (!found || vkAllocateMemory(device_, &alloc, nullptr, &r.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(device_, r.image, r.memory, 0);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = r.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = r.format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, r.levels, 0, 1};
    if (vkCreateImageView(device_, &view, nullptr, &r.view) != VK_SUCCESS) return false;

    VkDeviceSize total = 0;
    for (const texturedumper::Level& l : img.levels) total += l.px.size();
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* map = nullptr;
    StagingBuffer(device_, mem_props_, total ? total : 1, buffer, memory, &map);
    if (!map) return false;
    std::vector<VkBufferImageCopy> copies;
    copies.reserve(img.levels.size());
    VkDeviceSize at = 0;
    for (uint32_t l = 0; l < img.levels.size(); l++) {
        const texturedumper::Level& level = img.levels[l];
        std::memcpy(static_cast<uint8_t*>(map) + at, level.px.data(), level.px.size());
        VkBufferImageCopy copy{};
        copy.bufferOffset = at;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, l, 0, 1};
        copy.imageExtent = {std::max(1u, img.width >> l), std::max(1u, img.height >> l), 1};
        copies.push_back(copy);
        at += level.px.size();
    }
    EndRecord(true);
    BeginRecord();
    Transition(r.image, r.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, r.format, r.levels);
    for (const VkBufferImageCopy& copy : copies)
        vkCmdCopyBufferToImage(cmd_, buffer, r.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    Transition(r.image, r.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, r.format, r.levels);
    EndRecord(true);
    vkUnmapMemory(device_, memory);
    vkDestroyBuffer(device_, buffer, nullptr);
    vkFreeMemory(device_, memory, nullptr);
    LOGR("texture pack: %ux%u %s image ready", r.w, r.h, r.format == VK_FORMAT_UNDEFINED ? "?" : "pack");
    return true;
}

// the pack's files changed: each image is made again when next drawn
void VulkanBackend::ReleasePackTextures() {
    if (DeferredPending()) FlushDeferredScene();
    for (auto& entry : pack_textures_) pack_textures_retired_.push_back(entry.second);
    pack_textures_.clear();
}

// a copy of the target to read while the draw writes to it, as the D3D12 backend does
Tex* VulkanBackend::FeedbackTexture(const State& s, Target* t, const TextureDesc& d) {
    if (!t || t->depth || !t->image) return nullptr;
    if (t->samples > 1) {
        if (!t->fb_tex) {
            t->fb_tex = new Tex();
            t->fb_tex->borrowed = true;
            t->fb_tex->image = t->resolved_image;
            t->fb_tex->view = t->resolved_view;
            t->fb_tex->format = t->format;
            t->fb_tex->w = t->w;
            t->fb_tex->h = t->h;
            t->fb_tex->levels = 1;
            t->fb_tex->layers = 1;
            t->fb_tex->depth = 1;
        }
        t->fb_tex->data = d.data;
        t->fb_tex->guest_format = d.format;
        FlushPass();
        BeginRecord();
        Transition(t->resolved_image, t->resolved_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, t->format);
        t->fb_tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        return t->fb_tex;
    }
    if (!t->fb_image) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = t->format;
        info.extent = {t->w, t->h, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (vkCreateImage(device_, &info, nullptr, &t->fb_image) != VK_SUCCESS) return nullptr;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device_, t->fb_image, &req);
        VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
        alloc.allocationSize = req.size;
        for (uint32_t i = 0; i < mem_props_.memoryTypeCount; i++)
            if ((req.memoryTypeBits & (1u << i)) &&
                (mem_props_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                alloc.memoryTypeIndex = i;
                break;
            }
        if (vkAllocateMemory(device_, &alloc, nullptr, &t->fb_memory) != VK_SUCCESS) return nullptr;
        vkBindImageMemory(device_, t->fb_image, t->fb_memory, 0);
        t->fb_layout = VK_IMAGE_LAYOUT_UNDEFINED;
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = t->fb_image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = t->format;
        view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &view, nullptr, &t->fb_view) != VK_SUCCESS) return nullptr;
        t->fb_tex = new Tex();
        t->fb_tex->borrowed = true;
        t->fb_tex->image = t->fb_image;
        t->fb_tex->view = t->fb_view;
        t->fb_tex->format = t->format;
        t->fb_tex->w = t->w;
        t->fb_tex->h = t->h;
        t->fb_tex->levels = 1;
        t->fb_tex->layers = 1;
        t->fb_tex->depth = 1;
    }
    // copies and barriers must be outside a render pass
    if (pass_ != VK_NULL_HANDLE) FlushPass();
    BeginRecord();
    Transition(t->image, t->layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->format);
    Transition(t->fb_image, t->fb_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->format);
    VkImageCopy region{};
    region.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.dstSubresource = region.srcSubresource;
    region.extent = {t->w, t->h, 1};
    vkCmdCopyImage(cmd_, t->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->fb_image,
                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    Transition(t->fb_image, t->fb_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, t->format);
    Transition(t->image, t->layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, t->format);
    t->fb_tex->layout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    // the pass was flushed for the copy: reopen the draw's targets before it continues
    Target* rt = nullptr;
    Target* ds = nullptr;
    BindTargets(s, rt, ds);
    t->fb_tex->data = d.data;
    t->fb_tex->guest_format = d.format;
    return t->fb_tex;
}

bool VulkanBackend::MakeTexImage(Tex& t) {
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = t.format;
    info.extent = {t.w, t.h, 1};
    info.mipLevels = t.levels;
    info.arrayLayers = t.layers;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (t.cube) info.flags = VK_IMAGE_CREATE_CUBE_COMPATIBLE_BIT;
    if (vkCreateImage(device_, &info, nullptr, &t.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, t.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    bool found = false;
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mem_props_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            alloc.memoryTypeIndex = i;
            found = true;
            break;
        }
    if (!found || vkAllocateMemory(device_, &alloc, nullptr, &t.memory) != VK_SUCCESS) {
        vkDestroyImage(device_, t.image, nullptr);
        t.image = VK_NULL_HANDLE;
        return false;
    }
    vkBindImageMemory(device_, t.image, t.memory, 0);
    t.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = t.image;
    view.viewType = t.cube ? VK_IMAGE_VIEW_TYPE_CUBE : VK_IMAGE_VIEW_TYPE_2D;
    view.format = t.format;
    // Tex::depth is the volume depth, not the format's range: a 2D colour texture always has 1.
    const bool depth_format = Format(t.guest_format).depth;
    view.subresourceRange = {depth_format ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT, 0, t.levels, 0,
                             t.layers};
    if (vkCreateImageView(device_, &view, nullptr, &t.view) != VK_SUCCESS) {
        vkDestroyImage(device_, t.image, nullptr);
        vkFreeMemory(device_, t.memory, nullptr);
        t.image = VK_NULL_HANDLE;
        t.memory = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

// The Windows backend's ReleaseOldTextures: a district streams through thousands of textures, and
// keeping every one alive exhausts device memory (white textures, then a wedged GPU). A forced
// sweep keeps everything this frame used, so it can run in the middle of a draw.
size_t VulkanBackend::ReleaseOldTextures(bool force) {
    constexpr uint64_t kSweepEvery = 60;
    constexpr size_t kTexturesKept = 2048;
    constexpr uint64_t kIdleFrames = 600;
    if (!force && (frames_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept)) return 0;
    if (DeferredPending()) {
        FlushDeferredScene();
        EndRecord(true);  // the retired images may be named by the replay commands
    }
    const size_t before = textures_.size();
    for (auto it = textures_.begin(); it != textures_.end();) {
        Tex* t = it->second;
        const bool in_use = t && t->frame == frames_ + 1;
        if (t && (in_use || (!force && t->frame + kIdleFrames >= frames_))) {
            ++it;
            continue;
        }
        if (t) {
            if (t->view) vkDestroyImageView(device_, t->view, nullptr);
            if (t->image) vkDestroyImage(device_, t->image, nullptr);
            if (t->memory) vkFreeMemory(device_, t->memory, nullptr);
            delete t;
        }
        it = textures_.erase(it);
    }
    const size_t released = before - textures_.size();
    if (released) LOGR("vulkan: texture cache %zu -> %zu (released %zu)", before, textures_.size(), released);
    return released;
}

void VulkanBackend::UploadTexture(Tex* t, const TextureDesc& d, const uint32_t* palette, bool depth_tex) {
    if (!t || !t->image) return;
    const FormatInfo& fi = Format(d.format);
    // every mip level of every face through one staging buffer
    LOG_FIRST(24, "vulkan: upload tex %08X %ux%u fmt %X host %d levels %u", d.data, d.width, d.height, d.format,
              int(fi.host), t->levels);
    const uint32_t levels = std::max(1u, t->levels);
    const VkImageAspectFlags aspect = depth_tex ? VK_IMAGE_ASPECT_DEPTH_BIT : VK_IMAGE_ASPECT_COLOR_BIT;
    std::vector<std::vector<uint8_t>> images;
    std::vector<VkBufferImageCopy> copies;
    images.reserve(size_t(levels) * t->layers);
    copies.reserve(size_t(levels) * t->layers);
    std::vector<uint8_t> px;
    uint32_t pitch = 0;
    for (uint32_t l = 0; l < levels; l++) {
        for (uint32_t f = 0; f < t->layers; f++) {
            DecodeLevel(d, l, f, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), palette, px, pitch);
            if (t->bc_fallback) {
                std::vector<uint8_t> decoded;
                uint32_t decoded_pitch = 0;
                const uint32_t lw = std::max(1u, t->w >> l), lh = std::max(1u, t->h >> l);
                if (DecodeBcLevel(px.data(), lw, lh, fi.host, decoded, decoded_pitch)) {
                    images.push_back(std::move(decoded));
                    pitch = decoded_pitch;
                } else {
                    images.push_back({});
                }
            } else {
                if (t->prompt_style != input::PromptStyle::Original) ReplaceControllerGlyphs(px, pitch, t->prompt_style);
                images.push_back(px);
            }
            VkBufferImageCopy copy{};
            copy.imageSubresource = {aspect, l, f, 1};
            copy.imageExtent = {std::max(1u, t->w >> l), std::max(1u, t->h >> l), 1};
            copies.push_back(copy);
        }
    }
    VkDeviceSize total = 0;
    for (const auto& im : images) total += im.size();
    VkBuffer buffer = VK_NULL_HANDLE;
    VkDeviceMemory memory = VK_NULL_HANDLE;
    void* map = nullptr;
    StagingBuffer(device_, mem_props_, total ? total : 1, buffer, memory, &map);
    if (!map) return;
    VkDeviceSize at = 0;
    for (size_t i = 0; i < images.size(); i++) {
        copies[i].bufferOffset = at;
        std::memcpy(static_cast<uint8_t*>(map) + at, images[i].data(), images[i].size());
        at += images[i].size();
    }
    // An upload of a texture no pending scene draw uses need not force a replay. The upload
    // itself stays in submission order before any later deferred replay and cannot affect it.
    if (deferred_read_images_.count(t->image)) FlushDeferredScene();
    const bool old_skip = skip_deferred_flush_;
    skip_deferred_flush_ = true;
    EndRecord(true);
    BeginRecord();
    Transition(t->image, t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->format, t->levels, t->layers);
    for (const VkBufferImageCopy& copy : copies)
        vkCmdCopyBufferToImage(cmd_, buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    Transition(t->image, t->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, t->format, t->levels, t->layers);
    EndRecord(true);
    skip_deferred_flush_ = old_skip;
    vkUnmapMemory(device_, memory);
    vkDestroyBuffer(device_, buffer, nullptr);
    vkFreeMemory(device_, memory, nullptr);
    tex_uploads_++;
}

VkSampler VulkanBackend::EnsureSampler(const State& s, int stage, const TexInfo& info, bool clamp, uint32_t levels) {
    const auto& t = s.tss[size_t(stage)];
    const uint32_t mag = t[kTssMagFilter], min = t[kTssMinFilter], mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    const float lodbias = Bits(t[kTssMipLodBias]);
    const uint32_t maxmip = t[kTssMaxMipLevel];
    uint32_t lodbits;
    std::memcpy(&lodbits, &lodbias, sizeof(lodbits));

    // D3DTSS_MAXANISOTROPY, or the menu's override where the texture has mips
    uint32_t an = (mag == kTexfAnisotropic || min == kTexfAnisotropic)
                      ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1)
                      : 1;
    if (aniso_setting_ && lin(min) && mip && levels > 1 && !info.shadow) an = aniso_setting_;
    if (info.shadow) an = 1;
    const uint32_t max_an = sampler_anisotropy_ ? uint32_t(std::max(1.0f, max_anisotropy_)) : 1;
    an = std::min<uint32_t>(an, max_an);

    SamplerKey key{};
    key.mag = mag;
    key.min = min;
    key.mip = mip;
    key.addr_u = t[kTssAddressU];
    key.addr_v = t[kTssAddressV];
    key.addr_w = t[kTssAddressW];
    key.maxmip = maxmip;
    key.aniso = an;
    key.shadow = info.shadow;
    key.clamp = clamp;
    key.lodbias = lodbits;
    key.border = t[kTssBorderColor];
    key.shadow_func = s.rs[kRsShadowFunc];
    auto it = samplers_.find(key);
    if (it != samplers_.end()) return it->second;
    VkSamplerCreateInfo info_vk{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    info_vk.magFilter = an > 1 || lin(mag) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info_vk.minFilter = an > 1 || lin(min) ? VK_FILTER_LINEAR : VK_FILTER_NEAREST;
    info_vk.mipmapMode = an > 1 || mip == kTexfLinear ? VK_SAMPLER_MIPMAP_MODE_LINEAR
                                                      : VK_SAMPLER_MIPMAP_MODE_NEAREST;
    info_vk.addressModeU = AddressMode(clamp && t[kTssAddressU] == 1 ? 3 : t[kTssAddressU]);
    info_vk.addressModeV = AddressMode(clamp && t[kTssAddressV] == 1 ? 3 : t[kTssAddressV]);
    info_vk.addressModeW = AddressMode(t[kTssAddressW]);
    info_vk.mipLodBias = lodbias;
    info_vk.minLod = float(maxmip);
    info_vk.maxLod = mip ? VK_LOD_CLAMP_NONE : float(maxmip);
    info_vk.anisotropyEnable = an > 1;
    info_vk.maxAnisotropy = float(an);
    info_vk.compareEnable = info.shadow != 0;
    info_vk.compareOp = CompareOp(ReverseCompare(s.rs[kRsShadowFunc]));
    // the guest's per-stage border colour, when the driver can use a custom one; transparent black
    // is the fallback (Vulkan's fixed colours are the only others without the extension)
    float border[4];
    Color(t[kTssBorderColor], border);
    if (t[kTssAddressU] == 4 || t[kTssAddressV] == 4)
        LOG_FIRST(4, "vulkan: border sampler %08X -> %.2f %.2f %.2f %.2f", t[kTssBorderColor], border[0], border[1],
                  border[2], border[3]);
    VkSamplerCustomBorderColorCreateInfoEXT custom{
        VK_STRUCTURE_TYPE_SAMPLER_CUSTOM_BORDER_COLOR_CREATE_INFO_EXT};
    if (border_custom_) {
        custom.customBorderColor.float32[0] = border[0];
        custom.customBorderColor.float32[1] = border[1];
        custom.customBorderColor.float32[2] = border[2];
        custom.customBorderColor.float32[3] = border[3];
        custom.format = VK_FORMAT_UNDEFINED;  // customBorderColorWithoutFormat
        info_vk.pNext = &custom;
        info_vk.borderColor = VK_BORDER_COLOR_FLOAT_CUSTOM_EXT;
    } else {
        info_vk.borderColor = VK_BORDER_COLOR_FLOAT_TRANSPARENT_BLACK;
    }
    VkSampler sampler = VK_NULL_HANDLE;
    if (vkCreateSampler(device_, &info_vk, nullptr, &sampler) != VK_SUCCESS) return white_sampler_;
    samplers_[key] = sampler;
    return sampler;
}

// A small draw whose every float2 input lies in 0..1: sprites, HUD pieces, text and full screen
// passes. Wrap is then read as clamp, because upscaled the far edge's last half texel would
// otherwise blend in the opposite edge (thin dark lines on HUD sprites). Mirrors d3d12.cpp.
bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!((a.regs >> r) & 1) || e.format != 16 || e.buffer >= a.buffers.size()) continue;  // R32G32_FLOAT
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || e.offset + 2 * sizeof(float) > vb.stride ||
            uint64_t(a.vertex_count) * vb.stride > vb.bytes)
            return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float xy[2];
            std::memcpy(xy, vb.src + size_t(v) * vb.stride + e.offset, sizeof(xy));
            if (xy[0] < kLo || xy[0] > kHi || xy[1] < kLo || xy[1] > kHi) return false;
        }
        any = true;
    }
    return any;
}

// UnitTexcoords only changes stages that wrap
bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss)
        if (t[kTssAddressU] == 1 || t[kTssAddressV] == 1) return true;  // D3DTADDRESS_WRAP
    return false;
}

// Screen space draws onto a back buffer sized target are the title's 2D layer: HUD, text, menus,
// full screen pictures and movies, fades and passes. Titles lay that layer out for 16:9 at most; on
// a wider frame it would stretch, so it is drawn at 16:9 in the middle (mirrors d3d12.cpp UiSqueeze).
// D3D12's RegionClip (d3d12.cpp), for the shared TexUv clamp. A screen space pass reading a render target holds its
// reads to the texels its coordinates span, from the first texel centre to the last: the glow chain keeps several
// blur levels side by side in one target, and the far edge's pixels would blend in the next, less blurred level (an
// offset copy of the scene along the right and bottom of the frame). A smaller glow target composited over the whole
// frame also keeps off its outermost texels, which the title blacks out, so the glow does not fade to a dark band at
// the frame's edges (unlike the console). Every other target read stays open.
void RegionClip(const Assembled& a, const std::array<TexInfo, 4>& info, const std::array<TextureDesc, 4>& descs,
                uint32_t gw, uint32_t gh, DrawConstants& c) {
    constexpr float kOpen = 1e9f;
    constexpr uint32_t kSmallDrawVertices = 64;
    constexpr uint8_t kR32G32Float = 16;
    for (int i = 0; i < 4; i++) {
        if (!(info[size_t(i)].flags & kTexTarget)) continue;
        c.rtclip[i][0] = c.rtclip[i][1] = -kOpen;
        c.rtclip[i][2] = c.rtclip[i][3] = kOpen;
    }
    if (!a.pretransformed || !a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return;
    // one texture coordinate set: the pass's quad
    int reg = -1;
    for (int r = 0; r < 16; r++) {
        if (!(a.regs >> r & 1) || a.elements[size_t(r)].format != kR32G32Float) continue;
        if (reg >= 0) return;
        reg = r;
    }
    if (reg < 0) return;
    const VertexElement& e = a.elements[size_t(reg)];
    if (e.buffer >= a.buffers.size()) return;
    const VertexBuffer& vb = a.buffers[e.buffer];
    if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return;
    float lo[2] = {kOpen, kOpen}, hi[2] = {-kOpen, -kOpen};
    for (uint32_t v = 0; v < a.vertex_count; v++) {
        float uv[2];
        std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
        for (int k = 0; k < 2; k++) {
            lo[k] = std::min(lo[k], uv[k]);
            hi[k] = std::max(hi[k], uv[k]);
        }
    }
    // the quad covers the whole bound target: a composite over the frame
    bool full_frame = false;
    const VertexElement& pe = a.elements[0];
    if (gw && gh && (a.regs & 1) && pe.buffer < a.buffers.size()) {
        const VertexBuffer& pb = a.buffers[pe.buffer];
        if (pb.src && pb.stride && uint64_t(a.vertex_count) * pb.stride <= pb.bytes) {
            float plo[2] = {kOpen, kOpen}, phi[2] = {-kOpen, -kOpen};
            for (uint32_t v = 0; v < a.vertex_count; v++) {
                float p[2];
                std::memcpy(p, pb.src + size_t(v) * pb.stride + pe.offset, sizeof(p));
                for (int k = 0; k < 2; k++) {
                    plo[k] = std::min(plo[k], p[k]);
                    phi[k] = std::max(phi[k], p[k]);
                }
            }
            full_frame = plo[0] <= 0.5f && plo[1] <= 0.5f && phi[0] >= float(gw) - 0.5f && phi[1] >= float(gh) - 0.5f;
        }
    }
    for (int i = 0; i < 4; i++) {
        const TextureDesc& d = descs[size_t(i)];
        if (!(info[size_t(i)].flags & kTexTarget) || !d.width || !d.height) continue;
        const float size[2] = {float(d.width), float(d.height)};
        for (int k = 0; k < 2; k++) {
            const float n = info[size_t(i)].rect ? size[k] : 1.0f;  // coordinates per texture width
            if (hi[k] - lo[k] < 2 * n / size[k]) continue;          // too narrow to be a region
            // the far edge's last texel centre: as far in from it as the near edge's coordinate is past a texel corner
            const float texel = n / size[k];
            const float past = lo[k] / texel - std::floor(lo[k] / texel);
            c.rtclip[i][k] = lo[k] / n;
            c.rtclip[i][2 + k] = (hi[k] - (past > 0.25f ? 1.0f : 0.5f) * texel) / n;
            const bool whole = lo[k] <= texel * 1.01f && hi[k] >= n - texel * 0.01f;
            if (full_frame && whole && size[k] < float(k ? gh : gw) && size[k] > 4) {
                c.rtclip[i][k] += texel / n;
                c.rtclip[i][2 + k] -= texel / n;
            }
        }
    }
}

float VulkanBackend::UiSqueeze(const State& s, const Assembled& a, const std::array<TexInfo, 4>& info,
                               const std::array<Tex*, 4>& tex, const Target* rt) const {
    constexpr float kUiAspect = 16.0f / 9.0f;
    if (!rt || rt->gw != width_ || rt->gh != height_) return 1.0f;
    if (rt->cpu && rt->scene_frame != frames_ + 1) return 1.0f;  // over a CPU picture: the frame is 16:9
    if (aspect_ <= kUiAspect + 1e-3f) return 1.0f;
    const float squeeze = kUiAspect / aspect_;
    if (!a.pretransformed) {
        // A 3D draw into a sub-rectangle of the main target is a menu preview, not the full-screen
        // world; it has to follow the centred UI.
        const bool preview = s.viewport.w > 0 && s.viewport.h > 0 && s.viewport.w < width_ - 1 &&
                             s.viewport.h < height_ - 1;
        return preview ? squeeze : 1.0f;
    }
    // the horizontal extent of the positions (register 0), from the converted vertices or the stream
    float lo = 1e30f, hi = -1e30f;
    if (!a.vertices.empty()) {
        uint32_t n = 0;
        for (int r = 0; r < 16; r++) n += (a.regs >> r) & 1;
        for (size_t v = 0; n && v + 1 <= a.vertices.size() / (n * 4); v++) {
            lo = std::min(lo, a.vertices[v * n * 4]);
            hi = std::max(hi, a.vertices[v * n * 4]);
        }
    } else {
        const VertexElement& e = a.elements[0];
        const VertexBuffer& b = a.buffers[e.buffer];
        for (uint32_t v = 0; b.src && b.stride && size_t(v) * b.stride + e.offset + 4 <= b.bytes; v++) {
            float x;
            std::memcpy(&x, b.src + size_t(v) * b.stride + e.offset, 4);
            lo = std::min(lo, x);
            hi = std::max(hi, x);
        }
    }
    const bool full_width = lo <= 0.5f && hi >= float(width_) - 0.5f;
    if (!full_width) return squeeze;
    const bool picture = tex[0] && !(info[0].flags & kTexTarget);
    return picture ? squeeze : 1.0f;
}

void VulkanBackend::FillConstants(const State& s, const PixelKey& pk, const std::array<TexInfo, 4>& info, bool prog,
                                  bool fixed_transform, const GuestExtent& size, DrawConstants& c) {
    c = {};
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    if (s.rs[kRsFogEnable]) {
        if (s.fog_full) {  // linear fog whose range lies far below any fog value: the factor saturates to 0
            c.fog[0] = -2e9f;
            c.fog[1] = -1e9f;
            c.fog[3] = kFogTableLinear;
        } else if (prog && s.rs[kRsFogTableMode] == kFogTableNone) {
            c.fog[0] = 1.0f;
            c.fog[3] = kFogTableLinear;
        } else {
            c.fog[0] = Bits(s.rs[kRsFogStart]);
            c.fog[1] = Bits(s.rs[kRsFogEnd]);
            c.fog[2] = Bits(s.rs[kRsFogDensity]);
            c.fog[3] = float(s.rs[kRsFogTableMode]);
        }
    }
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(CompareOpIndex(SimpleState(s, kMAlphaFunc))) : kAlphaTestAlways;
    c.misc[2] = float(CompareOpIndex(s.rs[kRsShadowFunc]));
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
    for (int i = 0; i < 4; i++) {
        const auto& ts = s.tss[size_t(i)];
        c.bump[i][0] = Bits(ts[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(ts[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(ts[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(ts[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(ts[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(ts[kTssBumpEnvLOffset]);
    }
    if (fixed_transform) {
        float wv[16], wvp[16], vpm[16], full[16];
        MatMul(s.transforms[256].data(), s.transforms[2].data(), wv);
        MatMul(wv, s.transforms[3].data(), wvp);
        const auto& v = s.viewport;
        float sx = v.w ? float(v.w) / size.w : 1;
        float sy = v.h ? float(v.h) / size.h : 1;
        float ox = (2.0f * v.x + v.w) / size.w - 1;
        float oy = 1 - (2.0f * v.y + v.h) / size.h;
        float m[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
        std::memcpy(vpm, m, sizeof(m));
        MatMul(wvp, vpm, full);
        Transpose(full, c.wvp);
        Transpose(wv, c.wv);
        float n[16] = {};
        InverseTranspose3x3(wv, n);
        Transpose(n, c.wvn);
        for (int t = 0; t < 4; t++) Transpose(s.transforms[16 + size_t(t)].data(), c.tm[t]);
        std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
        c.mat[4][0] = s.material[16];
        Color(s.rs[kRsAmbient], c.amb);
        const FixedKey fk = MakeFixedKey(s);
        for (int i = 0; i < 8; i++) {
            if (!((fk.types >> (i * 2)) & 3)) continue;
            float f[26];
            std::memcpy(f, s.lights[size_t(i)].data(), sizeof(f));
            for (int j = 0; j < 4; j++) {
                c.lt[i][0][j] = f[1 + j];
                c.lt[i][1][j] = f[5 + j];
                c.lt[i][2][j] = f[9 + j];
            }
            const float* pos = f + 13;
            const float* dir = f + 16;
            const float* vm = s.transforms[2].data();
            for (int j = 0; j < 3; j++) {
                c.lt[i][3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
                c.lt[i][4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
            }
            c.lt[i][3][3] = f[19];
            c.lt[i][4][3] = f[20];
            c.lt[i][5][0] = f[21];
            c.lt[i][5][1] = f[22];
            c.lt[i][5][2] = f[23];
            c.lt[i][6][0] = std::cos(f[24] * 0.5f);
            c.lt[i][6][1] = std::cos(f[25] * 0.5f);
        }
    }
    std::memcpy(c.vdef, s.vdef, sizeof(c.vdef));
    c.zclips[0] = s.zclip[0];
    c.zclips[1] = s.zclip[1];
    c.zclips[2] = s.zclip_on && prog ? 1.0f : 0.0f;
    // building_seam_fix: the same host-Z scale the Windows shader applies, but in Vulkan's
    // non-reversed 0..1 depth convention. Only the restricted distant-city z-buffer pass uses it.
    c.zparams[0] = c.zparams[1] = c.zparams[2] = 0.0f;
    c.zparams[3] = prog && s.rs[kRsZEnable] != kZUseW ? s.program_depth_scale : 1.0f;
    if (c.zparams[3] != 1.0f) LOG_FIRST(4, "vulkan: depth scale %.3f for the distant city pass", c.zparams[3]);
    for (int i = 0; i < 4; i++) {
        c.tscale[i] = 1.0f;
        if (info[size_t(i)].flags & kTexTarget) {
            c.rtshift[i] = (!prog || ReadsViewportOffset(s.vs->program)) ? -0.5f : 0.0f;
            // TexUv clamps target reads. Leave the region open so the sampler
            // handles addressing instead of pinning every sample to (0, 0).
            constexpr float kOpen = 1e9f;
            c.rtclip[i][0] = c.rtclip[i][1] = -kOpen;
            c.rtclip[i][2] = c.rtclip[i][3] = kOpen;
        }
    }
    (void)pk;
}

// PART5

VkPipeline VulkanBackend::ClearPipeline(VkFormat format, VkFormat depth, uint32_t mask, uint32_t samples) {
    const uint64_t key = uint64_t(format) | (uint64_t(depth) << 32) | (uint64_t(mask) << 40) |
                         (uint64_t(samples) << 48);
    if (auto it = clear_pipelines_.find(key); it != clear_pipelines_.end()) return it->second;
    if (!clear_vs_) {
        static const char* kClearHlsl = R"(
[[vk::push_constant]] cbuffer PC { float4 colour; };
struct O { float4 pos : SV_Position; };
O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float4 psmain() : SV_Target { return colour; }
)";
        clear_vs_ = CompileShader(kClearHlsl, shaderc_vertex_shader, "vs");
        clear_ps_ = CompileShader(kClearHlsl, shaderc_fragment_shader, "psmain");
        if (!clear_vs_ || !clear_ps_) return VK_NULL_HANDLE;
    }
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VkSampleCountFlagBits(std::max(1u, samples));
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = mask & 0xF;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &cb;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dyn;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = clear_vs_;
    stages[0].pName = "vs";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = clear_ps_;
    stages[1].pName = "psmain";
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &ds;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = pipeline_layout_;
    info.renderPass = samples > 1 ? RenderPassForMs(format, depth, samples) : RenderPassFor(format, depth);
    if (info.renderPass == VK_NULL_HANDLE) return VK_NULL_HANDLE;
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (CreatePipelines(1, &info, nullptr, &pipeline) != VK_SUCCESS)
        return VK_NULL_HANDLE;
    clear_pipelines_[key] = pipeline;
    return pipeline;
}

void VulkanBackend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                          uint32_t stencil) {
    ScopeTimer timer(perf_, &perf_clear_ms_);
    Target* rt = nullptr;
    Target* ds = nullptr;
    BindTargets(s, rt, ds);
    if (!pass_) return;
    // A clear is an immediate command. Pending scene draws must run first when it clears a target
    // they render into or sample, and before a full-size depth clear, which starts a new scene view
    // (below). Clears of other targets (HUD slots, shadow and reflection maps) leave the list
    // alone: each replay runs the whole AO pass again.
    const bool view_clear = ds && (flags & kClearZ) && ds->gw == width_ && ds->gh == height_;
    if (DeferredPending() && (view_clear || DeferredWrites(rt) || DeferredWrites(ds) ||
                              (rt && deferred_read_targets_.count(rt)) || (ds && deferred_read_targets_.count(ds)))) {
        FlushDeferredScene();
        BindTargets(s, rt, ds);
        if (!pass_) return;
    }
    // A full-size depth clear starts a new scene view; the AO prepass starts empty again.
    if (view_clear) {
        ao_depth_clear_ = true;
        defer_rt_ = defer_ds_ = nullptr;
    }
    const Target* sized = rt ? rt : ds;
    const uint32_t tw = sized->w;
    const uint32_t th = sized->h;
    const float sx = sized->gw ? float(tw) / float(sized->gw) : 1.0f;
    const float sy = sized->gh ? float(th) / float(sized->gh) : 1.0f;
    std::vector<VkClearRect> area;
    if (count && rects) {
        area.reserve(std::min<uint32_t>(count, 64));
        for (uint32_t i = 0; i < count; i++) {
            int32_t rc[4];
            std::memcpy(rc, rects + size_t(i) * 16, sizeof(rc));
            const int32_t left = std::clamp(int32_t(float(rc[0]) * sx), 0, int32_t(tw));
            const int32_t top = std::clamp(int32_t(float(rc[1]) * sy), 0, int32_t(th));
            const int32_t right = std::clamp(int32_t(float(rc[2]) * sx), 0, int32_t(tw));
            const int32_t bottom = std::clamp(int32_t(float(rc[3]) * sy), 0, int32_t(th));
            if (right <= left || bottom <= top) continue;
            area.push_back({{left, top, uint32_t(right - left), uint32_t(bottom - top)}, 0, 1});
        }
    } else {
        area.push_back({{0, 0, tw, th}, 0, 1});
    }
    const uint32_t n = uint32_t(area.size());
    if (!n) return;  // every requested rect was empty: the title asked for no clear

    VkClearAttachment att[2]{};
    uint32_t na = 0;
    const uint32_t color_mask = uint32_t((flags & kClearTarget) >> 4);
    if (rt && (flags & kClearTarget) && color_mask == 0xF) {
        att[na].aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        att[na].colorAttachment = 0;
        Color(color, att[na].clearValue.color.float32);
        na++;
    }
    if (!depth_has_stencil_) flags &= ~kClearStencil;
    if (ds && (flags & (kClearZ | kClearStencil))) {
        att[na].aspectMask = ((flags & kClearZ) ? VK_IMAGE_ASPECT_DEPTH_BIT : 0) |
                             ((flags & kClearStencil) ? VK_IMAGE_ASPECT_STENCIL_BIT : 0);
        att[na].clearValue.depthStencil = {std::clamp(z, 0.0f, 1.0f), stencil};
        na++;
        ds->drawn = true;
    }
    BeginRecord();
    if (na) vkCmdClearAttachments(cmd_, na, att, n, area.data());

    if (rt && (flags & kClearTarget)) {
        rt->drawn = true;
        // vkCmdClearAttachments has no channel mask; draw the colour over the rects
        if (color_mask != 0xF) {
            VkPipeline pipeline = ClearPipeline(rt->format, ds ? ds->format : VK_FORMAT_UNDEFINED, color_mask,
                                                 rt->samples);
            if (pipeline) {
                float col[4];
                Color(color, col);
                VkViewport viewport{0, 0, float(tw), float(th), 0, 1};
                vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
                vkCmdSetViewport(cmd_, 0, 1, &viewport);
                vkCmdPushConstants(cmd_, pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16, col);
                for (uint32_t i = 0; i < n; i++) {
                    VkRect2D sc{area[i].rect.offset, area[i].rect.extent};
                    vkCmdSetScissor(cmd_, 0, 1, &sc);
                    vkCmdDraw(cmd_, 3, 1, 0, 0);
                }
            }
        }
    }
    bound_pipe_ = VK_NULL_HANDLE;
    viewport_valid_ = false;
}

void VulkanBackend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    // a program's point sprites under the fixed-function stages: the XDK builds their combiners
    // from stage 3 alone, the stage with the sprite's coordinates (d3d12.cpp SpriteOnStage3)
    if (prim == 1 /* D3DPT_POINTLIST */ && (s.vs_handle & 1) && s.vs && !s.vs->program.empty() && !s.ps &&
        s.rs[kRsPointSpriteEnable] && s.tss[3][kTssColorOp] != 1 /* D3DTOP_DISABLE */) {
        State moved = s;
        moved.tss[0] = s.tss[3];
        moved.textures[0] = s.textures[3];
        moved.palettes[0] = s.palettes[3];
        for (int i = 1; i < 4; i++) {
            moved.tss[i][kTssColorOp] = 1;
            moved.tss[i][kTssAlphaOp] = 1;
            moved.textures[i] = 0;
        }
        Draw(moved, prim, start, count, indices);
        return;
    }
    ScopeTimer timer(perf_, &perf_draw_ms_);
    LapTimer laps(perf_, &perf_assemble_ms_);
    Assembled& a = scratch_;
    if (!Assemble(s, prim, start, count, indices, a) || !a.vertex_count) return;

    // Shaders and textures first: an upload may submit and close any open pass, so
    // the target pass must not be open yet.
    laps.Lap(&perf_vertex_ms_);
    const bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    const bool sprite = a.topology == Topology::Points && prog && s.rs[kRsPointSpriteEnable];
    const bool fixed_transform = !prog && !a.pretransformed;
    uint64_t vkey = 0;
    VkShaderModule vs = VertexShaderFor(s, a, prog, sprite, vkey);
    if (!vs) return;

    laps.Lap(&perf_tex_ms_);
    std::array<TexInfo, 4> info{};
    std::array<TextureDesc, 4> descs{};
    std::array<Tex*, 4> tex{};
    std::array<Target*, 4> tex_targets{};
    std::array<const char*, 4> tex_reasons{};
    std::array<float, 4> wide{};
    for (int i = 0; i < 4; i++) {
        if (!s.textures[size_t(i)] || !Describe(s.textures[size_t(i)], descs[size_t(i)])) continue;
        const TextureDesc& d = descs[size_t(i)];
        info[size_t(i)].kind = d.cube ? kTexCube : d.depth > 1 ? kTex3D : kTex2D;
        info[size_t(i)].rect = d.linear ? 1 : 0;
        if (Format(d.format).depth) info[size_t(i)].shadow = uint8_t(d.format == 0x2C || d.format == 0x30 ? 2 : 1);
        bool is_target = false;
        tex[size_t(i)] = EnsureTexture(s, i, &is_target);
        tex_targets[size_t(i)] = is_target ? sampled_target_ : nullptr;
        tex_reasons[size_t(i)] = tex_reason_;
        if (is_target) info[size_t(i)].flags |= kTexTarget;
        // D3D12 BindTargetTexture: spread taps only when an upscaled target is shrunk into a smaller
        // one. A target read through a smaller texture view at 100% scale is not upscaled.
        if (Target* t = is_target ? sampled_target_ : nullptr; t && !t->depth && t->gw) {
            const float k = float(t->w) / float(t->gw);
            const Target* cur = TargetAt(s.rt.data, false, true);
            if (k > 1.01f && cur && cur != t && (t->gw > cur->gw || t->gh > cur->gh)) {
                info[size_t(i)].flags |= kTexWide;
                wide[size_t(i)] = k;
            }
        }
    }
    if (trace_)
        for (int i = 0; i < 4; i++)
            if (descs[size_t(i)].data) frame_tex_.insert(descs[size_t(i)].data);
    laps.Lap(&perf_pixel_ms_);
    laps.Lap(&perf_target_ms_);
    Target* rt = nullptr;
    Target* ds = nullptr;
    BindTargets(s, rt, ds);
    if (!pass_) return;

    // Deferred scene display list: world draws into the full-size game view are recorded and
    // replayed after the AO prepass. Everything else stays immediate.
    const bool scene_view = s.viewport.w == width_ && s.viewport.h * 4 >= height_ * 3;
    const bool scene_sized = rt && !rt->depth && rt->gw == width_ && rt->gh == height_ && ds &&
                             ds->w == rt->w && ds->h == rt->h && ds->samples == rt->samples;
    // only looked at with AO on
    auto samples_colour_rt = [&] {
        for (int i = 0; i < 4; i++)
            if ((info[size_t(i)].flags & kTexTarget) && !info[size_t(i)].shadow) return true;
        return false;
    };
    const uint8_t ao_roles = ao_mode_ != 0 && scene_sized && !a.pretransformed && scene_view
                                 ? AoRoles(s, samples_colour_rt())
                                 : 0;
    // Other draws into the open scene list's own targets (screen-space and small-viewport effects
    // between world draws) join the list in order, without an AO role. Drawing them immediately
    // would replay the whole list, AO pass included, before each one.
    const bool joins_scene = DeferredPending() && rt == defer_rt_ && ds == defer_ds_;
    bool defer_now = defer_scene_ && !defer_abandoned_ && !SimpleState(s, kMCountEnable) &&
                     ((scene_sized && !a.pretransformed && scene_view) || joins_scene);
    // Only a deferred receiver can get the t4 set at replay. Immediate frames (visibility tests,
    // fallback) must use the game's original pixel shader.
    const bool ao_receiver = defer_now && (ao_roles & kAoColour) != 0;
    if (DeferredPending() &&
        ((rt && deferred_read_targets_.count(rt)) || (ds && deferred_read_targets_.count(ds)))) {
        // This draw writes a target the pending list samples.
        FlushDeferredScene();
        BindTargets(s, rt, ds);
        if (!pass_) return;
    }
    if (defer_now && DeferredPending() && (defer_rt_ != rt || defer_ds_ != ds)) {
        FlushDeferredScene();
        BindTargets(s, rt, ds);
        if (!pass_) return;
    }
    if (defer_now && !DeferredPending()) {
        defer_rt_ = rt;
        defer_ds_ = ds;
    }
    if (!defer_now && DeferredPending()) {
        const bool touches = (rt && (rt == defer_rt_ || deferred_read_targets_.count(rt))) ||
                             (ds && (ds == defer_ds_ || deferred_read_targets_.count(ds)));
        if (touches) {
            FlushDeferredScene();
            BindTargets(s, rt, ds);
            if (!pass_) return;
        }
    }

    PixelKey pk = MakePixelKey(s, info.data());
    pk.quad = count <= 4 && !s.rs[kRsZEnable] && a.topology != Topology::Points;
    pk.ao = ao_receiver ? 1 : 0;
    uint64_t pkey = 0;
    VkShaderModule ps = PixelShaderFor(pk, pkey);
    if (!ps && pk.ao) {
        // The AO variant failed to compile: draw the game's original shader.
        LOG_FIRST(4, "vulkan: AO pixel shader variant failed, drawing without AO");
        pk.ao = 0;
        ps = PixelShaderFor(pk, pkey);
    }
    if (!ps) return;

    if (draw_log_ || trace_) {
        char head[560];
        uint64_t ef0 = 0, ef1 = 0;
        for (int r = 0; r < 16; r++)
            if ((a.regs >> r) & 1) {
                const uint64_t f = a.elements[size_t(r)].format & 0xFF;
                if (r < 8) ef0 |= f << (r * 8);
                else ef1 |= f << ((r - 8) * 8);
            }
        std::snprintf(head, sizeof(head),
                      "%llu rt %08X ds %08X prim %u count %u vs %016llX ps %016llX blend %u src%X dst%X op%X "
                      "bcol%08X zen %u zf%X cull %u/%u pk %u/%u tm%X q%u pr%X ef%016llX%016llX",
                      ++draw_log_n_, rt ? rt->data : 0, ds ? ds->data : 0, prim, count,
                      (unsigned long long)vkey, (unsigned long long)pkey, int(SimpleState(s, kMBlendEnable) != 0),
                      SimpleState(s, kMSrcBlend), SimpleState(s, kMDestBlend), SimpleState(s, kMBlendOp),
                      SimpleState(s, kMBlendColor), int(s.rs[kRsZEnable] != 0), SimpleState(s, kMZFunc),
                      int(s.rs[kRsCullMode] != 0), int(s.rs[kRsCullMode] == kCullCw), pk.programmable, pk.count,
                      pk.tex_modes, pk.quad, pk.proj, (unsigned long long)ef0, (unsigned long long)ef1);
        std::string line = head;
        for (int i = 0; i < 4; i++) {
            const TextureDesc& d = descs[size_t(i)];
            if (!d.width) {
                line += " | -";
                continue;
            }
            char part[256];
            const auto& ts = s.tss[size_t(i)];
            std::snprintf(part, sizeof(part),
                          " | %08X f%02X %ux%u lv%u k%u fl%02X op%X mm%u tf%X tc%X ad%X/%X/%X ft%X/%X/%X %s", d.data,
                          d.format, d.width, d.height, d.levels, info[size_t(i)].kind, info[size_t(i)].flags,
                          pk.ff[i][0], ts[kTssMaxMipLevel], ts[kTssTextureTransformFlags], ts[kTssTexCoordIndex],
                          ts[kTssAddressU], ts[kTssAddressV], ts[kTssAddressW], ts[kTssMagFilter], ts[kTssMinFilter],
                          ts[kTssMipFilter], tex[size_t(i)] ? "ok" : (tex_reasons[size_t(i)] ? tex_reasons[size_t(i)] : "NULL"));
            line += part;
        }
        if (draw_log_) {
            std::fputs(line.c_str(), draw_log_);
            std::fputc('\n', draw_log_);
        }
        if (trace_) {
            frame_trace_ += line;
            frame_trace_ += '\n';
        }
    }
    // TEMP diagnostic: TCNYC_SKIP_PS=<hex,...> drops draws whose pixel-shader key starts with one of the prefixes
    {
        static const std::vector<std::string> skip = [] {
            std::vector<std::string> v;
            if (const char* e = std::getenv("TCNYC_SKIP_PS")) {
                std::string t(e);
                for (size_t a = 0; a <= t.size();) {
                    size_t b = t.find(',', a);
                    if (b == std::string::npos) b = t.size();
                    if (b > a) v.push_back(t.substr(a, b - a));
                    a = b + 1;
                }
            }
            return v;
        }();
        if (!skip.empty()) {
            char hex[17];
            std::snprintf(hex, sizeof(hex), "%016llX", (unsigned long long)pkey);
            for (const auto& p : skip)
                if (std::strncmp(hex, p.c_str(), p.size()) == 0) return;
        }
    }

    laps.Lap(&perf_pipe_ms_);
    PsoKey k;
    std::memset(&k, 0, sizeof(k));
    k.vs = vkey;
    k.ps = pkey;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++)
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
            if (e.buffer < 16) k.strides[e.buffer] = a.buffers[e.buffer].stride;
        }
    k.topo = uint32_t(a.topology);
    k.strip = a.strip ? 1 : 0;
    k.sprite = sprite ? 1 : 0;
    k.rtfmt = rt ? uint32_t(rt->format) : 0;
    k.dsfmt = ds ? uint32_t(ds->format) : 0;
    k.samples = rt ? rt->samples : ds ? ds->samples : 1;
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    const uint32_t cw = SimpleState(s, kMColorWriteEnable);
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
    k.zen = s.rs[kRsZEnable] != 0;
    if (k.zen) {
        k.zwrite = SimpleState(s, kMZWriteEnable) != 0;
        k.zfunc = SimpleState(s, kMZFunc);
    }
    k.sten = s.rs[kRsStencilEnable] != 0;
    if (k.sten) {
        k.sfunc = SimpleState(s, kMStencilFunc);
        k.smask = SimpleState(s, kMStencilMask) & 0xFF;
        k.swmask = SimpleState(s, kMStencilWriteMask) & 0xFF;
        k.sfail = s.rs[kRsStencilFail];
        k.szfail = SimpleState(s, kMStencilZFail);
        k.spass = SimpleState(s, kMStencilPass);
    }
    const uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;
    k.fill = s.rs[kRsFillMode];
    if (a.topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable))
        k.zslope = SimpleState(s, kMSlopeScale) ? -Bits(SimpleState(s, kMSlopeScale)) : 0.0f;

    VkPipeline pipe = VK_NULL_HANDLE;
    if (auto it = pipelines_.find(k); it != pipelines_.end()) {
        pipe = it->second;
    } else {
        if (!CreatePipeline(k, vs, ps, pipe)) return;
        pipelines_[k] = pipe;
    }

    // The same draw for the AO depth prepass: depth plus the R32F view distance, with blending
    // off and only the red channel written. Alpha tests and discards still apply.
    VkPipeline depth_pipe = VK_NULL_HANDLE;
    if (defer_now && (ao_roles & kAoDepth)) {
        PixelKey dpk = pk;
        dpk.ao = 3;
        uint64_t dkey = 0;
        VkShaderModule dps = PixelShaderFor(dpk, dkey);
        if (dps) {
            PsoKey dk = k;
            dk.ps = dkey;
            dk.rtfmt = uint32_t(VK_FORMAT_R32_SFLOAT);
            dk.dsfmt = uint32_t(depth_format_);
            const uint32_t ao_samples = PickSampleCount(rt->samples, ao_color_samples_);
            dk.samples = ao_samples;
            dk.blend = dk.src = dk.dst = dk.op = 0;
            dk.wmask = 1;  // red
            dk.sten = dk.sfunc = dk.smask = dk.swmask = dk.sfail = dk.szfail = dk.spass = 0;
            if (auto it = pipelines_.find(dk); it != pipelines_.end()) {
                depth_pipe = it->second;
            } else {
                const VkRenderPass pass = AoPrepassPass(ao_samples);
                if (pass && CreatePipeline(dk, vs, dps, depth_pipe, pass)) pipelines_[dk] = depth_pipe;
            }
        }
    }
    laps.Lap(&perf_const_ms_);

    // constants
    GuestExtent size;
    size.w = rt ? rt->gw : ds->gw;
    size.h = rt ? rt->gh : ds->gh;
    DrawConstants c;
    FillConstants(s, pk, info, prog, fixed_transform, size, c);
    const uint32_t host_w = rt ? rt->w : ds->w, host_h = rt ? rt->h : ds->h;
    c.tscale[0] = float(host_w) / float(std::max(1u, size.w));
    c.tscale[1] = float(host_h) / float(std::max(1u, size.h));
    c.tscale[2] = UiSqueeze(s, a, info, tex, rt);
    // the frame's size when the draw targets the frame, for RegionClip's full-frame composites
    const bool frame = rt && rt->gw == width_ && rt->gh == height_;
    RegionClip(a, info, descs, frame ? width_ : 0, frame ? height_ : 0, c);

    if (rt && !a.pretransformed) rt->scene_frame = frames_ + 1;
    Target* dof_ds = ds ? ds : pass_ds_;
    // Distance blur uses the main target's full-size scene depth, multisampled or not (as d3d12.cpp).
    if (scene_dof_ && dof_ds && rt && !a.pretransformed &&
        s.rs[kRsZEnable] && SimpleState(s, kMZWriteEnable) && rt->gw == width_ && rt->gh == height_ &&
        dof_ds->w == rt->w && dof_ds->h == rt->h && s.viewport.w == width_ && s.viewport.h == height_) {
        const WRange r = DrawWRange(s, prog);
        if (r.near_w > 0 && r.far_w > r.near_w) scene_depth_[rt] = {dof_ds, r.near_w, r.far_w};
    }
    c.tscale[3] = 1.0f;
    for (int i = 0; i < 4; i++)
        if ((info[size_t(i)].flags & kTexWide) && descs[size_t(i)].width && descs[size_t(i)].height) {
            // D3D12 FillTexelConstants: a 3 console texel box per tap, n taps from the upscale
            const float n = std::ceil(3.0f * wide[size_t(i)] * 0.5f);
            c.spread[i][0] = 3.0f / (n * float(descs[size_t(i)].width));
            c.spread[i][1] = 3.0f / (n * float(descs[size_t(i)].height));
            c.spread[i][2] = n;
        }
    for (int i = 0; i < 4; i++)
        if (descs[size_t(i)].width) {
            c.texsize[i][0] = float(descs[size_t(i)].width);
            c.texsize[i][1] = float(descs[size_t(i)].height);
        }
    void* vc = nullptr;
    void* dc = nullptr;
    const UploadAlloc vc_alloc = AllocUpload(sizeof(s.vsc), 256, &vc);
    const UploadAlloc dc_alloc = AllocUpload(sizeof(c), 256, &dc);
    if (!vc || !dc) return;
    std::memcpy(vc, s.vsc.data(), sizeof(s.vsc));
    std::memcpy(dc, &c, sizeof(c));

    if (draw_set_next_ >= draw_sets_.size()) {
        LOG_FIRST(4, "vulkan: more than %zu draws in a frame, dropping the rest", draw_sets_.size());
        return;
    }
    VkDescriptorSet set = draw_sets_[draw_set_next_++];
    VkDescriptorBufferInfo bi[2] = {{vc_alloc.buffer, vc_alloc.offset, VkDeviceSize(sizeof(s.vsc))},
                                    {dc_alloc.buffer, dc_alloc.offset, VkDeviceSize(sizeof(c))}};
    VkDescriptorImageInfo ii[4]{};
    VkDescriptorImageInfo si[4]{};
    VkWriteDescriptorSet writes[10]{};
    for (int i = 0; i < 2; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = uint32_t(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
        writes[i].pBufferInfo = &bi[i];
    }
    const bool clamp_samplers = AnyStageWraps(s) && UnitTexcoords(a);
    for (int i = 0; i < 4; i++) {
        const Tex* drawn = tex[i] && tex[i]->swap ? tex[i]->swap : tex[i];
        ii[i].imageView = drawn ? drawn->view : white_view_;
        const VkImageLayout layout = drawn ? drawn->layout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        ii[i].imageLayout = layout != VK_IMAGE_LAYOUT_UNDEFINED ? layout : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        si[i].sampler = drawn ? EnsureSampler(s, i, info[size_t(i)], clamp_samplers, drawn->levels)
                              : white_sampler_;
        writes[2 + i] = {};
        writes[2 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[2 + i].dstSet = set;
        writes[2 + i].dstBinding = uint32_t(2 + i);
        writes[2 + i].descriptorCount = 1;
        writes[2 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[2 + i].pImageInfo = &ii[i];
        writes[6 + i] = {};
        writes[6 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[6 + i].dstSet = set;
        writes[6 + i].dstBinding = uint32_t(6 + i);
        writes[6 + i].descriptorCount = 1;
        writes[6 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[6 + i].pImageInfo = &si[i];
    }
    vkUpdateDescriptorSets(device_, 10, writes, 0, nullptr);
    laps.Lap(&perf_geom_ms_);

    // geometry
    uint32_t bound[16];
    VkBuffer vbufs[16];
    VkDeviceSize offs[16];
    uint32_t nb = 0;
    for (int b = 0; b < 16; b++) {
        if (!a.buffers[size_t(b)].src || !a.buffers[size_t(b)].bytes) continue;
        void* dst = nullptr;
        const UploadAlloc alloc = AllocUpload(a.buffers[size_t(b)].bytes, 4, &dst);
        if (!dst) return;
        std::memcpy(dst, a.buffers[size_t(b)].src, a.buffers[size_t(b)].bytes);
        bound[nb] = uint32_t(b);
        vbufs[nb] = alloc.buffer;
        offs[nb] = alloc.offset;
        nb++;
    }
    VkBuffer idx_buffer = VK_NULL_HANDLE;
    VkDeviceSize idx_off = 0;
    VkIndexType idx_type = VK_INDEX_TYPE_UINT16;
    uint32_t index_count = 0;
    if (a.index_count && a.index_src) {
        const uint32_t width = a.index16 ? 2 : 4;
        void* dst = nullptr;
        const UploadAlloc alloc = AllocUpload(uint64_t(a.index_count) * width, 4, &dst);
        if (!dst) return;
        std::memcpy(dst, a.index_src, size_t(a.index_count) * width);
        idx_buffer = alloc.buffer;
        idx_off = alloc.offset;
        idx_type = a.index16 ? VK_INDEX_TYPE_UINT16 : VK_INDEX_TYPE_UINT32;
        index_count = a.index_count;
    }

    VkViewport vp{0, 0, float(host_w), float(host_h), 0, 1};
    VkRect2D sc{{0, 0}, {host_w, host_h}};
    if (s.scissor_count) {
        const float sx = float(host_w) / float(std::max(1u, size.w));
        const float sy = float(host_h) / float(std::max(1u, size.h));
        const int32_t l = std::clamp(int32_t(float(s.scissors[0]) * sx), 0, int32_t(host_w));
        const int32_t t = std::clamp(int32_t(float(s.scissors[1]) * sy), 0, int32_t(host_h));
        const int32_t r = std::clamp(int32_t(float(s.scissors[2]) * sx), 0, int32_t(host_w));
        const int32_t b = std::clamp(int32_t(float(s.scissors[3]) * sy), 0, int32_t(host_h));
        if (r > l && b > t) sc = {{l, t}, {uint32_t(r - l), uint32_t(b - t)}};
    }
    float blend_color[4];
    Color(SimpleState(s, kMBlendColor), blend_color);
    const uint32_t stencil_ref = SimpleState(s, kMStencilRef) & 0xFF;

    laps.Lap(&perf_record_ms_);
    if (defer_now) {
        DeferredDraw d;
        d.pipe = pipe;
        d.depth_pipe = depth_pipe;
        d.set = set;
        d.vb_count = nb;
        for (uint32_t i = 0; i < nb; i++) {
            d.vbind[i] = bound[i];
            d.vbufs[i] = vbufs[i];
            d.voffs[i] = offs[i];
        }
        d.ibuf = idx_buffer;
        d.ioff = idx_off;
        d.itype = idx_type;
        d.index_count = index_count;
        d.vertex_count = a.vertex_count;
        d.base_vertex = a.base_vertex;
        d.vp = vp;
        d.sc = sc;
        std::memcpy(d.blend, blend_color, sizeof(d.blend));
        d.stencil = stencil_ref;
        d.zfunc = k.zfunc;
        d.receiver = ao_receiver && pk.ao == 1;
        if (d.receiver) {
            PixelKey plain_pk = pk;
            plain_pk.ao = 0;
            d.plain = int32_t(deferred_plain_.size());
            deferred_plain_.push_back({k, plain_pk, vs});
        }
        d.rt = rt;
        d.ds = ds;
        deferred_.push_back(d);
        if (rt) {
            rt->drawn = true;
            deferred_write_targets_.insert(rt);
        }
        if (ds) {
            ds->drawn = true;
            deferred_write_targets_.insert(ds);
        }
        for (int i = 0; i < 4; i++) {
            if (tex[size_t(i)]) {
                if (tex[size_t(i)]->image) deferred_read_images_.insert(tex[size_t(i)]->image);
                if (tex[size_t(i)]->swap && tex[size_t(i)]->swap->image)
                    deferred_read_images_.insert(tex[size_t(i)]->swap->image);
            }
            if (tex_targets[size_t(i)]) deferred_read_targets_.insert(tex_targets[size_t(i)]);
        }
        laps.Stop();
        return;
    }

    BeginRecord();
    if (pipe != bound_pipe_) {
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        bound_pipe_ = pipe;
    }
    for (uint32_t i = 0; i < nb; i++) vkCmdBindVertexBuffers(cmd_, bound[i], 1, &vbufs[i], &offs[i]);
    if (idx_buffer) vkCmdBindIndexBuffer(cmd_, idx_buffer, idx_off, idx_type);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set, 0, nullptr);
    if (!viewport_valid_ || std::memcmp(&vp, &bound_vp_, sizeof(vp)) != 0 ||
        std::memcmp(&sc, &bound_sc_, sizeof(sc)) != 0) {
        vkCmdSetViewport(cmd_, 0, 1, &vp);
        vkCmdSetScissor(cmd_, 0, 1, &sc);
        bound_vp_ = vp;
        bound_sc_ = sc;
        viewport_valid_ = true;
    }
    vkCmdSetBlendConstants(cmd_, blend_color);
    vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, stencil_ref);
    uint32_t query = kOcclusionQueries;
    if (SimpleState(s, kMCountEnable)) LOG_FIRST(1, "vulkan: visibility count enabled");
    if (query_pool_ && SimpleState(s, kMCountEnable) && query_next_ < kOcclusionQueries) {
        query = query_next_++;
        vkCmdBeginQuery(cmd_, query_pool_, query, 0);
    }
    if (!recording_ || pass_ == VK_NULL_HANDLE)
        LOGR("linux: draw without pass: recording %d pass %d rt %08X", int(recording_), int(pass_ != VK_NULL_HANDLE),
             rt ? rt->data : 0);
    if (index_count) vkCmdDrawIndexed(cmd_, index_count, 1, 0, a.base_vertex, 0);
    else vkCmdDraw(cmd_, a.vertex_count, 1, 0, 0);
    if (query != kOcclusionQueries) {
        vkCmdEndQuery(cmd_, query_pool_, query);
        counted_.push_back(query);
    }
    draws_++;
    if (rt) rt->drawn = true;
    if (ds) ds->drawn = true;
    laps.Stop();
}

void VulkanBackend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t rects, uint32_t points) {
    ScopeTimer timer(perf_, &perf_copy_ms_);
    FlushPass();
    EndRecord(true);
    auto virtual_data = [](const Surface& surface) {
        TextureDesc d;
        return DescribeSurface(surface, d) ? d.data : surface.data;
    };
    const uint32_t src_data = virtual_data(src), dst_data = virtual_data(dst);
    Target* src_t = TargetAt(src_data, false, true);
    Target* dst_t = TargetAt(dst_data, false, true);
    for (Target* t : {src_t, dst_t})
        if (t && DirtyEpoch(t->data) == t->epoch) ReadTarget(t);
    if (!gpu::CopySurfaceMemory(src, dst, count, rects, points))
        reference::Copy(src, dst, count, rects, points);
    // Carry the 3D scene's depth association across a full-frame scene->front copy, as the D3D12
    // backend does. Without this the present pass cannot find the scene depth and DOF stays off.
    if (dst_t) scene_depth_.erase(dst_t);
    if (src_t && dst_t && src_t != dst_t) {
        bool full = !count || !rects;
        if (count == 1 && rects) {
            const int32_t left = int32_t(kernel::R32(rects));
            const int32_t top = int32_t(kernel::R32(rects + 4));
            const int32_t right = int32_t(kernel::R32(rects + 8));
            const int32_t bottom = int32_t(kernel::R32(rects + 12));
            int32_t dx = left, dy = top;
            if (points) {
                dx = int32_t(kernel::R32(points));
                dy = int32_t(kernel::R32(points + 4));
            }
            full = left == 0 && top == 0 && dx == 0 && dy == 0 && right == int32_t(src_t->gw) &&
                   bottom == int32_t(src_t->gh);
        }
        if (full && src_t->gw == dst_t->gw && src_t->gh == dst_t->gh) {
            dst_t->scene_frame = src_t->scene_frame;
            dst_t->cpu = src_t->cpu;
            if (auto depth = scene_depth_.find(src_t); depth != scene_depth_.end())
                scene_depth_.insert_or_assign(dst_t, depth->second);
        }
    }
    for (auto& [key, t] : targets_)
        if (!t->depth && t->data == dst_data) t->upload_pending = true;
}

void VulkanBackend::Sync(uint32_t data, uint32_t size) {
    ScopeTimer timer(perf_, &perf_sync_ms_);
    FlushPass();
    EndRecord(true);
    for (auto& [key, t] : targets_) {
        if (t->depth) continue;
        if (data == t->data || (data >= t->data && data < t->data + t->size)) {
            // the title wrote guest memory after the last GPU use: do not replace it with stale pixels
            if (DirtyEpoch(t->data) == t->epoch) ReadTarget(t);
            return;
        }
    }
}

// A NOOVERWRITE lock: the console reads whatever the GPU has finished, so the CPU never waits on
// the GPU; the copy recorded in order with the draws is delivered once its submission completed.
void VulkanBackend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool VulkanBackend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    LateTarget& l = it->second;
    const uint64_t done = completed_serial_.load();
    const uint64_t now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < 3; i++) {
        LateSlot& s = l.slot[i];
        if (!s.used || s.serial <= l.delivered || s.serial > done) continue;
        if (s.presents > now || now - s.presents > 4) continue;  // not shown yet, or too old
        if (best < 0 || s.serial > l.slot[best].serial) best = i;
    }
    if (best < 0) return false;
    const LateSlot& s = l.slot[best];
    WriteGuestSurface(l.data, l.size, static_cast<const uint8_t*>(s.map), s.w, s.h, s.pitch, l.gw, l.gh, l.pitch,
                      l.swizzled, l.bytes);
    l.delivered = s.serial;
    LOG_FIRST(4, "vulkan: NOOVERWRITE lock %08X delivered from a finished copy", data);
    return true;
}

void VulkanBackend::RecordLate(uint32_t data) {
    Target* t = TargetAt(data, false, true);
    if (!t) return;
    if (!t || t->depth) return;
    // The copy must see the pending draws that write t, and it leaves t in a transfer layout, which
    // pending draws that sample t cannot replay against: replay first in both cases.
    if (DeferredWrites(t) || (DeferredPending() && deferred_read_targets_.count(t))) FlushDeferredScene();
    if (DirtyEpoch(t->data) != t->epoch) return;  // guest memory is newer than the image
    VkImage image = t->samples > 1 ? t->resolved_image : t->image;
    if (!image || !t->w || !t->h) return;

    if (!recording_) BeginRecord();
    if (!recording_) return;
    FlushPass();
    std::lock_guard lock(late_lock_);
    LateTarget& l = late_[data];
    if (l.gw != t->gw || l.gh != t->gh || l.slot[0].w != t->w || l.slot[0].h != t->h) {
        for (LateSlot& s : l.slot)
            if (s.buffer) late_retired_.push_back({s.buffer, s.memory});
        l = LateTarget{};
    }
    l.data = t->data;
    l.size = t->size;
    l.gw = t->gw;
    l.gh = t->gh;
    l.pitch = t->pitch;
    l.swizzled = t->swizzled;
    l.bytes = t->bytes;

    const uint32_t slot = l.next;
    LateSlot& s = l.slot[slot];
    if (s.used && s.serial > completed_serial_.load()) return;  // the ring is a whole submission behind
    const uint32_t pitch = t->w * 4;
    if (s.buffer && (s.w != t->w || s.h != t->h)) {
        late_retired_.push_back({s.buffer, s.memory});
        s = LateSlot{};
    }
    if (!s.buffer) {
        StagingBuffer(device_, mem_props_, uint64_t(pitch) * t->h, s.buffer, s.memory, &s.map);
        if (!s.map) return;
        s.w = t->w;
        s.h = t->h;
        s.pitch = pitch;
    }
    VkImageLayout& layout = t->samples > 1 ? t->resolved_layout : t->layout;
    Transition(image, layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, t->format);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {t->w, t->h, 1};
    vkCmdCopyImageToBuffer(cmd_, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buffer, 1, &copy);
    s.used = true;
    s.serial = submit_serial_ + 1;  // the next draw submission carries the copy
    s.presents = presents_seen_.load();
    l.next = (slot + 1) % 3;
}

void VulkanBackend::Flush() {
    FlushPass();
    EndRecord(true);
    if (device_) vkDeviceWaitIdle(device_);
}

void VulkanBackend::DrawSmaaPass(uint32_t pass, VkDescriptorSet set, SmaaImage& dst, uint32_t w, uint32_t h) {
    Transition(dst.image, dst.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, dst.format);
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = RenderPassFor(dst.format, VK_FORMAT_UNDEFINED);
    rp.framebuffer = dst.framebuffer;
    rp.renderArea = {{0, 0}, {w, h}};
    vkCmdBeginRenderPass(cmd_, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, smaa_pipelines_[pass]);
    VkViewport viewport{0, 0, float(w), float(h), 0, 1};
    VkRect2D scissor{{0, 0}, {w, h}};
    vkCmdSetViewport(cmd_, 0, 1, &viewport);
    vkCmdSetScissor(cmd_, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, smaa_pipeline_layout_, 0, 1, &set, 0, nullptr);
    vkCmdDraw(cmd_, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd_);
    Transition(dst.image, dst.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, dst.format);
    bound_pipe_ = VK_NULL_HANDLE;
    viewport_valid_ = false;
}

bool VulkanBackend::CreateSmaaImage(SmaaImage& image, uint32_t w, uint32_t h, VkFormat format, bool framebuffer) {
    image.w = w;
    image.h = h;
    image.format = format;
    image.layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    info.imageType = VK_IMAGE_TYPE_2D;
    info.format = format;
    info.extent = {w, h, 1};
    info.mipLevels = 1;
    info.arrayLayers = 1;
    info.samples = VK_SAMPLE_COUNT_1_BIT;
    info.tiling = VK_IMAGE_TILING_OPTIMAL;
    info.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                 VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    if (vkCreateImage(device_, &info, nullptr, &image.image) != VK_SUCCESS) return false;
    VkMemoryRequirements req{};
    vkGetImageMemoryRequirements(device_, image.image, &req);
    VkMemoryAllocateInfo alloc{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    alloc.allocationSize = req.size;
    bool found = false;
    for (uint32_t i = 0; i < mem_props_.memoryTypeCount; i++)
        if ((req.memoryTypeBits & (1u << i)) &&
            (mem_props_.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
            alloc.memoryTypeIndex = i;
            found = true;
            break;
        }
    if (!found || vkAllocateMemory(device_, &alloc, nullptr, &image.memory) != VK_SUCCESS) return false;
    vkBindImageMemory(device_, image.image, image.memory, 0);
    VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    view.image = image.image;
    view.viewType = VK_IMAGE_VIEW_TYPE_2D;
    view.format = format;
    view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    if (vkCreateImageView(device_, &view, nullptr, &image.view) != VK_SUCCESS) return false;
    if (!framebuffer) return true;
    VkFramebufferCreateInfo fb{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fb.renderPass = RenderPassFor(format, VK_FORMAT_UNDEFINED);
    fb.attachmentCount = 1;
    fb.pAttachments = &image.view;
    fb.width = w;
    fb.height = h;
    fb.layers = 1;
    return vkCreateFramebuffer(device_, &fb, nullptr, &image.framebuffer) == VK_SUCCESS;
}

void VulkanBackend::DestroySmaaTargets() {
    for (SmaaImage* image : {&smaa_edges_, &smaa_weights_, &smaa_output_}) {
        if (image->framebuffer) vkDestroyFramebuffer(device_, image->framebuffer, nullptr);
        if (image->view) vkDestroyImageView(device_, image->view, nullptr);
        if (image->image) vkDestroyImage(device_, image->image, nullptr);
        if (image->memory) vkFreeMemory(device_, image->memory, nullptr);
        *image = SmaaImage();
    }
    smaa_w_ = smaa_h_ = 0;
    smaa_format_ = VK_FORMAT_UNDEFINED;
}

// The reference shader's porting macros, kept identical to the D3D12 prologue except that the
// metrics come in as push constants and the register names are mapped to bindings by
// InjectBindings (textures at 2+N, samplers at 6+N). SMAA_CUSTOM_SL makes the reference use these.
const char* kSmaaPrologue = R"(
struct SmaaPC { float4 metrics; float4 unused; };
[[vk::push_constant]] ConstantBuffer<SmaaPC> smaaPC;
SamplerState ls : register(s0);
SamplerState ps : register(s1);
#define SMAA_RT_METRICS smaaPC.metrics
#define SMAA_CUSTOM_SL 1
#define SMAA_PRESET_HIGH 1
#define SMAATexture2D(tex) Texture2D tex
#define SMAATexturePass2D(tex) tex
#define SMAASampleLevelZero(tex, coord) tex.SampleLevel(ls, coord, 0)
#define SMAASampleLevelZeroPoint(tex, coord) tex.SampleLevel(ps, coord, 0)
#define SMAASampleLevelZeroOffset(tex, coord, offset) tex.SampleLevel(ls, coord, 0, offset)
#define SMAASample(tex, coord) tex.Sample(ls, coord)
#define SMAASamplePoint(tex, coord) tex.Sample(ps, coord)
#define SMAASampleOffset(tex, coord, offset) tex.Sample(ls, coord, offset)
#define SMAA_FLATTEN
#define SMAA_BRANCH
)";

// a full screen triangle; uv 0..1 with the origin at the top left, like the present pass
const char* kSmaaTriangle = R"(
float2 FullScreen(uint id, out float4 pos) {
    float2 t = float2((id << 1) & 2, id & 2);
    pos = float4(t * float2(2, 2) + float2(-1, -1), 0, 1);
    return t;
}
)";

const char* kSmaaEdgesTail = R"(
Texture2D colorTex : register(t0);
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 o0 : TEXCOORD1; float4 o1 : TEXCOORD2; float4 o2 : TEXCOORD3; };
V vs(uint id : SV_VertexID) {
    V v;
    v.uv = FullScreen(id, v.p);
    float4 o[3];
    SMAAEdgeDetectionVS(v.uv, o);
    v.o0 = o[0]; v.o1 = o[1]; v.o2 = o[2];
    return v;
}
float4 edges(V v) : SV_Target {
    float4 o[3] = {v.o0, v.o1, v.o2};
    return float4(SMAALumaEdgeDetectionPS(v.uv, o, colorTex), 0, 0);
}
)";

const char* kSmaaWeightsTail = R"(
Texture2D edgesTex : register(t0);
Texture2D areaTex : register(t1);
Texture2D searchTex : register(t2);
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; float2 pix : TEXCOORD1; float4 o0 : TEXCOORD2; float4 o1 : TEXCOORD3; float4 o2 : TEXCOORD4; };
V vs(uint id : SV_VertexID) {
    V v;
    v.uv = FullScreen(id, v.p);
    float4 o[3];
    SMAABlendingWeightCalculationVS(v.uv, v.pix, o);
    v.o0 = o[0]; v.o1 = o[1]; v.o2 = o[2];
    return v;
}
float4 weights(V v) : SV_Target {
    float4 o[3] = {v.o0, v.o1, v.o2};
    return SMAABlendingWeightCalculationPS(v.uv, v.pix, o, edgesTex, areaTex, searchTex, float4(0, 0, 0, 0));
}
)";

const char* kSmaaBlendTail = R"(
Texture2D colorTex : register(t0);
Texture2D blendTex : register(t1);
struct V { float4 p : SV_Position; float2 uv : TEXCOORD0; float4 o : TEXCOORD1; };
V vs(uint id : SV_VertexID) {
    V v;
    v.uv = FullScreen(id, v.p);
    SMAANeighborhoodBlendingVS(v.uv, v.o);
    return v;
}
float4 blend(V v) : SV_Target { return SMAANeighborhoodBlendingPS(v.uv, v.o, colorTex, blendTex); }
)";

// The descriptor and pipeline layout of the full-screen passes on the finished frame (SMAA, bloom): images at
// bindings 2 to 4 and the linear and point samplers at 6 and 7, as InjectBindings places t0-t2 and s0-s1, and 8
// floats of push constants.
bool VulkanBackend::EnsurePostLayout() {
    if (smaa_pipeline_layout_) return true;
    auto make_sampler = [&](VkFilter filter, VkSampler* out) {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = filter;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        return vkCreateSampler(device_, &si, nullptr, out) == VK_SUCCESS;
    };
    if (!smaa_linear_ && !make_sampler(VK_FILTER_LINEAR, &smaa_linear_)) return false;
    if (!smaa_point_ && !make_sampler(VK_FILTER_NEAREST, &smaa_point_)) return false;
    if (!smaa_set_layout_) {
        VkDescriptorSetLayoutBinding bind[5]{};
        for (int i = 0; i < 3; i++) {
            bind[i].binding = uint32_t(2 + i);
            bind[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
            bind[i].descriptorCount = 1;
            bind[i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        for (int i = 0; i < 2; i++) {
            bind[3 + i].binding = uint32_t(6 + i);
            bind[3 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
            bind[3 + i].descriptorCount = 1;
            bind[3 + i].stageFlags = VK_SHADER_STAGE_FRAGMENT_BIT;
        }
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 5;
        li.pBindings = bind;
        if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &smaa_set_layout_) != VK_SUCCESS) return false;
    }
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &smaa_set_layout_;
    VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 8};
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device_, &pl, nullptr, &smaa_pipeline_layout_) != VK_SUCCESS) {
        smaa_pipeline_layout_ = VK_NULL_HANDLE;
        return false;
    }
    return true;
}

// a pool for count sets of the post layout
VkDescriptorPool VulkanBackend::CreatePostPool(uint32_t count) {
    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3 * count},
                                     {VK_DESCRIPTOR_TYPE_SAMPLER, 2 * count}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.maxSets = count;
    pi.poolSizeCount = 2;
    pi.pPoolSizes = sizes;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    if (vkCreateDescriptorPool(device_, &pi, nullptr, &pool) != VK_SUCCESS) return VK_NULL_HANDLE;
    return pool;
}

bool VulkanBackend::AllocatePostSets(VkDescriptorPool pool, uint32_t count, VkDescriptorSet* out) {
    std::vector<VkDescriptorSetLayout> layouts(count, smaa_set_layout_);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = pool;
    ai.descriptorSetCount = count;
    ai.pSetLayouts = layouts.data();
    return vkAllocateDescriptorSets(device_, &ai, out) == VK_SUCCESS;
}

// t0 to t2 of a post set (white where null) and its two samplers
void VulkanBackend::WritePostSet(VkDescriptorSet set, VkImageView t0, VkImageView t1, VkImageView t2) {
    VkDescriptorImageInfo images[3]{};
    const VkImageView views[3] = {t0, t1, t2};
    for (int i = 0; i < 3; i++) {
        images[i].imageView = views[i] ? views[i] : white_view_;
        images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }
    VkDescriptorImageInfo samplers[2]{};
    samplers[0].sampler = smaa_linear_;
    samplers[1].sampler = smaa_point_;
    VkWriteDescriptorSet writes[5]{};
    for (int i = 0; i < 3; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = set;
        writes[i].dstBinding = uint32_t(2 + i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[i].pImageInfo = &images[i];
    }
    for (int i = 0; i < 2; i++) {
        writes[3 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3 + i].dstSet = set;
        writes[3 + i].dstBinding = uint32_t(6 + i);
        writes[3 + i].descriptorCount = 1;
        writes[3 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[3 + i].pImageInfo = &samplers[i];
    }
    vkUpdateDescriptorSets(device_, 5, writes, 0, nullptr);
}

// a full-screen pass pipeline of the post layout, writing all of one colour attachment of this format
VkPipeline VulkanBackend::MakePostPipeline(VkShaderModule vs, VkShaderModule ps, const char* entry, VkFormat format) {
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vp.viewportCount = 1;
    vp.scissorCount = 1;
    VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rs.polygonMode = VK_POLYGON_MODE_FILL;
    rs.cullMode = VK_CULL_MODE_NONE;
    rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rs.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    VkPipelineColorBlendAttachmentState cb{};
    cb.colorWriteMask = 0xF;
    VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    blend.attachmentCount = 1;
    blend.pAttachments = &cb;
    VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dynamic.dynamicStateCount = 2;
    dynamic.pDynamicStates = dyn;
    VkPipelineShaderStageCreateInfo stages[2]{};
    stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
    stages[0].module = vs;
    stages[0].pName = "vs";
    stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
    stages[1].module = ps;
    stages[1].pName = entry;
    VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    info.stageCount = 2;
    info.pStages = stages;
    info.pVertexInputState = &vi;
    info.pInputAssemblyState = &ia;
    info.pViewportState = &vp;
    info.pRasterizationState = &rs;
    info.pMultisampleState = &ms;
    info.pDepthStencilState = &dss;
    info.pColorBlendState = &blend;
    info.pDynamicState = &dynamic;
    info.layout = smaa_pipeline_layout_;
    info.renderPass = RenderPassFor(format, VK_FORMAT_UNDEFINED);
    VkPipeline pipeline = VK_NULL_HANDLE;
    if (CreatePipelines(1, &info, nullptr, &pipeline) != VK_SUCCESS) return VK_NULL_HANDLE;
    return pipeline;
}

// one full-screen post pass into dst, which is then left readable
void VulkanBackend::DrawPostPass(VkPipeline pipeline, VkDescriptorSet set, SmaaImage& dst, const float p[8]) {
    vkCmdPushConstants(cmd_, smaa_pipeline_layout_, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0,
                       sizeof(float) * 8, p);
    Transition(dst.image, dst.layout, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, dst.format);
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = RenderPassFor(dst.format, VK_FORMAT_UNDEFINED);
    rp.framebuffer = dst.framebuffer;
    rp.renderArea = {{0, 0}, {dst.w, dst.h}};
    vkCmdBeginRenderPass(cmd_, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline);
    VkViewport viewport{0, 0, float(dst.w), float(dst.h), 0, 1};
    VkRect2D scissor{{0, 0}, {dst.w, dst.h}};
    vkCmdSetViewport(cmd_, 0, 1, &viewport);
    vkCmdSetScissor(cmd_, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, smaa_pipeline_layout_, 0, 1, &set, 0, nullptr);
    vkCmdDraw(cmd_, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd_);
    Transition(dst.image, dst.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, dst.format);
    bound_pipe_ = VK_NULL_HANDLE;
    viewport_valid_ = false;
}

void VulkanBackend::DestroyBloomTargets() {
    for (SmaaImage* image : {&bloom_.down[0], &bloom_.down[1], &bloom_.down[2], &bloom_.down[3], &bloom_.up[0],
                             &bloom_.up[1], &bloom_.up[2]}) {
        if (image->framebuffer) vkDestroyFramebuffer(device_, image->framebuffer, nullptr);
        if (image->view) vkDestroyImageView(device_, image->view, nullptr);
        if (image->image) vkDestroyImage(device_, image->image, nullptr);
        if (image->memory) vkFreeMemory(device_, image->memory, nullptr);
        *image = SmaaImage();
    }
    bloom_.src_w = bloom_.src_h = 0;
}

// The chain's images and pipelines for a source of this size (the shared post_fx.h shader). The sets of every pass
// but the first read fixed images, so they are written here; the bright pass's source changes per frame.
bool VulkanBackend::EnsureBloom(uint32_t src_w, uint32_t src_h) {
    static_assert(kBloomLevels == 4, "DestroyBloomTargets lists the levels");
    if (bloom_.failed) return false;
    if (bloom_.down[0].image && bloom_.src_w == src_w && bloom_.src_h == src_h) return true;
    if (!EnsurePostLayout()) return false;
    if (!bloom_.pipelines[0]) {
        bloom_.vs = CompileShader(kBloomHlsl, shaderc_vertex_shader, "vs");
        const char* entries[3] = {"bright", "down", "up"};
        for (int i = 0; i < 3; i++) {
            bloom_.ps[i] = CompileShader(kBloomHlsl, shaderc_fragment_shader, entries[i]);
            if (!bloom_.vs || !bloom_.ps[i]) return false;
            bloom_.pipelines[i] = MakePostPipeline(bloom_.vs, bloom_.ps[i], entries[i], kBloomFormat);
            if (!bloom_.pipelines[i]) return false;
        }
        if (!(bloom_.pool = CreatePostPool(kBloomSets))) return false;
        if (!AllocatePostSets(bloom_.pool, kBloomSets, bloom_.sets)) return false;
    }
    vkDeviceWaitIdle(device_);
    DestroyBloomTargets();
    uint32_t w = src_w, h = src_h;
    for (uint32_t i = 0; i < kBloomLevels; i++) {
        w = std::max(1u, (w + 1) / 2);
        h = std::max(1u, (h + 1) / 2);
        if (!CreateSmaaImage(bloom_.down[i], w, h, kBloomFormat, true)) return false;
        if (i + 1 < kBloomLevels && !CreateSmaaImage(bloom_.up[i], w, h, kBloomFormat, true)) return false;
    }
    // sets: 0 bright (written per frame), 1-3 down, 4-6 up (largest first)
    for (uint32_t i = 1; i < kBloomLevels; i++) WritePostSet(bloom_.sets[i], bloom_.down[i - 1].view, nullptr, nullptr);
    for (uint32_t i = 0; i + 1 < kBloomLevels; i++) {
        const bool smallest = i + 2 == kBloomLevels;
        WritePostSet(bloom_.sets[kBloomLevels + i], bloom_.down[i].view,
                     smallest ? bloom_.down[i + 1].view : bloom_.up[i + 1].view, nullptr);
    }
    bloom_.src_w = src_w;
    bloom_.src_h = src_h;
    LOGR("vulkan: bloom chain %ux%u to %ux%u", bloom_.down[0].w, bloom_.down[0].h, w, h);
    return true;
}

// the finished half-size bloom, readable; null when the chain is unavailable (the present pass then taps the source)
VkImageView VulkanBackend::BuildBloom(VkImageView src, uint32_t w, uint32_t h) {
    if (!EnsureBloom(w, h)) {
        if (!bloom_.failed) LOGE("vulkan: bloom chain could not be created, using the single-pass bloom");
        bloom_.failed = true;
        DestroyBloomTargets();
        return VK_NULL_HANDLE;
    }
    WritePostSet(bloom_.sets[0], src, gamma_view_, nullptr);
    {
        const float p[8] = {1.0f / float(w), 1.0f / float(h), 1.0f, 0};
        DrawPostPass(bloom_.pipelines[0], bloom_.sets[0], bloom_.down[0], p);
    }
    for (uint32_t i = 1; i < kBloomLevels; i++) {
        const float p[8] = {1.0f / float(bloom_.down[i - 1].w), 1.0f / float(bloom_.down[i - 1].h), 0, 0};
        DrawPostPass(bloom_.pipelines[1], bloom_.sets[i], bloom_.down[i], p);
    }
    for (uint32_t i = kBloomLevels - 1; i-- > 0;) {
        const bool smallest = i + 2 == kBloomLevels;
        const float p[8] = {1.0f / float(bloom_.down[i + 1].w), 1.0f / float(bloom_.down[i + 1].h), kBloomWeights[i],
                            smallest ? kBloomWeights[i + 1] : 1.0f};
        DrawPostPass(bloom_.pipelines[2], bloom_.sets[kBloomLevels + i], bloom_.up[i], p);
    }
    return bloom_.up[0].view;
}

void VulkanBackend::DestroyFsrImage(SmaaImage& image) {
    if (image.framebuffer) vkDestroyFramebuffer(device_, image.framebuffer, nullptr);
    if (image.view) vkDestroyImageView(device_, image.view, nullptr);
    if (image.image) vkDestroyImage(device_, image.image, nullptr);
    if (image.memory) vkFreeMemory(device_, image.memory, nullptr);
    image = SmaaImage();
}

VkImageView VulkanBackend::UpscaleAndSharpen(VkImageView src, VkFormat format, uint32_t w, uint32_t h,
                                             uint32_t out_w, uint32_t out_h, const app::Settings& s) {
    if (fsr_.failed || !w || !h) return src;
    const bool upscale = s.upscaler == 1 && out_w && out_h && h < out_h;
    if (!upscale && !s.sharpening) return src;
    const uint32_t sw = upscale ? out_w : w, sh = upscale ? out_h : h;
    auto fail = [&](const char* why) {
        fsr_.failed = true;
        LOGE("vulkan: FSR 1 %s, upscaling and sharpening are off", why);
        return src;
    };
    if (!EnsurePostLayout()) return fail("has no pass layout");
    if (!fsr_.pipelines[0] || fsr_.pipeline_format != format) {
        vkDeviceWaitIdle(device_);
        for (VkPipeline& p : fsr_.pipelines)
            if (p) vkDestroyPipeline(device_, std::exchange(p, VK_NULL_HANDLE), nullptr);
        if (!fsr_.vs) {
            fsr_.vs = CompileShader(FsrHlsl(), shaderc_vertex_shader, "vs");
            fsr_.ps[0] = CompileShader(FsrHlsl(), shaderc_fragment_shader, "easu");
            fsr_.ps[1] = CompileShader(FsrHlsl(), shaderc_fragment_shader, "rcas");
            if (!fsr_.vs || !fsr_.ps[0] || !fsr_.ps[1]) return fail("shaders did not compile");
        }
        const char* entries[2] = {"easu", "rcas"};
        for (int i = 0; i < 2; i++)
            if (!(fsr_.pipelines[i] = MakePostPipeline(fsr_.vs, fsr_.ps[i], entries[i], format)))
                return fail("pipelines could not be created");
        fsr_.pipeline_format = format;
        if (!fsr_.pool) {
            if (!(fsr_.pool = CreatePostPool(2))) return fail("descriptor pool could not be created");
            if (!AllocatePostSets(fsr_.pool, 2, fsr_.sets)) return fail("descriptor sets could not be allocated");
        }
    }
    auto ready = [&](const SmaaImage& image) {
        return image.image && image.w == sw && image.h == sh && image.format == format;
    };
    if ((upscale && !ready(fsr_.easu)) || !ready(fsr_.rcas)) {
        vkDeviceWaitIdle(device_);
        if (upscale && !ready(fsr_.easu)) {
            DestroyFsrImage(fsr_.easu);
            if (!CreateSmaaImage(fsr_.easu, sw, sh, format, true)) return fail("images could not be created");
        }
        if (!ready(fsr_.rcas)) {
            DestroyFsrImage(fsr_.rcas);
            if (!CreateSmaaImage(fsr_.rcas, sw, sh, format, true)) return fail("images could not be created");
        }
        LOGR("vulkan: FSR 1 %s %ux%u to %ux%u", upscale ? "upscaling" : "sharpening", w, h, sw, sh);
    }
    // the previous frame is finished (Present waited for its fence), so its sets can be rewritten
    const float p[8] = {float(w), float(h), float(sw), float(sh), RcasStops(s.sharpening), 0, 0, 0};
    VkImageView rcas_src = src;
    if (upscale) {
        WritePostSet(fsr_.sets[0], src, nullptr, nullptr);
        DrawPostPass(fsr_.pipelines[0], fsr_.sets[0], fsr_.easu, p);
        rcas_src = fsr_.easu.view;
    }
    WritePostSet(fsr_.sets[1], rcas_src, nullptr, nullptr);
    DrawPostPass(fsr_.pipelines[1], fsr_.sets[1], fsr_.rcas, p);
    return fsr_.rcas.view;
}

bool VulkanBackend::EnsureSmaa(uint32_t w, uint32_t h, VkFormat format) {
    if (smaa_failed_) return false;
    if (smaa_output_.image && smaa_w_ == w && smaa_h_ == h && smaa_format_ == format) return true;
    vkDeviceWaitIdle(device_);
    DestroySmaaTargets();
    if (!EnsurePostLayout()) return false;

    auto upload = [&](SmaaImage& image, uint32_t iw, uint32_t ih, VkFormat fmt) {
        return CreateSmaaImage(image, iw, ih, fmt, false);
    };
    if (!smaa_area_.image) {
        if (!upload(smaa_area_, AREATEX_WIDTH, AREATEX_HEIGHT, VK_FORMAT_R8G8_UNORM) ||
            !upload(smaa_search_, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, VK_FORMAT_R8_UNORM))
            return false;
        // the bytes ride in this frame's upload ring; the copies are recorded with the SMAA passes
        void* area = nullptr;
        void* search = nullptr;
        const UploadAlloc area_alloc = AllocUpload(AREATEX_SIZE, 16, &area);
        const UploadAlloc search_alloc = AllocUpload(SEARCHTEX_SIZE, 16, &search);
        if (!area || !search) return false;
        smaa_area_off_ = area_alloc.offset;
        smaa_search_off_ = search_alloc.offset;
        smaa_area_buffer_ = area_alloc.buffer;
        smaa_search_buffer_ = search_alloc.buffer;
        std::memcpy(area, areaTexBytes, AREATEX_SIZE);
        std::memcpy(search, searchTexBytes, SEARCHTEX_SIZE);
        smaa_lookups_pending_ = true;
    }

    if (!smaa_pool_) {
        if (!(smaa_pool_ = CreatePostPool(3))) return false;
        if (!AllocatePostSets(smaa_pool_, 3, smaa_sets_)) return false;
    }

    if (!smaa_pipelines_[0] || smaa_pipeline_format_ != format) {
        if (smaa_pipelines_[0]) {
            for (uint32_t i = 0; i < 3; i++) {
                if (smaa_pipelines_[i]) vkDestroyPipeline(device_, smaa_pipelines_[i], nullptr);
                if (smaa_vs_[i]) vkDestroyShaderModule(device_, smaa_vs_[i], nullptr);
                if (smaa_ps_[i]) vkDestroyShaderModule(device_, smaa_ps_[i], nullptr);
                smaa_pipelines_[i] = VK_NULL_HANDLE;
                smaa_vs_[i] = smaa_ps_[i] = VK_NULL_HANDLE;
            }
        }
        struct Pass {
            const char* tail;
            const char* entry;
            VkFormat format;
        } passes[3] = {{kSmaaEdgesTail, "edges", VK_FORMAT_R8G8_UNORM},
                       {kSmaaWeightsTail, "weights", VK_FORMAT_R8G8B8A8_UNORM},
                       {kSmaaBlendTail, "blend", format}};
        for (uint32_t i = 0; i < 3; i++) {
            const std::string src =
                std::string(kSmaaPrologue) + kSmaaHlsl + kSmaaTriangle + passes[i].tail;
            smaa_vs_[i] = CompileShader(src, shaderc_vertex_shader, "vs");
            smaa_ps_[i] = CompileShader(src, shaderc_fragment_shader, passes[i].entry);
            if (!smaa_vs_[i] || !smaa_ps_[i]) {
                smaa_failed_ = true;
                LOGE("vulkan: SMAA shaders failed to compile, SMAA is off");
                return false;
            }
            VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
            VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
            ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
            VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
            vp.viewportCount = 1;
            vp.scissorCount = 1;
            VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
            rs.polygonMode = VK_POLYGON_MODE_FILL;
            rs.cullMode = VK_CULL_MODE_NONE;
            rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
            rs.lineWidth = 1.0f;
            VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
            ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
            VkPipelineDepthStencilStateCreateInfo dss{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
            VkPipelineColorBlendAttachmentState cb{};
            cb.colorWriteMask = 0xF;
            VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
            blend.attachmentCount = 1;
            blend.pAttachments = &cb;
            VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
            VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
            dynamic.dynamicStateCount = 2;
            dynamic.pDynamicStates = dyn;
            VkPipelineShaderStageCreateInfo stages[2]{};
            stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
            stages[0].module = smaa_vs_[i];
            stages[0].pName = "vs";
            stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
            stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
            stages[1].module = smaa_ps_[i];
            stages[1].pName = passes[i].entry;
            VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
            info.stageCount = 2;
            info.pStages = stages;
            info.pVertexInputState = &vi;
            info.pInputAssemblyState = &ia;
            info.pViewportState = &vp;
            info.pRasterizationState = &rs;
            info.pMultisampleState = &ms;
            info.pDepthStencilState = &dss;
            info.pColorBlendState = &blend;
            info.pDynamicState = &dynamic;
            info.layout = smaa_pipeline_layout_;
            info.renderPass = RenderPassFor(passes[i].format, VK_FORMAT_UNDEFINED);
            if (CreatePipelines(1, &info, nullptr, &smaa_pipelines_[i]) !=
                VK_SUCCESS) {
                smaa_failed_ = true;
                LOGE("vulkan: SMAA pipelines could not be created, SMAA is off");
                return false;
            }
        }
        smaa_pipeline_format_ = format;
    }

    if (!CreateSmaaImage(smaa_edges_, w, h, VK_FORMAT_R8G8_UNORM, true) ||
        !CreateSmaaImage(smaa_weights_, w, h, VK_FORMAT_R8G8B8A8_UNORM, true) ||
        !CreateSmaaImage(smaa_output_, w, h, format, true)) {
        DestroySmaaTargets();
        smaa_failed_ = true;
        LOGE("vulkan: SMAA resources could not be created, SMAA is off");
        return false;
    }
    smaa_w_ = w;
    smaa_h_ = h;
    smaa_format_ = format;
    LOGR("vulkan: SMAA targets %ux%u", w, h);
    return true;
}

void VulkanBackend::Present(const Surface& front) {
    LapTimer present_lap(perf_, &perf_present_ms_);
    if (draw_log_) std::fprintf(draw_log_, "FRAME %llu\n", frames_ + 1);
    FlushPass();
    EndRecord(true);
    // every path must clear the per-frame rings, even when the front buffer cannot be read
    struct FrameEnd {
        VulkanBackend* self;
        bool finished = false;
        ~FrameEnd() {
            if (!finished) self->ResetFrame();
        }
    } frame_end{this};
    if (failed_) return;  // a dead fence is never waited on again
    // a previous resize may have failed: retry before acquiring from a null or partial swapchain
    if ((!swapchain_ || swap_framebuffers_.empty() || render_done_.empty()) && !CreateSwapchain()) return;
    const float want_scale = app::RenderScale(height_);
    const float want_stretch = app::RenderStretch(width_, height_);
    if (std::fabs(want_scale - scale_) > 1e-4f || std::fabs(want_stretch - stretch_) > 1e-4f) {
        vkDeviceWaitIdle(device_);
        scale_ = want_scale;
        stretch_ = want_stretch;
        bound_pipe_ = VK_NULL_HANDLE;
        viewport_valid_ = false;
        LOGR("linux: render scale %.3f, stretch %.3f", scale_, stretch_);
    }
    const uint32_t want_msaa =
        api12_ && depth_resolve_
            ? PickSampleCount(app::MsaaSamples(), color_samples_ & depth_samples_)
            : 1;
    if (want_msaa != msaa_) {
        vkDeviceWaitIdle(device_);
        msaa_ = want_msaa;
        bound_pipe_ = VK_NULL_HANDLE;
        viewport_valid_ = false;
        LOGR("linux: MSAA %ux", msaa_);
    }
    const app::Settings snapshot = app::Snapshot();
    if (snapshot.vsync != vsync_ || snapshot.hdr != hdr_setting_) {
        vkDeviceWaitIdle(device_);
        if (!CreateSwapchain()) return;
    }
    aniso_setting_ = snapshot.aniso;
    aspect_ = app::AspectRatio();
    if (texturedumper::Update()) ReleasePackTextures();
    Target* ft = nullptr;
    TextureDesc fd;
    const uint32_t front_data = DescribeSurface(front, fd) ? fd.data : front.data;
    for (auto& [key, t] : targets_)
        if (!t->depth && t->data == front_data && t->gw == fd.width && t->gh == fd.height) {
            ft = t;
            break;
        }
    if (!ft) ft = TargetAt(front_data, false, true);

    // F3 (gpu::g_capture): save this frame's draw list and a TGA of what it presented
    static const long trace_at = [] {
        const char* e = std::getenv("TCNYC_TRACE_AT");
        return e ? std::atol(e) : 0;
    }();
    static const long trace_every = [] {
        const char* e = std::getenv("TCNYC_TRACE_EVERY");
        return e ? std::atol(e) : 0;
    }();
    if (trace_ && trace_at > 0 && int64_t(frames_) + 1 == trace_at) gpu::g_capture.store(true);
    if (trace_ && trace_every > 0 && frames_ > 0 && frames_ % uint64_t(trace_every) == 0) gpu::g_capture.store(true);
    const bool captured = gpu::g_capture.exchange(false);
    const bool trace_now = trace_ && captured;
    const uint32_t trace_frame = uint32_t(frames_ + 1);
    std::string trace_base;
    if (trace_now) {
        dump_frame_ = trace_frame;
        if (ft && ft->drawn) ReadTarget(ft);
        std::error_code ec;
        std::filesystem::create_directories(app::ExeDir() / "logs", ec);
        trace_base = (app::ExeDir() / "logs" / ("trace_" + std::to_string(trace_frame))).string();
        uint32_t tw = 0, th = 0;
        if (reference::ReadRgba(front, frame_, tw, th) && tw && th)
            reference::WriteTga(front, (trace_base + ".tga").c_str());
        if (dump_targets_) {
            dump_now_ = true;
            for (auto& [key, t] : targets_)
                if (!t->depth && t->drawn && t->w && t->h) ReadTarget(t);
            dump_now_ = false;
        }
        for (uint32_t addr : dump_tex_list_) {
            for (auto& [key, t] : textures_)
                if (t->data == addr) {
                    DumpTexture(t);
                    break;
                }
        }
        if (dump_targets_) {
            for (uint32_t addr : frame_tex_)
                for (auto& [key, t] : textures_)
                    if (t->data == addr) {
                        DumpTexture(t);
                        break;
                    }
        }
        if (FILE* f = std::fopen((trace_base + ".txt").c_str(), "w")) {
            std::fwrite(frame_trace_.data(), 1, frame_trace_.size(), f);
            std::fclose(f);
        }
        LOGR("linux: trace frame %u saved, %zu draw lines", trace_frame,
             std::count(frame_trace_.begin(), frame_trace_.end(), '\n'));
    }

    // A frame the GPU drew this pass is sampled straight from its target; guest
    // memory stays untouched until something actually reads the surface.
    // TC_VK_CAP forces the read-back path so a diagnostic capture has a frame.
    // Guest memory newer than the GPU image (a CPU-decoded movie frame, a lock or
    // copy) must win: presenting the old image is the intro/legal-screen flicker.
    const bool capture = std::getenv("TC_VK_CAP") != nullptr;
    const bool stale = ft && DirtyEpoch(ft->data) != ft->epoch;
    const bool direct = ft && ft->drawn && !stale && !capture;
    if (perf_) {
        if (direct) perf_direct_++;
        else if (stale) perf_stale_++;
        else perf_readback_++;
    }

    uint32_t sw = 0, sh = 0;
    uint64_t staging_off = 0;
    VkBuffer staging_buffer = VK_NULL_HANDLE;
    VkImageView source_view = VK_NULL_HANDLE;
    if (!direct) {
        if (ft && ft->drawn && !stale) ReadTarget(ft);  // a CPU-visible present of a GPU-drawn target
        if (!reference::ReadRgba(front, frame_, sw, sh) || !sw || !sh) return;
        if (!EnsureFrameImage(sw, sh)) return;
        if (capture) {
            static const long every = [] {
                const char* e = std::getenv("TC_VK_CAP");
                return e ? std::atol(e) : 0;
            }();
            if (every > 0 && frames_ % uint64_t(every) == 0) {
                std::error_code ec;
                std::filesystem::create_directories(app::ExeDir() / "cap", ec);
                reference::WriteTga(front, (app::ExeDir() / ("cap/vk_" + std::to_string(frames_) + ".tga")).c_str());
            }
        }
        void* staging = nullptr;
        const UploadAlloc staging_alloc = AllocUpload(uint64_t(sw) * sh * 4, 4, &staging);
        if (!staging) return;
        staging_buffer = staging_alloc.buffer;
        staging_off = staging_alloc.offset;
        std::memcpy(staging, frame_.data(), size_t(sw) * sh * 4);
        source_view = frame_view_;
    } else {
        sw = ft->w;
        sh = ft->h;
        source_view = ft->samples > 1 ? ft->resolved_view : ft->view;
    }

    uint64_t gamma_off = 0;
    VkBuffer gamma_buffer = VK_NULL_HANDLE;
    if (gamma_dirty_) {
        void* gamma = nullptr;
        const UploadAlloc gamma_alloc = AllocUpload(256 * 4, 4, &gamma);
        if (!gamma) return;
        gamma_buffer = gamma_alloc.buffer;
        gamma_off = gamma_alloc.offset;
        uint8_t* out = static_cast<uint8_t*>(gamma);
        for (int i = 0; i < 256; i++) {
            out[i * 4 + 0] = gamma_ramp_[i];
            out[i * 4 + 1] = gamma_ramp_[256 + i];
            out[i * 4 + 2] = gamma_ramp_[512 + i];
            out[i * 4 + 3] = 255;
        }
    }

    const app::Mode pixels = app::WindowPixels();
    if (pixels.width && pixels.height && (pixels.width != extent_.width || pixels.height != extent_.height)) {
        vkDeviceWaitIdle(device_);
        if (!CreateSwapchain()) return;
    }
    const bool record_swap_trace =
        trace_now && trace_present_ && swap_capture_supported_ &&
        EnsureTraceBuffer(VkDeviceSize(extent_.width) * extent_.height * SwapBytesPerPixel(swapchain_format_));
    if (!present_pipeline_ || present_format_ != swapchain_format_) {
        if (present_pipeline_) vkDestroyPipeline(device_, present_pipeline_, nullptr);
        present_pipeline_ = VK_NULL_HANDLE;
        VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
        VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
        ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
        VkPipelineViewportStateCreateInfo vp{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
        vp.viewportCount = 1;
        vp.scissorCount = 1;
        VkPipelineRasterizationStateCreateInfo rs{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
        rs.polygonMode = VK_POLYGON_MODE_FILL;
        rs.cullMode = VK_CULL_MODE_NONE;
        rs.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
        rs.lineWidth = 1.0f;
        VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
        ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
        VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
        VkPipelineColorBlendAttachmentState cb{};
        cb.colorWriteMask = 0xF;
        VkPipelineColorBlendStateCreateInfo blend{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
        blend.attachmentCount = 1;
        blend.pAttachments = &cb;
        VkDynamicState dyn[] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
        VkPipelineDynamicStateCreateInfo dynamic{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
        dynamic.dynamicStateCount = 2;
        dynamic.pDynamicStates = dyn;
        VkPipelineShaderStageCreateInfo stages[2]{};
        stages[0].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[0].stage = VK_SHADER_STAGE_VERTEX_BIT;
        stages[0].module = present_vs_;
        stages[0].pName = "vs";
        stages[1].sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        stages[1].stage = VK_SHADER_STAGE_FRAGMENT_BIT;
        stages[1].module = present_ps_;
        stages[1].pName = "psmain";
        VkGraphicsPipelineCreateInfo info{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
        info.stageCount = 2;
        info.pStages = stages;
        info.pVertexInputState = &vi;
        info.pInputAssemblyState = &ia;
        info.pViewportState = &vp;
        info.pRasterizationState = &rs;
        info.pMultisampleState = &ms;
        info.pDepthStencilState = &ds;
        info.pColorBlendState = &blend;
        info.pDynamicState = &dynamic;
        info.layout = present_pipeline_layout_;
        info.renderPass = swap_pass_;
        if (CreatePipelines(1, &info, nullptr, &present_pipeline_) != VK_SUCCESS)
            return;
        present_format_ = swapchain_format_;
    }

    RenderUi();

    if (!swapchain_) return;
    VkResult r = vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    if (r != VK_SUCCESS) {
        Fail("wait for present fence", r);
        return;
    }
    uint32_t image = 0;
    r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_available_, VK_NULL_HANDLE, &image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        vkDeviceWaitIdle(device_);
        if (!CreateSwapchain()) LOGE("vulkan: could not rebuild the swapchain");
        return;  // the next frame retries and rebuilds the present pipeline
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        Fail("acquire", r);
        return;
    }
    if (image >= render_done_.size() || image >= swap_framebuffers_.size()) {
        LOGE("vulkan: acquired image %u outside %zu render-done and %zu swapchain framebuffers", image,
             render_done_.size(), swap_framebuffers_.size());
        failed_ = true;
        return;
    }

    r = vkResetFences(device_, 1, &fence_);
    if (r != VK_SUCCESS) {
        Fail("reset present fence", r);
        return;
    }
    r = vkResetCommandBuffer(cmd_, 0);
    if (r != VK_SUCCESS) {
        Fail("reset command buffer", r);
        return;
    }
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    r = vkBeginCommandBuffer(cmd_, &begin);
    if (r != VK_SUCCESS) {
        Fail("begin command buffer", r);
        return;
    }
    bound_pipe_ = VK_NULL_HANDLE;
    viewport_valid_ = false;

    if (direct) {
        VkImage src_img = ft->samples > 1 ? ft->resolved_image : ft->image;
        VkImageLayout& src_layout = ft->samples > 1 ? ft->resolved_layout : ft->layout;
        Transition(src_img, src_layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, ft->format);
    } else {
        Transition(frame_image_, frame_layout_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_FORMAT_R8G8B8A8_UNORM);
        VkBufferImageCopy copy{};
        copy.bufferOffset = staging_off;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {sw, sh, 1};
        vkCmdCopyBufferToImage(cmd_, staging_buffer, frame_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Transition(frame_image_, frame_layout_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_FORMAT_R8G8B8A8_UNORM);
    }
    if (gamma_dirty_) {
        Transition(gamma_image_, gamma_layout_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_FORMAT_R8G8B8A8_UNORM);
        VkBufferImageCopy copy{};
        copy.bufferOffset = gamma_off;
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {256, 1, 1};
        vkCmdCopyBufferToImage(cmd_, gamma_buffer, gamma_image_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
        Transition(gamma_image_, gamma_layout_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_FORMAT_R8G8B8A8_UNORM);
        gamma_dirty_ = false;
    }

    // SMAA 1x on the finished scene, before the post chain samples it
    const bool smaa_on = snapshot.smaa && direct && ft && !ft->depth && ft->w && ft->h;
    if (smaa_on && EnsureSmaa(ft->w, ft->h, ft->format)) {
        auto write = [&](VkDescriptorSet set, VkImageView t0, VkImageView t1, VkImageView t2) {
            VkDescriptorImageInfo images[3]{};
            const VkImageView views[3] = {t0, t1, t2};
            for (int i = 0; i < 3; i++) {
                images[i].imageView = views[i] ? views[i] : white_view_;
                images[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
            }
            VkDescriptorImageInfo samplers[2]{};
            samplers[0].sampler = smaa_linear_;
            samplers[1].sampler = smaa_point_;
            VkWriteDescriptorSet writes[5]{};
            for (int i = 0; i < 3; i++) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = set;
                writes[i].dstBinding = uint32_t(2 + i);
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
                writes[i].pImageInfo = &images[i];
            }
            for (int i = 0; i < 2; i++) {
                writes[3 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[3 + i].dstSet = set;
                writes[3 + i].dstBinding = uint32_t(6 + i);
                writes[3 + i].descriptorCount = 1;
                writes[3 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
                writes[3 + i].pImageInfo = &samplers[i];
            }
            vkUpdateDescriptorSets(device_, 5, writes, 0, nullptr);
        };
        float metrics[8] = {1.0f / float(ft->w), 1.0f / float(ft->h), float(ft->w), float(ft->h),
                            0, 0, 0, 0};
        if (smaa_lookups_pending_) {
            auto upload = [&](SmaaImage& image, VkBuffer buffer, uint64_t offset, uint32_t w, uint32_t h) {
                Transition(image.image, image.layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, image.format);
                VkBufferImageCopy copy{};
                copy.bufferOffset = offset;
                copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                copy.imageExtent = {w, h, 1};
                vkCmdCopyBufferToImage(cmd_, buffer, image.image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
                Transition(image.image, image.layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, image.format);
            };
            upload(smaa_area_, smaa_area_buffer_, smaa_area_off_, AREATEX_WIDTH, AREATEX_HEIGHT);
            upload(smaa_search_, smaa_search_buffer_, smaa_search_off_, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT);
            smaa_lookups_pending_ = false;
        }
        vkCmdPushConstants(cmd_, smaa_pipeline_layout_,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(metrics),
                           metrics);
        write(smaa_sets_[0], source_view, nullptr, nullptr);
        DrawSmaaPass(0, smaa_sets_[0], smaa_edges_, ft->w, ft->h);
        write(smaa_sets_[1], smaa_edges_.view, smaa_area_.view, smaa_search_.view);
        DrawSmaaPass(1, smaa_sets_[1], smaa_weights_, ft->w, ft->h);
        write(smaa_sets_[2], source_view, smaa_weights_.view, nullptr);
        DrawSmaaPass(2, smaa_sets_[2], smaa_output_, ft->w, ft->h);
        source_view = smaa_output_.view;
    }

    // the bloom chain from the image the present pass reads; without it the pass taps that image itself
    VkImageView bloom_view = VK_NULL_HANDLE;
    if (snapshot.bloom && direct && ft && !ft->depth && ft->w && ft->h)
        bloom_view = BuildBloom(source_view, ft->w, ft->h);
    // FSR 1 upscaling and sharpening of the 3D scene (not of movies and still pictures)
    if (direct && ft && !ft->depth && front.picture == gpu::Picture::Unknown)
        source_view = UpscaleAndSharpen(source_view, ft->format, ft->w, ft->h, extent_.width, extent_.height, snapshot);

    // PC distance blur: the main target's depth, resolved with MSAA or the image itself without (as d3d12.cpp)
    const SceneDepth* dof = nullptr;
    if (snapshot.dof && direct && ft) {
        auto it = scene_depth_.find(ft);
        const Target* d = it != scene_depth_.end() ? it->second.depth : nullptr;
        if (d && d->sample_view && (d->samples > 1 ? d->resolved_image != VK_NULL_HANDLE : d->image != VK_NULL_HANDLE))
            dof = &it->second;
    }
    VkImageView depth_view = white_view_;
    if (dof) {
        Target* d = dof->depth;
        depth_view = d->sample_view;
        if (d->samples > 1)
            Transition(d->resolved_image, d->resolved_layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, d->format);
        else
            Transition(d->image, d->layout, VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, d->format);
    }
    if ((dof != nullptr) != dof_active_) {
        dof_active_ = dof != nullptr;
        LOGR("vulkan: distance blur %s", dof_active_ ? "on" : "off");
    }

    VkDescriptorImageInfo ii[3]{};
    ii[0].imageView = source_view;
    ii[0].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ii[1].imageView = gamma_view_;
    ii[1].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    ii[2].imageView = depth_view;
    ii[2].imageLayout = dof ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                            : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkDescriptorImageInfo samplers[2]{};
    samplers[0].sampler = present_sampler_;
    samplers[1].sampler = present_point_sampler_;
    VkDescriptorImageInfo bloom_info{};
    bloom_info.imageView = bloom_view ? bloom_view : white_view_;
    bloom_info.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    VkWriteDescriptorSet writes[6]{};
    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = present_set_;
    writes[5].dstBinding = 5;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
    writes[5].pImageInfo = &bloom_info;
    for (int i = 0; i < 3; i++) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = present_set_;
        writes[i].dstBinding = uint32_t(i);
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        writes[i].pImageInfo = &ii[i];
    }
    for (int i = 0; i < 2; i++) {
        writes[3 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[3 + i].dstSet = present_set_;
        writes[3 + i].dstBinding = uint32_t(3 + i);
        writes[3 + i].descriptorCount = 1;
        writes[3 + i].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        writes[3 + i].pImageInfo = &samplers[i];
    }
    vkUpdateDescriptorSets(device_, 6, writes, 0, nullptr);

    const bool hdr = hdr_output_ != HdrOutput::Off;
    const uint32_t reported_peak = display_peak_nits_ ? display_peak_nits_ : 1000;
    const float hdr_mode = hdr ? (hdr_output_ == HdrOutput::ScRgb ? 1.0f : 2.0f) : 0.0f;
    const float hdr_paper = float(snapshot.hdr_paper_white);
    const float hdr_peak = float(snapshot.hdr_peak ? snapshot.hdr_peak : reported_peak);
    const float hdr_strength = float(snapshot.hdr_highlights) / 100.0f;
    const bool ui_data = ui_initialized_ && ImGui::GetDrawData() && ImGui::GetDrawData()->CmdListsCount;
    bool ui_ready = false;
    if (ui_data && !ui_direct_ && ui_fb_ && CreateUiComposite()) {
        Transition(ui_image_, ui_layout_, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL, VK_FORMAT_B8G8R8A8_UNORM);
        VkClearValue ui_clear{};
        VkRenderPassBeginInfo ui_begin{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
        ui_begin.renderPass = ui_pass_;
        ui_begin.framebuffer = ui_fb_;
        ui_begin.renderArea = {{0, 0}, {ui_w_, ui_h_}};
        ui_begin.clearValueCount = 1;
        ui_begin.pClearValues = &ui_clear;
        vkCmdBeginRenderPass(cmd_, &ui_begin, VK_SUBPASS_CONTENTS_INLINE);
        {
            std::lock_guard<std::recursive_mutex> guard(app::UiLock());
            ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd_);
        }
        vkCmdEndRenderPass(cmd_);
        // the pass's final layout transitioned the image for sampling
        ui_layout_ = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        VkDescriptorImageInfo image_info{ui_sampler_, ui_view_, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
        VkDescriptorImageInfo sampler_info{};
        sampler_info.sampler = ui_sampler_;
        VkWriteDescriptorSet ui_writes[2]{};
        ui_writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ui_writes[0].dstSet = ui_set_;
        ui_writes[0].dstBinding = 0;
        ui_writes[0].descriptorCount = 1;
        ui_writes[0].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE;
        ui_writes[0].pImageInfo = &image_info;
        ui_writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        ui_writes[1].dstSet = ui_set_;
        ui_writes[1].dstBinding = 1;
        ui_writes[1].descriptorCount = 1;
        ui_writes[1].descriptorType = VK_DESCRIPTOR_TYPE_SAMPLER;
        ui_writes[1].pImageInfo = &sampler_info;
        vkUpdateDescriptorSets(device_, 2, ui_writes, 0, nullptr);
        ui_ready = true;
    } else if (ui_data && !ui_direct_ && !ui_composite_logged_) {
        LOGE("vulkan: overlay composite unavailable; the Graphics overlay stays hidden");
        ui_composite_logged_ = true;
    }

    VkClearValue clear{};
    VkRenderPassBeginInfo rp{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rp.renderPass = swap_pass_;
    rp.framebuffer = swap_framebuffers_[image];
    rp.renderArea = {{0, 0}, extent_};
    rp.clearValueCount = 1;
    rp.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd_, &rp, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline_);
    VkViewport viewport{0, 0, float(extent_.width), float(extent_.height), 0, 1};
    VkRect2D scissor{{0, 0}, extent_};
    vkCmdSetViewport(cmd_, 0, 1, &viewport);
    vkCmdSetScissor(cmd_, 0, 1, &scissor);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, present_pipeline_layout_, 0, 1, &present_set_, 0,
                            nullptr);
    const bool bloom_on = snapshot.bloom && direct;
    if (bloom_on != bloom_active_) {
        bloom_active_ = bloom_on;
        LOGR("vulkan: bloom %s", bloom_active_ ? "on" : "off");
    }
    const uint32_t bloom_w = ft ? ft->w : sw, bloom_h = ft ? ft->h : sh;
    const bool dof_on = dof != nullptr;
    // a picture the title wrote (gpu::LayoutPicture): a movie's 4:3 picture stretched across the window, a still
    // page at 4:3 in the middle
    float frame_aspect = app::Widescreen() ? app::AspectRatio() : 4.0f / 3.0f;
    const gpu::PictureLayout layout = gpu::LayoutPicture(front.picture, frame_aspect);
    VkViewport frame_viewport = viewport;
    if (front.picture != gpu::Picture::Unknown) {
        // as D3D12's BlitFrame: the layout's shape, centred and fitted in both dimensions
        const float ow = float(extent_.width), oh = float(extent_.height);
        const float w = std::min(ow, oh * layout.aspect);
        const float h = w / layout.aspect;
        frame_viewport.x = (ow - w) / 2;
        frame_viewport.y = (oh - h) / 2;
        frame_viewport.width = w;
        frame_viewport.height = h;
    }
    vkCmdSetViewport(cmd_, 0, 1, &frame_viewport);
    const bool picture = front.picture != gpu::Picture::Unknown;
    float present_pc[16] = {snapshot.pc_contrast ? 1.0f : 0.0f, bloom_on ? float(snapshot.bloom_strength) / 100.0f : 0.0f,
                            dof_on ? 1.0f : 0.0f, bloom_view ? 1.0f : 0.0f,
                            bloom_w ? 1.0f / float(bloom_w) : 0.0f,
                            bloom_h ? 1.0f / float(bloom_h) : 0.0f,
                            dof ? dof->near_w : 0.1f, dof ? dof->far_w : 1000.0f,
                            hdr_mode, hdr_paper, hdr_peak, hdr_strength,
                            picture ? layout.uv[0] : 0.0f, picture ? layout.uv[1] : 0.0f,
                            picture ? layout.uv[2] : 1.0f, picture ? layout.uv[3] : 1.0f};
    vkCmdPushConstants(cmd_, present_pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(present_pc),
                       present_pc);
    vkCmdDraw(cmd_, 3, 1, 0, 0);
    if (ui_data && ui_direct_) {
        // SDR: the overlay blends straight over the game image, as the composite would have
        std::lock_guard<std::recursive_mutex> guard(app::UiLock());
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd_);
    }
    if (ui_ready) {
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline_);
        vkCmdSetViewport(cmd_, 0, 1, &viewport);
        vkCmdSetScissor(cmd_, 0, 1, &scissor);
        vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, ui_pipeline_layout_, 0, 1, &ui_set_, 0,
                                nullptr);
        const float ui_pc[4] = {hdr_mode, hdr_paper, hdr_peak, hdr_strength};
        vkCmdPushConstants(cmd_, ui_pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(ui_pc), ui_pc);
        vkCmdDraw(cmd_, 3, 1, 0, 0);
    }
    vkCmdEndRenderPass(cmd_);
    if (record_swap_trace) {
        VkImageLayout swap_layout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        Transition(swapchain_images_[image], swap_layout, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, swapchain_format_);
        VkBufferImageCopy copy{};
        copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        copy.imageExtent = {extent_.width, extent_.height, 1};
        vkCmdCopyImageToBuffer(cmd_, swapchain_images_[image], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, trace_buffer_, 1,
                               &copy);
        Transition(swapchain_images_[image], swap_layout, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, swapchain_format_);
    }
    vkEndCommandBuffer(cmd_);

    VkPipelineStageFlags wait_stage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSemaphore render_done = render_done_[image];
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.waitSemaphoreCount = 1;
    submit.pWaitSemaphores = &image_available_;
    submit.pWaitDstStageMask = &wait_stage;
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_;
    submit.signalSemaphoreCount = 1;
    submit.pSignalSemaphores = &render_done;
    r = vkQueueSubmit(queue_, 1, &submit, fence_);
    if (r != VK_SUCCESS) {
        Fail("present submit", r);
        return;
    }
    VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    present.waitSemaphoreCount = 1;
    present.pWaitSemaphores = &render_done;
    present.swapchainCount = 1;
    present.pSwapchains = &swapchain_;
    present.pImageIndices = &image;
    r = vkQueuePresentKHR(present_queue_, &present);
    if (r == VK_ERROR_OUT_OF_DATE_KHR || r == VK_SUBOPTIMAL_KHR) {
        vkDeviceWaitIdle(device_);
        if (!CreateSwapchain()) LOGE("vulkan: could not rebuild the swapchain");
    } else if (r == VK_ERROR_DEVICE_LOST || r == VK_ERROR_SURFACE_LOST_KHR) {
        Fail("present", r);
    } else if (r != VK_SUCCESS) {
        LOGE("vulkan: present failed: %s", VkError(r));
    }

    // the next frame may reuse the upload ring and descriptor pool only now
    present_lap.Stop();
    bool waited = false;
    {
        const auto wait_at = perf_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        const VkResult wait_result = vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
        if (wait_result != VK_SUCCESS)
            Fail("wait for present fence", wait_result);
        else
            waited = true;
        if (perf_)
            perf_wait_ms_ +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_at).count();
    }
    if (record_swap_trace && waited) {
        const std::string path = trace_base + "_present.tga";
        if (WriteSwapTga(path.c_str(), trace_ptr_, extent_.width, extent_.height, swapchain_format_,
                         hdr_output_, float(snapshot.hdr_paper_white)))
            LOGR("linux: trace present %s saved (%ux%u)", path.c_str(), extent_.width, extent_.height);
    }
    frame_end.finished = true;
    ResetFrame();
    ++frames_;
    presents_seen_.store(frames_);
    SavePipelineCache(false);
    if (frames_ % 60 == 0) {
        if (perf_) {
            const auto now = std::chrono::steady_clock::now();
            const double ms =
                std::chrono::duration<double, std::milli>(now - perf_log_at_).count() / 60.0;
            LOGI("renderer: frame %llu, %.2f ms (%.1f fps), draw %.2f [asm %.2f vert %.2f tex %.2f pix %.2f tgt "
                 "%.2f pipe %.2f const %.2f geo %.2f rec %.2f], ui %.2f, wait %.2f, sync %.2f, copy %.2f, clear "
                 "%.2f, present %.2f, %llu draws, %llu textures, %llu compiles, direct %llu stale %llu readback "
                 "%llu, binds %llu, passes %llu, fbs %llu, new targets %llu, target uploads %llu, deferred flushes %.1f, "
                 "AO runs %.1f per frame (vulkan)",
                 frames_, ms, ms > 0 ? 1000.0 / ms : 0.0, perf_draw_ms_ / 60.0, perf_assemble_ms_ / 60.0,
                 perf_vertex_ms_ / 60.0, perf_tex_ms_ / 60.0, perf_pixel_ms_ / 60.0, perf_target_ms_ / 60.0,
                 perf_pipe_ms_ / 60.0, perf_const_ms_ / 60.0, perf_geom_ms_ / 60.0, perf_record_ms_ / 60.0,
                 perf_ui_ms_ / 60.0, perf_wait_ms_ / 60.0, perf_sync_ms_ / 60.0, perf_copy_ms_ / 60.0,
                 perf_clear_ms_ / 60.0, perf_present_ms_ / 60.0, draws_, tex_uploads_, shader_compiles_, perf_direct_,
                 perf_stale_, perf_readback_, perf_bind_, perf_pass_new_, perf_fb_new_, perf_target_new_,
                 perf_target_upload_, double(perf_defer_flushes_) / 60.0, double(perf_ao_runs_) / 60.0);
            perf_log_at_ = now;
            perf_draw_ms_ = perf_ui_ms_ = perf_wait_ms_ = 0.0;
            perf_assemble_ms_ = perf_vertex_ms_ = perf_tex_ms_ = perf_pixel_ms_ = 0.0;
            perf_target_ms_ = perf_pipe_ms_ = perf_const_ms_ = perf_geom_ms_ = perf_record_ms_ = 0.0;
            perf_sync_ms_ = perf_copy_ms_ = perf_clear_ms_ = perf_present_ms_ = 0.0;
            perf_direct_ = perf_stale_ = perf_readback_ = 0;
            perf_bind_ = perf_pass_new_ = perf_fb_new_ = perf_target_new_ = perf_target_upload_ = 0;
            if (!perf_flush_lines_.empty()) {
                // the callers that flushed the deferred scene most in this sample, by source line
                std::vector<std::pair<unsigned long long, uint32_t>> top;
                for (auto [line, count] : perf_flush_lines_) top.emplace_back(count, line);
                std::ranges::sort(top, std::greater{});
                char text[160] = "";
                size_t used = 0;
                for (size_t i = 0; i < top.size() && i < 4 && used < sizeof(text); i++)
                    used += size_t(std::snprintf(text + used, sizeof(text) - used, " line %u: %.1f/frame",
                                                 top[i].second, double(top[i].first) / 60.0));
                LOGI("renderer: deferred flushes by caller (backend_vk.cpp):%s", text);
                perf_flush_lines_.clear();
            }
            perf_defer_flushes_ = perf_ao_runs_ = 0;
        } else {
            size_t late_count = 0;
            {
                std::lock_guard lock(late_lock_);
                late_count = late_.size();
            }
            LOGI("renderer: frame %llu, %llu draws, %llu textures, %llu shader compiles, cache %zu tex, targets %zu, "
                 "late %zu, pipes %zu, samplers %zu, fbs %zu, upload %llu MB (vulkan)",
                 frames_, draws_, tex_uploads_, shader_compiles_, textures_.size(), targets_.size(), late_count,
                 pipelines_.size(), samplers_.size(), framebuffers_.size(),
                 static_cast<unsigned long long>(upload_size_ >> 20));
        }
    }
    draws_ = 0;
    if (trace_) frame_trace_.clear();
}

std::unique_ptr<Backend> CreateVulkanBackend() { return std::make_unique<VulkanBackend>(); }

} // namespace gpu




