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
#include <mutex>
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
#include "core/log.h"
#include "gpu/backend.h"
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

uint32_t SimpleState(const State& s, uint32_t method) { return s.simple[method >> 2]; }
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
    // texture pack replacement: the key whose image is being loaded, and the ready image
    uint64_t pack_key = 0;
    uint32_t pack_gen = 0;
    Tex* swap = nullptr;
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
    bool CreateCommands();
    bool CreateUpload();
    bool CreateUploadBuffer(uint64_t size, VkBuffer& buffer, VkDeviceMemory& memory, uint8_t*& ptr);
    bool CreatePresent();
    bool CreateUi();
    void DestroyUi();
    void RenderUi();
    void Shutdown();
    bool EnsureFrameImage(uint32_t w, uint32_t h);
    bool EnsureTraceBuffer(VkDeviceSize size);
    void ResetFrame();

    // recording
    void BeginRecord();
    void EndRecord(bool wait);
    void Transition(VkImage image, VkImageLayout& layout, VkImageLayout want, VkFormat format, uint32_t levels = 1,
                    uint32_t layers = 1);
    VkRenderPass RenderPassFor(VkFormat color, VkFormat depth);
    VkRenderPass RenderPassForMs(VkFormat color, VkFormat depth, uint32_t samples);
    VkFramebuffer FramebufferFor(Target* rt, Target* ds);
    void BindTargets(const State& s, Target*& rt, Target*& ds);
    void FlushPass();
    VkPipeline ClearPipeline(VkFormat format, VkFormat depth, uint32_t mask, uint32_t samples);

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
        bool CreatePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps, VkPipeline& out);

    // targets and textures
    Target* EnsureTarget(const Surface& s, bool depth);
    Target* TargetAt(uint32_t data, bool depth, bool newest);
    bool ReadTarget(Target* t);
    void DumpTexture(const Tex* t);
    bool UploadTarget(Target* t);
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
    uint32_t msaa_ = 1;
    uint32_t color_samples_ = 1, depth_samples_ = 1;
    uint32_t instance_version_ = VK_API_VERSION_1_0;
    bool depth_resolve_ = false;
    VkResolveModeFlagBits depth_resolve_mode_ = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
    PFN_vkCreateRenderPass2 create_render_pass2_ = nullptr;
    bool api12_ = false;
    VkInstance instance_ = VK_NULL_HANDLE;
    VkSurfaceKHR surface_ = VK_NULL_HANDLE;
    VkPhysicalDevice physical_ = VK_NULL_HANDLE;
    VkDevice device_ = VK_NULL_HANDLE;
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

    VkCommandPool command_pool_ = VK_NULL_HANDLE;
    VkCommandBuffer cmd_ = VK_NULL_HANDLE;
    VkSemaphore image_available_ = VK_NULL_HANDLE;
    std::vector<VkSemaphore> render_done_;  // one per swapchain image: a present may still own the previous one
    VkFormat depth_format_ = VK_FORMAT_D24_UNORM_S8_UINT;
    bool depth_has_stencil_ = true;
    VkFence fence_ = VK_NULL_HANDLE;
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

    VkDescriptorSetLayout set_layout_ = VK_NULL_HANDLE;
    VkPipelineLayout pipeline_layout_ = VK_NULL_HANDLE;
    VkDescriptorPool descriptor_pool_ = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> draw_sets_;
    size_t draw_set_next_ = 0;

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
    bool dof_active_ = false;

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
bool WriteSwapTga(const char* path, const uint8_t* px, uint32_t w, uint32_t h, VkFormat fmt) {
    FILE* f = std::fopen(path, "wb");
    if (!f) return false;
    uint8_t head[18] = {0, 0, 2};  // uncompressed true colour
    head[12] = uint8_t(w), head[13] = uint8_t(w >> 8), head[14] = uint8_t(h), head[15] = uint8_t(h >> 8);
    head[16] = 24;
    head[17] = 0x20;  // top row first
    std::fwrite(head, 1, 18, f);
    const bool bgr = fmt == VK_FORMAT_B8G8R8A8_UNORM || fmt == VK_FORMAT_B8G8R8A8_SRGB;
    std::vector<uint8_t> row(size_t(w) * 3);
    for (uint32_t y = 0; y < h; y++) {
        for (uint32_t x = 0; x < w; x++) {
            const uint8_t* p = px + (size_t(y) * w + x) * 4;
            row[size_t(x) * 3 + 0] = bgr ? p[2] : p[0];
            row[size_t(x) * 3 + 1] = p[1];
            row[size_t(x) * 3 + 2] = bgr ? p[0] : p[2];
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

bool VulkanBackend::Init(uint32_t width, uint32_t height) {
    width_ = width;
    height_ = height;
    scale_ = app::RenderScale(height_);
    stretch_ = app::RenderStretch(width_, height_);
    aniso_setting_ = app::Snapshot().aniso;
    aspect_ = app::AspectRatio();
    perf_ = std::getenv("TCNYC_PERF") != nullptr;
    trace_ = std::getenv("TCNYC_TRACE") != nullptr;
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
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(physical_, &props);
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
        if (dsr.supportedDepthResolveModes & VK_RESOLVE_MODE_MAX_BIT)
            depth_resolve_mode_ = VK_RESOLVE_MODE_MAX_BIT;
        else if (dsr.supportedDepthResolveModes & VK_RESOLVE_MODE_SAMPLE_ZERO_BIT)
            depth_resolve_mode_ = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;
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
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &set_layout_;
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, 16};  // the masked-clear colour
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device_, &pl, nullptr, &pipeline_layout_) != VK_SUCCESS) return false;

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
    if (!CreatePresent()) return false;

    LOGR("vulkan: %s, %ux%u swapchain, GPU draws", device_name_.c_str(), extent_.width, extent_.height);
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
    info.enabledExtensionCount = count;
    info.ppEnabledExtensionNames = extensions;
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
    const char* extensions[2] = {VK_KHR_SWAPCHAIN_EXTENSION_NAME, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME};
    uint32_t extension_count = 1;
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
        for (const VkExtensionProperties& e : available)
            has_border = has_border || !std::strcmp(e.extensionName, VK_EXT_CUSTOM_BORDER_COLOR_EXTENSION_NAME);
        if (has_border) {
            VkPhysicalDeviceFeatures2 features2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
            features2.pNext = &border_features;
            vkGetPhysicalDeviceFeatures2(physical_, &features2);
            border_custom_ = border_features.customBorderColors && border_features.customBorderColorWithoutFormat;
            border_features.customBorderColors = border_custom_ ? VK_TRUE : VK_FALSE;
            border_features.customBorderColorWithoutFormat = border_custom_ ? VK_TRUE : VK_FALSE;
            if (border_custom_) extension_count = 2;
        }
    }
    LOGR("vulkan: custom sampler border colours %s", border_custom_ ? "on" : "off (fixed colours)");
    VkPhysicalDeviceFeatures features{};
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
    info.enabledExtensionCount = extension_count;
    info.ppEnabledExtensionNames = extensions;
    info.pEnabledFeatures = &features;
    info.pNext = border_custom_ ? &border_features : nullptr;
    VkResult r = vkCreateDevice(physical_, &info, nullptr, &device_);
    if (r != VK_SUCCESS) {
        LOGE("vulkan: vkCreateDevice failed: %s", VkError(r));
        return false;
    }
    vkGetDeviceQueue(device_, graphics_family_, 0, &queue_);
    vkGetDeviceQueue(device_, present_family_, 0, &present_queue_);
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
    VkSurfaceFormatKHR chosen = formats[0];
    for (const auto& f : formats)
        if (f.format == VK_FORMAT_B8G8R8A8_UNORM || f.format == VK_FORMAT_R8G8B8A8_UNORM) {
            chosen = f;
            break;
        }
    uint32_t mode_count = 0;
    vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &mode_count, nullptr);
    std::vector<VkPresentModeKHR> modes(mode_count);
    if (mode_count)
        vkGetPhysicalDeviceSurfacePresentModesKHR(physical_, surface_, &mode_count, modes.data());
    const bool vsync = app::Snapshot().vsync;
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
    // F3 can read the finished swapchain image back when the surface allows it
    swap_capture_supported_ = (caps.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) != 0;
    if (swapchain_) {
        vkDeviceWaitIdle(device_);
        DestroyUi();
        for (VkFramebuffer fb : swap_framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
        swap_framebuffers_.clear();
        for (VkImageView view : swap_views_) vkDestroyImageView(device_, view, nullptr);
        swap_views_.clear();
        if (swap_pass_) vkDestroyRenderPass(device_, swap_pass_, nullptr);
        swap_pass_ = VK_NULL_HANDLE;
        if (present_pipeline_) vkDestroyPipeline(device_, present_pipeline_, nullptr);
        present_pipeline_ = VK_NULL_HANDLE;
        present_format_ = VK_FORMAT_UNDEFINED;
        vkDestroySwapchainKHR(device_, swapchain_, nullptr);
        swapchain_ = VK_NULL_HANDLE;
    }
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
    VkResult r = vkCreateSwapchainKHR(device_, &info, nullptr, &swapchain_);
    if (r != VK_SUCCESS) {
        LOGE("vulkan: vkCreateSwapchainKHR failed: %s", VkError(r));
        return false;
    }
    uint32_t count = 0;
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, nullptr);
    swapchain_images_.resize(count);
    vkGetSwapchainImagesKHR(device_, swapchain_, &count, swapchain_images_.data());
    extent_ = extent;
    swapchain_format_ = chosen.format;
    vsync_ = vsync;
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
    CreateUi();  // the Graphics overlay is optional; rendering continues without it
    return true;
}

bool VulkanBackend::CreateUi() {
    if (ui_initialized_ || !swap_pass_ || !ImGui::GetCurrentContext()) return true;
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
    info.PipelineInfoMain.RenderPass = swap_pass_;
    info.PipelineInfoMain.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
    if (!ImGui_ImplVulkan_Init(&info)) {
        LOGE("vulkan: ImGui Vulkan backend failed to start");
        return false;
    }
    ui_initialized_ = true;
    LOGR("vulkan: ImGui overlay ready");
    return true;
}

void VulkanBackend::DestroyUi() {
    if (!ui_initialized_) return;
    std::lock_guard<std::recursive_mutex> guard(app::UiLock());
    ImGui_ImplVulkan_Shutdown();
    ui_initialized_ = false;
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

    VkDescriptorSetLayoutBinding bind[5]{};
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
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 5;
    li.pBindings = bind;
    if (vkCreateDescriptorSetLayout(device_, &li, nullptr, &present_set_layout_) != VK_SUCCESS) return false;

    VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 3},
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
    VkPushConstantRange push{VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 8};  // contrast, bloom, dof, source size, near/far
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &push;
    if (vkCreatePipelineLayout(device_, &pl, nullptr, &present_pipeline_layout_) != VK_SUCCESS) return false;

    static const char* kPresentHlsl = R"(
[[vk::binding(0, 0)]] Texture2D<float4> frameTex;
[[vk::binding(1, 0)]] Texture2D<float4> gammaTex;
[[vk::binding(2, 0)]] Texture2D<float4> depthTex;
[[vk::binding(3, 0)]] SamplerState frameSampler;
[[vk::binding(4, 0)]] SamplerState depthSampler;
struct PC { float pc; float bloom; float dof; float pad; float2 src; float2 zrange; };
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
    float4 c = frameTex.Sample(frameSampler, i.uv);
    if (pc.dof > 0.0) c.rgb = DistanceBlur(i.uv, c.rgb);
    c.rgb = ToGamma(c.rgb);
    if (pc.bloom > 0.0) c.rgb += BloomAt(i.uv) * 1.2;
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

void VulkanBackend::EndRecord(bool wait) {
    if (!recording_) return;
    FlushPass();
    vkEndCommandBuffer(cmd_);
    recording_ = false;
    VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    submit.commandBufferCount = 1;
    submit.pCommandBuffers = &cmd_;
    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    vkResetFences(device_, 1, &fence_);
    const uint64_t serial = ++submit_serial_;
    VkResult r = vkQueueSubmit(queue_, 1, &submit, fence_);
    if (r != VK_SUCCESS) {
        LOGE("vulkan: submit failed: %s", VkError(r));
        return;
    }
    if (wait) {
        vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
        completed_serial_.store(serial);
    }
}

void VulkanBackend::ResetFrame() {
    upload_used_ = 0;
    draw_set_next_ = 0;
    scene_dof_ = app::Snapshot().dof;
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
    for (VkFramebuffer fb : retired_framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    retired_framebuffers_.clear();
    for (Tex* tx : retired_borrowed_) delete tx;
    retired_borrowed_.clear();
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
}

void VulkanBackend::GetReport(uint32_t report) {
    if (!report) return;
    LOGR("linux: GetReport %08X recording %d pass %d queries %zu", report, int(recording_), int(pass_ != VK_NULL_HANDLE),
         counted_.size());
    // the console's GPU fills the report after the counted draws; finish them first
    if (recording_)
        EndRecord(true);
    else
        vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
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
        att[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
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
    uint64_t key = uint64_t(color) | (uint64_t(depth) << 32) | (uint64_t(samples) << 48) | (1ull << 63);
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
        att[n].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
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
        att[n].storeOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
        att[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        att[n].initialLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        att[n].finalLayout = VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL;
        depth_ref = n++;
        att[n] = {VK_STRUCTURE_TYPE_ATTACHMENT_DESCRIPTION_2};
        att[n].format = depth;
        att[n].samples = VK_SAMPLE_COUNT_1_BIT;
        att[n].loadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[n].storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        att[n].stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        att[n].stencilStoreOp = VK_ATTACHMENT_STORE_OP_STORE;
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
    dsr.stencilResolveMode = VK_RESOLVE_MODE_SAMPLE_ZERO_BIT;

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

void VulkanBackend::BindTargets(const State& s, Target*& rt, Target*& ds) {
    if (perf_) perf_bind_++;
    rt = nullptr;
    ds = nullptr;
    const bool want_rt = s.rt.header && s.rt.size;
    const bool want_ds = s.zs.header && s.zs.size;
    TextureDesc rd, dd;
    const uint32_t rt_data = want_rt ? (DescribeSurface(s.rt, rd) ? rd.data : s.rt.data) : 0;
    const uint32_t ds_data = want_ds ? (DescribeSurface(s.zs, dd) ? dd.data : s.zs.data) : 0;
    const auto same_target = [](Target* old, uint32_t data, const TextureDesc& desc, bool depth) {
        return old && old->data == data && old->depth == depth && old->gw == desc.width && old->gh == desc.height &&
               old->guest_format == desc.format;
    };
    if (pass_ != VK_NULL_HANDLE &&
        ((want_rt && same_target(pass_rt_, rt_data, rd, false)) || (!want_rt && !pass_rt_)) &&
        ((want_ds && same_target(pass_ds_, ds_data, dd, true)) || (!want_ds && !pass_ds_))) {
        rt = pass_rt_;
        ds = pass_ds_;
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
        return;
    }
    VkRenderPassBeginInfo info{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    info.renderPass = pass_;
    info.framebuffer = pass_fb_;
    info.renderArea = {{0, 0}, {rt ? rt->w : ds->w, rt ? rt->h : ds->h}};
    vkCmdBeginRenderPass(cmd_, &info, VK_SUBPASS_CONTENTS_INLINE);
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
    const std::string injected0 = InjectBindings(src);
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

bool VulkanBackend::CreatePipeline(const PsoKey& k, VkShaderModule vs, VkShaderModule ps, VkPipeline& out) {
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
        if (bind_of[buffer] < 0) {
            bind_of[buffer] = int(nb);
            bindings[nb].binding = buffer;
            bindings[nb].stride = k.strides[buffer];
            bindings[nb].inputRate = VK_VERTEX_INPUT_RATE_VERTEX;
            nb++;
        }
        attrs[na].location = na;
        attrs[na].binding = buffer;
        attrs[na].format = ElementFormat(e & 0xFF);
        attrs[na].offset = (e >> 16);
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
    info.renderPass = k.samples > 1 ? RenderPassForMs(VkFormat(k.rtfmt), VkFormat(k.dsfmt), k.samples)
                                   : RenderPassFor(VkFormat(k.rtfmt), VkFormat(k.dsfmt));
    if (info.renderPass == VK_NULL_HANDLE) return false;
    VkResult r = vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &out);
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
    if (pass_rt_ == t) pass_rt_ = nullptr;
    if (pass_ds_ == t) pass_ds_ = nullptr;

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
        if (it->second.depth == t)
            it = scene_depth_.erase(it);
        else
            ++it;
    }
}

void VulkanBackend::DestroyTargets() {
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
    scene_depth_.clear();
    {
        std::lock_guard lock(late_lock_);
        for (auto& [address, l] : late_)
            for (LateSlot& s : l.slot)
                if (s.buffer) late_retired_.push_back({s.buffer, s.memory});
        late_.clear();
    }
    for (auto& [key, t] : borrowed_) delete t;
    borrowed_.clear();
    for (auto& [key, fb] : framebuffers_) vkDestroyFramebuffer(device_, fb, nullptr);
    framebuffers_.clear();
    framebuffer_targets_.clear();
    pass_ = VK_NULL_HANDLE;
    pass_fb_ = VK_NULL_HANDLE;
    pass_rt_ = pass_ds_ = nullptr;
}

Target* VulkanBackend::TargetAt(uint32_t data, bool depth, bool newest) {
    Target* best = nullptr;
    for (auto& [key, t] : targets_) {
        if (!t || t->depth != depth || t->data != data) continue;
        if (!best || (newest ? t->last_use > best->last_use : t->last_use < best->last_use)) best = t;
    }
    return best;
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
        t->header = s.header;
        t->size = s.size;
        t->last_use = frames_ + 1;
        if ((DirtyEpoch(t->data) != t->epoch || t->upload_pending) && !depth) {
            EndRecord(true);
            UploadTarget(t);
            t->upload_pending = false;
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
    t->pitch = d.pitch;
    t->linear = d.linear;
    t->swizzled = !d.linear;
    t->depth = depth;
    t->bytes = uint8_t(std::max<uint32_t>(1u, Format(d.format).bits / 8));
    t->format = depth ? depth_format_ : VK_FORMAT_B8G8R8A8_UNORM;

    t->samples = msaa_;
    auto create_image = [&](uint32_t samples, bool resolved, VkImage& image, VkDeviceMemory& memory,
                            VkImageView& out_view) {
        VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = t->format;
        info.extent = {t->w, t->h, 1};
        info.mipLevels = 1;
        info.arrayLayers = 1;
        info.samples = VkSampleCountFlagBits(samples);
        info.tiling = VK_IMAGE_TILING_OPTIMAL;
        const VkImageUsageFlags attachment =
            depth ? VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT : VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
        info.usage = resolved ? (VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
                                 VK_IMAGE_USAGE_TRANSFER_DST_BIT | attachment)
                              : attachment;
        if (vkCreateImage(device_, &info, nullptr, &image) != VK_SUCCESS) return false;
        VkMemoryRequirements req{};
        vkGetImageMemoryRequirements(device_, image, &req);
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
        if (!found || vkAllocateMemory(device_, &alloc, nullptr, &memory) != VK_SUCCESS) return false;
        vkBindImageMemory(device_, image, memory, 0);
        VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        view.image = image;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = t->format;
        view.subresourceRange = {depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
                                       : VK_IMAGE_ASPECT_COLOR_BIT,
                                 0, 1, 0, 1};
        return vkCreateImageView(device_, &view, nullptr, &out_view) == VK_SUCCESS;
    };
    if (!create_image(t->samples, t->samples == 1, t->image, t->memory, t->view)) {
        if (t->samples == 1) {
            delete t;
            return nullptr;
        }
        t->samples = 1;
        t->image = VK_NULL_HANDLE;
        t->memory = VK_NULL_HANDLE;
        t->view = VK_NULL_HANDLE;
        if (!create_image(1, true, t->image, t->memory, t->view)) {
            delete t;
            return nullptr;
        }
    }
    if (t->samples > 1) {
        if (!create_image(1, true, t->resolved_image, t->resolved_memory, t->resolved_view)) {
            delete t;
            return nullptr;
        }
        t->resolved_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    }
    if (depth) {
        // shaders read the depth aspect only; the resolved image's combined view stays for present
        VkImageViewCreateInfo sample{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        sample.image = t->samples > 1 ? t->resolved_image : t->image;
        sample.viewType = VK_IMAGE_VIEW_TYPE_2D;
        sample.format = t->format;
        sample.subresourceRange = {VK_IMAGE_ASPECT_DEPTH_BIT, 0, 1, 0, 1};
        if (vkCreateImageView(device_, &sample, nullptr, &t->sample_view) != VK_SUCCESS) return nullptr;
    }
    targets_[key] = t;
    // a reused address can collect a few shapes; drop the ones idle for a long time
    if (targets_.size() > 64) {
        for (auto e = targets_.begin(); e != targets_.end();) {
            Target* c = e->second;
            if (c != t && c->last_use + 600 < frames_) {
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
    if (pass_rt_ == t || pass_ds_ == t) FlushPass();
    VkImage old_image = t->image;
    VkDeviceMemory old_memory = t->memory;
    VkImageView old_view = t->view;
    VkImage old_resolved = t->resolved_image;
    VkDeviceMemory old_resolved_memory = t->resolved_memory;
    VkImageView old_resolved_view = t->resolved_view;
    VkImageView old_sample_view = t->sample_view;
    VkImage old_fb = t->fb_image;
    VkDeviceMemory old_fb_memory = t->fb_memory;
    VkImageView old_fb_view = t->fb_view;
    t->image = VK_NULL_HANDLE;
    t->memory = VK_NULL_HANDLE;
    t->view = VK_NULL_HANDLE;
    t->resolved_image = VK_NULL_HANDLE;
    t->resolved_memory = VK_NULL_HANDLE;
    t->resolved_view = VK_NULL_HANDLE;
    t->resolved_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    t->sample_view = VK_NULL_HANDLE;
    t->fb_image = VK_NULL_HANDLE;
    t->fb_memory = VK_NULL_HANDLE;
    t->fb_view = VK_NULL_HANDLE;
    t->fb_layout = VK_IMAGE_LAYOUT_UNDEFINED;
    delete t->fb_tex;
    t->fb_tex = nullptr;
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
            vkBindImageMemory(device_, t->image, t->memory, 0);
            t->layout = VK_IMAGE_LAYOUT_UNDEFINED;
            VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            view.image = t->image;
            view.viewType = VK_IMAGE_VIEW_TYPE_2D;
            view.format = t->format;
            view.subresourceRange = {t->depth ? VkImageAspectFlags(VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT)
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
    if (old_fb_view) vkDestroyImageView(device_, old_fb_view, nullptr);
    if (old_fb) vkDestroyImage(device_, old_fb, nullptr);
    if (old_fb_memory) vkFreeMemory(device_, old_fb_memory, nullptr);
    if (old_sample_view) vkDestroyImageView(device_, old_sample_view, nullptr);
    if (old_resolved_view) vkDestroyImageView(device_, old_resolved_view, nullptr);
    if (old_resolved) vkDestroyImage(device_, old_resolved, nullptr);
    if (old_resolved_memory) vkFreeMemory(device_, old_resolved_memory, nullptr);
    if (old_view) vkDestroyImageView(device_, old_view, nullptr);
    if (old_image) vkDestroyImage(device_, old_image, nullptr);
    if (old_memory) vkFreeMemory(device_, old_memory, nullptr);
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
    EndRecord(true);
    BeginRecord();
    VkImage upload_image = t->image;
    VkImageLayout& upload_layout = t->layout;
    Transition(upload_image, upload_layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->format);
    VkBufferImageCopy copy{};
    copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    copy.imageExtent = {t->w, t->h, 1};
    vkCmdCopyBufferToImage(cmd_, buffer, upload_image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    EndRecord(true);
    vkUnmapMemory(device_, memory);
    vkDestroyBuffer(device_, buffer, nullptr);
    vkFreeMemory(device_, memory, nullptr);
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
                         t->h, t->format);
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
            FlushPass();
            BeginRecord();
            VkImage target_image = target->samples > 1 ? target->resolved_image : target->image;
            VkImageLayout& target_layout = target->samples > 1 ? target->resolved_layout : target->layout;
            Transition(target_image, target_layout,
                       depth_tex ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL
                                 : VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                       target->format);
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
    if (!same || t->frame != frames_ + 1 || t->palette != palette_hash) {
        t->frame = frames_ + 1;
        t->palette = palette_hash;
        const uint64_t sig = MemSig(d.data, TotalSize(d)) ^ DirtyEpoch(d.data) ^ palette_hash;
        if (!same || sig != t->sig) {
            t->sig = sig;

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
    EndRecord(true);
    BeginRecord();
    Transition(t->image, t->layout, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, t->format, t->levels, t->layers);
    for (const VkBufferImageCopy& copy : copies)
        vkCmdCopyBufferToImage(cmd_, buffer, t->image, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copy);
    Transition(t->image, t->layout, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, t->format, t->levels, t->layers);
    EndRecord(true);
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
        if (prog && s.rs[kRsFogTableMode] == kFogTableNone) {
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
        if (info[size_t(i)].flags & kTexTarget)
            c.rtshift[i] = (!prog || ReadsViewportOffset(s.vs->program)) ? -0.5f : 0.0f;
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
    if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &pipeline) != VK_SUCCESS)
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
    PixelKey pk = MakePixelKey(s, info.data());
    pk.quad = count <= 4 && !s.rs[kRsZEnable] && a.topology != Topology::Points;
    uint64_t pkey = 0;
    VkShaderModule ps = PixelShaderFor(pk, pkey);
    if (!ps) return;

    laps.Lap(&perf_target_ms_);
    Target* rt = nullptr;
    Target* ds = nullptr;
    BindTargets(s, rt, ds);
    if (!pass_) return;

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
        k.zslope = -Bits(SimpleState(s, kMSlopeScale));

    VkPipeline pipe = VK_NULL_HANDLE;
    if (auto it = pipelines_.find(k); it != pipelines_.end()) {
        pipe = it->second;
    } else {
        if (!CreatePipeline(k, vs, ps, pipe)) return;
        pipelines_[k] = pipe;
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

    if (rt && !a.pretransformed) rt->scene_frame = frames_ + 1;
    // Distance blur only uses the main target's full-size, multisampled scene depth (as d3d12.cpp).
    if (scene_dof_ && ds && ds->samples > 1 && rt && !a.pretransformed &&
        s.rs[kRsZEnable] && SimpleState(s, kMZWriteEnable) && rt->gw == width_ && rt->gh == height_ &&
        ds->w == rt->w && ds->h == rt->h && s.viewport.w == width_ && s.viewport.h == height_) {
        const WRange r = DrawWRange(s, prog);
        if (r.near_w > 0 && r.far_w > r.near_w) scene_depth_[rt] = {ds, r.near_w, r.far_w};
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
        LOGE("vulkan: more than %zu draws in a frame", draw_sets_.size());
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
        ii[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
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

    laps.Lap(&perf_record_ms_);
    BeginRecord();
    if (pipe != bound_pipe_) {
        vkCmdBindPipeline(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipe);
        bound_pipe_ = pipe;
    }
    for (uint32_t i = 0; i < nb; i++) vkCmdBindVertexBuffers(cmd_, bound[i], 1, &vbufs[i], &offs[i]);
    if (idx_buffer) vkCmdBindIndexBuffer(cmd_, idx_buffer, idx_off, idx_type);
    vkCmdBindDescriptorSets(cmd_, VK_PIPELINE_BIND_POINT_GRAPHICS, pipeline_layout_, 0, 1, &set, 0, nullptr);
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
    if (!viewport_valid_ || std::memcmp(&vp, &bound_vp_, sizeof(vp)) != 0 ||
        std::memcmp(&sc, &bound_sc_, sizeof(sc)) != 0) {
        vkCmdSetViewport(cmd_, 0, 1, &vp);
        vkCmdSetScissor(cmd_, 0, 1, &sc);
        bound_vp_ = vp;
        bound_sc_ = sc;
        viewport_valid_ = true;
    }
    float blend_color[4];
    Color(SimpleState(s, kMBlendColor), blend_color);
    vkCmdSetBlendConstants(cmd_, blend_color);
    vkCmdSetStencilReference(cmd_, VK_STENCIL_FACE_FRONT_AND_BACK, SimpleState(s, kMStencilRef) & 0xFF);
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
    for (Target* t : {TargetAt(src_data, false, true), TargetAt(dst_data, false, true)})
        if (t && DirtyEpoch(t->data) == t->epoch) ReadTarget(t);
    if (!gpu::CopySurfaceMemory(src, dst, count, rects, points))
        reference::Copy(src, dst, count, rects, points);
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

bool VulkanBackend::EnsureSmaa(uint32_t w, uint32_t h, VkFormat format) {
    if (smaa_failed_) return false;
    if (smaa_output_.image && smaa_w_ == w && smaa_h_ == h && smaa_format_ == format) return true;
    vkDeviceWaitIdle(device_);
    DestroySmaaTargets();

    auto make_sampler = [&](VkFilter filter, VkSampler* out) {
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = filter;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        return vkCreateSampler(device_, &si, nullptr, out) == VK_SUCCESS;
    };
    if (!smaa_linear_ && !make_sampler(VK_FILTER_LINEAR, &smaa_linear_)) return false;
    if (!smaa_point_ && !make_sampler(VK_FILTER_NEAREST, &smaa_point_)) return false;

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
        VkDescriptorPoolSize sizes[2] = {{VK_DESCRIPTOR_TYPE_SAMPLED_IMAGE, 9},
                                         {VK_DESCRIPTOR_TYPE_SAMPLER, 6}};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.maxSets = 3;
        pi.poolSizeCount = 2;
        pi.pPoolSizes = sizes;
        if (vkCreateDescriptorPool(device_, &pi, nullptr, &smaa_pool_) != VK_SUCCESS) return false;
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = smaa_pool_;
        ai.descriptorSetCount = 3;
        VkDescriptorSetLayout layouts[3] = {smaa_set_layout_, smaa_set_layout_, smaa_set_layout_};
        ai.pSetLayouts = layouts;
        if (vkAllocateDescriptorSets(device_, &ai, smaa_sets_) != VK_SUCCESS) return false;
        VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        pl.setLayoutCount = 1;
        pl.pSetLayouts = &smaa_set_layout_;
        VkPushConstantRange push{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(float) * 8};
        pl.pushConstantRangeCount = 1;
        pl.pPushConstantRanges = &push;
        if (vkCreatePipelineLayout(device_, &pl, nullptr, &smaa_pipeline_layout_) != VK_SUCCESS) return false;
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
            if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &smaa_pipelines_[i]) !=
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
    // a previous resize may have failed: retry before acquiring from a null swapchain
    if (!swapchain_ && !CreateSwapchain()) return;
    const float want_scale = app::RenderScale(height_);
    const float want_stretch = app::RenderStretch(width_, height_);
    if (std::fabs(want_scale - scale_) > 1e-4f || std::fabs(want_stretch - stretch_) > 1e-4f) {
        vkDeviceWaitIdle(device_);
        DestroyTargets();
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
        DestroyTargets();
        msaa_ = want_msaa;
        bound_pipe_ = VK_NULL_HANDLE;
        viewport_valid_ = false;
        LOGR("linux: MSAA %ux", msaa_);
    }
    if (app::Snapshot().vsync != vsync_) {
        vkDeviceWaitIdle(device_);
        if (!CreateSwapchain()) return;
    }
    const app::Settings snapshot = app::Snapshot();
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
        EnsureTraceBuffer(VkDeviceSize(extent_.width) * extent_.height * 4);
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
        if (vkCreateGraphicsPipelines(device_, VK_NULL_HANDLE, 1, &info, nullptr, &present_pipeline_) != VK_SUCCESS)
            return;
        present_format_ = swapchain_format_;
    }

    RenderUi();

    vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
    uint32_t image = 0;
    VkResult r = vkAcquireNextImageKHR(device_, swapchain_, UINT64_MAX, image_available_, VK_NULL_HANDLE, &image);
    if (r == VK_ERROR_OUT_OF_DATE_KHR) {
        vkDeviceWaitIdle(device_);
        if (!CreateSwapchain()) LOGE("vulkan: could not rebuild the swapchain");
        return;  // the next frame retries and rebuilds the present pipeline
    }
    if (r != VK_SUCCESS && r != VK_SUBOPTIMAL_KHR) {
        LOGE("vulkan: acquire failed: %s", VkError(r));
        return;
    }

    vkResetFences(device_, 1, &fence_);
    vkResetCommandBuffer(cmd_, 0);
    VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd_, &begin);
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

    // PC distance blur: the main target's resolved depth, only available with MSAA (as d3d12.cpp)
    const SceneDepth* dof = nullptr;
    if (snapshot.dof && direct && ft && msaa_ > 1) {
        auto it = scene_depth_.find(ft);
        if (it != scene_depth_.end() && it->second.depth && it->second.depth->samples > 1 &&
            it->second.depth->resolved_image)
            dof = &it->second;
    }
    VkImageView depth_view = white_view_;
    if (dof) {
        depth_view = dof->depth->sample_view;
        Transition(dof->depth->resolved_image, dof->depth->resolved_layout,
                   VK_IMAGE_LAYOUT_DEPTH_STENCIL_READ_ONLY_OPTIMAL, dof->depth->format);
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
    VkWriteDescriptorSet writes[5]{};
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
    vkUpdateDescriptorSets(device_, 5, writes, 0, nullptr);

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
    const uint32_t bloom_w = ft ? ft->w : sw, bloom_h = ft ? ft->h : sh;
    const bool dof_on = dof != nullptr;
    float present_pc[8] = {snapshot.pc_contrast ? 1.0f : 0.0f, bloom_on ? 1.0f : 0.0f,
                           dof_on ? 1.0f : 0.0f, 0.0f,
                           bloom_w ? 1.0f / float(bloom_w) : 0.0f,
                           bloom_h ? 1.0f / float(bloom_h) : 0.0f,
                           dof ? dof->near_w : 0.1f, dof ? dof->far_w : 1000.0f};
    vkCmdPushConstants(cmd_, present_pipeline_layout_, VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(present_pc),
                       present_pc);
    vkCmdDraw(cmd_, 3, 1, 0, 0);
    if (ui_initialized_ && ImGui::GetDrawData() && ImGui::GetDrawData()->CmdListsCount) {
        std::lock_guard<std::recursive_mutex> guard(app::UiLock());
        ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), cmd_);
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
    if (image >= render_done_.size()) return;
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
        LOGE("vulkan: present submit failed: %s", VkError(r));
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
    } else if (r != VK_SUCCESS) {
        LOGE("vulkan: present failed: %s", VkError(r));
    }

    // the next frame may reuse the upload ring and descriptor pool only now
    present_lap.Stop();
    {
        const auto wait_at = perf_ ? std::chrono::steady_clock::now() : std::chrono::steady_clock::time_point{};
        vkWaitForFences(device_, 1, &fence_, VK_TRUE, UINT64_MAX);
        if (perf_)
            perf_wait_ms_ +=
                std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - wait_at).count();
    }
    if (record_swap_trace) {
        const std::string path = trace_base + "_present.tga";
        if (WriteSwapTga(path.c_str(), trace_ptr_, extent_.width, extent_.height, swapchain_format_))
            LOGR("linux: trace present %s saved (%ux%u)", path.c_str(), extent_.width, extent_.height);
    }
    frame_end.finished = true;
    ResetFrame();
    ++frames_;
    presents_seen_.store(frames_);
    if (frames_ % 60 == 0) {
        if (perf_) {
            const auto now = std::chrono::steady_clock::now();
            const double ms =
                std::chrono::duration<double, std::milli>(now - perf_log_at_).count() / 60.0;
            LOGI("renderer: frame %llu, %.2f ms (%.1f fps), draw %.2f [asm %.2f vert %.2f tex %.2f pix %.2f tgt "
                 "%.2f pipe %.2f const %.2f geo %.2f rec %.2f], ui %.2f, wait %.2f, sync %.2f, copy %.2f, clear "
                 "%.2f, present %.2f, %llu draws, %llu textures, %llu compiles, direct %llu stale %llu readback "
                 "%llu, binds %llu, passes %llu, fbs %llu, new targets %llu, target uploads %llu (vulkan)",
                 frames_, ms, ms > 0 ? 1000.0 / ms : 0.0, perf_draw_ms_ / 60.0, perf_assemble_ms_ / 60.0,
                 perf_vertex_ms_ / 60.0, perf_tex_ms_ / 60.0, perf_pixel_ms_ / 60.0, perf_target_ms_ / 60.0,
                 perf_pipe_ms_ / 60.0, perf_const_ms_ / 60.0, perf_geom_ms_ / 60.0, perf_record_ms_ / 60.0,
                 perf_ui_ms_ / 60.0, perf_wait_ms_ / 60.0, perf_sync_ms_ / 60.0, perf_copy_ms_ / 60.0,
                 perf_clear_ms_ / 60.0, perf_present_ms_ / 60.0, draws_, tex_uploads_, shader_compiles_, perf_direct_,
                 perf_stale_, perf_readback_, perf_bind_, perf_pass_new_, perf_fb_new_, perf_target_new_,
                 perf_target_upload_);
            perf_log_at_ = now;
            perf_draw_ms_ = perf_ui_ms_ = perf_wait_ms_ = 0.0;
            perf_assemble_ms_ = perf_vertex_ms_ = perf_tex_ms_ = perf_pixel_ms_ = 0.0;
            perf_target_ms_ = perf_pipe_ms_ = perf_const_ms_ = perf_geom_ms_ = perf_record_ms_ = 0.0;
            perf_sync_ms_ = perf_copy_ms_ = perf_clear_ms_ = perf_present_ms_ = 0.0;
            perf_direct_ = perf_stale_ = perf_readback_ = 0;
            perf_bind_ = perf_pass_new_ = perf_fb_new_ = perf_target_new_ = perf_target_upload_ = 0;
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




