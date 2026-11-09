// The title's Direct3D 8 calls drawn with Direct3D 12
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d12.h>
#include <d3d12sdklayers.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <dxgi1_6.h>
#include <wrl/client.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <optional>
#include <unordered_map>
#include <unordered_set>
#include <utility>

#include <imgui.h>
#include <backends/imgui_impl_dx12.h>
#include <backends/imgui_impl_win32.h>

#include "app/app.h"
#include "common/graphics_menu.h"
#include "gpu/prompt_glyphs.h"
// SMAA 1x: the reference shader and its lookup textures (third_party/smaa, MIT)
#include "../../third_party/smaa/AreaTex.h"
#include "../../third_party/smaa/SMAAShader.h"
#include "../../third_party/smaa/SearchTex.h"
#include "core/diag.h"
#include "core/input.h"
#include "core/log.h"
#include "gpu/ao.h"
#include "gpu/backend.h"
#include "gpu/ext.h"
#include "gpu/shaders.h"
#include "gpu/texture.h"
#include "gpu/texturedumper.h"
#include "gpu/vertex.h"
#include "kernel/kernel.h"

using Microsoft::WRL::ComPtr;

namespace gpu {

namespace {

constexpr int kFrames = 3;
constexpr uint64_t kStaleFrames = 8;  // frames a target may go unused and still be read back
// Small targets are cheap and often cached UI slots. The HUD samples its icon
// target only when that HUD layer redraws, so the readback window is too short
// to keep it across weapon/menu changes: it would be swept and the game would
// fall back to the static FS texture sitting in guest memory. Keep them longer;
// large scene targets still use the short window.
constexpr uint64_t kSmallTargetFrames = 3600;
constexpr uint64_t kSmallTargetPixels = 128 * 128;
constexpr uint64_t kUploadSize = 64ull << 20;
// a draw starting with less than this left in the frame's ring waits for the GPU and starts the ring over
constexpr uint64_t kUploadReserve = 8ull << 20;
constexpr uint32_t kSrvCount = 3 * 32768;
constexpr uint32_t kSamplerCount = 2048;
constexpr uint32_t kRtvCount = 1024;
constexpr uint32_t kDsvCount = 256;
constexpr uint32_t kSmaaRtvs = 3;  // after the back buffers' views: SMAA edges, weights, output
constexpr uint32_t kAoRtvs = 3;    // after SMAA: raw AO, full-size occlusion, prepass view distance
constexpr uint32_t kAoRtv = kRtvCount + kFrames + kSmaaRtvs;
constexpr uint32_t kAoOccRtv = kAoRtv + 1;
constexpr uint32_t kAoDistRtv = kAoRtv + 2;
// The AO prepass writes each pixel's view distance (clip w) here, so AO never has to turn
// hyperbolic depth back into distance with a guessed near/far: most draws are vertex programs
// whose projection the port cannot see, and a wrong guess bent flat surfaces into stripes.
constexpr DXGI_FORMAT kAoDistFormat = DXGI_FORMAT_R32_FLOAT;
constexpr DXGI_FORMAT kAoFormat = DXGI_FORMAT_R8_UNORM;
constexpr DXGI_FORMAT kBgra = DXGI_FORMAT_B8G8R8A8_UNORM;
constexpr uint32_t kQueries = 8192;  // occlusion queries in flight, several frames of counted draws
constexpr uint32_t kNoQuery = ~0u;

// perf diagnostic: where a frame's time goes, logged once a second with diag::Perf
using PerfClock = std::chrono::steady_clock;
double PerfMs(PerfClock::time_point t) {
    return std::chrono::duration<double, std::milli>(PerfClock::now() - t).count();
}
struct Perf {
    // milliseconds
    double draw = 0;
    double assemble = 0;
    double bind = 0;
    double sig = 0;
    double texup = 0;
    double compile = 0;
    double pipeline = 0;
    double clear = 0;
    double copy = 0;
    double present = 0;
    double wait = 0;
    double swap = 0;
    double sync = 0;
    // counts and bytes
    uint64_t draws = 0;
    uint64_t verts = 0;
    uint64_t upload = 0;
    uint64_t overflow = 0;
    uint64_t sigs = 0;
    uint64_t sig_bytes = 0;
    uint64_t texups = 0;
    uint64_t texup_bytes = 0;
    uint64_t newtex = 0;
    uint64_t targets = 0;
    uint64_t rtups = 0;
    uint64_t shrinks = 0;
    uint64_t syncs = 0;
    uint64_t psos = 0;
    uint64_t shaders = 0;
    uint64_t shader_hits = 0;
    uint64_t raw = 0;
    uint64_t lates = 0;
};
Perf g_perf;
struct PerfScope {
    double& acc;
    bool on = diag::Perf();
    PerfClock::time_point t = on ? PerfClock::now() : PerfClock::time_point{};
    ~PerfScope() {
        if (on) acc += PerfMs(t);
    }
};

// F3 captures only with diag::CaptureDir set
bool CaptureOn() {
    if (diag::CaptureDir().empty()) LOG_FIRST(1, "capture: set XBE_CAPTURE_DIR");
    return !diag::CaptureDir().empty();
}

const char* kBlitHlsl = R"(
cbuffer B : register(b1) { float4 uvrect; float4 opts; float4 post; float4 src; };
Texture2D img : register(t0);
Texture2D lut : register(t1);
Texture2DMS<float> depthTex : register(t3);
SamplerState ls : register(s0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };

float3 ToGamma(float3 c) {
    if (opts.x <= 0) return c;
    return float3(lut.Sample(ps, float2(c.r, 0.5)).r,
                  lut.Sample(ps, float2(c.g, 0.5)).g,
                  lut.Sample(ps, float2(c.b, 0.5)).b);
}

// A single-pass approximation of the PC release's bright-pass bloom. Each tap first averages a
// small box, so thin chrome/specular edges are diluted before the threshold while lamps and neon
// still pass.
float3 BrightAt(float2 uv) {
    float2 q = src.xy * 0.5;
    float3 c = (img.SampleLevel(ls, uv + float2(-q.x, -q.y), 0).rgb +
                img.SampleLevel(ls, uv + float2( q.x, -q.y), 0).rgb +
                img.SampleLevel(ls, uv + float2(-q.x,  q.y), 0).rgb +
                img.SampleLevel(ls, uv + float2( q.x,  q.y), 0).rgb) * 0.25;
    c = ToGamma(c);
    return max(c - 0.94, 0.0);
}

float3 Bloom(float2 uv) {
    static const float2 dirs[8] = {
        float2(1, 0), float2(-1, 0), float2(0, 1), float2(0, -1),
        float2(0.7071, 0.7071), float2(-0.7071, 0.7071),
        float2(0.7071, -0.7071), float2(-0.7071, -0.7071)
    };
    float2 t = src.xy * 8.0;
    float3 b = 0;
    [unroll] for (int k = 0; k < 8; k++) b += BrightAt(uv + dirs[k] * t);
    b += BrightAt(uv);
    return b * (1.0 / 9.0);
}

// PC distance blur: 13-tap hex blur weighted by the linearised scene depth. The scene depth is
// multisampled; sample 0 and undo the reversed depth before the PC linearisation.
float3 DistanceBlur(float2 uv, float3 color) {
    static const float2 hex[13] = {
        float2(0, 0), float2(0.5, 0.8660), float2(-0.5, 0.8660), float2(-1, 0),
        float2(-0.5, -0.8660), float2(0.5, -0.8660), float2(1, 0),
        float2(1, 1.7320), float2(-1, 1.7320), float2(-2, 0),
        float2(-1, -1.7320), float2(1, -1.7320), float2(2, 0)
    };
    uint w, h, samples;
    depthTex.GetDimensions(w, h, samples);
    uint2 p = uint2(saturate(uv) * float2(max(w, 1) - 1, max(h, 1) - 1));
    float raw = 1.0 - depthTex.Load(p, 0);
    float world = src.z * src.w / max(src.w - raw * (src.w - src.z), 1e-4);
    float coef = saturate((world - 40.0) / (65.0 - 40.0));
    if (coef <= 0.0) return color;
    float3 sum = 0;
    float count = 0;
    [unroll] for (int i = 0; i < 13; i++) {
        float wgt = exp(-dot(hex[i], hex[i]) * 0.5);
        sum += img.SampleLevel(ls, uv + hex[i] * src.xy * coef, 0).rgb * wgt;
        count += wgt;
    }
    return sum / count;
}

// Match the PC release's tone on the finished frame.
float3 PcContrast(float3 c) {
    const float3 L = float3(0.2126, 0.7152, 0.0722);
    float l = dot(c, L);
    const float pivot = 0.114;
    float l2 = pivot + (l - pivot) * 1.35;
    // After the Xbox gamma ramp the intro steps are already close to black. Lift that range while
    // anchoring black and the original contrast pivot. A square root has unbounded gain near
    // black and turns dark reflections into visible streaks; this toe caps the lift at about 2x.
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

O vs(uint id : SV_VertexID) {
    O o;
    float2 t = float2((id << 1) & 2, id & 2);
    o.p = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
    o.uv = uvrect.xy + t * uvrect.zw;
    return o;
}
float4 psmain(O i) : SV_Target {
    float4 c = 0;
    int n = max((int)opts.z, 1);
    float2 px = fwidth(i.uv);
    for (int y = 0; y < n; y++)
        for (int x = 0; x < n; x++) c += img.SampleLevel(ls, i.uv + px * ((float2(x, y) + 0.5) / n - 0.5), 0);
    c /= n * n;
    c.rgb *= opts.w;
    if (post.z > 0) c.rgb = DistanceBlur(i.uv, c.rgb);
    c.rgb = ToGamma(c.rgb);
    if (post.y > 0) c.rgb += Bloom(i.uv) * 1.2;
    if (post.x > 0) {
        c.rgb = lerp(c.rgb, PcContrast(c.rgb), post.x);
        // The contrast curve stretches the darkest 8-bit steps; an ordered dither breaks them up
        // before the back buffer quantises the result again.
        static const float kDither[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};
        int2 dp = int2(i.p.xy) % 4;
        float d = (kDither[dp.y * 4 + dp.x] + 0.5) / 16.0 - 0.5;
        c.rgb = saturate(c.rgb + d / 255.0);
    }
    return float4(c.rgb, opts.y > 0 ? c.a : 1);
}
)";

// Depth is reversed in a 32 bit float buffer: 1 at the near clip, 0 at the far one, where a float keeps its finest
// steps; a 24 bit buffer storing z / w lost the steps that told distant surfaces apart
constexpr DXGI_FORMAT kDepthTypeless = DXGI_FORMAT_R32G8X24_TYPELESS;
constexpr DXGI_FORMAT kDepthFormat = DXGI_FORMAT_D32_FLOAT_S8X24_UINT;
constexpr DXGI_FORMAT kDepthRead = DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
constexpr float kDepthFar = 0;

const char* kDepthHlsl = R"(
Texture2D<float> img : register(t0);
SamplerState ps : register(s1);
struct O { float4 p : SV_Position; float2 uv : TEXCOORD0; };
O vs(uint id : SV_VertexID) {
    O o;
    o.uv = float2((id << 1) & 2, id & 2);
    o.p = float4(o.uv * float2(2, -2) + float2(-1, 1), 0, 1);
    return o;
}
float psmain(O i) : SV_Depth { return img.SampleLevel(ps, i.uv, 0); }
)";

const char* kClearHlsl = R"(
cbuffer B : register(b1) { float4 colour; };
float4 vs(uint id : SV_VertexID) : SV_Position {
    float2 t = float2((id << 1) & 2, id & 2);
    return float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
}
float4 psmain() : SV_Target { return colour; }
)";

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

// D3DBLEND, the console uses GL values
constexpr uint32_t kBlendDestColor = 0x306;  // D3DBLEND_DESTCOLOR

D3D12_BLEND Blend(uint32_t v, bool alpha) {
    switch (v) {
        case 0: return D3D12_BLEND_ZERO;
        case 1: return D3D12_BLEND_ONE;
        case 0x300: return alpha ? D3D12_BLEND_SRC_ALPHA : D3D12_BLEND_SRC_COLOR;
        case 0x301: return alpha ? D3D12_BLEND_INV_SRC_ALPHA : D3D12_BLEND_INV_SRC_COLOR;
        case 0x302: return D3D12_BLEND_SRC_ALPHA;
        case 0x303: return D3D12_BLEND_INV_SRC_ALPHA;
        case 0x304: return D3D12_BLEND_DEST_ALPHA;
        case 0x305: return D3D12_BLEND_INV_DEST_ALPHA;
        case kBlendDestColor: return alpha ? D3D12_BLEND_DEST_ALPHA : D3D12_BLEND_DEST_COLOR;
        case 0x307: return alpha ? D3D12_BLEND_INV_DEST_ALPHA : D3D12_BLEND_INV_DEST_COLOR;
        case 0x308: return D3D12_BLEND_SRC_ALPHA_SAT;
        case 0x8001: case 0x8003: return D3D12_BLEND_BLEND_FACTOR;
        case 0x8002: case 0x8004: return D3D12_BLEND_INV_BLEND_FACTOR;
        default: return D3D12_BLEND_ONE;
    }
}

// D3DBLENDOP, GL values
D3D12_BLEND_OP BlendOp(uint32_t v) {
    switch (v) {
        case 0x800A: return D3D12_BLEND_OP_SUBTRACT;
        case 0x800B: return D3D12_BLEND_OP_REV_SUBTRACT;
        case 0x8007: return D3D12_BLEND_OP_MIN;
        case 0x8008: return D3D12_BLEND_OP_MAX;
        default: return D3D12_BLEND_OP_ADD;
    }
}

// D3DCMP_NEVER 0x200 to D3DCMP_ALWAYS 0x207, D3D12's order from 1
D3D12_COMPARISON_FUNC Cmp(uint32_t v) {
    uint32_t n = v >= 0x200 && v <= 0x207 ? v - 0x200 : 7;
    return D3D12_COMPARISON_FUNC(n + 1);
}

// D3DSTENCILOP, GL values
D3D12_STENCIL_OP StencilOp(uint32_t v) {
    switch (v) {
        case 0: return D3D12_STENCIL_OP_ZERO;
        case 0x1E01: return D3D12_STENCIL_OP_REPLACE;
        case 0x1E02: return D3D12_STENCIL_OP_INCR_SAT;
        case 0x1E03: return D3D12_STENCIL_OP_DECR_SAT;
        case 0x150A: return D3D12_STENCIL_OP_INVERT;
        case 0x8507: return D3D12_STENCIL_OP_INCR;
        case 0x8508: return D3D12_STENCIL_OP_DECR;
        default: return D3D12_STENCIL_OP_KEEP;
    }
}

// D3DTADDRESS: 1 wrap, 2 mirror, 3 clamp, 4 border, 5 clamp to edge
D3D12_TEXTURE_ADDRESS_MODE Address(uint32_t v) {
    switch (v) {
        case 2: return D3D12_TEXTURE_ADDRESS_MODE_MIRROR;
        case 3: case 5: return D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        case 4: return D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        default: return D3D12_TEXTURE_ADDRESS_MODE_WRAP;
    }
}

DXGI_FORMAT HostFormat(Host h) {
    switch (h) {
        case Host::BC1: return DXGI_FORMAT_BC1_UNORM;
        case Host::BC2: return DXGI_FORMAT_BC2_UNORM;
        case Host::BC3: return DXGI_FORMAT_BC3_UNORM;
        case Host::R32F: return DXGI_FORMAT_R32_FLOAT;
        default: return DXGI_FORMAT_B8G8R8A8_UNORM;
    }
}

constexpr uint32_t kFmtD16 = 0x2C;     // D3DFMT_D16
constexpr uint32_t kFmtLinD16 = 0x30;  // D3DFMT_LIN_D16

// TexInfo::shadow of a depth texture
uint8_t ShadowDepthKind(uint32_t format) { return format == kFmtD16 || format == kFmtLinD16 ? 2 : 1; }

// bytes per row of a 32 bit surface in guest memory, rounded to 64
uint32_t GuestPitch(uint32_t width) { return (width * 4 + 63) & ~63u; }

// rows of a texture upload start D3D12_TEXTURE_DATA_PITCH_ALIGNMENT apart
uint32_t UploadPitch(uint32_t row_bytes) { return (row_bytes + 255) & ~255u; }

D3D12_RESOURCE_DESC BufferDesc(uint64_t bytes) {
    D3D12_RESOURCE_DESC bd{};
    bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    bd.Width = bytes;
    bd.Height = 1;
    bd.DepthOrArraySize = 1;
    bd.MipLevels = 1;
    bd.SampleDesc.Count = 1;
    bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    return bd;
}

// one level, one sample: callers set Flags where they differ
D3D12_RESOURCE_DESC Texture2DDesc(DXGI_FORMAT fmt, uint64_t w, uint32_t h) {
    D3D12_RESOURCE_DESC td{};
    td.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = 1;
    td.MipLevels = 1;
    td.Format = fmt;
    td.SampleDesc.Count = 1;
    return td;
}

// MemSig hashes a surface up to kSigWholeMax bytes whole, a bigger one as evenly spaced samples
constexpr uint32_t kSigWholeMax = 65536;
constexpr uint32_t kSigSamples = 64;
constexpr uint32_t kSigSampleBytes = 64;

uint64_t MemSig(uint32_t data, uint64_t total) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(data));
    uint64_t sig = Hash(&total, sizeof(total));
    g_perf.sigs++;
    g_perf.sig_bytes += total <= kSigWholeMax ? total : uint64_t(kSigSamples) * kSigSampleBytes;
    if (total <= kSigWholeMax) return Hash(p, size_t(total), sig);
    for (uint32_t i = 0; i < kSigSamples; i++) {
        const uint8_t* sample = p + (total - kSigSampleBytes) * i / (kSigSamples - 1);
        sig = Hash(sample, kSigSampleBytes, sig);
    }
    return sig;
}

// MemSig samples a big surface and misses small writes (XFONT text): after a Sync every byte is compared
bool Changed(const Target& t) {
    if (t.drawn || t.read.empty()) return false;
    uint32_t row = t.gw * 4;
    uint32_t pitch = GuestPitch(t.gw);
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        if (std::memcmp(p + size_t(y) * pitch, &t.read[size_t(y) * row], row)) return true;
    }
    return false;
}

// a lock alone proves nothing, reads lock too: only changed bytes mean the CPU wrote it
bool LatchCpuWrite(Target& t) {
    uint64_t e = DirtyEpoch(t.data);
    if (e <= t.epoch) return false;
    t.epoch = e;
    uint64_t sig = MemSig(t.data, t.bytes);
    if (sig == t.sig && !Changed(t)) return false;
    t.sig = sig;
    return true;
}

struct Tex {
    input::PromptStyle prompt_style = input::PromptStyle::Original;
    ComPtr<ID3D12Resource> res;
    TextureDesc d;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    uint32_t levels = 1;
    uint64_t sig = 0;
    uint64_t frame = 0;
    uint64_t palette = 0;  // hash of the colours a P8 texture was decoded with
    uint8_t flags = 0;  // Extension::Decoded's
    const Tex* swap = nullptr;  // the texture pack's image drawn in its place
    uint64_t pack_key = 0;      // its pack image is still loading
    uint32_t pack_gen = 0;      // texturedumper::Generation the key was taken at, 0 after new bytes
};

struct PsoKey {
    uint64_t vs;
    uint64_t ps;
    uint32_t regs;
    uint32_t topo;
    uint32_t has_rt;
    uint32_t has_ds;
    uint32_t samples;
    uint32_t elements[16];  // per register: DXGI format, buffer << 8, offset << 16
    uint32_t blend;
    uint32_t src;
    uint32_t dst;
    uint32_t op;
    uint32_t wmask;
    uint32_t zen;
    uint32_t zwrite;
    uint32_t zfunc;
    uint32_t sten;
    uint32_t sfunc;
    uint32_t smask;
    uint32_t swmask;
    uint32_t sfail;
    uint32_t szfail;
    uint32_t spass;
    uint32_t cull;
    uint32_t ccw;
    uint32_t fill;
    int32_t zbias;
    float zslope;
    uint32_t rtfmt;
    uint32_t sprite;  // DrawShaders::sprite
};

// Blit's settings past the source and its rectangle
struct BlitOptions {
    bool gamma = false;       // through the gamma lookup
    bool alpha = false;       // the source's alpha, else 1
    uint32_t taps = 1;        // n x n box filter per pixel
    float mul = 1;            // colour scale
    DXGI_FORMAT rtv = kBgra;  // the target's format
    bool over = false;        // premultiplied over the target, which keeps its alpha
    uint32_t samples = 1;     // destination sample count
    float post = 0;           // PC contrast strength, 0 off
    float bloom = 0;          // bright-pass bloom strength, 0 off
    float src_w = 0, src_h = 0;  // source size in texels, for the bloom radius
    ID3D12Resource* depth = nullptr;  // scene depth for the distance blur
    float dof = 0;            // distance blur strength, 0 off
    float near_z = 0, far_z = 0;  // camera planes for the depth linearisation
};

// Shrink's settings past the source and the size
struct ShrinkOptions {
    uint32_t taps = 1;
    // into: a copy from an earlier call to draw over again, in state into_state
    ID3D12Resource* into = nullptr;
    D3D12_RESOURCE_STATES into_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    float mul = 1;
};

// a draw's four texture stages: BindStageTexturesAndTargets fills them, the constants, views and samplers read them
struct DrawTextures {
    std::optional<TextureDesc> desc[4];
    TexInfo info[4];
    D3D12_SHADER_RESOURCE_VIEW_DESC sv[4];
    ID3D12Resource* res[4];
    bool rt[4] = {};            // stage samples a render target
    float k[4] = {1, 1, 1, 1};  // Extension::Sample's k
    float wide[4] = {};         // host texels per console texel of a target read into a smaller one, 0 otherwise
};

// what the last draw set on the command list, so the next one sets only what differs; anything else that sets them
// clears it
struct DrawBinds {
    ID3D12PipelineState* pso = nullptr;
    D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
    bool rasterizer = false;  // viewport and scissor hold
    D3D12_VIEWPORT viewport{};
    D3D12_RECT scissor{};
    bool output = false;  // blend and stencil hold
    float blend[4] = {};
    uint32_t stencil = 0;
    D3D12_GPU_VIRTUAL_ADDRESS vsc = 0;  // root constant buffers
    D3D12_GPU_VIRTUAL_ADDRESS dc = 0;
    uint32_t views = UINT32_MAX;  // root descriptor table slots
    uint32_t samplers = UINT32_MAX;
};

// the last targets EnsureTarget and FindTarget marked used, so target_used_ is written once a frame each
struct UsedTarget {
    uint32_t data = 0;
    uint64_t frame = UINT64_MAX;
};

// a draw's shaders: what they are made from, their cache keys, the hashes Compile returned and the compiled blobs
struct DrawShaders {
    bool prog = false;  // a vertex program, else the fixed function pipeline
    bool quad = false;
    bool fixed_transform = false;  // fixed function on untransformed vertices: the shader reads matrices and lights
    bool sprite = false;           // a program's points drawn as point sprites (SpriteHlsl)
    FixedKey fk;
    uint64_t vkey = 0;
    uint64_t vh = 0;
    uint64_t pkey = 0;
    uint64_t ph = 0;
    ID3DBlob* vs_code = nullptr;
    ID3DBlob* ps_code = nullptr;
};

// the render target's console size in pixels, or the depth buffer's without one
struct GuestExtent {
    uint32_t w;
    uint32_t h;
};

class D3D12Backend final : public Backend, public Context {
public:
    bool Init(uint32_t width, uint32_t height) override;
    void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override;
    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override;
    void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects, uint32_t dst_points) override;
    void Present(const Surface& front) override;
    void WaitGpuFrame(uint64_t n) override;
    void Sync(uint32_t data, uint32_t size) override;
    void SyncLate(uint32_t data) override;
    bool DeliverLate(uint32_t data) override;
    void RecordLate(uint32_t data) override;
    void Flush() override;
    void SetGamma(const uint8_t* ramp) override;
    void ClearReport() override { counted_.clear(); }
    // Delivered by the next Submit, as before the Linux-port integration: the title presents
    // every frame, so the report still arrives in time. Flushing here adds a mid-frame GPU wait
    // that this backend does not need.
    void GetReport(uint32_t report) override { reports_.push_back({report, counted_, fence_value_ + 1}); }

    ID3D12Device* Device() override { return dev_.Get(); }
    ID3D12GraphicsCommandList* List() override { return cl_; }
    void Release(ComPtr<ID3D12Resource> r) override { CurrentFrame().garbage.push_back(std::move(r)); }
    D3D12_CPU_DESCRIPTOR_HANDLE View(const Target& t) override { return Cpu(rtv_heap_.Get(), t.view, rtv_inc_); }
    void Unbind() override { bound_rt_ = bound_ds_ = nullptr; }
    void Capture(Target& t, const char* tag, float mul) override;
    float Scale() const override { return scale_; }
    void GuestSize(uint32_t& w, uint32_t& h) const override {
        w = guest_w_;
        h = guest_h_;
    }
    uint64_t Presents() const override { return presents_; }

private:
    // One deferred draw recorded into a bundle. Bundles cannot set render targets, viewports,
    // scissors or resource barriers, so those are recorded on the direct list and replayed
    // immediately before the bundle.
    struct BundleDesc {
        ComPtr<ID3D12GraphicsCommandList> list;
        D3D12_CPU_DESCRIPTOR_HANDLE rtv{};
        D3D12_CPU_DESCRIPTOR_HANDLE dsv{};
        bool has_rt = false;
        bool has_ds = false;
        D3D12_VIEWPORT viewport{};
        D3D12_RECT scissor{};
        bool has_viewport = false;
        bool has_scissor = false;
        std::array<ID3D12Resource*, 4> reads{};
        ID3D12Resource* write = nullptr;
        ID3D12Resource* depth_write = nullptr;
        bool drew = false;
        bool opaque = false;  // an AO receiver: also recorded depth-only into the AO depth prepass
        ComPtr<ID3D12GraphicsCommandList> depth_list;  // that prepass bundle, null when none
    };
    // What a deferred AO receiver bound, so EndDeferredBundle can record its depth-only copy
    // once the main bundle is closed (bundles share one allocator, one open list at a time).
    struct DepthReplay {
        ID3D12PipelineState* pso = nullptr;
        D3D12_GPU_VIRTUAL_ADDRESS vsc = 0, dc = 0;
        uint32_t views = 0, samplers = 0;
        float blend[4] = {};
        uint32_t stencil = 0;
        D3D_PRIMITIVE_TOPOLOGY topology = D3D_PRIMITIVE_TOPOLOGY_UNDEFINED;
        D3D12_VERTEX_BUFFER_VIEW vbv[16] = {};
        uint32_t vb_count = 0;
        D3D12_INDEX_BUFFER_VIEW ibv{};
        uint32_t index_count = 0, draw_count = 0;
        int32_t base_vertex = 0;
        bool drew = false;
    };
    struct Frame {
        ComPtr<ID3D12CommandAllocator> alloc;
        ComPtr<ID3D12Resource> upload;
        uint8_t* map = nullptr;
        uint64_t used = 0;
        uint64_t fence = 0;
        std::vector<ComPtr<ID3D12Resource>> garbage;
        std::vector<uint32_t> rtv_garbage, dsv_garbage;  // view indices freed by this frame
        ComPtr<ID3D12Resource> readback;                // largest CPU read-back buffer used by this frame
        uint64_t readback_bytes = 0;
        uint32_t srv = 0;
        uint32_t smp = 0;
        // deferred scene display list: bundle lists share this frame's bundle allocator and
        // are only reset after the frame fence has completed
        ComPtr<ID3D12CommandAllocator> bundle_alloc;
        std::vector<ComPtr<ID3D12GraphicsCommandList>> bundle_pool;
        uint32_t bundle_next = 0;
        std::vector<BundleDesc> bundles;
    };
    // scratch RTVs borrowed by ResolveDepth/Shrink, never shared inside a frame
    static constexpr uint32_t kScratchViews = 8;
    uint32_t scratch_rtv_[kFrames][kScratchViews] = {};
    uint32_t scratch_used_[kFrames] = {};
    uint32_t ScratchRtv() {
        const uint32_t f = uint32_t(frame_ % kFrames);
        if (scratch_used_[f] < kScratchViews) return scratch_rtv_[f][scratch_used_[f]++];
        LOG_FIRST(4, "d3d12: scratch RTV pool exhausted in frame %llu", static_cast<unsigned long long>(frame_));
        return UINT32_MAX;
    }
    void RecycleFrameViews(Frame& f, uint32_t index) {
        for (uint32_t v : f.rtv_garbage) free_rtv_.push_back(v);
        for (uint32_t v : f.dsv_garbage) free_dsv_.push_back(v);
        f.rtv_garbage.clear();
        f.dsv_garbage.clear();
        scratch_used_[index] = 0;
    }
    struct Alloc {
        ID3D12Resource* res;
        uint64_t off;
        uint8_t* cpu;
        D3D12_GPU_VIRTUAL_ADDRESS gpu;
    };

    Alloc Upload(uint64_t size, uint64_t align = 256);
    Frame& CurrentFrame() { return frames_[frame_ % kFrames]; }
    uint32_t AllocSrvSlots(uint32_t n);
    D3D12_CPU_DESCRIPTOR_HANDLE Cpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        auto c = h->GetCPUDescriptorHandleForHeapStart();
        c.ptr += size_t(i) * inc;
        return c;
    }
    D3D12_GPU_DESCRIPTOR_HANDLE Gpu(ID3D12DescriptorHeap* h, uint32_t i, uint32_t inc) {
        auto g = h->GetGPUDescriptorHandleForHeapStart();
        g.ptr += uint64_t(i) * inc;
        return g;
    }
    void Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) override;
    void Begin();
    void Submit(bool wait);
    bool WaitFence(uint64_t value);
    [[noreturn]] void DeviceFailure(HRESULT hr, const char* what);
    void SetFullViewport(uint32_t w, uint32_t h);
    void SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h);
    void Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], bool gamma, bool alpha, uint32_t taps = 1,
              float mul = 1, DXGI_FORMAT rtv = kBgra) override {
        Blit(src, fmt, uvrect, BlitOptions{.gamma = gamma, .alpha = alpha, .taps = taps, .mul = mul, .rtv = rtv});
    }
    void Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o);
    uint32_t EnsureBlitSamplers();
    void DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                        std::optional<uint32_t> sampler_slot = {});
    ID3D12Resource* Feedback(Target& t) override;
    void Resolve(Target& t) {
        if (t.range != 1 && ext_) ext_->Resolve(*this, t);
    }
    ID3D12Resource* ResolveColor(Target& t);
    ID3D12Resource* ResolveDepth(Target& t);
    // Pass, then its step
    void Pass(const char* hlsl, const char* entry, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE rh, uint32_t w,
              uint32_t h, ID3D12Resource* const src[3], const DXGI_FORMAT fmt[3], const float p[8],
              float blend) override;
    ID3D12PipelineState* EnsurePassPipeline(const char* hlsl, const char* entry, DXGI_FORMAT rtv, float blend);
    // Draw, in call order, each callee right after its caller
    bool DepthCopy(const State& s);
    ID3D12PipelineState* EnsureDepthCopyPipeline(uint32_t samples);
    bool DrawDepthCopy(Target& src, Target& dst, ID3D12PipelineState* pso);
    bool AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices,
                      Assembled& a);
    bool BindTargets(const State& s);
    void EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh);
    bool BindStageTexturesAndTargets(const State& s, DrawTextures& stages);
    ID3D12Resource* BindTexture(const State& s, int stage, DrawTextures& stages);
    ID3D12Resource* BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                      DrawTextures& stages);
    ID3D12Resource* BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                     DrawTextures& stages);
    bool CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key);
    void UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p, const uint32_t* colours,
                       bool same);
    void UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px, uint32_t pitch);
    void LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint32_t* colours);
    void LatchPackImage(Tex& tx);
    void CreatePackTexture(Tex& r, const texturedumper::Image& img);
    void ReleasePackTextures();
    void EnsurePixelShader(const PixelKey& pk, DrawShaders& sh);
    void BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k);
    void KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k);
    ID3D12PipelineState* EnsurePipeline(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps);
    HRESULT CreatePipeline(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc, ComPtr<ID3D12PipelineState>& pso);
    void FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps, D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const;
    void ExpandPoints(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, bool sprite);
    void ExpandLines(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d);
    float TargetScale() const;   // host pixels per console pixel of the bound target, along x
    float TargetScaleY() const;  // and along y: less than TargetScale on a frame wider than the console's
    float UiSqueeze(const State& s, const Assembled& a, const DrawTextures& stages) const;
    float RenderTargetShift(const State& s, const DrawShaders& sh) const;
    void FillDepthConstants(const State& s, const DrawShaders& sh, const PsoKey& k, DrawConstants& c) const;
    uint8_t* UploadVertexConstants(const State& s);
    bool UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                         GuestExtent size, DrawConstants& c);
    void FillTextureConstants(const State& s, const DrawShaders& sh, const DrawTextures& stages, DrawConstants& c);
    void FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c);
    void CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                     const DrawTextures& stages, uint64_t vkey);
    void CaptureTextures(const State& s, const DrawTextures& stages);
    void EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c);
    void CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c);
    void CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk);
    void CaptureShader(const char* kind, uint64_t key, const std::string& src);
    void SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages);
    uint32_t EnsureViewSet(const DrawTextures& stages);
    uint32_t EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp);
    void SetViewportScissor(const State& s, uint32_t gw, uint32_t gh, float squeeze);
    void SetBlendFactorStencilRef(const State& s);
    void UpdateTargetUsed(uint32_t data);
    void DrawGeometry(const Assembled& a);
    // deferred scene display list, active only with TCNYC_DEFER_SCENE=1
    ID3D12GraphicsCommandList* Cmd() const { return bundle_ ? bundle_ : cl_; }
    bool DeferredPending() { return !CurrentFrame().bundles.empty(); }
    bool DeferredPendingResource(ID3D12Resource* r) const {
        return r && (defer_reads_.count(r) || defer_writes_.count(r));
    }
    bool BeginDeferredBundle(const DrawTextures& stages, bool opaque);
    void EndDeferredBundle();
    void FlushDeferredScene();
    bool DeferredBarrier(const char* why);
    void DeferredFallback(const char* why);
    void ReportDeferred();
    // ends the bundle on every return from Draw once one was started
    struct DeferredScope {
        D3D12Backend* backend;
        ~DeferredScope() {
            if (backend->bundle_) backend->EndDeferredBundle();
        }
    };
    bool UploadVertexBuffers(const Assembled& a);    // visibility tests
    bool EnsureQueries();
    uint32_t BeginCountedDraw(const State& s);
    void EndCountedDraw(uint32_t query);
    float SamplesPerPixel() const;
    void DeliverReports();
    // shaders by source hash: Compile returns the bytecode and sets hash
    ID3DBlob* Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash);
    // Blit's pipeline
    ID3D12PipelineState* EnsureBlitPipeline(DXGI_FORMAT rtv, bool over, uint32_t samples);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps, DXGI_FORMAT rtv) const;
    // Init, in order, each callee right after its caller
    bool CreateDevice();
    bool CreateSwapChain(HWND hwnd);
    bool CreateHeaps();
    uint32_t CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible, ComPtr<ID3D12DescriptorHeap>& out);
    bool CreateBackBufferViews();
    bool CreateRootSignature();
    bool CreateFrames();
    void CreateGammaLut();
    void LoadSettings();
    uint32_t SupportedMsaa(uint32_t samples);
    // Present, in call order, each callee right after its caller; DrawFrame runs the two after it
    void FinishCapture(const Surface& front);
    void StartCapture();
    bool ResizeSwap();
    void UploadGammaRamp();
    void DrawFrame(Target* t, uint32_t bi, ID3D12Resource* aa);
    void BlitFrame(Target& t, ID3D12Resource* aa);
    // SMAA 1x on the finished frame (app::Settings::smaa): the anti-aliased copy of t, null when off or failed
    ID3D12Resource* ApplySmaa(Target& t);
    bool EnsureSmaa(const Target& t);
    bool EnsureAoHalf(uint32_t w, uint32_t h);
    void RenderHalfAo(Target& t);
    ID3D12GraphicsCommandList* AcquireBundleList();
    bool EnsureAoDepth(const Target& like);
    bool EnsureAoOcclusion(uint32_t w, uint32_t h);
    bool RenderAoOcclusion(Target& rt, float near_z, float far_z);
    void BindAoView(ID3D12Resource* occlusion);
    void RenderUi();
    void ReleaseOldTextures();
    void ResetFrame();
    void CountFps();
    void ApplySettings();
    ComPtr<ID3D12Resource> Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt, uint32_t w,
                                  uint32_t h, const ShrinkOptions& o = {});
    bool ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out);
    void CapStep(const Surface& rt);
    // a resource CapSave shrinks to w x h and writes, in state *state, colour times mul
    struct CapImage {
        ID3D12Resource* res;
        D3D12_RESOURCE_STATES* state;
        DXGI_FORMAT fmt;
        uint32_t w;
        uint32_t h;
        float mul = 1;
    };
    void CapSave(const CapImage& img, const char* name);
    void CapSave(Target& t, const char* name);
    // a surface's place in targets_: its key, console size and entry, end when it has none
    struct TargetSlot {
        uint32_t data;
        uint64_t key;
        uint32_t gw;
        uint32_t gh;
        std::unordered_map<uint64_t, Target>::iterator it;
        bool sized;  // the entry is there at the surface's size
    };
    Target* FindTarget(const Surface& s, bool depth);
    Target* EnsureTarget(const Surface& s, bool depth);
    // EnsureTarget's steps, in call order
    TargetSlot FindTargetSlot(const Surface& s, bool depth, uint64_t alt);
    uint64_t SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const;
    bool TargetStale(uint32_t data) const;
    void ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it);
    void SweepTargets();
    bool CreateTargetResource(Target& t, const Surface& s);
    bool CreateTargetView(Target& t);
    // UploadTarget, then its step
    void UploadTarget(Target& t, const Surface& s);
    ComPtr<ID3D12Resource> CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w, uint32_t h);
    // Clear's arguments past the state
    struct ClearArgs {
        uint32_t count;
        const uint8_t* rects;
        uint32_t flags;
        uint32_t color;
        float z;
        uint32_t stencil;
    };
    void ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs);
    void ClearChannels(Target& t, const float col[4], uint8_t mask, const std::vector<D3D12_RECT>& rs);
    ID3D12PipelineState* EnsureClearPipeline(DXGI_FORMAT rtv, uint8_t mask, uint32_t samples);
    // Copy's guest lists: count src_rects, dst_points 0 or one per rect
    struct CopyList {
        uint32_t count;
        uint32_t src_rects;
        uint32_t dst_points;
    };
    void CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                    const CopyList& l);
    void CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state, Target& dst_t,
                      const CopyList& l);
    // the target as a guest sized BGRA copy source, and the shrunk copy that holds it when it is not one already
    struct CopySource {
        ID3D12Resource* res;
        ComPtr<ID3D12Resource> down;
    };
    CopySource PrepareCopySource(Target& t, const ShrinkOptions& o);
    // RecordLate's steps, under late_lock_
    struct Late;
    void ResetLate(Late& l, const Target& t);
    ID3D12Resource* PrepareLateSource(Target& t, Late& l);
    void RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src);

    ComPtr<IDXGIFactory4> factory_;
    ComPtr<ID3D12Device> dev_;
    ComPtr<ID3D12CommandQueue> queue_;
    ComPtr<IDXGISwapChain3> swap_;
    ComPtr<ID3D12Resource> bb_[kFrames];
    ComPtr<ID3D12Fence> fence_;
    uint64_t fence_value_ = 0;
    Frame frames_[kFrames];
    uint64_t frame_ = 0;
    // texture view sets made since the descriptor ring last started over, by a hash of their resources and views, to
    // their first slot; resources freed in that time go to the frame's garbage, so no other one takes their address
    std::unordered_map<uint64_t, uint32_t> view_sets_;
    uint64_t view_sets_frame_ = UINT64_MAX;
    // the constants last uploaded in this command list, reused while a draw's are the same; 0 after a submit
    decltype(State::vsc) last_vsc_{};
    D3D12_GPU_VIRTUAL_ADDRESS last_vsc_gpu_ = 0;
    DrawConstants last_dc_{};
    D3D12_GPU_VIRTUAL_ADDRESS last_dc_gpu_ = 0;
    ComPtr<ID3D12GraphicsCommandList> lists_[kFrames];
    ID3D12GraphicsCommandList* cl_ = nullptr;  // the list recording the current frame slot
    ComPtr<ID3D12DescriptorHeap> srv_heap_;
    ComPtr<ID3D12DescriptorHeap> smp_heap_;
    ComPtr<ID3D12DescriptorHeap> rtv_heap_;
    ComPtr<ID3D12DescriptorHeap> dsv_heap_;
    ComPtr<ID3D12DescriptorHeap> ui_heap_;
    uint32_t srv_inc_ = 0;
    uint32_t smp_inc_ = 0;
    uint32_t rtv_inc_ = 0;
    uint32_t dsv_inc_ = 0;
    std::vector<uint32_t> free_rtv_;
    std::vector<uint32_t> free_dsv_;
    ComPtr<ID3D12RootSignature> root_;
    std::unordered_map<uint64_t, ComPtr<ID3DBlob>> blobs_;
    std::unordered_map<uint64_t, uint64_t> shader_src_;  // vertex and pixel program keys to source hash
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> psos_;
    // sampler descriptors are rewritten per frame like the SRV ring; a descriptor set from an
    // earlier frame may still be referenced by in-flight GPU work until its fence completes.
    static constexpr uint32_t kSamplersPerFrame = (kSamplerCount / kFrames) & ~3u;
    std::array<std::unordered_map<uint64_t, uint32_t>, kFrames> samplers_;
    std::unordered_map<uint64_t, Target> targets_;
    std::unordered_map<uint32_t, uint64_t> target_used_;  // target address to the frame it was last used
    std::array<UsedTarget, 4> recent_used_{};
    uint32_t recent_used_next_ = 0;
    DrawBinds binds_;
    // TCNYC_DEFER_SCENE=1: main-scene draws are recorded into bundles and replayed at present.
    // Everything stays on the immediate path when the variable is unset.
    bool defer_scene_ = false;       // ambient occlusion needs the deferred display list
    bool defer_forced_ = false;      // TCNYC_DEFER_SCENE kept the prototype on regardless of AO
    bool defer_frame_active_ = false;  // a main-scene draw was seen this frame
    bool defer_abandoned_ = false;   // this frame fell back to immediate drawing
    bool defer_reported_ = false;    // the first deferred frame has been reported
    const char* defer_reason_ = nullptr;
    bool defer_flush_conflict_ = false;  // Transition flushed pending bundles during this draw
    bool defer_ao_applied_ = false;      // the scene-time AO pass has run for this frame
    Target* defer_rt_ = nullptr;     // this frame's full-size scene colour target
    ID3D12GraphicsCommandList* bundle_ = nullptr;  // the bundle being recorded, if any
    BundleDesc bundle_desc_;         // its direct-list state
    std::unordered_set<ID3D12Resource*> defer_reads_;   // resources pending bundles read
    std::unordered_set<ID3D12Resource*> defer_writes_;  // resources pending bundles write
    uint32_t defer_bundles_this_frame_ = 0;
    uint32_t defer_flushes_this_frame_ = 0;
    std::unordered_map<uint64_t, Tex> textures_;  // by data address, a P8 texture's with its palette's above
    std::unordered_map<uint64_t, Tex> pack_textures_;  // texture pack images by key, kept for the run
    std::unordered_map<uint64_t, ComPtr<ID3D12PipelineState>> blit_psos_;
    Extension* ext_ = nullptr;
    // one frame capture: every draw in a text file, each target as the frame leaves it, the textures it read
    FILE* cap_ = nullptr;
    std::filesystem::path cap_dir_;
    uint32_t cap_i_ = 0;
    uint32_t cap_rt_ = 0;
    std::unordered_set<uint32_t> cap_seen_;
    std::unordered_set<uint64_t> cap_sh_;
    struct CapTex {
        ComPtr<ID3D12Resource> res;
        DXGI_FORMAT fmt;
        uint32_t data;
        uint32_t w;
        uint32_t h;
    };
    std::vector<CapTex> cap_tex_;
    ComPtr<ID3D12Resource> lut_;
    struct SmaaTexture {
        ComPtr<ID3D12Resource> res;
        D3D12_RESOURCE_STATES state = D3D12_RESOURCE_STATE_COPY_DEST;
    };
    struct Smaa {
        SmaaTexture area, search;           // the reference lookups, uploaded once
        SmaaTexture edges, weights, output; // frame sized render targets
        uint32_t w = 0, h = 0;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        bool failed = false;
    } smaa_;
    D3D12_RESOURCE_STATES lut_state_ = D3D12_RESOURCE_STATE_COPY_DEST;
    uint8_t ramp_[768] = {};
    bool ramp_dirty_ = false;
    bool gamma_ = false;
    Target* bound_rt_ = nullptr;
    Target* bound_ds_ = nullptr;
    Target* draw_rt_ = nullptr;  // the draw's targets, the bound ones go when a texture's copy is drawn
    Target* draw_ds_ = nullptr;
    app::Settings render_;  // snapshot shared by draws and presentation of this frame
    struct SceneDepth {
        Target* target;
        float near_z, far_z;
    };
    std::unordered_map<Target*, SceneDepth> scene_depth_;  // per colour target, cleared at present
    ID3D12Resource* dof_depth_ = nullptr;  // this frame's readable depth, set at present
    float dof_near_ = 0.1f, dof_far_ = 1000.0f;
    ComPtr<ID3D12Resource> ao_half_;
    D3D12_RESOURCE_STATES ao_half_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    uint32_t ao_half_w_ = 0, ao_half_h_ = 0;
    bool ao_half_ready_ = false;
    // AO depth prepass: only the AO receivers' depth, so effects never reach the AO input
    ComPtr<ID3D12Resource> ao_depth_;
    D3D12_RESOURCE_STATES ao_depth_state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    uint32_t ao_depth_view_ = UINT32_MAX;  // its DSV, taken from the depth view pool
    bool ao_depth_clear_ = true;           // a new scene view: clear it before the next prepass
    ComPtr<ID3D12Resource> ao_dist_;       // the prepass's view distance, matching ao_depth_
    D3D12_RESOURCE_STATES ao_dist_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    // full-size occlusion (1 - AO factor) the receivers' pixel shaders read at t4
    ComPtr<ID3D12Resource> ao_occ_;
    D3D12_RESOURCE_STATES ao_occ_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    uint32_t ao_occ_w_ = 0, ao_occ_h_ = 0;
    DepthReplay depth_replay_;  // the current deferred receiver's bindings
    struct LastDraw {           // the geometry views DrawGeometry last recorded
        D3D12_VERTEX_BUFFER_VIEW vbv[16] = {};
        uint32_t vb_count = 0;
        D3D12_INDEX_BUFFER_VIEW ibv{};
        uint32_t index_count = 0, draw_count = 0;
        int32_t base_vertex = 0;
        bool ok = false;
    } last_draw_;
    Target* defer_ds_ = nullptr;  // the depth target of this frame's deferred scene
    uint32_t guest_w_ = 640;
    uint32_t guest_h_ = 480;
    uint32_t bb_w_ = 0;
    uint32_t bb_h_ = 0;
    uint32_t msaa_ = 0, requested_msaa_ = 0;
    float scale_ = 1.0f;
    float stretch_ = 1.0f;  // app::RenderStretch: targets are this much wider than scale_ makes them
    float aspect_ = 16.0f / 9.0f;  // latched once a frame: UiSqueeze reads it for every draw
    // surfaces locked without a wait: copies in flight, the newest finished one goes to guest memory
    struct Late {
        ComPtr<ID3D12Resource> rb[kFrames];
        ComPtr<ID3D12Resource> down;
        D3D12_RESOURCE_STATES down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
        uint64_t fence[kFrames] = {};
        uint64_t frame[kFrames] = {};
        uint64_t delivered = 0;
        uint32_t gw = 0;
        uint32_t gh = 0;
        uint32_t row_pitch = 0;
        uint32_t next = 0;
    };
    std::unordered_map<uint32_t, Late> late_;
    std::mutex late_lock_;                      // DeliverLate runs on the title's thread
    std::atomic<uint64_t> presents_seen_{0};    // presents_ for DeliverLate
    std::atomic<uint64_t> present_fence_[kFrames + 1] = {};  // by presents_, for WaitGpuFrame
    Assembled asm_;  // kept between draws for its buffers
    // visibility tests: an occlusion query per counted draw, summed into report memory once the GPU is done
    struct Query {
        uint32_t index;
        float samples_per_pixel;  // host samples one console pixel covers
    };
    struct Report {
        uint32_t va;
        std::vector<Query> queries;
        uint64_t fence;
    };
    ComPtr<ID3D12QueryHeap> query_heap_;
    ComPtr<ID3D12Resource> query_rb_;
    const uint64_t* query_counts_ = nullptr;
    uint32_t query_next_ = 0;
    std::vector<Query> counted_;  // since the last ClearReport
    std::deque<Report> reports_;
    bool tearing_ = false;
    uint64_t presents_ = 0;
    double fps_ = 0;
    std::chrono::steady_clock::time_point fps_t_ = std::chrono::steady_clock::now();
    uint64_t fps_n_ = 0;
};

bool D3D12Backend::Init(uint32_t width, uint32_t height) {
    render_ = app::Snapshot();
    g_reversed_depth = true;
    guest_w_ = width;
    guest_h_ = height;
    if (const char* e = std::getenv("TCNYC_DEFER_SCENE")) defer_forced_ = *e && std::atoi(e) != 0;
    // Experimental: the deferred AO path has known bright-effect artifacts. It is
    // opt-in until the clean-depth/effect-mask plan in docs/deferred-renderer.md is done.
    defer_scene_ = defer_forced_;
    if (defer_scene_) LOGR("d3d12: experimental deferred AO enabled (TCNYC_DEFER_SCENE=1)");
    HWND hwnd = static_cast<HWND>(app::Window());
    if (!CreateDevice()) return false;
    if (!CreateSwapChain(hwnd)) return false;
    if (!CreateHeaps()) return false;
    if (!CreateBackBufferViews()) return false;
    if (!CreateRootSignature()) return false;
    if (!CreateFrames()) return false;
    CreateGammaLut();
    ImGui_ImplDX12_Init(dev_.Get(), kFrames, DXGI_FORMAT_B8G8R8A8_UNORM, ui_heap_.Get(),
                        ui_heap_->GetCPUDescriptorHandleForHeapStart(), ui_heap_->GetGPUDescriptorHandleForHeapStart());
    LoadSettings();
    Begin();
    LOGI("d3d12: %ux%u guest, %ux%u window, render scale %.3f", width, height, bb_w_, bb_h_, scale_);
    return true;
}

bool D3D12Backend::CreateDevice() {
    if (GetEnvironmentVariableW(L"XBE_D3D12_DEBUG", nullptr, 0)) {
        ComPtr<ID3D12Debug> dbg;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg)))) dbg->EnableDebugLayer();
    }
    if (FAILED(CreateDXGIFactory2(0, IID_PPV_ARGS(&factory_)))) return false;
    // dedicated GPU first on hybrid laptops
    ComPtr<IDXGIFactory6> f6;
    ComPtr<IDXGIAdapter1> adapter;
    if (SUCCEEDED(factory_.As(&f6))) {
        f6->EnumAdapterByGpuPreference(0, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&adapter));
    }
    if (FAILED(D3D12CreateDevice(adapter.Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_))) &&
        FAILED(D3D12CreateDevice((adapter = nullptr).Get(), D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&dev_)))) {
        LOGE("d3d12: no device");
        return false;
    }
    DXGI_ADAPTER_DESC1 ad{};
    if (adapter) adapter->GetDesc1(&ad);
    LOGR("D3D12 adapter: %ls", adapter ? ad.Description : L"default");
    D3D12_COMMAND_QUEUE_DESC qd{};
    qd.Type = D3D12_COMMAND_LIST_TYPE_DIRECT;
    dev_->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue_));
    ComPtr<IDXGIFactory5> f5;
    BOOL tear = FALSE;
    if (SUCCEEDED(factory_.As(&f5))) f5->CheckFeatureSupport(DXGI_FEATURE_PRESENT_ALLOW_TEARING, &tear, sizeof(tear));
    tearing_ = tear;
    return true;
}

bool D3D12Backend::CreateSwapChain(HWND hwnd) {
    RECT rc;
    GetClientRect(hwnd, &rc);
    bb_w_ = std::max<LONG>(rc.right - rc.left, 1);
    bb_h_ = std::max<LONG>(rc.bottom - rc.top, 1);
    DXGI_SWAP_CHAIN_DESC1 sd{};
    sd.Width = bb_w_;
    sd.Height = bb_h_;
    sd.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = kFrames;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0;
    ComPtr<IDXGISwapChain1> sc1;
    if (FAILED(factory_->CreateSwapChainForHwnd(queue_.Get(), hwnd, &sd, nullptr, nullptr, &sc1))) {
        LOGE("d3d12: no swap chain");
        return false;
    }
    factory_->MakeWindowAssociation(hwnd, DXGI_MWA_NO_ALT_ENTER);
    sc1.As(&swap_);
    return true;
}

uint32_t D3D12Backend::CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE t, uint32_t n, bool visible,
                                  ComPtr<ID3D12DescriptorHeap>& out) {
    D3D12_DESCRIPTOR_HEAP_DESC hd{};
    hd.Type = t;
    hd.NumDescriptors = n;
    hd.Flags = visible ? D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE : D3D12_DESCRIPTOR_HEAP_FLAG_NONE;
    dev_->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&out));
    return dev_->GetDescriptorHandleIncrementSize(t);
}

bool D3D12Backend::CreateBackBufferViews() {
    for (int i = 0; i < kFrames; i++) {
        const HRESULT hr = swap_->GetBuffer(i, IID_PPV_ARGS(&bb_[i]));
        if (FAILED(hr) || !bb_[i]) {
            LOGE("d3d12: GetBuffer(%d) failed: 0x%08lX (device removed: 0x%08lX)", i, hr,
                 dev_->GetDeviceRemovedReason());
            for (auto& buffer : bb_) buffer.Reset();
            return false;
        }
        dev_->CreateRenderTargetView(bb_[i].Get(), nullptr, Cpu(rtv_heap_.Get(), kRtvCount + i, rtv_inc_));
    }
    return true;
}

bool D3D12Backend::CreateHeaps() {
    srv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, kSrvCount, true, srv_heap_);
    smp_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, kSamplerCount, true, smp_heap_);
    rtv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_RTV, kRtvCount + kFrames + kSmaaRtvs + kAoRtvs, false,
                          rtv_heap_);
    dsv_inc_ = CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_DSV, kDsvCount, false, dsv_heap_);
    CreateHeap(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, 1, true, ui_heap_);
    if (!srv_heap_ || !smp_heap_ || !rtv_heap_ || !dsv_heap_ || !ui_heap_) {
        LOGE("d3d12: descriptor heaps could not be created");
        return false;
    }
    for (uint32_t i = kRtvCount; i-- > 0;) free_rtv_.push_back(i);
    for (uint32_t i = kDsvCount; i-- > 0;) free_dsv_.push_back(i);
    // permanently reserve the few scratch views the resolve/shrink passes borrow
    for (uint32_t f = 0; f < kFrames; f++)
        for (uint32_t i = 0; i < kScratchViews; i++) {
            if (free_rtv_.empty()) return false;
            scratch_rtv_[f][i] = free_rtv_.back();
            free_rtv_.pop_back();
        }
    return true;
}

bool D3D12Backend::CreateRootSignature() {
    D3D12_DESCRIPTOR_RANGE ranges[3] = {};
    ranges[0].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[0].NumDescriptors = 4;
    ranges[1].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER;
    ranges[1].NumDescriptors = 4;
    // the scene AO occlusion (t4, after the stages' t0-t3): set on the direct list by the
    // deferred flush, which the AO receivers' bundles inherit; no other shader declares it
    ranges[2].RangeType = D3D12_DESCRIPTOR_RANGE_TYPE_SRV;
    ranges[2].NumDescriptors = 1;
    ranges[2].BaseShaderRegister = 4;
    D3D12_ROOT_PARAMETER params[5] = {};
    params[4].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
    params[4].DescriptorTable.NumDescriptorRanges = 1;
    params[4].DescriptorTable.pDescriptorRanges = &ranges[2];
    params[4].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    params[0].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[0].Descriptor.ShaderRegister = 0;
    params[1].ParameterType = D3D12_ROOT_PARAMETER_TYPE_CBV;
    params[1].Descriptor.ShaderRegister = 1;
    for (int i = 0; i < 2; i++) {
        params[2 + i].ParameterType = D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE;
        params[2 + i].DescriptorTable.NumDescriptorRanges = 1;
        params[2 + i].DescriptorTable.pDescriptorRanges = &ranges[i];
        params[2 + i].ShaderVisibility = D3D12_SHADER_VISIBILITY_PIXEL;
    }
    D3D12_ROOT_SIGNATURE_DESC rsd{};
    rsd.NumParameters = 5;
    rsd.pParameters = params;
    rsd.Flags = D3D12_ROOT_SIGNATURE_FLAG_ALLOW_INPUT_ASSEMBLER_INPUT_LAYOUT;
    ComPtr<ID3DBlob> rs;
    ComPtr<ID3DBlob> err;
    if (FAILED(D3D12SerializeRootSignature(&rsd, D3D_ROOT_SIGNATURE_VERSION_1, &rs, &err))) return false;
    if (FAILED(dev_->CreateRootSignature(0, rs->GetBufferPointer(), rs->GetBufferSize(), IID_PPV_ARGS(&root_))))
        return false;
    return true;
}

bool D3D12Backend::CreateFrames() {
    for (auto& f : frames_) {
        if (FAILED(dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&f.alloc)))) return false;
        if (FAILED(dev_->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_BUNDLE, IID_PPV_ARGS(&f.bundle_alloc)))) {
            LOGE("d3d12: no bundle command allocator, deferred scene drawing is off");
            defer_scene_ = false;
            defer_forced_ = false;
        }
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
        D3D12_RESOURCE_DESC bd = BufferDesc(kUploadSize);
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ,
                                                 nullptr, IID_PPV_ARGS(&f.upload))))
            return false;
        if (FAILED(f.upload->Map(0, nullptr, reinterpret_cast<void**>(&f.map))) || !f.map) return false;
    }
    if (FAILED(dev_->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence_)))) return false;
    // One list per frame slot. A list may only be reset after its own fence has
    // completed; sharing one list across slots reset it while the GPU was still
    // executing the submission made moments earlier.
    for (uint32_t i = 0; i < kFrames; i++) {
        if (FAILED(dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, frames_[i].alloc.Get(), nullptr,
                                           IID_PPV_ARGS(&lists_[i]))))
            return false;
        if (i != 0) lists_[i]->Close();
    }
    cl_ = lists_[0].Get();
    return true;
}

// gamma ramp as a 256 x 1 lookup
void D3D12Backend::CreateGammaLut() {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1);
    dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                  IID_PPV_ARGS(&lut_));
}

void D3D12Backend::LoadSettings() {
    requested_msaa_ = app::MsaaSamples();
    msaa_ = SupportedMsaa(requested_msaa_);
    scale_ = app::RenderScale(guest_h_);
    stretch_ = app::RenderStretch(guest_w_, guest_h_);
    aspect_ = app::AspectRatio();
    texturedumper::Update();
    ext_ = g_extension.get();
    if (ext_) {
        ext_->Settings();
        ext_->Reset(*this);
    }
}

void D3D12Backend::Begin() {
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), smp_heap_.Get()};
    cl_->SetDescriptorHeaps(2, heaps);
    cl_->SetGraphicsRootSignature(root_.Get());
    bound_rt_ = bound_ds_ = nullptr;
    binds_ = {};
}

void D3D12Backend::Submit(bool wait) {
    if (!cl_) return;  // init failed; Present already reports it
    last_vsc_gpu_ = 0;
    last_dc_gpu_ = 0;
    HRESULT hr = cl_->Close();
    if (FAILED(hr)) {
        DeviceFailure(hr, "command list close");
        return;
    }
    ID3D12CommandList* lists[] = {cl_};
    queue_->ExecuteCommandLists(1, lists);
    hr = queue_->Signal(fence_.Get(), ++fence_value_);
    if (FAILED(hr)) {
        DeviceFailure(hr, "queue signal");
        return;
    }
    Frame& f = CurrentFrame();
    f.fence = fence_value_;
    if (wait) {
        if (!WaitFence(fence_value_)) {
            LOGE("d3d12: GPU did not finish submission %llu", static_cast<unsigned long long>(fence_value_));
            kernel::Shutdown(1);
            return;
        }
        RecycleFrameViews(f, uint32_t(frame_ % kFrames));
        f.alloc->Reset();
        f.garbage.clear();
        f.used = 0;
        f.srv = 0;
        f.smp = uint32_t(frame_ % kFrames) * kSamplersPerFrame;
        samplers_[frame_ % kFrames].clear();
        view_sets_.clear();
        cl_ = lists_[frame_ % kFrames].Get();
        cl_->Reset(f.alloc.Get(), nullptr);
        Begin();
    }
}

void D3D12Backend::Flush() {
    if (defer_scene_) FlushDeferredScene();
    Submit(true);
}

// Every wait is bounded and checks for device removal. An INFINITE wait on a
// fence that will never be signalled turns a driver reset into a frozen game.
bool D3D12Backend::WaitFence(uint64_t value) {
    if (!fence_) return false;
    if (fence_->GetCompletedValue() >= value) return true;
    HANDLE event = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    if (!event) return false;
    const HRESULT hr = fence_->SetEventOnCompletion(value, event);
    if (FAILED(hr)) {
        LOGE("d3d12: SetEventOnCompletion(%llu) failed: 0x%08lX", static_cast<unsigned long long>(value),
             static_cast<unsigned long>(hr));
        CloseHandle(event);
        return false;
    }
    const ULONGLONG deadline = GetTickCount64() + 30000;
    bool done = false;
    for (;;) {
        const DWORD r = WaitForSingleObject(event, 1000);
        if (r == WAIT_OBJECT_0) {
            done = true;
            break;
        }
        if (r != WAIT_TIMEOUT) {
            LOGE("d3d12: fence wait failed (%lu)", GetLastError());
            break;
        }
        const HRESULT reason = dev_->GetDeviceRemovedReason();
        if (FAILED(reason)) {
            LOGE("d3d12: device lost while waiting for fence %llu (reason 0x%08lX)",
                 static_cast<unsigned long long>(value), static_cast<unsigned long>(reason));
            break;
        }
        if (GetTickCount64() >= deadline) {
            LOGE("d3d12: fence %llu did not complete within 30 s", static_cast<unsigned long long>(value));
            break;
        }
    }
    CloseHandle(event);
    return done;
}

[[noreturn]] void D3D12Backend::DeviceFailure(HRESULT hr, const char* what) {
    const HRESULT reason = dev_ ? dev_->GetDeviceRemovedReason() : E_FAIL;
    LOGE("d3d12: %s failed (0x%08lX), device removed reason 0x%08lX", what, static_cast<unsigned long>(hr),
         static_cast<unsigned long>(reason));
    kernel::Shutdown(1);
}

void D3D12Backend::SetFullViewport(uint32_t w, uint32_t h) {
    D3D12_VIEWPORT vp{0, 0, float(w), float(h), 0, 1};
    D3D12_RECT scissor{0, 0, LONG(w), LONG(h)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
    binds_ = {};
}

// one w x h colour target for a Blit, the draw's bound targets forgotten
void D3D12Backend::SetBlitTarget(D3D12_CPU_DESCRIPTOR_HANDLE rtv, uint32_t w, uint32_t h) {
    cl_->OMSetRenderTargets(1, &rtv, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    SetFullViewport(w, h);
}

D3D12Backend::Alloc D3D12Backend::Upload(uint64_t size, uint64_t align) {
    Frame& f = CurrentFrame();
    g_perf.upload += size;
    uint64_t o = (f.used + align - 1) & ~(align - 1);
    if (o + size <= kUploadSize) {
        f.used = o + size;
        return {f.upload.Get(), o, f.map + o, f.upload->GetGPUVirtualAddress() + o};
    }
    // too big for the ring: a buffer of its own, released with the frame
    g_perf.overflow++;
    ComPtr<ID3D12Resource> r;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_UPLOAD};
    constexpr uint64_t kOwnAlign = 0x10000;  // a committed buffer's size granularity
    D3D12_RESOURCE_DESC bd = BufferDesc((size + kOwnAlign - 1) & ~(kOwnAlign - 1));
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr,
                                             IID_PPV_ARGS(&r)))) {
        LOGE("d3d12: could not create a %llu byte upload buffer", (unsigned long long)size);
        return {};
    }
    uint8_t* p = nullptr;
    if (FAILED(r->Map(0, nullptr, reinterpret_cast<void**>(&p))) || !p) {
        LOGE("d3d12: could not map a %llu byte upload buffer", (unsigned long long)size);
        return {};
    }
    f.garbage.push_back(r);
    return {r.Get(), 0, p, r->GetGPUVirtualAddress()};
}

uint32_t D3D12Backend::AllocSrvSlots(uint32_t n) {
    Frame& f = CurrentFrame();
    const uint32_t per = kSrvCount / kFrames;
    if (f.srv + n > per) {
        // Never borrow another frame's region: those descriptors may still be
        // referenced by an in-flight command list. Start a new recording frame.
        if (defer_scene_ && !defer_abandoned_ && !bundle_) DeferredBarrier("mid-frame descriptor ring flush");
        LOG_FIRST(4, "d3d12: descriptor ring full within a frame, submitting early");
        Submit(true);
        if (f.srv + n > per) {
            LOGE("d3d12: descriptor ring exhausted even after a submit (%u + %u > %u)", f.srv, n, per);
            kernel::Shutdown(1);
        }
    }
    uint32_t i = uint32_t(frame_ % kFrames) * per + f.srv;
    f.srv += n;
    return i;
}

void D3D12Backend::Transition(ID3D12Resource* r, D3D12_RESOURCE_STATES& cur, D3D12_RESOURCE_STATES want) {
    if (cur == want) return;
    if (defer_scene_ && r && !CurrentFrame().bundles.empty()) {
        // A pending bundle must see the resource in the state it was recorded against. A
        // barrier to another state would change what the later bundle executes with.
        const bool pending_read = defer_reads_.count(r) != 0;
        const bool pending_write = defer_writes_.count(r) != 0;
        if (pending_read || pending_write) {
            if (want != D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE || pending_write) {
                if (!bundle_) {
                    // Materialise what has been deferred so the transition and the draw that
                    // needs it happen in the right order, then keep the frame deferred.
                    FlushDeferredScene();
                    defer_flush_conflict_ = true;
                    if (cur == want) return;
                } else {
                    DeferredFallback(want == D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE
                                         ? "scene target sampled after a deferred draw"
                                         : "target written after a deferred draw");
                }
            }
        }
    }
    D3D12_RESOURCE_BARRIER b{};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition.pResource = r;
    b.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    b.Transition.StateBefore = cur;
    b.Transition.StateAfter = want;
    cl_->ResourceBarrier(1, &b);
    cur = want;
}

// a resized target gives back its view and its resources
void D3D12Backend::ReleaseTarget(std::unordered_map<uint64_t, Target>::iterator it) {
    if (DeferredPendingResource(it->second.res.Get()) ||
        (it->second.resolved && DeferredPendingResource(it->second.resolved.Get()))) {
        DeferredBarrier("target released while deferred");
        if (defer_rt_ == &it->second) defer_rt_ = nullptr;
        if (defer_ds_ == &it->second) defer_ds_ = nullptr;
    }
    if (it->second.depth)
        std::erase_if(scene_depth_, [&](const auto& entry) { return entry.second.target == &it->second; });
    else
        scene_depth_.erase(&it->second);
    if (bound_rt_ == &it->second || bound_ds_ == &it->second) bound_rt_ = bound_ds_ = nullptr;
    // The command list still references this view index: return it only after the frame's fence.
    (it->second.depth ? CurrentFrame().dsv_garbage : CurrentFrame().rtv_garbage).push_back(it->second.view);
    CurrentFrame().garbage.push_back(it->second.res);
    if (it->second.resolved) CurrentFrame().garbage.push_back(it->second.resolved);
    if (it->second.fb) CurrentFrame().garbage.push_back(it->second.fb);
    targets_.erase(it);
}

// the host texture of a new target, at render scale or the extension's
bool D3D12Backend::CreateTargetResource(Target& t, const Surface& s) {
    float sc = scale_;
    t.want_samples = msaa_;
    t.want_scale = scale_;
    t.want_stretch = stretch_;
    if (ext_) ext_->MakeTarget(s, t.depth, t.gw, t.gh, sc, t.fmt);
    t.w = std::max(1u, uint32_t(std::lround(t.gw * sc * stretch_)));
    t.h = std::max(1u, uint32_t(std::lround(t.gh * sc)));
    if (t.w > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION || t.h > D3D12_REQ_TEXTURE2D_U_OR_V_DIMENSION) {
        LOGE("d3d12: requested render target is too large: %ux%u", t.w, t.h);
        return false;
    }

    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_UNKNOWN, t.w, t.h);
    D3D12_CLEAR_VALUE cv{};
    if (t.depth) {
        td.Format = kDepthTypeless;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
        cv.Format = kDepthFormat;
        cv.DepthStencil.Depth = kDepthFar;
        t.state = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    } else {
        td.Format = t.fmt;
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        cv.Format = td.Format;
        t.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
    }
    if (msaa_) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{cv.Format, msaa_};
        HRESULT msaa_hr = E_FAIL;
        if (SUCCEEDED(dev_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels,
                                               sizeof(levels))) && levels.NumQualityLevels) {
            td.SampleDesc.Count = msaa_;
            msaa_hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.state, &cv,
                                                    IID_PPV_ARGS(&t.res));
            if (SUCCEEDED(msaa_hr)) {
                static bool reported[2] = {};  // colour, depth: the first of each
                if (!std::exchange(reported[t.depth ? 1 : 0], true))
                    LOGR("d3d12: allocated %ux MSAA %s target", msaa_, t.depth ? "depth" : "color");
                return true;
            }
            td.SampleDesc.Count = 1;
        }
        static unsigned reports = 0;
        if (reports++ < 8)
            LOGR("d3d12: %ux MSAA unavailable for format %u at %ux%u (0x%08lX, device removed reason 0x%08lX), "
                 "using one sample",
                 msaa_, unsigned(cv.Format), t.w, t.h, msaa_hr, dev_->GetDeviceRemovedReason());
    }
    const HRESULT hr = dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.state, &cv,
                                                     IID_PPV_ARGS(&t.res));
    if (FAILED(hr)) LOGE("d3d12: target %ux%u format %u could not be created (0x%08lX), device removed reason 0x%08lX",
                         t.w, t.h, unsigned(td.Format), hr, dev_->GetDeviceRemovedReason());
    return SUCCEEDED(hr);
}

// a view from the free pool, cleared; false when the pool is empty
bool D3D12Backend::CreateTargetView(Target& t) {
    auto& pool = t.depth ? free_dsv_ : free_rtv_;
    if (pool.empty()) return false;
    t.view = pool.back();
    pool.pop_back();
    if (t.depth) {
        D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
        dv.Format = kDepthFormat;
        dv.ViewDimension = t.res->GetDesc().SampleDesc.Count > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS
                                                               : D3D12_DSV_DIMENSION_TEXTURE2D;
        dev_->CreateDepthStencilView(t.res.Get(), &dv, Cpu(dsv_heap_.Get(), t.view, dsv_inc_));
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_),
                                   D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, kDepthFar, 0, 0, nullptr);
    } else {
        dev_->CreateRenderTargetView(t.res.Get(), nullptr, Cpu(rtv_heap_.Get(), t.view, rtv_inc_));
        float black[4] = {0, 0, 0, 0};
        cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), black, 0, nullptr);
    }
    return true;
}

// targets_ key of a surface's colour or depth target
uint64_t TargetMapKey(uint32_t data, bool depth) { return uint64_t(data) << 1 | (depth ? 1 : 0); }

// a smaller depth surface's console size, above the address bits
constexpr int kKeyWidthShift = 34;
constexpr int kKeyHeightShift = 48;

// a smaller depth surface laid on the depth buffer's memory is a target of its own
uint64_t D3D12Backend::SizedTargetKey(uint32_t data, bool depth, uint32_t gw, uint32_t gh) const {
    uint64_t key = TargetMapKey(data, depth);
    bool small_depth = depth && (gw != guest_w_ || gh != guest_h_);
    if (!small_depth) return key;
    return key | uint64_t(gw) << kKeyWidthShift | uint64_t(gh) << kKeyHeightShift;
}

// alt: the extension's bits of the key
D3D12Backend::TargetSlot D3D12Backend::FindTargetSlot(const Surface& s, bool depth, uint64_t alt) {
    TargetSlot slot;
    slot.data = mem::VirtualFromPhysical(s.data);
    SurfaceSize(s, slot.gw, slot.gh);
    slot.key = SizedTargetKey(slot.data, depth, slot.gw, slot.gh) | alt;
    slot.it = targets_.find(slot.key);
    slot.sized = slot.it != targets_.end() && slot.it->second.gw == slot.gw && slot.it->second.gh == slot.gh;
    return slot;
}

// the surface's target at its size, null when there is none
Target* D3D12Backend::FindTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint64_t alt = 0;
    if (ext_ && !ext_->TargetKey(s, depth, alt)) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth, alt);
    if (!slot.sized) return nullptr;
    UpdateTargetUsed(slot.data);
    return &slot.it->second;
}

// the surface's target, made again at its size when that changed
Target* D3D12Backend::EnsureTarget(const Surface& s, bool depth) {
    if (!s.header) return nullptr;
    uint64_t alt = 0;
    if (ext_ && !ext_->TargetKey(s, depth, alt)) return nullptr;
    TargetSlot slot = FindTargetSlot(s, depth, alt);
    UpdateTargetUsed(slot.data);
    if (slot.sized) {
        Target& t = slot.it->second;
        // Targets that are only sampled keep their contents across a settings change; one that is
        // drawn into again is rebuilt when its MSAA/scale no longer match.
        if (t.want_samples == msaa_ && t.want_scale == scale_ && t.want_stretch == stretch_)
            return &t;
        ReleaseTarget(slot.it);
    } else if (slot.it != targets_.end()) {
        ReleaseTarget(slot.it);
    }
    Target t;
    t.depth = depth;
    t.gw = slot.gw;
    t.gh = slot.gh;
    if (!CreateTargetResource(t, s)) return nullptr;
    g_perf.targets++;
    // an empty pool drops the new resource with t
    if (!CreateTargetView(t)) return nullptr;
    t.data = slot.data;
    t.bytes = GuestPitch(slot.gw) * slot.gh;
    if (depth) {
        // D16/D24 depth surfaces are not four bytes per texel; the signature must not run past them
        const uint32_t bpp = std::max<uint32_t>(1, Format(s.format).bits / 8);
        t.bytes = ((slot.gw * bpp + 63) & ~63u) * slot.gh;
    }
    t.epoch = DirtyEpoch(slot.data);
    t.sig = MemSig(slot.data, t.bytes);
    auto& ref = targets_[slot.key] = std::move(t);
    return &ref;
}

void D3D12Backend::UpdateTargetUsed(uint32_t data) {
    for (const UsedTarget& u : recent_used_) {
        if (u.data == data && u.frame == frame_) return;
    }
    target_used_[data] = frame_;
    recent_used_[recent_used_next_++ % recent_used_.size()] = {data, frame_};
}

bool D3D12Backend::BindTargets(const State& s) {
    Target* rt = EnsureTarget(s.rt, false);
    Target* ds = s.rs[kRsZEnable] || s.rs[kRsStencilEnable] ? EnsureTarget(s.zs, true) : nullptr;
    if (rt && ds && (rt->w != ds->w || rt->h != ds->h ||
                     rt->res->GetDesc().SampleDesc.Count != ds->res->GetDesc().SampleDesc.Count))
        ds = nullptr;
    if (!rt && !ds) return false;
    if (rt) {
        if (LatchCpuWrite(*rt)) UploadTarget(*rt, s.rt);
        Transition(rt->res.Get(), rt->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        rt->drawn = true;
    }
    if (ds) {
        Transition(ds->res.Get(), ds->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        ds->drawn = true;
    }
    if (rt != bound_rt_ || ds != bound_ds_) {
        D3D12_CPU_DESCRIPTOR_HANDLE rh = rt ? Cpu(rtv_heap_.Get(), rt->view, rtv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        D3D12_CPU_DESCRIPTOR_HANDLE dh = ds ? Cpu(dsv_heap_.Get(), ds->view, dsv_inc_) : D3D12_CPU_DESCRIPTOR_HANDLE{};
        cl_->OMSetRenderTargets(rt ? 1 : 0, rt ? &rh : nullptr, FALSE, ds ? &dh : nullptr);
        bound_rt_ = rt;
        bound_ds_ = ds;
    }
    return true;
}

constexpr uint64_t kDepthCopyKey = ~0ull;  // DepthCopy's pipeline in blit_psos_

// DepthCopy's pipeline, kept in blit_psos_: writes depth only, always passing; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsureDepthCopyPipeline(uint32_t samples) {
    auto& pso = blit_psos_[kDepthCopyKey ^ (uint64_t(samples) << 32)];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(kDepthHlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(kDepthHlsl, "psmain", "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC p{};
    p.pRootSignature = root_.Get();
    p.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    p.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    p.SampleMask = UINT_MAX;
    p.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    p.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    p.RasterizerState.DepthClipEnable = TRUE;
    p.DepthStencilState.DepthEnable = TRUE;
    p.DepthStencilState.DepthWriteMask = D3D12_DEPTH_WRITE_MASK_ALL;
    p.DepthStencilState.DepthFunc = D3D12_COMPARISON_FUNC_ALWAYS;
    p.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    p.DSVFormat = kDepthFormat;
    p.SampleDesc.Count = samples;
    if (FAILED(CreatePipeline(p, pso))) return nullptr;
    return pso.Get();
}

// src's depth drawn into dst's depth buffer
bool D3D12Backend::DrawDepthCopy(Target& src, Target& dst, ID3D12PipelineState* pso) {
    if (DeferredPending()) DeferredBarrier("depth copy after a deferred draw");
    if (cap_) fprintf(cap_, "%04u depth copy %08X to %08X\n", cap_i_++, src.data, dst.data);
    const bool multisampled = src.res->GetDesc().SampleDesc.Count > 1;
    ID3D12Resource* depth = multisampled ? ResolveDepth(src) : src.res.Get();
    if (!depth) return false;
    auto& state = multisampled ? src.resolved_state : src.state;
    Transition(depth, state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(dst.res.Get(), dst.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(dsv_heap_.Get(), dst.view, dsv_inc_);
    cl_->OMSetRenderTargets(0, nullptr, FALSE, &dh);
    SetFullViewport(dst.w, dst.h);
    uint32_t slot = AllocSrvSlots(1);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = multisampled ? DXGI_FORMAT_R32_FLOAT : kDepthRead;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(depth, &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    Alloc cb = Upload(256, 256);
    if (!cb.res) return false;
    DrawFullScreen(pso, cb.gpu, slot);
    dst.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
    return true;
}

// a depth buffer drawn as colour into another depth buffer's memory: copied as depth
bool D3D12Backend::DepthCopy(const State& s) {
    TextureDesc d;
    if (!s.rt.header || !s.textures[0] || !Describe(s.textures[0], d) || Format(d.format).depth || d.cube ||
        d.depth != 1) {
        return false;
    }
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(s.rt, gw, gh);
    auto di = targets_.find(SizedTargetKey(mem::VirtualFromPhysical(s.rt.data), true, gw, gh));
    auto si = targets_.find(SizedTargetKey(d.data, true, d.width, d.height));
    if (di == targets_.end() || si == targets_.end() || di == si) return false;
    Target& dst = di->second;
    Target& src = si->second;
    ID3D12PipelineState* pso = EnsureDepthCopyPipeline(dst.res->GetDesc().SampleDesc.Count);
    if (!pso) return false;
    return DrawDepthCopy(src, dst, pso);
}

// the pixels the CPU changed since the last Sync, opaque, the rest 0; t.read takes them
std::vector<uint8_t> LatchCpuChanges(Target& t) {
    std::vector<uint8_t> px(t.read.size(), 0);
    uint32_t guest_pitch = GuestPitch(t.gw);
    const uint8_t* guest = static_cast<const uint8_t*>(kernel::HostPtr(t.data));
    for (uint32_t y = 0; y < t.gh; y++) {
        for (uint32_t x = 0; x < t.gw; x++) {
            const uint8_t* c = guest + size_t(y) * guest_pitch + x * 4;
            size_t i = (size_t(y) * t.gw + x) * 4;
            if (!std::memcmp(c, &t.read[i], 4)) continue;
            std::memcpy(&t.read[i], c, 4);
            std::memcpy(&px[i], c, 3);
            px[i + 3] = 255;
        }
    }
    return px;
}

// BGRA pixels into a new w x h texture, left as a shader resource
ComPtr<ID3D12Resource> D3D12Backend::CreatePixelTexture(const std::vector<uint8_t>& px, uint32_t pitch, uint32_t w,
                                                        uint32_t h) {
    ComPtr<ID3D12Resource> tex;
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&tex))))
        return nullptr;
    uint32_t row = UploadPitch(w * 4);
    Alloc a = Upload(uint64_t(row) * h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (!a.res) return nullptr;
    for (uint32_t y = 0; y < h; y++) std::memcpy(a.cpu + size_t(y) * row, &px[size_t(y) * pitch], w * 4);
    D3D12_TEXTURE_COPY_LOCATION dst{tex.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION src{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    src.PlacedFootprint.Offset = a.off;
    src.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, w, h, 1, row};
    cl_->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tex.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return tex;
}

// CPU pixels of a color surface into its host target, scaled
void D3D12Backend::UploadTarget(Target& t, const Surface& s) {
    if (DeferredPendingResource(t.res.Get())) DeferredBarrier("CPU pixels written to a deferred target");
    g_perf.rtups++;
    TextureDesc d;
    if (!Describe(s.header, d) || Format(d.format).depth) return;
    std::vector<uint8_t> px;
    uint32_t pitch;
    // after a Sync only the pixels the CPU changed go over the picture, which stays at render scale
    bool over = !t.drawn && d.width == t.gw && d.height == t.gh && t.read.size() == size_t(t.gw) * t.gh * 4;
    if (over) {
        LOG_FIRST(4, "d3d12: CPU pixels drawn over target %08X", t.data);
        pitch = t.gw * 4;
        px = LatchCpuChanges(t);
    } else {
        DecodeLevel(d, 0, 0, static_cast<const uint8_t*>(kernel::HostPtr(d.data)), nullptr, px, pitch);
    }
    ComPtr<ID3D12Resource> tex = CreatePixelTexture(px, pitch, d.width, d.height);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    SetBlitTarget(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.w, t.h);
    float uv[4] = {0, 0, 1, 1};
    float mul = 1 / t.range;
    Blit(tex.Get(), kBgra, uv, {.alpha = true, .mul = mul, .rtv = t.fmt, .over = over,
                             .samples = t.res->GetDesc().SampleDesc.Count});
    CurrentFrame().garbage.push_back(tex);
    t.cpu = true;
    if (!over) {
        t.scene_frame = ~0ull;
        scene_depth_.erase(&t);
    }
    t.epoch = DirtyEpoch(t.data);
    t.sig = MemSig(t.data, t.bytes);
}

// samplers_ key of the full screen passes' samplers, apart from the stage samplers' hashed keys
constexpr uint64_t kBlitSamplerKey = 0xB117;

// four clamp samplers: linear at slot 0, point at slots 1 to 3
uint32_t D3D12Backend::EnsureBlitSamplers() {
    uint64_t key = kBlitSamplerKey;
    const uint32_t frame_index = uint32_t(frame_ % kFrames);
    auto& cache = samplers_[frame_index];
    auto it = cache.find(key);
    uint32_t si;
    if (it == cache.end()) {
        const uint32_t start = frame_index * kSamplersPerFrame;
        const uint32_t end = start + kSamplersPerFrame;
        Frame& f = CurrentFrame();
        if (f.smp < start) f.smp = start;
        if (f.smp + 4 > end) {
            LOG_FIRST(4, "d3d12: sampler ring full within a frame, using the first set");
            si = start;
        } else {
            si = f.smp;
            f.smp += 4;
        }
        for (int i = 0; i < 4; i++) {
            D3D12_SAMPLER_DESC sd{};
            sd.Filter = i == 0 ? D3D12_FILTER_MIN_MAG_MIP_LINEAR : D3D12_FILTER_MIN_MAG_MIP_POINT;
            sd.AddressU = sd.AddressV = sd.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
            sd.MaxLOD = D3D12_FLOAT32_MAX;
            dev_->CreateSampler(&sd, Cpu(smp_heap_.Get(), si + i, smp_inc_));
        }
        cache[key] = si;
    } else {
        si = it->second;
    }
    return si;
}

// the full screen triangle: cb at b0 and b1, views at srv_slot; without a sampler slot EnsureBlitSamplers gives it
void D3D12Backend::DrawFullScreen(ID3D12PipelineState* pso, D3D12_GPU_VIRTUAL_ADDRESS cb, uint32_t srv_slot,
                                  std::optional<uint32_t> sampler_slot) {
    binds_ = {};
    Cmd()->SetPipelineState(pso);
    Cmd()->SetGraphicsRootConstantBufferView(0, cb);
    Cmd()->SetGraphicsRootConstantBufferView(1, cb);
    Cmd()->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), srv_slot, srv_inc_));
    // without a slot the passes take the blit samplers, looked up at this point as they always were
    uint32_t si = sampler_slot ? *sampler_slot : EnsureBlitSamplers();
    Cmd()->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), si, smp_inc_));
    Cmd()->IASetPrimitiveTopology(D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    Cmd()->DrawInstanced(3, 1, 0, 0);
}

void D3D12Backend::Blit(ID3D12Resource* src, DXGI_FORMAT fmt, const float uvrect[4], const BlitOptions& o) {
    // a single sampled resource cannot back a Texture2DMS view
    ID3D12Resource* depth = o.depth && o.depth->GetDesc().SampleDesc.Count > 1 ? o.depth : nullptr;
    uint32_t slot = AllocSrvSlots(4);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = fmt;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(src, &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    sv.Format = kBgra;
    dev_->CreateShaderResourceView(lut_.Get(), &sv, Cpu(srv_heap_.Get(), slot + 1, srv_inc_));
    dev_->CreateShaderResourceView(nullptr, &sv, Cpu(srv_heap_.Get(), slot + 2, srv_inc_));
    sv = {};
    sv.Format = kDepthRead;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2DMS;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    dev_->CreateShaderResourceView(depth, &sv, Cpu(srv_heap_.Get(), slot + 3, srv_inc_));
    uint32_t si = EnsureBlitSamplers();
    Alloc cb = Upload(256, 256);
    if (!cb.res) return;
    float c[16] = {uvrect[0], uvrect[1], uvrect[2], uvrect[3],
                   o.gamma ? 1.0f : 0.0f, o.alpha ? 1.0f : 0.0f, float(o.taps), o.mul,
                   o.post, o.bloom, depth ? o.dof : 0.0f, 0.0f,
                   o.src_w > 0 ? 1.0f / o.src_w : 0.0f, o.src_h > 0 ? 1.0f / o.src_h : 0.0f, o.near_z, o.far_z};
    std::memcpy(cb.cpu, c, sizeof(c));
    DrawFullScreen(EnsureBlitPipeline(o.rtv, o.over, o.samples), cb.gpu, slot, si);
}

// Portable DXBC only: pipeline caches would additionally need adapter/driver keys.
// The directory fingerprints the compiler DLL, flags and cache format. The file
// key includes the complete generated source, entry point and shader profile.
constexpr uint64_t kShaderCacheMagic = 0x3145484341435358ull;
constexpr uint32_t kShaderCompileFlags = D3DCOMPILE_OPTIMIZATION_LEVEL1;
constexpr uint64_t kShaderCacheMaxBytes = 16ull << 20;
struct ShaderCacheHeader {
    uint64_t magic, key, size, checksum;
};

std::vector<uint8_t> ReadShaderCache(const std::filesystem::path& file, uint64_t key) {
    std::ifstream in(file, std::ios::binary | std::ios::ate);
    if (!in) return {};
    auto length = in.tellg();
    if (length < std::streamoff(sizeof(ShaderCacheHeader)) ||
        length > std::streamoff(sizeof(ShaderCacheHeader) + kShaderCacheMaxBytes)) return {};
    in.seekg(0);
    ShaderCacheHeader header{};
    if (!in.read(reinterpret_cast<char*>(&header), sizeof(header)) || header.magic != kShaderCacheMagic ||
        header.key != key || header.size < 4 || header.size != uint64_t(length) - sizeof(header)) return {};
    std::vector<uint8_t> bytes(size_t(header.size));
    if (!in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size())) ||
        std::memcmp(bytes.data(), "DXBC", 4) || Hash(bytes.data(), bytes.size()) != header.checksum) return {};
    return bytes;
}

void WriteShaderCache(const std::filesystem::path& file, uint64_t key, const void* data, size_t size) {
    if (size < 4 || size > kShaderCacheMaxBytes) return;
    std::error_code ec;
    std::filesystem::create_directories(file.parent_path(), ec);
    if (ec) return;
    auto temporary = file;
    temporary += "." + std::to_string(GetCurrentProcessId()) + ".tmp";
    ShaderCacheHeader header{kShaderCacheMagic, key, size, Hash(data, size)};
    {
        std::ofstream out(temporary, std::ios::binary | std::ios::trunc);
        if (!out) return;
        out.write(reinterpret_cast<const char*>(&header), sizeof(header));
        out.write(static_cast<const char*>(data), std::streamsize(size));
        out.close();
        if (out) {
            if (MoveFileExW(temporary.c_str(), file.c_str(), MOVEFILE_REPLACE_EXISTING)) return;
        }
    }
    std::filesystem::remove(temporary, ec);
}

uint64_t ShaderCompilerFingerprint() {
    // Taking &D3DCompile can name an import thunk in the executable. Resolve
    // the DLL named by the linked compiler SDK instead of hashing that thunk's module.
    HMODULE compiler = GetModuleHandleW(D3DCOMPILER_DLL_W);
    if (!compiler) return 0;
    wchar_t path[MAX_PATH];
    DWORD count = GetModuleFileNameW(compiler, path, MAX_PATH);
    if (!count || count == MAX_PATH) return 0;
    std::ifstream in(std::filesystem::path(path), std::ios::binary | std::ios::ate);
    if (!in) return 0;
    auto size = in.tellg();
    if (size <= 0 || size > 64 * 1024 * 1024) return 0;
    std::vector<uint8_t> bytes(static_cast<size_t>(size));
    in.seekg(0);
    if (!in.read(reinterpret_cast<char*>(bytes.data()), std::streamsize(bytes.size()))) return 0;
    uint64_t fingerprint = Hash(bytes.data(), bytes.size());
    return Hash(&kShaderCompileFlags, sizeof(kShaderCompileFlags), fingerprint);
}

const std::filesystem::path& ShaderCacheDirectory() {
    static const std::filesystem::path directory = []() -> std::filesystem::path {
        const char* setting = std::getenv("XBE_SHADER_CACHE");
        if (setting && std::strcmp(setting, "0") == 0) return {};
        const uint64_t fingerprint = ShaderCompilerFingerprint();
        if (!fingerprint) return {};
        wchar_t path[MAX_PATH];
        DWORD count;
        count = GetModuleFileNameW(nullptr, path, MAX_PATH);
        if (!count || count == MAX_PATH) return {};
        char name[24];
        std::snprintf(name, sizeof(name), "%016llX", static_cast<unsigned long long>(fingerprint));
        return std::filesystem::path(path).parent_path() / "shader-cache" / "v1" / name;
    }();
    return directory;
}

ID3DBlob* D3D12Backend::Compile(const std::string& src, const char* entry, const char* profile, uint64_t& hash) {
    // one source can hold several entry points. The shared AO HLSL has a Vulkan-only branch.
    const std::string source = "#define TCNYC_VULKAN 0\n" + src;
    uint64_t profile_seed = Hash(profile, std::strlen(profile));
    uint64_t entry_seed = Hash(entry, std::strlen(entry), profile_seed);
    hash = Hash(source.data(), source.size(), entry_seed);
    // Guest microcode should not create an unbounded cache over a long session.
    constexpr size_t kMaxShaderBlobs = 4096;
    if (blobs_.size() >= kMaxShaderBlobs) {
        LOGW("d3d12: shader cache reached %zu entries, clearing it", blobs_.size());
        blobs_.clear();
        shader_src_.clear();
    }
    auto it = blobs_.find(hash);
    if (it != blobs_.end()) return it->second.Get();
    ComPtr<ID3DBlob> code;
    ComPtr<ID3DBlob> err;
    std::filesystem::path cache;
    if (!ShaderCacheDirectory().empty()) {
        char name[24];
        std::snprintf(name, sizeof(name), "%016llX.dxbc", static_cast<unsigned long long>(hash));
        cache = ShaderCacheDirectory() / name;
        auto bytes = ReadShaderCache(cache, hash);
        if (!bytes.empty()) {
            ComPtr<ID3D11ShaderReflection> reflection;
            // mingw's IID_PPV_ARGS wants a __mingw_uuidof specialization its import
            // libraries do not define; the reflection IID is fixed.
            static const GUID kShaderReflectionIid = {
                0x8d536ca1, 0x0cca, 0x4956, {0xa8, 0x37, 0x78, 0x69, 0x63, 0x75, 0x55, 0x84}};
            if (SUCCEEDED(D3DReflect(bytes.data(), bytes.size(), kShaderReflectionIid,
                                     reinterpret_cast<void**>(reflection.GetAddressOf()))) &&
                SUCCEEDED(D3DCreateBlob(bytes.size(), &code))) {
                std::memcpy(code->GetBufferPointer(), bytes.data(), bytes.size());
                ++g_perf.shader_hits;
                blobs_[hash] = code;
                return code.Get();
            }
        }
    }
    PerfScope perf_compile{g_perf.compile};
    g_perf.shaders++;
    HRESULT hr = D3DCompile(source.data(), source.size(), nullptr, nullptr, nullptr, entry, profile,
                            kShaderCompileFlags, 0, &code, &err);
    if (FAILED(hr)) {
        LOGE("d3d12: %s compile failed: %s", profile, err ? static_cast<const char*>(err->GetBufferPointer()) : "?");
        LOGE("%s", source.c_str());
        blobs_[hash] = nullptr;
        return nullptr;
    }
    if (!cache.empty()) WriteShaderCache(cache, hash, code->GetBufferPointer(), code->GetBufferSize());
    blobs_[hash] = code;
    return code.Get();
}

// a full screen pass's pipeline: solid, no culling, one target, writing all channels; callers set the blend
D3D12_GRAPHICS_PIPELINE_STATE_DESC D3D12Backend::FullScreenPipelineDesc(ID3DBlob* vs, ID3DBlob* ps,
                                                                        DXGI_FORMAT rtv) const {
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = D3D12_COLOR_WRITE_ENABLE_ALL;
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.DepthClipEnable = TRUE;
    d.PrimitiveTopologyType = D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    d.NumRenderTargets = 1;
    d.RTVFormats[0] = rtv;
    d.SampleDesc.Count = 1;
    return d;
}

ID3D12PipelineState* D3D12Backend::EnsureBlitPipeline(DXGI_FORMAT rtv, bool over, uint32_t samples) {
    auto& pso = blit_psos_[uint64_t(rtv) | uint64_t(over) << 32 | uint64_t(samples) << 33];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(kBlitHlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(kBlitHlsl, "psmain", "ps_5_0", h);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv);
    d.SampleDesc.Count = samples;
    if (over) {
        auto& blend = d.BlendState.RenderTarget[0];
        // premultiplied colour, the target keeps its alpha
        blend.BlendEnable = TRUE;
        blend.SrcBlend = D3D12_BLEND_ONE;
        blend.DestBlend = D3D12_BLEND_INV_SRC_ALPHA;
        blend.SrcBlendAlpha = D3D12_BLEND_ZERO;
        blend.DestBlendAlpha = D3D12_BLEND_ONE;
        blend.BlendOp = blend.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    CreatePipeline(d, pso);
    return pso.Get();
}

// Pass's pipeline for the shader, kept in blit_psos_; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsurePassPipeline(const char* hlsl, const char* entry, DXGI_FORMAT rtv,
                                                      float blend) {
    uint64_t seed = uint64_t(rtv) << 1 | (blend >= 0 ? 1 : 0);
    uint64_t source_seed = Hash(hlsl, std::strlen(hlsl), seed);
    auto& pso = blit_psos_[Hash(entry, std::strlen(entry), source_seed)];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(hlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(hlsl, entry, "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv);
    if (blend >= 0) {
        auto& b = d.BlendState.RenderTarget[0];
        b.BlendEnable = TRUE;
        b.SrcBlend = b.SrcBlendAlpha = D3D12_BLEND_BLEND_FACTOR;
        b.DestBlend = b.DestBlendAlpha = D3D12_BLEND_INV_BLEND_FACTOR;
        b.BlendOp = b.BlendOpAlpha = D3D12_BLEND_OP_ADD;
    }
    if (FAILED(CreatePipeline(d, pso))) return nullptr;
    return pso.Get();
}

void D3D12Backend::Pass(const char* hlsl, const char* entry, DXGI_FORMAT rtv, D3D12_CPU_DESCRIPTOR_HANDLE rh,
                        uint32_t w, uint32_t h, ID3D12Resource* const src[3], const DXGI_FORMAT fmt[3],
                        const float p[8], float blend) {
    ID3D12PipelineState* pso = EnsurePassPipeline(hlsl, entry, rtv, blend);
    if (!pso) return;
    uint32_t slot = AllocSrvSlots(4);
    for (int i = 0; i < 4; i++) {
        D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
        sv.Format = i < 3 && src[i] ? fmt[i] : kBgra;
        sv.ViewDimension = i < 3 && src[i] && src[i]->GetDesc().SampleDesc.Count > 1
                               ? D3D12_SRV_DIMENSION_TEXTURE2DMS : D3D12_SRV_DIMENSION_TEXTURE2D;
        sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
        if (sv.ViewDimension == D3D12_SRV_DIMENSION_TEXTURE2D) sv.Texture2D.MipLevels = 1;
        dev_->CreateShaderResourceView(i < 3 ? src[i] : nullptr, &sv, Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    Alloc cb = Upload(256, 256);
    if (!cb.res) return;
    std::memcpy(cb.cpu, p, 8 * sizeof(float));
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    SetFullViewport(w, h);
    if (blend >= 0) {
        float f[4] = {blend, blend, blend, blend};
        cl_->OMSetBlendFactor(f);
        binds_ = {};
    }
    DrawFullScreen(pso, cb.gpu, slot);
    bound_rt_ = bound_ds_ = nullptr;
}

// a copy of the target to read while drawing to it
ID3D12Resource* D3D12Backend::Feedback(Target& t) {
    if (DeferredPendingResource(t.res.Get())) DeferredBarrier("scene target copied for feedback");
    if (!t.fb) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        D3D12_RESOURCE_DESC td = t.res->GetDesc();
        td.Flags = D3D12_RESOURCE_FLAG_NONE;
        t.fb_state = D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, t.fb_state, nullptr,
                                                 IID_PPV_ARGS(&t.fb)))) {
            return nullptr;
        }
    }
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_COPY_DEST);
    cl_->CopyResource(t.fb.Get(), t.res.Get());
    Transition(t.fb.Get(), t.fb_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return t.fb.Get();
}

// the key's vertex attributes, ATTR0 to ATTR15 in register order; returns how many
uint32_t FillInputLayout(const PsoKey& k, D3D12_INPUT_ELEMENT_DESC el[16]) {
    uint32_t n = 0;
    for (uint32_t r = 0; r < 16; r++) {
        if (!((k.regs >> r) & 1)) continue;
        uint32_t e = k.elements[r];
        el[n++] = {"ATTR", r, DXGI_FORMAT(e & 0xFF), (e >> 8) & 0xFF, e >> 16,
                   D3D12_INPUT_CLASSIFICATION_PER_VERTEX_DATA, 0};
    }
    return n;
}

D3D12_PRIMITIVE_TOPOLOGY_TYPE TopologyType(uint32_t topo) {
    switch (topo) {
        case 0: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_POINT;
        case 1: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_LINE;
        default: return D3D12_PRIMITIVE_TOPOLOGY_TYPE_TRIANGLE;
    }
}

constexpr uint32_t kFillWireframe = 0x1B01;  // D3DFILL_WIREFRAME
constexpr uint32_t kCullCw = 0x900;          // D3DCULL_CW

// a D3DCMP test on reversed depth: less and greater trade places
D3D12_COMPARISON_FUNC ReversedCmp(uint32_t v) {
    static const uint8_t mirror[8] = {0, 4, 2, 6, 1, 5, 3, 7};
    return D3D12_COMPARISON_FUNC(mirror[Cmp(v) - 1] + 1);
}

void FillDepthStencilDesc(const PsoKey& k, D3D12_DEPTH_STENCIL_DESC& ds) {
    ds.DepthEnable = k.zen;
    ds.DepthWriteMask = k.zwrite ? D3D12_DEPTH_WRITE_MASK_ALL : D3D12_DEPTH_WRITE_MASK_ZERO;
    ds.DepthFunc = ReversedCmp(k.zfunc);
    ds.StencilEnable = k.sten;
    ds.StencilReadMask = UINT8(k.smask);
    ds.StencilWriteMask = UINT8(k.swmask);
    D3D12_DEPTH_STENCILOP_DESC so{StencilOp(k.sfail), StencilOp(k.szfail), StencilOp(k.spass), Cmp(k.sfunc)};
    ds.FrontFace = ds.BackFace = so;
}

// everything of the pipeline but its input layout
void D3D12Backend::FillPipelineDesc(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps,
                                    D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) const {
    d.pRootSignature = root_.Get();
    d.VS = {vs->GetBufferPointer(), vs->GetBufferSize()};
    d.PS = {ps->GetBufferPointer(), ps->GetBufferSize()};
    auto& b = d.BlendState.RenderTarget[0];
    b.BlendEnable = k.blend;
    b.SrcBlend = Blend(k.src, false);
    b.DestBlend = Blend(k.dst, false);
    b.BlendOp = BlendOp(k.op);
    b.SrcBlendAlpha = Blend(k.src, true);
    b.DestBlendAlpha = Blend(k.dst, true);
    b.BlendOpAlpha = BlendOp(k.op);
    b.RenderTargetWriteMask = UINT8(k.wmask);
    d.SampleMask = UINT_MAX;
    d.RasterizerState.FillMode = k.fill == kFillWireframe ? D3D12_FILL_MODE_WIREFRAME : D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = k.cull ? D3D12_CULL_MODE_BACK : D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = k.ccw;
    d.RasterizerState.DepthBias = k.zbias;
    d.RasterizerState.SlopeScaledDepthBias = k.zslope;
    // with no depth buffer nothing clips on z: a title's lines at z 1 (the edge of the range) reach 0 or a rounding
    // below in reversed depth, where D3D12 would drop them
    d.RasterizerState.DepthClipEnable = k.has_ds ? TRUE : FALSE;
    FillDepthStencilDesc(k, d.DepthStencilState);
    d.PrimitiveTopologyType = TopologyType(k.topo);
    d.NumRenderTargets = k.has_rt ? 1 : 0;
    d.RTVFormats[0] = k.has_rt ? DXGI_FORMAT(k.rtfmt) : DXGI_FORMAT_UNKNOWN;
    d.DSVFormat = k.has_ds ? kDepthFormat : DXGI_FORMAT_UNKNOWN;
    d.SampleDesc.Count = k.samples;
}

// A point sprite is a square of the program's point size in console pixels (oPts, which SpriteSizeHlsl passes in
// tc3.x) with the sprite's coordinates across it, 0 to 1 from the top left, as the console makes them for stage 3.
std::string SpriteHlsl() {
    std::string s = PointHlsl();
    s.erase(s.find("[maxvertexcount"));
    return s + R"([maxvertexcount(4)]
void main(point VOut v[1], inout TriangleStream<VOut> strip) {
    float2 half_size = v[0].tc3.x * v[0].pos.w / surf.xy;  // in clip units, the target spans 2 across surf.xy pixels
    for (int i = 0; i < 4; i++) {
        VOut o = v[0];
        float2 corner = float2(i & 1, i >> 1);
        o.pos.x += (corner.x * 2 - 1) * half_size.x;
        o.pos.y -= (corner.y * 2 - 1) * half_size.y;
        o.tc0 = float4(corner, 0, 1);  // stage 3 drawn as stage 0 (SpriteStageZero)
        o.tc3 = o.tc0;
        strip.Append(o);
    }
}
)";
}

// a point is drawn as its whole console pixel, or a sprite as its square, by a geometry shader, solid, front facing
// and never culled
void D3D12Backend::ExpandPoints(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d, bool sprite) {
    uint64_t hash;
    ID3DBlob* gs = Compile(sprite ? SpriteHlsl() : PointHlsl(), "main", "gs_5_0", hash);
    if (!gs) return;
    d.GS = {gs->GetBufferPointer(), gs->GetBufferSize()};
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
    d.RasterizerState.FrontCounterClockwise = FALSE;
}

void D3D12Backend::ExpandLines(D3D12_GRAPHICS_PIPELINE_STATE_DESC& d) {
    uint64_t hash;
    ID3DBlob* gs = Compile(LineHlsl(), "main", "gs_5_0", hash);
    if (!gs) return;
    d.GS = {gs->GetBufferPointer(), gs->GetBufferSize()};
    d.RasterizerState.FillMode = D3D12_FILL_MODE_SOLID;
    d.RasterizerState.CullMode = D3D12_CULL_MODE_NONE;
}

float D3D12Backend::TargetScale() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw) return 1;
    return float(t->w) / float(t->gw);
}

float D3D12Backend::TargetScaleY() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gh) return 1;
    return float(t->h) / float(t->gh);
}

// Screen space (pretransformed) draws onto a back buffer sized target are the title's 2D layer: HUD, text, menus,
// full screen pictures and movies, fades and passes. Titles lay that layer out for 16:9 at most; on a wider frame it
// would stretch, so it is drawn at 16:9 in the middle. Draws that span the whole width are pictures (a texture:
// movies, legal and loading screens), kept at 16:9 too, or passes, which still cover the frame: fades and letterbox
// bars (no texture) and composites of the title's own targets (bloom, blur).
float D3D12Backend::UiSqueeze(const State& s, const Assembled& a, const DrawTextures& stages) const {
    constexpr float kUiAspect = 16.0f / 9.0f;
    if (!bound_rt_ || bound_rt_->gw != guest_w_ || bound_rt_->gh != guest_h_) return 1;
    if (bound_rt_->cpu && bound_rt_->scene_frame != frame_) return 1;  // over a CPU picture: the whole frame is shown at 16:9
    if (aspect_ <= kUiAspect + 1e-3f) return 1;
    const float squeeze = kUiAspect / aspect_;
    if (!a.pretransformed) {
        // A 3D draw into a sub-rectangle of the main target is a menu preview
        // (locker, garage, character and car select), not the full-screen
        // world. The panel around it is 2D and moves with the centred UI, so
        // the preview has to move with it or it ends up off centre on a wider
        // frame. Full-width and full-height viewports stay unsqueezed.
        const bool preview = s.viewport.w > 0 && s.viewport.h > 0 && s.viewport.w < guest_w_ - 1 &&
                             s.viewport.h < guest_h_ - 1;
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
    const bool full_width = lo <= 0.5f && hi >= float(guest_w_) - 0.5f;
    if (!full_width) return squeeze;
    const bool picture = stages.res[0] && !stages.rt[0];
    return picture ? squeeze : 1.0f;
}

// the XDK's screen space offset puts the console's positions half a pixel right and down of the host's, unless a
// program writes them without c-37; render target reads move back that far (TexUv)
float D3D12Backend::RenderTargetShift(const State& s, const DrawShaders& sh) const {
    bool offset = !sh.prog || ReadsViewportOffset(s.vs->program);
    return offset ? -0.5f : 0.0f;
}

HRESULT D3D12Backend::CreatePipeline(const D3D12_GRAPHICS_PIPELINE_STATE_DESC& desc,
                                     ComPtr<ID3D12PipelineState>& pso) {
    PerfScope perf_pipeline{g_perf.pipeline};
    ++g_perf.psos;
    return dev_->CreateGraphicsPipelineState(&desc, IID_PPV_ARGS(&pso));
}

ID3D12PipelineState* D3D12Backend::EnsurePipeline(const PsoKey& k, ID3DBlob* vs, ID3DBlob* ps) {
    uint64_t h = Hash(&k, sizeof(k));
    auto it = psos_.find(h);
    if (it != psos_.end()) return it->second.Get();
    D3D12_INPUT_ELEMENT_DESC el[16];
    uint32_t n = FillInputLayout(k, el);
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d{};
    d.InputLayout = {el, n};
    FillPipelineDesc(k, vs, ps, d);
    if (k.topo == uint32_t(Topology::Points)) ExpandPoints(d, k.sprite);
    else if (k.topo == uint32_t(Topology::Lines)) ExpandLines(d);
    ComPtr<ID3D12PipelineState> pso;
    const HRESULT hr = CreatePipeline(d, pso);
    if (FAILED(hr)) {
        const auto& b = d.BlendState.RenderTarget[0];
        const auto& r = d.RasterizerState;
        static int failed = 0;
        if (failed++ < 16) LOGE("d3d12: pipeline creation failed (0x%08lX, device removed reason 0x%08lX): blend %d logic %d op %d src %d dst %d bop %d srcA %d dstA %d bopA %d "
                      "mask %X | fill %d cull %d bias %d clamp %.3f slope %.3f | rt %d fmt %d ds %d samples %d | depth %d "
                      "zfunc %d stencil %d sfunc %d", hr, dev_->GetDeviceRemovedReason(),
                  b.BlendEnable, b.LogicOpEnable, b.LogicOp, b.SrcBlend, b.DestBlend, b.BlendOp, b.SrcBlendAlpha,
                  b.DestBlendAlpha, b.BlendOpAlpha, b.RenderTargetWriteMask, r.FillMode, r.CullMode, r.DepthBias,
                  r.DepthBiasClamp, r.SlopeScaledDepthBias, d.NumRenderTargets, d.RTVFormats[0], d.DSVFormat,
                  d.SampleDesc.Count, d.DepthStencilState.DepthEnable, d.DepthStencilState.DepthFunc,
                  d.DepthStencilState.StencilEnable, d.DepthStencilState.FrontFace.StencilFunc);
    }
    psos_[h] = pso;
    return pso.Get();
}

TexKind TextureKind(const TextureDesc& d) {
    if (d.cube) return kTexCube;
    if (d.depth > 1) return kTex3D;
    return kTex2D;
}

constexpr float kUpscaledMin = 1.01f;  // host texels per console texel past rounding: the target is upscaled

ID3D12Resource* D3D12Backend::BindTargetTexture(const State& s, int stage, const TextureDesc& d, Target& t, bool box,
                                                DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    if (&t == draw_ds_) return nullptr;
    if (defer_writes_.count(t.res.Get()) || (t.resolved && defer_writes_.count(t.resolved.Get()))) {
        // The draw needs the scene as it stands. Materialise the pending bundles, then keep
        // this draw immediate so it samples what it was just made to wait for.
        if (!bundle_) {
            FlushDeferredScene();
            defer_flush_conflict_ = true;
        } else {
            DeferredFallback("scene target sampled after a deferred draw");
        }
    }
    bool feedback = &t == draw_rt_;
    stages.rt[stage] = true;
    info.flags |= kTexTarget;
    if (!t.depth && LatchCpuWrite(t)) {
        Surface su;
        su.header = s.textures[size_t(stage)];
        UploadTarget(t, su);
    }
    info.kind = kTex2D;
    // a pass shrinking it averages each tap over a console texel, else upscaled taps skip texels and ghost
    Target* rt = FindTarget(s.rt, false);
    float k = float(t.w) / (t.gw * stages.k[stage]);
    if (!t.depth && box && k > kUpscaledMin && rt && rt != &t && (t.gw > rt->gw || t.gh > rt->gh)) {
        stages.wide[stage] = k;
        info.flags |= kTexWide;
    }
    if (ext_) ext_->Sampled(d, &t, 0, info);
    sv.Format = t.depth ? kDepthRead : t.fmt;
    if (t.depth) info.shadow = ShadowDepthKind(d.format);
    if (t.res->GetDesc().SampleDesc.Count > 1) {
        ID3D12Resource* resolved = t.depth ? ResolveDepth(t) : ResolveColor(t);
        if (t.depth) sv.Format = DXGI_FORMAT_R32_FLOAT;
        if (resolved) Transition(resolved, t.resolved_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return resolved;
    }
    // the console reads the pixels it is about to overwrite: give the draw a copy
    if (feedback) return Feedback(t);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    info.kind = kTex2D;
    return t.res.Get();
}

bool SameTexture(const Tex& tx, const TextureDesc& d) {
    return tx.res && tx.d.format == d.format && tx.d.width == d.width && tx.d.height == d.height &&
           tx.d.depth == d.depth && tx.d.levels == d.levels && tx.d.cube == d.cube && tx.d.pitch == d.pitch;
}

// false when the texture could not be made, its entry dropped
bool D3D12Backend::CreateTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, uint64_t key) {
    g_perf.newtex++;
    if (tx.res) CurrentFrame().garbage.push_back(tx.res);
    tx = Tex{};
    tx.d = d;
    tx.fmt = HostFormat(fi.host);
    bool bc = fi.compressed;
    uint32_t w = d.width;
    uint32_t h = d.height;
    tx.levels = d.levels;
    if (bc && ((w & 3) || (h & 3))) {
        w = (w + 3) & ~3u;
        h = (h + 3) & ~3u;
        tx.levels = 1;
    }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td{};
    td.Dimension = d.depth > 1 ? D3D12_RESOURCE_DIMENSION_TEXTURE3D : D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    td.Width = w;
    td.Height = h;
    td.DepthOrArraySize = UINT16(d.cube ? 6 : d.depth);
    td.MipLevels = UINT16(tx.levels);
    td.Format = tx.fmt;
    td.SampleDesc.Count = 1;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&tx.res)))) {
        textures_.erase(key);
        return false;
    }
    return true;
}

void CopyLevelRows(uint8_t* out, const D3D12_PLACED_SUBRESOURCE_FOOTPRINT& fp, UINT rows, UINT64 row_size,
                   const std::vector<uint8_t>& px, uint32_t pitch) {
    uint32_t slices = fp.Footprint.Depth;
    size_t src_rows = pitch ? px.size() / pitch / std::max(slices, 1u) : 0;
    for (uint32_t z = 0; z < slices; z++) {
        for (uint32_t y = 0; y < rows; y++) {
            uint8_t* dst = out + (size_t(z) * rows + y) * fp.Footprint.RowPitch;
            if (y < src_rows) {
                std::memcpy(dst, &px[(size_t(z) * src_rows + y) * pitch], std::min<size_t>(pitch, row_size));
            } else {
                std::memset(dst, 0, size_t(row_size));
            }
        }
    }
}

void D3D12Backend::UploadTextureLevel(Tex& tx, uint32_t f, uint32_t l, const std::vector<uint8_t>& px,
                                      uint32_t pitch) {
    D3D12_RESOURCE_DESC rd = tx.res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    uint32_t sub = f * tx.levels + l;
    dev_->GetCopyableFootprints(&rd, sub, 1, 0, &fp, &rows, &row_size, &bytes);
    Alloc a = Upload(bytes, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (!a.res) return;
    CopyLevelRows(a.cpu, fp, rows, row_size, px, pitch);
    fp.Offset = a.off;
    D3D12_TEXTURE_COPY_LOCATION dl{tx.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    dl.SubresourceIndex = sub;
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint = fp;
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

void D3D12Backend::UploadTexture(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint8_t* p,
                                 const uint32_t* colours, bool same) {
    D3D12_RESOURCE_STATES st = same ? D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE : D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_COPY_DEST);
    uint32_t faces = d.cube ? 6 : 1;
    std::vector<uint8_t> px;
    for (uint32_t f = 0; f < faces; f++) {
        for (uint32_t l = 0; l < tx.levels; l++) {
            uint32_t pitch;
            DecodeLevel(d, l, f, p, colours, px, pitch);
            if (ext_) ext_->Decoded(d, fi, f, l, px, pitch, tx.flags);
            if (tx.prompt_style != input::PromptStyle::Original) ReplaceControllerGlyphs(px, pitch, tx.prompt_style);
            UploadTextureLevel(tx, f, l, px, pitch);
        }
    }
    Transition(tx.res.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

void FillTextureView(const Tex& tx, const TextureDesc& d, D3D12_SHADER_RESOURCE_VIEW_DESC& sv) {
    sv.Format = tx.fmt;
    if (d.cube) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURECUBE;
        sv.TextureCube.MipLevels = tx.levels;
    } else if (d.depth > 1) {
        sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE3D;
        sv.Texture3D.MipLevels = tx.levels;
    } else {
        sv.Texture2D.MipLevels = tx.levels;
    }
}

// a P8 texture's palette: its colours, their address (part of the texture's key) and their hash, all 0 without one
struct StagePalette {
    uint32_t colours[256] = {};
    uint32_t va = 0;
    uint64_t hash = 0;
};

// a P8 texture is decoded with the stage's palette: 256 colours, fewer by the size code in the top two bits
void ReadStagePalette(const State& s, int stage, const TextureDesc& d, StagePalette& palette) {
    constexpr uint32_t kFmtP8 = 0x0B;          // D3DFMT_P8
    constexpr uint32_t kPaletteCommon = 0;     // D3DPalette: Common, its top two bits the size code
    constexpr uint32_t kPaletteData = 4;       // D3DPalette: Data, physical address of the colours
    constexpr uint32_t kPaletteSizeShift = 30;
    if (d.format != kFmtP8 || !s.palettes[size_t(stage)]) return;
    uint32_t header = s.palettes[size_t(stage)];
    palette.va = mem::VirtualFromPhysical(kernel::R32(header + kPaletteData));
    uint32_t size_code = kernel::R32(header + kPaletteCommon) >> kPaletteSizeShift;
    size_t bytes = (std::size(palette.colours) >> size_code) * sizeof(uint32_t);
    std::memcpy(palette.colours, kernel::HostPtr(palette.va), bytes);
    palette.hash = Hash(palette.colours, sizeof(palette.colours));
}

// a texture in guest memory: made again when its layout changed, uploaded again when its bytes did
ID3D12Resource* D3D12Backend::BindGuestTexture(const State& s, int stage, const TextureDesc& d, const FormatInfo& fi,
                                               DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    StagePalette palette;
    ReadStagePalette(s, stage, d, palette);
    const uint64_t key = uint64_t(palette.va) << 32 | d.data;
    const uint32_t* colours = palette.va ? palette.colours : nullptr;
    Tex& tx = textures_[key];
    bool same = SameTexture(tx, d);
    if (!same && !CreateTexture(tx, d, fi, key)) return nullptr;
    // cheap change check once per frame: lock epochs plus a sample of the bytes
    const auto prompt_style = IsPromptAtlas(d) ? input::ControllerPromptStyle() : input::PromptStyle::Original;
    if (tx.frame != presents_ + 1 || !same || tx.palette != palette.hash || tx.prompt_style != prompt_style) {
        const uint64_t total = TotalSize(d);
        tx.frame = presents_ + 1;
        tx.palette = palette.hash;
        const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
        uint64_t sig;
        {
            PerfScope perf_sig{g_perf.sig};
            sig = MemSig(d.data, total) ^ DirtyEpoch(d.data) ^ palette.hash;
        }
        if (sig != tx.sig || !same || tx.prompt_style != prompt_style) {
            tx.prompt_style = prompt_style;
            PerfScope perf_up{g_perf.texup};
            g_perf.texups++;
            g_perf.texup_bytes += total;
            tx.sig = sig;
            UploadTexture(tx, d, fi, p, colours, same);
            tx.pack_gen = 0;
        }
    }
    if (tx.pack_gen != texturedumper::Generation()) LatchPackKey(tx, d, fi, colours);
    if (tx.pack_key) LatchPackImage(tx);
    const Tex& drawn = tx.swap ? *tx.swap : tx;
    FillTextureView(drawn, d, stages.sv[stage]);
    if (fi.depth) info.shadow = ShadowDepthKind(d.format);
    if (ext_) ext_->Sampled(d, nullptr, tx.flags, info);
    return drawn.res.Get();
}

// after new bytes or a texture setting change: the name the pack and the dumps know the bytes by
void D3D12Backend::LatchPackKey(Tex& tx, const TextureDesc& d, const FormatInfo& fi, const uint32_t* colours) {
    const uint8_t* p = static_cast<const uint8_t*>(kernel::HostPtr(d.data));
    tx.pack_gen = texturedumper::Generation();
    tx.swap = nullptr;
    uint64_t key = texturedumper::Key(d, fi, p, colours);
    texturedumper::Dump(key, d, p, colours);
    // dumping alone keys the bytes too: the pack image is not drawn then
    tx.pack_key = texturedumper::Replacing() ? key : 0;
}

// the pack image for tx's bytes once it has loaded; textures with the same bytes share its GPU copy
void D3D12Backend::LatchPackImage(Tex& tx) {
    auto it = pack_textures_.find(tx.pack_key);
    if (it == pack_textures_.end()) {
        texturedumper::Image img;
        texturedumper::Lookup got = texturedumper::Take(tx.pack_key, img);
        if (got == texturedumper::Lookup::Loading) return;
        if (got == texturedumper::Lookup::None) {
            tx.pack_key = 0;
            return;
        }
        it = pack_textures_.emplace(tx.pack_key, Tex{}).first;
        CreatePackTexture(it->second, img);
    }
    if (it->second.res) tx.swap = &it->second;
    tx.pack_key = 0;
}

void D3D12Backend::CreatePackTexture(Tex& r, const texturedumper::Image& img) {
    r.fmt = img.format;
    r.levels = uint32_t(img.levels.size());
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(r.fmt, img.width, img.height);
    td.MipLevels = UINT16(r.levels);
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                             IID_PPV_ARGS(&r.res)))) {
        LOGE("texture pack: a %ux%u image could not be made", img.width, img.height);
        return;
    }
    for (uint32_t l = 0; l < r.levels; l++) UploadTextureLevel(r, 0, l, img.levels[l].px, img.levels[l].pitch);
    D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_COPY_DEST;
    Transition(r.res.Get(), st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
}

// the pack's files changed: each image is made again when next drawn; Generation moved on, so every texture lets
// go of its old copy before it draws
void D3D12Backend::ReleasePackTextures() {
    for (auto& entry : pack_textures_) CurrentFrame().garbage.push_back(entry.second.res);
    pack_textures_.clear();
}

ID3D12Resource* D3D12Backend::BindTexture(const State& s, int stage, DrawTextures& stages) {
    TexInfo& info = stages.info[stage];
    D3D12_SHADER_RESOURCE_VIEW_DESC& sv = stages.sv[stage];
    info = {};
    sv = {};
    stages.rt[stage] = false;
    stages.k[stage] = 1;
    stages.wide[stage] = 0;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    sv.Texture2D.MipLevels = 1;
    TextureDesc d;
    if (!s.textures[size_t(stage)] || !Describe(s.textures[size_t(stage)], d)) return nullptr;
    stages.desc[stage] = d;
    const FormatInfo& fi = Format(d.format);
    info.kind = TextureKind(d);
    info.rect = d.linear;

    // rendered by the GPU: sample the target itself; DXT data loaded over a target's memory is not it
    uint64_t alt = 0;
    bool box = true;
    if (ext_) ext_->Sample(d.data, fi.depth, stage, alt, stages.k[stage], box);
    auto tit = targets_.find(SizedTargetKey(d.data, fi.depth, d.width, d.height) | alt);
    if (tit != targets_.end() && !d.cube && d.depth == 1 && !fi.compressed) {
        // A target sampled here but not drawn into this frame must stay live: the sweep only
        // retires targets unused for kStaleFrames, and the submitted frame may still read it.
        UpdateTargetUsed(d.data);
        return BindTargetTexture(s, stage, d, tit->second, box, stages);
    }
    return BindGuestTexture(s, stage, d, fi, stages);
}

// D3DTEXF: 1 point, 2 linear, 3 anisotropic
constexpr uint32_t kTexfLinear = 2;
constexpr uint32_t kTexfAnisotropic = 3;
// D3DTADDRESS values the samplers test
constexpr uint32_t kTaddressWrap = 1;
constexpr uint32_t kTaddressClamp = 3;

D3D12_SAMPLER_DESC StageSampler(const State& s, int i, const DrawTextures& stages, bool clamp, uint32_t aniso) {
    D3D12_SAMPLER_DESC sd = {};
    const auto& t = s.tss[size_t(i)];
    uint32_t mag = t[kTssMagFilter];
    uint32_t min = t[kTssMinFilter];
    uint32_t mip = t[kTssMipFilter];
    auto lin = [](uint32_t f) { return f >= kTexfLinear; };
    D3D12_FILTER_TYPE fmin = lin(min) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmag = lin(mag) ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    D3D12_FILTER_TYPE fmip = mip == kTexfLinear ? D3D12_FILTER_TYPE_LINEAR : D3D12_FILTER_TYPE_POINT;
    sd.Filter = D3D12_FILTER(fmin << 4 | fmag << 2 | fmip);
    bool anisotropic = min == kTexfAnisotropic || mag == kTexfAnisotropic;
    uint32_t an = anisotropic ? std::max<uint32_t>(t[kTssMaxAnisotropy], 1) : 1;
    // forced only where the title filters across mip levels and the texture has them
    if (aniso && lin(min) && mip && stages.sv[i].Texture2D.MipLevels > 1 && !stages.info[i].shadow) an = aniso;
    if (an > 1) {
        sd.Filter = D3D12_FILTER_ANISOTROPIC;
        sd.MaxAnisotropy = std::min<uint32_t>(an, 16);
    }
    // clamp: the draw's coordinates all lie in 0..1 (UnitTexcoords), so wrap is read as clamp
    sd.AddressU = Address(clamp && t[kTssAddressU] == kTaddressWrap ? kTaddressClamp : t[kTssAddressU]);
    sd.AddressV = Address(clamp && t[kTssAddressV] == kTaddressWrap ? kTaddressClamp : t[kTssAddressV]);
    sd.AddressW = Address(t[kTssAddressW]);
    sd.MipLODBias = Bits(t[kTssMipLodBias]);
    sd.MinLOD = float(t[kTssMaxMipLevel]);
    sd.MaxLOD = mip ? D3D12_FLOAT32_MAX : sd.MinLOD;
    Color(t[kTssBorderColor], sd.BorderColor);
    if (stages.info[i].shadow) {
        // the console tests texel OP r, D3D12 tests r OP texel; with both reversed (g_reversed_depth) it is the same OP
        sd.Filter = lin(min) || lin(mag) ? D3D12_FILTER_COMPARISON_MIN_MAG_LINEAR_MIP_POINT
                                         : D3D12_FILTER_COMPARISON_MIN_MAG_MIP_POINT;
        sd.ComparisonFunc = Cmp(s.rs[kRsShadowFunc]);
        sd.MaxAnisotropy = 0;
    }
    return sd;
}

uint32_t D3D12Backend::EnsureStageSamplers(const State& s, const DrawTextures& stages, bool clamp) {
    D3D12_SAMPLER_DESC sd[4];
    uint32_t aniso = render_.aniso;
    for (int i = 0; i < 4; i++) sd[i] = StageSampler(s, i, stages, clamp, aniso);
    uint64_t key = Hash(sd, sizeof(sd));
    const uint32_t frame_index = uint32_t(frame_ % kFrames);
    auto& cache = samplers_[frame_index];
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    const uint32_t start = frame_index * kSamplersPerFrame;
    const uint32_t end = start + kSamplersPerFrame;
    Frame& f = CurrentFrame();
    if (f.smp < start) f.smp = start;
    if (f.smp + 4 > end) {
        // The frame's sampler region is full. Reusing slot 0 would rewrite descriptors an earlier
        // draw in the same command list still references; use a fixed fallback set instead.
        LOG_FIRST(4, "d3d12: sampler ring full within a frame, using the first set");
        return 0;
    }
    uint32_t base = f.smp;
    f.smp += 4;
    for (int i = 0; i < 4; i++) dev_->CreateSampler(&sd[i], Cpu(smp_heap_.Get(), base + i, smp_inc_));
    cache[key] = base;
    return base;
}

constexpr uint32_t kQuadVertices = 6;  // a quad drawn as two triangles

DrawShaders ClassifyShaders(const State& s, const Assembled& a, uint32_t count) {
    DrawShaders sh;
    // a shader made from a declaration alone (no program) is the fixed function pipeline on those inputs
    sh.prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    // up to a quad with no depth test: it may be a pass over the whole target, see QuadHlsl; points never are
    bool points = a.topology == Topology::Points;
    sh.quad = count <= kQuadVertices && !s.rs[kRsZEnable] && !points;
    sh.fixed_transform = !sh.prog && !a.pretransformed;
    sh.sprite = points && sh.prog && s.rs[kRsPointSpriteEnable];
    if (!sh.prog) sh.fk = MakeFixedKey(s);
    return sh;
}

// the program's hash is cached in the guest shader object
void EnsureProgramHash(const State& s) {
    if (!s.vs->hash) s.vs->hash = Hash(s.vs->program.data(), s.vs->program.size() * 4) | 1;
}

// vertex shader cache key salts
constexpr uint64_t kDeclarationSalt = 0x10000;    // fixed function seed, above the 16 bit register mask
constexpr uint64_t kPretransformedSalt = 0x5A5A;  // screen space fixed function vertices, apart from untransformed
constexpr uint64_t kQuadSalt = 0xC3C30000;        // the QuadHlsl variant, apart from the plain shader
constexpr uint64_t kSpriteSalt = 0x3C3C0000;      // the SpriteSizeHlsl variant

uint64_t VertexKey(const State& s, const Assembled& a, const DrawShaders& sh) {
    uint64_t seed = sh.prog ? s.vs->hash ^ a.regs : Hash(&s.vs_handle, 4, a.regs | kDeclarationSalt);
    uint64_t vkey = Hash(&a.decode, 8, seed);
    if (!sh.prog && a.pretransformed) vkey ^= kPretransformedSalt;
    if (sh.quad) vkey ^= kQuadSalt;
    if (sh.sprite) vkey ^= kSpriteSalt;
    if (!sh.prog) vkey = Hash(&sh.fk, sizeof(sh.fk), vkey);
    return vkey;
}

// Recognize the city's prelit material family in the translated program. The light count
// and declaration vary, but the normal, ambient mask and texture scale use the same inputs.
// a point sprite's program hands its point size to SpriteHlsl in tc3, whose coordinates the sprite makes
std::string SpriteSizeHlsl(std::string s) {
    const char* tc3 = "    o.tc3 = oT3;\n";
    size_t at = s.find(tc3);
    if (at == std::string::npos) {
        LOGE("d3d12: a vertex shader no longer fits SpriteSizeHlsl");
        return s;
    }
    s.replace(at, std::strlen(tc3), "    o.tc3 = oPts;\n");
    return s;
}

std::string VertexHlsl(const State& s, const Assembled& a, const DrawShaders& sh) {
    if (sh.prog) {
        std::string src = VertexProgramHlsl(s.vs->program, a.regs, a.decode);
        return sh.sprite ? SpriteSizeHlsl(std::move(src)) : src;
    }
    return FixedVertexHlsl(a.regs, a.pretransformed, a.decode, sh.fk);
}

// sets sh.vh and sh.vs_code, compiling the shader on a cache miss
void D3D12Backend::EnsureVertexShader(const State& s, const Assembled& a, DrawShaders& sh) {
    auto vit = shader_src_.find(sh.vkey);
    if (vit != shader_src_.end()) {
        sh.vs_code = blobs_[vit->second].Get();
        sh.vh = vit->second;
        return;
    }
    std::string src = ReversedDepthHlsl(VertexHlsl(s, a, sh));
    sh.vs_code = Compile(sh.quad ? QuadHlsl(std::move(src)) : src, "main", "vs_5_0", sh.vh);
    shader_src_[sh.vkey] = sh.vh;
}

bool D3D12Backend::BindStageTexturesAndTargets(const State& s, DrawTextures& stages) {
    draw_rt_ = bound_rt_;
    draw_ds_ = bound_ds_;
    {
        PerfScope perf_bind{g_perf.bind};
        for (int i = 0; i < 4; i++) stages.res[i] = BindTexture(s, i, stages);
    }
    // textures may have recorded copies that unbound the targets or moved them out of their target states
    if (bound_rt_ != draw_rt_ || bound_ds_ != draw_ds_) return BindTargets(s);
    if (bound_rt_) Transition(bound_rt_->res.Get(), bound_rt_->state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    if (bound_ds_) Transition(bound_ds_->res.Get(), bound_ds_->state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
    return true;
}

constexpr uint64_t kPixelKeySalt = 0x9E3779B97F4A7C15ull;

// sets sh.ph and sh.ps_code, compiling the shader on a cache miss
void D3D12Backend::EnsurePixelShader(const PixelKey& pk, DrawShaders& sh) {
    // pixel shaders share the map with vertex shaders, the salt keeps their keys apart
    auto pit = shader_src_.find(sh.pkey ^ kPixelKeySalt);
    if (pit != shader_src_.end()) {
        sh.ps_code = blobs_[pit->second].Get();
        sh.ph = pit->second;
        return;
    }
    sh.ps_code = Compile(PixelHlsl(pk), "main", "ps_5_0", sh.ph);
    shader_src_[sh.pkey ^ kPixelKeySalt] = sh.ph;
}

void KeyBlend(const State& s, PsoKey& k) {
    k.blend = SimpleState(s, kMBlendEnable) != 0;
    if (k.blend) {
        k.src = SimpleState(s, kMSrcBlend);
        k.dst = SimpleState(s, kMDestBlend);
        k.op = SimpleState(s, kMBlendOp);
    }
    uint32_t cw = SimpleState(s, kMColorWriteEnable);
    // console ARGB bytes to D3D12 RGBA bits
    k.wmask = ((cw >> 16) & 1) | ((cw >> 8) & 1) << 1 | (cw & 1) << 2 | ((cw >> 24) & 1) << 3;
}

constexpr uint32_t kZUseW = 2;  // D3DZB_USEW

bool OffsetsPolygons(const State& s, Topology topology) {
    return topology == Topology::Triangles && SimpleState(s, kMSolidOffsetEnable);
}

// polygon offset slopes are per console pixel, an upscaled target's pixels are smaller
float HostSlopeScale(const State& s, const Target* ds) {
    return Bits(SimpleState(s, kMSlopeScale)) * (ds ? float(ds->w) / ds->gw : 1.0f);
}

void D3D12Backend::KeyDepthStencil(const State& s, const Assembled& a, PsoKey& k) {
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
    // polygon offset: the slope here, toward the camera is up on reversed depth; the units are in ReversedDepthHlsl
    if (OffsetsPolygons(s, a.topology)) k.zslope = -HostSlopeScale(s, bound_ds_);
}

void KeyRasterizer(const State& s, const Assembled& a, PsoKey& k) {
    uint32_t cull = s.rs[kRsCullMode];
    k.cull = cull != 0 && a.topology == Topology::Triangles;
    k.ccw = cull == kCullCw;  // culling clockwise leaves counter clockwise as the front
    k.fill = s.rs[kRsFillMode];
}

void D3D12Backend::BuildPipelineKey(const State& s, const Assembled& a, uint64_t vh, uint64_t ph, PsoKey& k) {
    std::memset(&k, 0, sizeof(k));
    k.vs = vh;
    k.ps = ph;
    k.regs = a.regs;
    for (int r = 0; r < 16; r++) {
        if ((a.regs >> r) & 1) {
            const VertexElement& e = a.elements[size_t(r)];
            k.elements[r] = e.format | uint32_t(e.buffer) << 8 | uint32_t(e.offset) << 16;
        }
    }
    k.topo = uint32_t(a.topology);
    k.has_rt = bound_rt_ != nullptr;
    k.has_ds = bound_ds_ != nullptr;
    const Target* target = bound_rt_ ? bound_rt_ : bound_ds_;
    k.samples = target ? target->res->GetDesc().SampleDesc.Count : 1;
    k.rtfmt = bound_rt_ ? bound_rt_->fmt : 0;
    KeyBlend(s, k);
    if (k.has_ds) KeyDepthStencil(s, a, k);
    KeyRasterizer(s, a, k);
}

// alpha test off: compare always, in Cmp() minus 1 terms like the enabled test
constexpr float kAlphaTestAlways = float(D3D12_COMPARISON_FUNC_ALWAYS - 1);

// D3DRS_FOGTABLEMODE values
constexpr uint32_t kFogTableNone = 0;
constexpr float kFogTableLinear = 3.0f;

// table mode none under a vertex program: oFog is the blend factor, passed on as linear fog from 1 to 0
void FillFogConstants(const State& s, bool vertex_program, DrawConstants& c) {
    if (!s.rs[kRsFogEnable]) return;
    if (vertex_program && s.rs[kRsFogTableMode] == kFogTableNone) {
        c.fog[0] = 1.0f;
        c.fog[1] = 0.0f;
        c.fog[2] = 0.0f;
        c.fog[3] = kFogTableLinear;
        return;
    }
    c.fog[0] = Bits(s.rs[kRsFogStart]);
    c.fog[1] = Bits(s.rs[kRsFogEnd]);
    c.fog[2] = Bits(s.rs[kRsFogDensity]);
    c.fog[3] = float(s.rs[kRsFogTableMode]);
}

void FillSurfaceConstants(const State& s, GuestExtent size, DrawConstants& c) {
    c.surf[0] = float(size.w);
    c.surf[1] = float(size.h);
    c.surf[2] = 1.0f / s.zscale;
    Color(s.rs[kRsFogColor], c.fog_color);
    c.misc[0] = float(SimpleState(s, kMAlphaRef) & 0xFF);
    c.misc[1] = SimpleState(s, kMAlphaTestEnable) ? float(Cmp(SimpleState(s, kMAlphaFunc)) - 1) : kAlphaTestAlways;
    c.misc[2] = float(Cmp(s.rs[kRsShadowFunc]) - 1);
    c.misc[3] = s.rs[kRsSpecularEnable] ? 1.0f : 0.0f;
}

void FillCombinerConstants(const State& s, DrawConstants& c) {
    for (int i = 0; i < 8; i++) {
        Color(SimpleState(s, kMPsConstant0 + i * 4), c.pc[i * 2]);
        Color(SimpleState(s, kMPsConstant1 + i * 4), c.pc[i * 2 + 1]);
    }
    Color(SimpleState(s, kMPsFinalConstant0), c.pc[16]);
    Color(SimpleState(s, kMPsFinalConstant1), c.pc[17]);
    Color(s.rs[kRsTextureFactor], c.tfactor);
}

constexpr float kGlowBoxTexels = 3.0f;  // console texels across the box each wide tap averages

void D3D12Backend::FillTexelConstants(int i, const TextureDesc& d, const DrawTextures& stages, DrawConstants& c) {
    c.texsize[i][0] = float(d.width);
    c.texsize[i][1] = float(d.height);
    if (!stages.rt[i]) return;
    float k = stages.k[i];
    // a 3 console texel box per tap keeps the console chain's glow width and brightness
    if (stages.wide[i] > 0) {
        float n = std::ceil(kGlowBoxTexels * stages.wide[i] * 0.5f);
        c.spread[i][0] = kGlowBoxTexels / (n * float(d.width) * k);
        c.spread[i][1] = kGlowBoxTexels / (n * float(d.height) * k);
        c.spread[i][2] = n;
    }
}

void D3D12Backend::FillTextureConstants(const State& s, const DrawShaders& sh, const DrawTextures& stages,
                                        DrawConstants& c) {
    for (int i = 0; i < 4; i++) {
        if (stages.desc[i]) {
            FillTexelConstants(i, *stages.desc[i], stages, c);
        }
        if (stages.rt[i]) c.rtshift[i] = RenderTargetShift(s, sh);
        const auto& t = s.tss[size_t(i)];
        c.bump[i][0] = Bits(t[kTssBumpEnvMat00]);
        c.bump[i][1] = Bits(t[kTssBumpEnvMat01]);
        c.bump[i][2] = Bits(t[kTssBumpEnvMat10]);
        c.bump[i][3] = Bits(t[kTssBumpEnvMat11]);
        c.bumpl[i][0] = Bits(t[kTssBumpEnvLScale]);
        c.bumpl[i][1] = Bits(t[kTssBumpEnvLOffset]);
    }
}

void MatMul(const float* x, const float* y, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) {
            o[r * 4 + col] = x[r * 4] * y[col] + x[r * 4 + 1] * y[4 + col] + x[r * 4 + 2] * y[8 + col] +
                             x[r * 4 + 3] * y[12 + col];
        }
    }
}

// HLSL mul(row vector, matrix) with column major packing wants the transpose
void Transpose(const float* m, float* o) {
    for (int r = 0; r < 4; r++) {
        for (int col = 0; col < 4; col++) o[col * 4 + r] = m[r * 4 + col];
    }
}

void InverseTranspose3x3(const float* wv, float* n) {
    float det = wv[0] * (wv[5] * wv[10] - wv[6] * wv[9]) - wv[1] * (wv[4] * wv[10] - wv[6] * wv[8]) +
                wv[2] * (wv[4] * wv[9] - wv[5] * wv[8]);
    float id = det != 0 ? 1.0f / det : 0.0f;
    for (int r = 0; r < 3; r++) {
        for (int col = 0; col < 3; col++) {
            int r1 = (r + 1) % 3;
            int r2 = (r + 2) % 3;
            int c1 = (col + 1) % 3;
            int c2 = (col + 2) % 3;
            n[r * 4 + col] = (wv[r1 * 4 + c1] * wv[r2 * 4 + c2] - wv[r1 * 4 + c2] * wv[r2 * 4 + c1]) * id;
        }
    }
}

// D3DTRANSFORMSTATETYPE indices into s.transforms
constexpr size_t kTsView = 2;
constexpr size_t kTsProjection = 3;
constexpr size_t kTsTexture0 = 16;
constexpr size_t kTsWorld = 256;

void FillTransformConstants(const State& s, GuestExtent size, DrawConstants& c) {
    uint32_t gw = size.w;
    uint32_t gh = size.h;
    // world, view, projection, then the viewport inside the target
    float wv[16];
    float wvp[16];
    MatMul(s.transforms[kTsWorld].data(), s.transforms[kTsView].data(), wv);
    MatMul(wv, s.transforms[kTsProjection].data(), wvp);
    const auto& v = s.viewport;
    float sx = float(v.w) / gw;
    float sy = float(v.h) / gh;
    float ox = (2.0f * v.x + v.w) / gw - 1;
    float oy = 1 - (2.0f * v.y + v.h) / gh;
    float vpm[16] = {sx, 0, 0, 0, 0, sy, 0, 0, 0, 0, v.max_z - v.min_z, 0, ox, oy, v.min_z, 1};
    float full[16];
    MatMul(wvp, vpm, full);
    Transpose(full, c.wvp);
    Transpose(wv, c.wv);
    // normals take the inverse transpose of the upper 3x3
    float n[16] = {};
    InverseTranspose3x3(wv, n);
    Transpose(n, c.wvn);
    for (int t = 0; t < 4; t++) Transpose(s.transforms[kTsTexture0 + size_t(t)].data(), c.tm[t]);
}

// D3DLIGHT8 as floats: Type, Diffuse[4], Specular[4], Ambient[4], Position[3], Direction[3], Range, Falloff,
// Attenuation0 to 2, Theta, Phi
constexpr int kLightFloats = 26;
constexpr int kLightDiffuse = 1;
constexpr int kLightSpecular = 5;
constexpr int kLightAmbient = 9;
constexpr int kLightPosition = 13;
constexpr int kLightDirection = 16;
constexpr int kLightRange = 19;
constexpr int kLightFalloff = 20;
constexpr int kLightAttenuation0 = 21;
constexpr int kLightAttenuation1 = 22;
constexpr int kLightAttenuation2 = 23;
constexpr int kLightTheta = 24;
constexpr int kLightPhi = 25;

void LightToView(const std::array<uint32_t, kLightFloats>& l, const float* vm, float (&o)[7][4]) {
    float f[kLightFloats];
    std::memcpy(f, l.data(), sizeof(f));
    for (int j = 0; j < 4; j++) {
        o[0][j] = f[kLightDiffuse + j];
        o[1][j] = f[kLightSpecular + j];
        o[2][j] = f[kLightAmbient + j];
    }
    const float* pos = f + kLightPosition;
    const float* dir = f + kLightDirection;
    for (int j = 0; j < 3; j++) {
        o[3][j] = pos[0] * vm[j] + pos[1] * vm[4 + j] + pos[2] * vm[8 + j] + vm[12 + j];
        o[4][j] = dir[0] * vm[j] + dir[1] * vm[4 + j] + dir[2] * vm[8 + j];
    }
    o[3][3] = f[kLightRange];
    o[4][3] = f[kLightFalloff];
    o[5][0] = f[kLightAttenuation0];
    o[5][1] = f[kLightAttenuation1];
    o[5][2] = f[kLightAttenuation2];
    o[6][0] = std::cos(f[kLightTheta] * 0.5f);
    o[6][1] = std::cos(f[kLightPhi] * 0.5f);
}

void FillLightConstants(const State& s, const FixedKey& fk, DrawConstants& c) {
    std::memcpy(c.mat, s.material.data(), sizeof(float) * 16);
    c.mat[4][0] = s.material[16];
    Color(s.rs[kRsAmbient], c.amb);
    // lights to view space
    const float* vm = s.transforms[kTsView].data();
    for (int i = 0; i < 8; i++) {
        if (!((fk.types >> (i * 2)) & 3)) continue;
        LightToView(s.lights[size_t(i)], vm, c.lt[i]);
    }
}

// a w buffer's clip range in the units of a draw's clip w
struct WRange {
    float near_w;
    float far_w;
};

// the projection's near and far clip w as D3DDevice_SetTransform finds them
WRange ProjectionWRange(const float* m) {
    float m33 = m[10];
    float m34 = m[11];
    float m43 = m[14];
    float m44 = m[15];
    if (m33 == m34 || m33 == 0) return {0, 1};
    return {m44 - m43 / m33 * m34, (m44 - m43) / (m33 - m34) * m34 + m44};
}

// NV097_SET_CLIP_MIN and MAX in buffer units, sent by D3DDevice_SetDepthClipPlanes
constexpr uint32_t kMClipMin = 0x394;
constexpr uint32_t kMClipMax = 0x398;
constexpr float kFallbackNear = 1.0f / 65536;  // of the far w, when no range is known

// Fixed function clip w is the host's view depth over the projection's range; a program's is in buffer units up to
// zscale, its near end the clip minimum the title sent
WRange DrawWRange(const State& s, bool program) {
    WRange r{0, s.zscale};
    if (!program) {
        r = ProjectionWRange(s.transforms[kTsProjection].data());
    } else if (SimpleState(s, kMClipMax)) {
        r.near_w = Bits(SimpleState(s, kMClipMin));
    }
    if (r.near_w <= 0 || r.near_w >= r.far_w) r.near_w = r.far_w * kFallbackNear;
    return r;
}

// ReversedDepthHlsl's zparams: x the w buffer's scale (0 for a z buffer), y its far w, z the polygon offset in units of
// the clip w for a w buffer, of depth for a z buffer
void D3D12Backend::FillDepthConstants(const State& s, const DrawShaders& sh, const PsoKey& k, DrawConstants& c) const {
    std::memcpy(c.vdef, s.vdef, sizeof(c.vdef));
    c.zclips[0] = s.zclip[0];
    c.zclips[1] = s.zclip[1];
    c.zclips[2] = s.zclip_on && sh.prog ? 1.0f : 0.0f;  // the vertex program planes
    bool offset = OffsetsPolygons(s, Topology(k.topo));
    float units = offset ? Bits(SimpleState(s, kMDepthOffset)) : 0;
    if (s.rs[kRsZEnable] != kZUseW) {
        c.zparams[2] = units / s.zscale;
        c.zparams[3] = sh.prog ? s.program_depth_scale : 1.0f;
        return;
    }
    WRange r = DrawWRange(s, sh.prog);
    c.zparams[0] = r.near_w / (r.far_w - r.near_w);
    c.zparams[1] = r.far_w;
    c.zparams[2] = sh.prog ? units : units * r.far_w / s.zscale;
}

// a fresh copy of the draw's vertex constants, kept as the last upload
uint8_t* D3D12Backend::UploadVertexConstants(const State& s) {
    Alloc vc = Upload(sizeof(s.vsc), 256);
    if (!vc.res) return nullptr;
    std::memcpy(vc.cpu, s.vsc.data(), sizeof(s.vsc));
    std::memcpy(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc));
    last_vsc_gpu_ = vc.gpu;
    return vc.cpu;
}

bool D3D12Backend::UploadConstants(const State& s, const DrawShaders& sh, const PsoKey& k, const DrawTextures& stages,
                                   GuestExtent size, DrawConstants& c) {
    // the extension edits the uploaded copy, so its draws always get their own
    bool same = !ext_ && last_vsc_gpu_ && !std::memcmp(last_vsc_.data(), s.vsc.data(), sizeof(s.vsc));
    uint8_t* vsc = same ? nullptr : UploadVertexConstants(s);
    if (!same && !vsc) return false;
    FillSurfaceConstants(s, size, c);
    // points: host pixels per console pixel, also keeps them off the far edge pull out of screen passes
    if (k.topo == uint32_t(Topology::Points)) c.surf[3] = TargetScale();
    // lines: negative, half a pixel onto the host's pixel centers (the vertex epilogue), widened by LineHlsl
    else if (k.topo == uint32_t(Topology::Lines)) c.surf[3] = -TargetScale();
    FillFogConstants(s, sh.prog, c);
    FillCombinerConstants(s, c);
    if (ext_) ext_->Constants(reinterpret_cast<float(*)[4]>(vsc), c.ext, bound_rt_, k.blend ? k.src : 1);
    FillDepthConstants(s, sh, k, c);
    FillTextureConstants(s, sh, stages, c);
    if (sh.fixed_transform) {
        FillTransformConstants(s, size, c);
        FillLightConstants(s, sh.fk, c);
    }
    if (!last_dc_gpu_ || std::memcmp(&last_dc_, &c, sizeof(c))) {
        Alloc dc = Upload(sizeof(DrawConstants), 256);
        if (!dc.res) return false;
        std::memcpy(dc.cpu, &c, sizeof(c));
        last_dc_ = c;
        last_dc_gpu_ = dc.gpu;
    }
    if (last_vsc_gpu_ != binds_.vsc) Cmd()->SetGraphicsRootConstantBufferView(0, last_vsc_gpu_);
    if (last_dc_gpu_ != binds_.dc) Cmd()->SetGraphicsRootConstantBufferView(1, last_dc_gpu_);
    binds_.vsc = last_vsc_gpu_;
    binds_.dc = last_dc_gpu_;
    return true;
}

void D3D12Backend::CaptureTextures(const State& s, const DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        ID3D12Resource* res = stages.res[i];
        if (!stages.desc[i]) continue;
        const TextureDesc& d = *stages.desc[i];
        bool tg = targets_.count(SizedTargetKey(d.data, Format(d.format).depth, d.width, d.height));
        const auto& t = s.tss[size_t(i)];
        fprintf(cap_, " | t%d %08X fmt %X %ux%u%s%s addr %u%u filt %u%u border %08X", i, d.data, d.format, d.width,
                d.height, tg ? " target" : "", res ? "" : " unbound", t[kTssAddressU], t[kTssAddressV],
                t[kTssMinFilter], t[kTssMagFilter], t[kTssBorderColor]);
        fprintf(cap_, " mips %u mipfilter %u lodbias %g maxmip %u", d.levels, t[kTssMipFilter],
                Bits(t[kTssMipLodBias]), t[kTssMaxMipLevel]);
        if (res && !tg && stages.info[i].kind == kTex2D && cap_seen_.insert(d.data).second) {
            cap_tex_.push_back({res, stages.sv[i].Format, d.data, d.width, d.height});
        }
    }
}

// s.vsc holds the 192 vertex shader constants in hardware order: D3D register + 96
constexpr int kVsConstants = 192;
constexpr int kVsConstantBias = 96;

void D3D12Backend::CaptureInputs(const State& s, const Assembled& a, const DrawConstants& c) {
    fprintf(cap_, "\n     verts");
    uint32_t n = 0;
    for (int r = 0; r < 16; r++) n += (a.regs >> r) & 1;
    for (size_t v = 0; v < std::min<size_t>(4, n ? a.vertices.size() / (n * 4) : 0); v++) {
        fprintf(cap_, " [");
        for (uint32_t k = 0; k < n * 4; k++) fprintf(cap_, " %g", a.vertices[v * n * 4 + k]);
        fprintf(cap_, " ]");
    }
    // the raw path: the first vertices of buffer 0 as dwords (floats shown as floats, colors in hex)
    if (a.vertices.empty() && a.buffer_count && a.buffers[0].src && a.buffers[0].stride) {
        const VertexBuffer& b = a.buffers[0];
        for (uint32_t v = 0; v < std::min<uint32_t>(4, b.bytes / b.stride); v++) {
            fprintf(cap_, " [");
            for (uint32_t o = 0; o + 4 <= b.stride; o += 4) {
                uint32_t d;
                std::memcpy(&d, b.src + v * b.stride + o, 4);
                float f;
                std::memcpy(&f, &d, 4);
                if (std::abs(f) < 1e7f && (std::abs(f) > 1e-6f || d == 0)) fprintf(cap_, " %g", f);
                else fprintf(cap_, " %08X", d);
            }
            fprintf(cap_, " ]");
        }
    }
    for (int r = 0; r < kVsConstants; r++) {
        const auto& v = s.vsc[r];
        if (v[0] || v[1] || v[2] || v[3]) {
            fprintf(cap_, "\n     c%d %g %g %g %g", r - kVsConstantBias, v[0], v[1], v[2], v[3]);
        }
    }
    fprintf(cap_, "\n     pc");
    for (size_t r = 0; r < std::size(c.pc); r++) {
        fprintf(cap_, " (%.3f %.3f %.3f %.3f)", c.pc[r][0], c.pc[r][1], c.pc[r][2], c.pc[r][3]);
    }
}

// the draw's line: its state, then its textures
void D3D12Backend::CaptureDraw(const State& s, uint32_t prim, uint32_t count, const PsoKey& k, const DrawConstants& c,
                               const DrawTextures& stages, uint64_t vkey) {
    fprintf(cap_,
            "%04u draw rt %08X ds %08X prim %u count %u vs %08X vk %016llX ps %08X blend %u %X %X zen %u %X zwrite %u "
            "zbias %d %.2f cull %u cw %X c0 %.3f %.3f %.3f %.3f",
            cap_i_++, s.rt.data, bound_ds_ ? s.zs.data : 0, prim, count, s.vs_handle, (unsigned long long)vkey,
            s.ps ? s.ps->handle : 0, k.blend,
            k.src, k.dst, k.zen, k.zfunc, k.zwrite, k.zbias, k.zslope, k.cull, k.wmask, c.pc[0][0], c.pc[0][1],
            c.pc[0][2], c.pc[0][3]);
    if (k.sten)
        fprintf(cap_, " sten func %X ref %u mask %X wmask %X fail %X zfail %X pass %X", k.sfunc,
                SimpleState(s, kMStencilRef) & 0xFF, k.smask, k.swmask, k.sfail, k.szfail, k.spass);
    if (s.scissor_count || s.viewport.x || s.viewport.y)
        fprintf(cap_, " vp %u %u %u %u sc %u %u %u %u %u", s.viewport.x, s.viewport.y, s.viewport.w, s.viewport.h,
                s.scissor_count, s.scissors[0], s.scissors[1], s.scissors[2], s.scissors[3]);
    fprintf(cap_, " v3 %.3f %.3f %.3f %.3f zclip %g %g %u bcol %08X", s.vdef[3][0], s.vdef[3][1], s.vdef[3][2], s.vdef[3][3], s.zclip[0], s.zclip[1], s.zclip_on, SimpleState(s, kMBlendColor));
    if (c.zparams[3] > 0 && c.zparams[3] != 1.0f) fprintf(cap_, " depthscale %g", c.zparams[3]);
    // the lighting constants the vertex programs read; a camera-dependent flip here explains walls that change brightness
    fprintf(cap_, " amb %.4f %.4f %.4f L0 %.4f %.4f %.4f i0 %.4f i1 %.4f i2 %.4f L1 %.4f %.4f %.4f L2 %.4f %.4f %.4f"
                   " mat %.4f %.4f %.4f spec %.4f %.4f %.4f pow %.4f",
            s.vsc[186][0], s.vsc[186][1], s.vsc[186][2],
            s.vsc[180][0], s.vsc[180][1], s.vsc[180][2],
            s.vsc[181][0], s.vsc[183][0], s.vsc[185][0],
            s.vsc[182][0], s.vsc[182][1], s.vsc[182][2],
            s.vsc[184][0], s.vsc[184][1], s.vsc[184][2],
            s.vsc[168][0], s.vsc[168][1], s.vsc[168][2],
            s.vsc[187][0], s.vsc[187][1], s.vsc[187][2],
            s.vsc[179][0]);
    if (SimpleState(s, kMAlphaTestEnable))
        fprintf(cap_, " atest func %.0f ref %.0f", c.misc[1], c.misc[0]);
    if (s.rs[kRsFogEnable])
        fprintf(cap_, " fog %08X mode %u %.3f %.3f %.4f", s.rs[kRsFogColor], s.rs[kRsFogTableMode], c.fog[0], c.fog[1],
                c.fog[2]);
    CaptureTextures(s, stages);
}

// small draws: their inputs and constants; then ends the draw's line
void D3D12Backend::EndDrawCapture(const State& s, const Assembled& a, uint32_t count, const DrawConstants& c) {
    // XBE_CAPTURE_INPUTS: every draw's first vertices and constants, not only small ones
    static const bool all_inputs = getenv("XBE_CAPTURE_INPUTS") != nullptr;
    if (count <= kQuadVertices || all_inputs) CaptureInputs(s, a, c);
    fputc('\n', cap_);
}

// each shader once, as translated
void D3D12Backend::CaptureShader(const char* kind, uint64_t key, const std::string& src) {
    if (!cap_sh_.insert(key).second) return;
    char name[48];
    snprintf(name, sizeof(name), "%s_%016llX.hlsl", kind, (unsigned long long)key);
    if (FILE* f = _wfopen((cap_dir_ / name).c_str(), L"w")) {
        fwrite(src.data(), 1, src.size(), f);
        fclose(f);
    }
    fprintf(cap_, "     %s\n", name);
}

void D3D12Backend::CaptureShaders(const State& s, const Assembled& a, const DrawShaders& sh, const PixelKey& pk) {
    std::string src = ReversedDepthHlsl(VertexHlsl(s, a, sh));
    CaptureShader("vs", sh.vkey, sh.quad ? QuadHlsl(std::move(src)) : src);
    CaptureShader("ps", sh.pkey, PixelHlsl(pk));
}

D3D12_SRV_DIMENSION NullViewDimension(uint8_t kind) {
    if (kind == kTexCube) return D3D12_SRV_DIMENSION_TEXTURECUBE;
    if (kind == kTex3D) return D3D12_SRV_DIMENSION_TEXTURE3D;
    return D3D12_SRV_DIMENSION_TEXTURE2D;
}

// A small draw whose every float2 input lies in 0..1: sprites, HUD pieces, text, full screen passes. The console's
// pixel centres sample such a quad from its first texel to half a texel short of its far edge; upscaled, the far
// edge's last half texel is drawn too, and wrapped it blends in the opposite edge (thin dark lines on HUD sprites).
// Clamped, it takes the edge texel instead, which a wrapped texture used once over 0..1 has there anyway.
// A screen space pass reading a render target holds its reads to the texels its coordinates span, from the first
// texel centre to the last. A glow chain keeps several blur levels side by side in one target and scales each up from
// texel 0.5 to its width + 0.5: the far edge's pixels would blend in the next texel, which belongs to a larger, less
// blurred level, and show an offset copy of the scene along the right and bottom of the frame.
void RegionClip(const Assembled& a, const DrawTextures& stages, uint32_t gw, uint32_t gh, DrawConstants& c) {
    constexpr float kOpen = 1e9f;
    constexpr uint32_t kSmallDrawVertices = 64;
    for (int i = 0; i < 4; i++) {
        if (!stages.rt[i]) continue;
        c.rtclip[i][0] = c.rtclip[i][1] = -kOpen;
        c.rtclip[i][2] = c.rtclip[i][3] = kOpen;
    }
    if (!a.pretransformed || !a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return;
    // one texture coordinate set: the pass's quad
    int reg = -1;
    for (int r = 0; r < 16; r++) {
        if (!(a.regs >> r & 1) || a.elements[size_t(r)].format != DXGI_FORMAT_R32G32_FLOAT) continue;
        if (reg >= 0) return;
        reg = r;
    }
    if (reg < 0) return;
    const VertexElement& e = a.elements[size_t(reg)];
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
    const VertexBuffer& pb = a.buffers[pe.buffer];
    if (gw && gh && (a.regs & 1) && pb.src && pb.stride && uint64_t(a.vertex_count) * pb.stride <= pb.bytes) {
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
    for (int i = 0; i < 4; i++) {
        if (!stages.rt[i] || !stages.desc[i] || !stages.desc[i]->width || !stages.desc[i]->height) continue;
        const float size[2] = {float(stages.desc[i]->width), float(stages.desc[i]->height)};
        for (int k = 0; k < 2; k++) {
            const float n = stages.info[i].rect ? size[k] : 1.0f;  // coordinates per texture width
            if (hi[k] - lo[k] < 2 * n / size[k]) continue;         // too narrow to be a region
            // the far edge's last texel centre: as far in from it as the near edge's coordinate is past a texel corner
            const float texel = n / size[k];
            const float past = lo[k] / texel - std::floor(lo[k] / texel);
            c.rtclip[i][k] = lo[k] / n;
            c.rtclip[i][2 + k] = (hi[k] - (past > 0.25f ? 1.0f : 0.5f) * texel) / n;
            // Unlike the console: a smaller glow target scaled over the whole frame keeps off its outermost texels,
            // which the title blacks out, so the glow does not fade to nothing along the frame's edges (most visibly
            // the right and bottom, where the half texel start lands more of the frame on them).
            const bool whole = lo[k] <= texel * 1.01f && hi[k] >= n - texel * 0.01f;
            if (full_frame && whole && size[k] < float(k ? gh : gw) && size[k] > 4) {
                c.rtclip[i][k] += texel / n;
                c.rtclip[i][2 + k] -= texel / n;
            }
        }
    }
}

bool UnitTexcoords(const Assembled& a) {
    constexpr float kLo = -1e-4f;
    constexpr float kHi = 1.0001f;
    constexpr uint32_t kSmallDrawVertices = 4096;  // most vertices a small draw has
    if (!a.vertices.empty() || !a.vertex_count || a.vertex_count > kSmallDrawVertices) return false;
    bool any = false;
    for (int r = 0; r < 16; r++) {
        const VertexElement& e = a.elements[size_t(r)];
        if (!(a.regs >> r & 1) || e.format != DXGI_FORMAT_R32G32_FLOAT) continue;
        const VertexBuffer& vb = a.buffers[e.buffer];
        if (!vb.src || !vb.stride || uint64_t(a.vertex_count) * vb.stride > vb.bytes) return false;
        for (uint32_t v = 0; v < a.vertex_count; v++) {
            float uv[2];
            std::memcpy(uv, vb.src + size_t(v) * vb.stride + e.offset, sizeof(uv));
            if (!(uv[0] >= kLo && uv[0] <= kHi && uv[1] >= kLo && uv[1] <= kHi)) return false;
        }
        any = true;
    }
    return any;
}

// UnitTexcoords only changes stages that wrap
bool AnyStageWraps(const State& s) {
    for (const auto& t : s.tss) {
        if (t[kTssAddressU] == kTaddressWrap || t[kTssAddressV] == kTaddressWrap) return true;
    }
    return false;
}

// the first slot of the stages' four views, made only when this frame has not made the same set yet
uint32_t D3D12Backend::EnsureViewSet(const DrawTextures& stages) {
    if (view_sets_frame_ != frame_) {
        view_sets_.clear();
        view_sets_frame_ = frame_;
    }
    uint64_t key = Hash(stages.sv, sizeof(stages.sv), Hash(stages.res, sizeof(stages.res)));
    auto it = view_sets_.find(key);
    if (it != view_sets_.end()) return it->second;
    uint32_t slot = AllocSrvSlots(4);
    for (int i = 0; i < 4; i++) {
        dev_->CreateShaderResourceView(stages.res[i], &stages.sv[i], Cpu(srv_heap_.Get(), slot + i, srv_inc_));
    }
    view_sets_[key] = slot;
    return slot;
}

// a stage without a texture gets a null view of its kind; the samplers read the views after
void D3D12Backend::SetTextureViewsAndSamplers(const State& s, const Assembled& a, DrawTextures& stages) {
    for (int i = 0; i < 4; i++) {
        if (stages.res[i]) continue;
        stages.sv[i].ViewDimension = NullViewDimension(stages.info[i].kind);
        stages.sv[i].Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    }
    uint32_t views = EnsureViewSet(stages);
    if (views != binds_.views) Cmd()->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), views, srv_inc_));
    binds_.views = views;
    uint32_t samplers = EnsureStageSamplers(s, stages, AnyStageWraps(s) && UnitTexcoords(a));
    if (samplers != binds_.samplers) Cmd()->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), samplers, smp_inc_));
    binds_.samplers = samplers;
}

// The host viewport covers the target; the guest viewport transform is in the shader.
// SetScissors(0) clips to the guest viewport. An explicit inclusive scissor replaces
// that clip, rather than intersecting it (XDK 5849 SetScissors, 0x2B8F0F..0x2B9059).
// The locker preview deliberately draws above its viewport through a larger scissor.
void D3D12Backend::SetViewportScissor(const State& s, uint32_t gw, uint32_t gh, float squeeze) {
    float W = float(bound_rt_ ? bound_rt_->w : bound_ds_->w);
    float H = float(bound_rt_ ? bound_rt_->h : bound_ds_->h);
    D3D12_VIEWPORT vp{0, 0, W, H, 0, 1};
    float sx = W / gw, sy = H / gh;
    LONG x0 = LONG(s.viewport.x * sx);
    LONG y0 = LONG(s.viewport.y * sy);
    LONG x1 = LONG((s.viewport.x + s.viewport.w) * sx);
    LONG y1 = LONG((s.viewport.y + s.viewport.h) * sy);
    if (s.scissor_count) {
        x0 = LONG(s.scissors[0] * sx);
        y0 = LONG(s.scissors[1] * sy);
        x1 = LONG(s.scissors[2] * sx);
        y1 = LONG(s.scissors[3] * sy);
    }
    if (squeeze < 1.0f) {
        // Preserve the guest clip after moving the HUD into the centered 16:9 region.
        const float inset = W * (1.0f - squeeze) * 0.5f;
        x0 = LONG(std::ceil(inset + std::clamp(x0, 0L, LONG(W)) * squeeze));
        x1 = LONG(std::ceil(inset + std::clamp(x1, 0L, LONG(W)) * squeeze));
    }
    D3D12_RECT rect{std::clamp<LONG>(x0, 0, LONG(W)), std::clamp<LONG>(y0, 0, LONG(H)),
                    std::clamp<LONG>(x1, 0, LONG(W)), std::clamp<LONG>(y1, 0, LONG(H))};
    bool same = binds_.rasterizer && !std::memcmp(&vp, &binds_.viewport, sizeof(vp)) &&
                !std::memcmp(&rect, &binds_.scissor, sizeof(rect));
    if (same) return;
    if (bundle_) {
        // Bundles cannot set viewports or scissors: keep them for the direct list at replay.
        bundle_desc_.viewport = vp;
        bundle_desc_.scissor = rect;
        bundle_desc_.has_viewport = true;
        bundle_desc_.has_scissor = true;
    } else {
        cl_->RSSetViewports(1, &vp);
        cl_->RSSetScissorRects(1, &rect);
    }
    binds_.rasterizer = true;
    binds_.viewport = vp;
    binds_.scissor = rect;
}

void D3D12Backend::SetBlendFactorStencilRef(const State& s) {
    float bf[4];
    Color(SimpleState(s, kMBlendColor), bf);
    // D3D12 has one blend factor for both constant blends: CONSTANTALPHA (0x8003/0x8004) scales every channel by the
    // blend colour's alpha, so broadcast it when no CONSTANTCOLOR (0x8001/0x8002) factor is in use. True Crime NYC's
    // light buffer chain uses (0x8003, 0) and (0x8003, 0x8004) with a black blend colour: as colour it came out black.
    if (SimpleState(s, kMBlendEnable)) {
        auto alpha_only = [](uint32_t f) { return f == 0x8003 || f == 0x8004; };
        auto colour = [](uint32_t f) { return f == 0x8001 || f == 0x8002; };
        uint32_t src = SimpleState(s, kMSrcBlend), dst = SimpleState(s, kMDestBlend);
        if ((alpha_only(src) || alpha_only(dst)) && !colour(src) && !colour(dst)) bf[0] = bf[1] = bf[2] = bf[3];
    }
    uint32_t ref = SimpleState(s, kMStencilRef) & 0xFF;
    if (binds_.output && !std::memcmp(bf, binds_.blend, sizeof(bf)) && ref == binds_.stencil) return;
    Cmd()->OMSetBlendFactor(bf);
    Cmd()->OMSetStencilRef(ref);
    binds_.output = true;
    std::memcpy(binds_.blend, bf, sizeof(bf));
    binds_.stencil = ref;
}

D3D_PRIMITIVE_TOPOLOGY PrimitiveTopology(const Assembled& a) {
    if (a.topology == Topology::Points) return D3D_PRIMITIVE_TOPOLOGY_POINTLIST;
    if (a.topology == Topology::Lines) {
        return a.strip ? D3D_PRIMITIVE_TOPOLOGY_LINESTRIP : D3D_PRIMITIVE_TOPOLOGY_LINELIST;
    }
    return a.strip ? D3D_PRIMITIVE_TOPOLOGY_TRIANGLESTRIP : D3D_PRIMITIVE_TOPOLOGY_TRIANGLELIST;
}

bool D3D12Backend::UploadVertexBuffers(const Assembled& a) {
    D3D12_VERTEX_BUFFER_VIEW vbv[16];
    for (uint32_t i = 0; i < a.buffer_count; i++) {
        const VertexBuffer& b = a.buffers[i];
        Alloc vb = Upload(b.bytes, 16);
        if (!vb.res) return false;
        std::memcpy(vb.cpu, b.src, b.bytes);
        vbv[i] = {vb.gpu, b.bytes, b.stride};
    }
    Cmd()->IASetVertexBuffers(0, a.buffer_count, vbv);
    last_draw_.vb_count = std::min<uint32_t>(a.buffer_count, 16);
    std::memcpy(last_draw_.vbv, vbv, last_draw_.vb_count * sizeof(vbv[0]));
    return true;
}

void D3D12Backend::DrawGeometry(const Assembled& a) {
    last_draw_.ok = false;
    if (!UploadVertexBuffers(a)) return;
    D3D_PRIMITIVE_TOPOLOGY topology = PrimitiveTopology(a);
    if (topology != binds_.topology) Cmd()->IASetPrimitiveTopology(topology);
    binds_.topology = topology;
    if (a.index_count) {
        uint32_t ibytes = a.index_count * (a.index16 ? 2 : 4);
        Alloc ib = Upload(ibytes, 4);
        if (!ib.res) return;
        std::memcpy(ib.cpu, a.index_src, ibytes);
        D3D12_INDEX_BUFFER_VIEW ibv{ib.gpu, ibytes, a.index16 ? DXGI_FORMAT_R16_UINT : DXGI_FORMAT_R32_UINT};
        Cmd()->IASetIndexBuffer(&ibv);
        Cmd()->DrawIndexedInstanced(a.index_count, 1, 0, a.base_vertex, 0);
        last_draw_.ibv = ibv;
        last_draw_.index_count = a.index_count;
        last_draw_.base_vertex = int32_t(a.base_vertex);
    } else {
        Cmd()->DrawInstanced(a.draw_count, 1, 0, 0);
        last_draw_.index_count = 0;
        last_draw_.draw_count = a.draw_count;
    }
    last_draw_.ok = true;
}

// calls the extension's EndDraw on every return from Draw once made
struct DrawEnd {
    Extension* e;
    ~DrawEnd() {
        if (e) e->EndDraw();
    }
};

// Assemble, timed and counted
bool D3D12Backend::AssembleDraw(const State& s, uint32_t prim, uint32_t start, uint32_t count,
                                const uint8_t* indices, Assembled& a) {
    bool assembled;
    {
        PerfScope perf_assemble{g_perf.assemble};
        assembled = Assemble(s, prim, start, count, indices, a);
    }
    g_perf.verts += a.vertex_count;
    g_perf.raw += a.vertices.empty();
    return assembled;
}

bool D3D12Backend::EnsureQueries() {
    if (query_counts_) return true;
    D3D12_QUERY_HEAP_DESC qd{D3D12_QUERY_HEAP_TYPE_OCCLUSION, kQueries, 0};
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
    D3D12_RESOURCE_DESC bd = BufferDesc(uint64_t(kQueries) * sizeof(uint64_t));
    void* p = nullptr;
    bool made = SUCCEEDED(dev_->CreateQueryHeap(&qd, IID_PPV_ARGS(&query_heap_))) &&
                SUCCEEDED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                        nullptr, IID_PPV_ARGS(&query_rb_))) &&
                SUCCEEDED(query_rb_->Map(0, nullptr, &p));
    if (!made) {
        LOG_FIRST(1, "d3d12: no occlusion queries, visibility tests stay pending");
        return false;
    }
    query_counts_ = static_cast<const uint64_t*>(p);
    return true;
}

// a draw while the title counts visible pixels gets an occlusion query of its own, kNoQuery otherwise
uint32_t D3D12Backend::BeginCountedDraw(const State& s) {
    if (!s.simple[kMCountEnable >> 2] || !EnsureQueries() || query_next_ >= kQueries) return kNoQuery;
    uint32_t query = query_next_++;
    cl_->BeginQuery(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
    return query;
}

void D3D12Backend::EndCountedDraw(uint32_t query) {
    if (query == kNoQuery) return;
    cl_->EndQuery(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, query);
    uint64_t at = uint64_t(query) * sizeof(uint64_t);
    cl_->ResolveQueryData(query_heap_.Get(), D3D12_QUERY_TYPE_OCCLUSION, query, 1, query_rb_.Get(), at);
    counted_.push_back({query, SamplesPerPixel()});
}

// the console counts its own pixels: the bound target's scale and multisampling come out
float D3D12Backend::SamplesPerPixel() const {
    const Target* t = bound_rt_ ? bound_rt_ : bound_ds_;
    if (!t || !t->gw || !t->gh) return 1;
    float area = float(t->w) * float(t->h) / (float(t->gw) * float(t->gh));
    return area * float(t->res->GetDesc().SampleDesc.Count);
}

// reports whose draws the GPU has finished, written as the console's GPU writes them: time, count, then 0 for done
void D3D12Backend::DeliverReports() {
    constexpr uint32_t kTime = 0;
    constexpr uint32_t kTimeHigh = 4;
    constexpr uint32_t kCount = 8;
    constexpr uint32_t kStatus = 12;
    uint64_t done = fence_->GetCompletedValue();
    while (!reports_.empty() && reports_.front().fence <= done) {
        const Report& r = reports_.front();
        double pixels = 0;
        for (const Query& q : r.queries) pixels += double(query_counts_[q.index]) / q.samples_per_pixel;
        kernel::W32(r.va + kTime, uint32_t(presents_));
        kernel::W32(r.va + kTimeHigh, 0);
        kernel::W32(r.va + kCount, uint32_t(std::lround(pixels)));
        std::atomic_thread_fence(std::memory_order_release);
        kernel::W32(r.va + kStatus, 0);
        reports_.pop_front();
    }
    if (reports_.empty()) query_next_ = 0;  // no pending report references the query ring now
}

constexpr uint32_t kPointList = 1;   // D3DPT_POINTLIST
constexpr uint32_t kTopDisable = 1;  // the XDK's D3DTOP_DISABLE
constexpr int kSpriteStage = 3;

// a program's point sprites under the fixed function stages: the XDK builds their combiners from stage 3 alone,
// the stage with the sprite's coordinates; a pixel shader reads t3 itself. SpriteStageZero disables stage 3.
bool SpriteOnStage3(const State& s, uint32_t prim) {
    bool prog = (s.vs_handle & 1) && s.vs && !s.vs->program.empty();
    bool stage3 = s.tss[kSpriteStage][kTssColorOp] != kTopDisable;
    return prim == kPointList && prog && !s.ps && s.rs[kRsPointSpriteEnable] && stage3;
}

// stage 3 drawn as stage 0, the only stage
State SpriteStageZero(const State& s) {
    State moved = s;
    moved.tss[0] = s.tss[kSpriteStage];
    moved.textures[0] = s.textures[kSpriteStage];
    moved.palettes[0] = s.palettes[kSpriteStage];
    for (int i = 1; i < 4; i++) {
        moved.tss[i][kTssColorOp] = kTopDisable;
        moved.tss[i][kTssAlphaOp] = kTopDisable;
        moved.textures[i] = 0;
    }
    return moved;
}

void D3D12Backend::Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) {
    if (SpriteOnStage3(s, prim)) {
        Draw(SpriteStageZero(s), prim, start, count, indices);
        return;
    }
    PerfScope perf_draw{g_perf.draw};
    g_perf.draws++;
    if (cap_) CapStep(s.rt);
    if (DepthCopy(s)) return;
    DrawEnd draw_end{ext_};
    if (ext_) ext_->BeginDraw(s);
    Assembled& a = asm_;
    if (!AssembleDraw(s, prim, start, count, indices, a)) return;

    // The full-size scene colour target is the only one whose draws are recorded. Queries,
    // capture and extension draws need the immediate path.
    bool scene_sized = false;
    if (defer_scene_ && s.rt.data) {
        uint32_t gw;
        uint32_t gh;
        SurfaceSize(s.rt, gw, gh);
        scene_sized = gw == guest_w_ && gh == guest_h_;
    }
    bool defer_possible = scene_sized && !defer_abandoned_ && !cap_ && !ext_ && !s.simple[kMCountEnable >> 2];
    if (defer_possible) defer_frame_active_ = true;
    else if (scene_sized && !defer_abandoned_) {
        if (cap_)
            DeferredBarrier("scene capture");
        else if (ext_)
            DeferredBarrier("scene draw with an extension");
        else if (s.simple[kMCountEnable >> 2])
            DeferredBarrier("counted scene draw");
    }
    // A scene draw that cannot be recorded must run after the pending bundles it overlaps.
    if (!defer_possible && defer_scene_ && !CurrentFrame().bundles.empty() &&
        FindTarget(s.rt, false) == defer_rt_) {
        if (DeferredBarrier("main scene draw could not be deferred")) {
            if (!BindTargets(s)) return;
        }
    }

    // past the ring every upload is a buffer of its own: seconds lost on a frame that draws a whole level once
    if (CurrentFrame().used > kUploadSize - kUploadReserve) {
        if (defer_scene_ && !bundle_) FlushDeferredScene();
        Submit(true);
    }
    // Keep a margin for this draw's texture views and any resolves/uploads below; a wrap
    // would alias descriptors already recorded, including those in pending bundles.
    if (CurrentFrame().srv + 48 > kSrvCount / kFrames) {
        if (defer_scene_ && !bundle_) FlushDeferredScene();
        Submit(true);
    }
    if (!BindTargets(s)) return;
    if (bound_ds_) bound_ds_->depth_resolved = false;
    if (ext_ && ext_->Bound(*this, s, bound_rt_, bound_ds_, a.pretransformed) && !BindTargets(s)) return;

    // vertex shader
    DrawShaders sh = ClassifyShaders(s, a, count);
    if (sh.prog) EnsureProgramHash(s);
    sh.vkey = VertexKey(s, a, sh);
    EnsureVertexShader(s, a, sh);

    // stage textures, then the pixel shader made for them
    DrawTextures stages;
    defer_flush_conflict_ = false;
    if (!BindStageTexturesAndTargets(s, stages)) return;
    // A sampled resource forced a flush while those textures were being bound. The draw must
    // stay immediate so it sees the scene contents it was just made to wait for.
    const bool stage_conflict = defer_flush_conflict_;

    // Only draws into this frame's single full-size scene target are bundled. A draw that
    // samples a full-size target reads scene contents and stays immediate.
    bool defer_now = defer_possible && !defer_abandoned_ && !stage_conflict && bound_rt_ && !bound_rt_->depth &&
                     bound_rt_->gw == guest_w_ && bound_rt_->gh == guest_h_;
    if (defer_now) {
        if (!defer_rt_) {
            defer_rt_ = bound_rt_;
        } else if (defer_rt_ != bound_rt_) {
            defer_now = false;
        }
    }
    DeferredScope defer_scope{this};
    // AO receivers: world geometry in the scene view (see AoReceiver). They get the AO shader
    // variant and a depth-only copy for the AO depth prepass; everything else replays as it
    // was drawn, in the original order.
    const bool scene_view = s.viewport.w == guest_w_ && s.viewport.h * 4 >= guest_h_ * 3;
    // Draws that sample a colour render target (water reflections and refraction, ripple maps,
    // mirrors) take no AO role. Water is a wavy animated mesh: AO on it darkened the troughs
    // between its triangles, which turned the smooth bump-mapped surface into faceted spikes,
    // and liquid and mirror surfaces show no ambient occlusion anyway. A depth target sampled
    // as a shadow map does not count.
    bool samples_colour_rt = false;
    for (int i = 0; i < 4; i++)
        if (stages.rt[i] && !stages.info[i].shadow) samples_colour_rt = true;
    uint8_t ao_roles = render_.ao != 0 && bound_rt_ && bound_ds_ && bound_ds_->res && !a.pretransformed &&
                                     scene_view && bound_ds_->w == bound_rt_->w && bound_ds_->h == bound_rt_->h
                                 ? AoRoles(s, samples_colour_rt)
                                 : 0;
    const bool opaque = (ao_roles & kAoDepth) != 0;  // recorded into the AO depth prepass
    if (defer_now && !BeginDeferredBundle(stages, opaque)) {
        defer_now = false;
        DeferredFallback("bundle allocation failed");
    }
    // An immediate draw that touches a pending target must run after the pending bundles.
    if (!defer_now && DeferredPending()) {
        bool hit = (bound_rt_ && defer_writes_.count(bound_rt_->res.Get())) ||
                   (bound_ds_ && defer_writes_.count(bound_ds_->res.Get()));
        if (hit) {
            if (DeferredBarrier("main scene target written by an immediate draw")) {
                if (!BindTargets(s)) return;
            }
        }
    }
    bool ao_receiver = defer_now && (ao_roles & kAoColour);
    PixelKey pk = MakePixelKey(s, stages.info);
    pk.quad = sh.quad;
    pk.ao = ao_receiver ? 1 : 0;
    sh.pkey = Hash(&pk, sizeof(pk));
    EnsurePixelShader(pk, sh);
    if (ao_receiver && !sh.ps_code) {
        // the AO variant failed to compile: draw it as the game made it, without AO
        LOG_FIRST(4, "d3d12: AO pixel shader variant failed, drawing without AO");
        ao_receiver = false;
        pk.ao = 0;
        sh.pkey = Hash(&pk, sizeof(pk));
        EnsurePixelShader(pk, sh);
    }
    if (!sh.vs_code || !sh.ps_code) return;

    // pipeline
    PsoKey k;
    BuildPipelineKey(s, a, sh.vh, sh.ph, k);
    k.sprite = sh.sprite;
    ID3D12PipelineState* pso = EnsurePipeline(k, sh.vs_code, sh.ps_code);
    if (!pso) return;
    depth_replay_ = {};
    if (defer_now && opaque) {
        // The same draw for the AO depth prepass: depth plus its view distance (clip w) into the
        // R32F distance target, through the pixel shader's distance variant so alpha tests and
        // discards still apply. No stencil: the AO depth's stencil is always 0, so the game's
        // stencil test would reject the writes.
        DrawShaders dsh = sh;
        PixelKey dpk = pk;
        dpk.ao = 3;
        dsh.pkey = Hash(&dpk, sizeof(dpk));
        EnsurePixelShader(dpk, dsh);
        if (dsh.ps_code) {
            PsoKey dk = k;
            dk.ps = dsh.ph;
            dk.has_rt = 1;
            dk.rtfmt = kAoDistFormat;
            dk.blend = dk.src = dk.dst = dk.op = 0;
            dk.wmask = 1;  // red
            dk.sten = dk.sfunc = dk.smask = dk.swmask = dk.sfail = dk.szfail = dk.spass = 0;
            depth_replay_.pso = EnsurePipeline(dk, dsh.vs_code, dsh.ps_code);
        }
    }
    if (ext_) binds_ = {};  // the extension may set anything
    if (pso != binds_.pso) Cmd()->SetPipelineState(pso);
    binds_.pso = pso;

    // constants
    GuestExtent size;
    SurfaceSize(bound_rt_ ? s.rt : s.zs, size.w, size.h);
    DrawConstants c{};
    c.tscale[0] = TargetScale();
    c.tscale[1] = TargetScaleY();
    c.tscale[2] = UiSqueeze(s, a, stages);
    // the frame's size when the draw targets the frame, for RegionClip's full-frame composites
    const bool frame = bound_rt_ && bound_rt_->gw == guest_w_ && bound_rt_->gh == guest_h_;
    RegionClip(a, stages, frame ? guest_w_ : 0, frame ? guest_h_ : 0, c);
    if (bound_rt_ && !a.pretransformed) bound_rt_->scene_frame = frame_;
    // Distance blur only uses this frame's full-size, multisampled scene depth. The allocation can
    // fall back to one sample, so check the resource, not the configured mode. Scene-time AO
    // supports both sample counts.
    const bool depth_available = bound_ds_ && bound_ds_->res;
    // scene_view: a widescreen game view is full width but shorter than the 640x480 surface
    // (letterboxed by the title); a shadow or mirror pass is much smaller.
    if ((render_.dof || render_.ao != 0) && depth_available && bound_rt_ && !a.pretransformed && k.zwrite &&
        bound_rt_->gw == guest_w_ && bound_rt_->gh == guest_h_ &&
        bound_ds_->w == bound_rt_->w && bound_ds_->h == bound_rt_->h && scene_view) {
        const WRange r = DrawWRange(s, sh.prog);
        // A program's w buffer is in buffer units up to zscale. Normalise that scale to the
        // same world-like span the fixed-function projection uses, so the AO radius and DOF
        // distance thresholds mean the same thing for both kinds of draw.
        WRange range = r;
        if (range.far_w > 1000.0f) {
            range.near_w *= 1000.0f / range.far_w;
            range.far_w = 1000.0f;
        }
        if (range.near_w > 0 && range.far_w > range.near_w)
            scene_depth_.insert_or_assign(bound_rt_, SceneDepth{bound_ds_, range.near_w, range.far_w});
    }
    if (!UploadConstants(s, sh, k, stages, size, c)) return;
    if (ext_) binds_ = {};

    if (cap_) {
        CaptureDraw(s, prim, count, k, c, stages, sh.vkey);
        EndDrawCapture(s, a, count, c);
        CaptureShaders(s, a, sh, pk);
    }

    // texture views and samplers, viewport and scissor, blend factor and stencil reference
    SetTextureViewsAndSamplers(s, a, stages);
    SetViewportScissor(s, size.w, size.h, c.tscale[2]);
    SetBlendFactorStencilRef(s);

    // geometry, counted while the title runs a visibility test
    uint32_t query = BeginCountedDraw(s);
    if (defer_now) bundle_desc_.drew = true;
    DrawGeometry(a);
    EndCountedDraw(query);
    if (depth_replay_.pso && bundle_) {
        DepthReplay& d = depth_replay_;
        d.vsc = binds_.vsc;
        d.dc = binds_.dc;
        d.views = binds_.views;
        d.samplers = binds_.samplers;
        std::memcpy(d.blend, binds_.blend, sizeof(d.blend));
        d.stencil = binds_.stencil;
        d.topology = binds_.topology;
        // DrawGeometry left the vertex and index views it uploaded in last_draw_
        d.vb_count = last_draw_.vb_count;
        std::memcpy(d.vbv, last_draw_.vbv, sizeof(d.vbv));
        d.ibv = last_draw_.ibv;
        d.index_count = last_draw_.index_count;
        d.draw_count = last_draw_.draw_count;
        d.base_vertex = last_draw_.base_vertex;
        d.drew = last_draw_.ok;
    }
}

// Start a bundle for the draw whose direct-list target bind has already been recorded. The
// bundle carries the PSO, root arguments, descriptor tables, blend state and geometry; the
// direct list carries the target bind, viewport/scissor and barriers.
// The frame's next pooled bundle list, reset and open for recording; null on failure.
ID3D12GraphicsCommandList* D3D12Backend::AcquireBundleList() {
    Frame& f = CurrentFrame();
    if (!f.bundle_alloc) return nullptr;
    if (f.bundle_next >= f.bundle_pool.size()) {
        ComPtr<ID3D12GraphicsCommandList> list;
        const HRESULT hr = dev_->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_BUNDLE, f.bundle_alloc.Get(), nullptr,
                                                   IID_PPV_ARGS(&list));
        if (FAILED(hr)) {
            LOG_FIRST(1, "d3d12: bundle command list creation failed (0x%08lX)", static_cast<unsigned long>(hr));
            return nullptr;
        }
        // CreateCommandList returns an open list. Close it before the Reset below; Reset on an
        // open list fails and leaves the allocator unusable.
        if (FAILED(list->Close())) {
            LOG_FIRST(1, "d3d12: initial bundle close failed");
            return nullptr;
        }
        f.bundle_pool.push_back(std::move(list));
    }
    ID3D12GraphicsCommandList* list = f.bundle_pool[f.bundle_next].Get();
    const HRESULT hr = list->Reset(f.bundle_alloc.Get(), nullptr);
    if (FAILED(hr)) {
        LOG_FIRST(1, "d3d12: bundle command list reset failed (0x%08lX)", static_cast<unsigned long>(hr));
        return nullptr;
    }
    f.bundle_next++;
    return list;
}

bool D3D12Backend::BeginDeferredBundle(const DrawTextures& stages, bool opaque) {
    Frame& f = CurrentFrame();
    ID3D12GraphicsCommandList* list = AcquireBundleList();
    if (!list) return false;
    bundle_desc_ = {};
    bundle_desc_.list = f.bundle_pool[f.bundle_next - 1];
    bundle_desc_.has_rt = bound_rt_ != nullptr;
    bundle_desc_.has_ds = bound_ds_ != nullptr;
    if (bound_rt_) bundle_desc_.rtv = Cpu(rtv_heap_.Get(), bound_rt_->view, rtv_inc_);
    if (bound_ds_) bundle_desc_.dsv = Cpu(dsv_heap_.Get(), bound_ds_->view, dsv_inc_);
    bundle_desc_.write = bound_rt_ ? bound_rt_->res.Get() : nullptr;
    bundle_desc_.depth_write = bound_ds_ ? bound_ds_->res.Get() : nullptr;
    bundle_desc_.opaque = opaque;
    if (!defer_rt_) defer_rt_ = bound_rt_;
    if (!defer_ds_) defer_ds_ = bound_ds_;
    for (int i = 0; i < 4; i++) bundle_desc_.reads[i] = stages.res[i];
    bundle_ = list;
    // The bundle inherits PSO and topology from nowhere: force this draw to set everything.
    binds_ = {};
    return true;
}

void D3D12Backend::EndDeferredBundle() {
    if (!bundle_) return;
    ID3D12GraphicsCommandList* list = bundle_;
    bundle_ = nullptr;
    const HRESULT hr = list->Close();
    if (FAILED(hr)) {
        bundle_desc_ = {};
        DeviceFailure(hr, "bundle close");
        return;
    }
    // An AO receiver's depth-only copy, recorded from the bindings the main bundle used. It
    // writes only the AO depth target, which no guest draw touches.
    const DepthReplay& d = depth_replay_;
    if (bundle_desc_.drew && bundle_desc_.opaque && d.pso && d.drew) {
        if (ID3D12GraphicsCommandList* dl = AcquireBundleList()) {
            dl->SetPipelineState(d.pso);
            dl->SetGraphicsRootConstantBufferView(0, d.vsc);
            dl->SetGraphicsRootConstantBufferView(1, d.dc);
            dl->SetGraphicsRootDescriptorTable(2, Gpu(srv_heap_.Get(), d.views, srv_inc_));
            dl->SetGraphicsRootDescriptorTable(3, Gpu(smp_heap_.Get(), d.samplers, smp_inc_));
            dl->OMSetBlendFactor(d.blend);
            dl->OMSetStencilRef(d.stencil);
            dl->IASetPrimitiveTopology(d.topology);
            dl->IASetVertexBuffers(0, d.vb_count, d.vbv);
            if (d.index_count) {
                dl->IASetIndexBuffer(&d.ibv);
                dl->DrawIndexedInstanced(d.index_count, 1, 0, d.base_vertex, 0);
            } else {
                dl->DrawInstanced(d.draw_count, 1, 0, 0);
            }
            if (SUCCEEDED(dl->Close())) bundle_desc_.depth_list = CurrentFrame().bundle_pool[CurrentFrame().bundle_next - 1];
        }
    }
    depth_replay_ = {};
    if (bundle_desc_.drew) {
        for (ID3D12Resource* r : bundle_desc_.reads) {
            if (r) defer_reads_.insert(r);
        }
        if (bundle_desc_.write) defer_writes_.insert(bundle_desc_.write);
        if (bundle_desc_.depth_write) defer_writes_.insert(bundle_desc_.depth_write);
    }
    CurrentFrame().bundles.push_back(std::move(bundle_desc_));
    bundle_desc_ = {};
    binds_ = {};
}

// Execute the frame's pending bundles in their original order. Called at present, and before
// any command that would touch a resource a pending bundle still has to write.
void D3D12Backend::FlushDeferredScene() {
    if (bundle_) return;  // bundles are never flushed from inside a recorded draw
    Frame& f = CurrentFrame();
    if (f.bundles.empty()) {
        defer_reads_.clear();
        defer_writes_.clear();
        return;
    }
    ID3D12DescriptorHeap* heaps[] = {srv_heap_.Get(), smp_heap_.Get()};
    cl_->SetDescriptorHeaps(2, heaps);
    cl_->SetGraphicsRootSignature(root_.Get());
    binds_ = {};
    defer_flushes_this_frame_++;
    SIZE_T rtv = 0;
    SIZE_T dsv = 0;
    auto execute = [&](BundleDesc& b) {
        const SIZE_T want_rtv = b.has_rt ? b.rtv.ptr : 0;
        const SIZE_T want_dsv = b.has_ds ? b.dsv.ptr : 0;
        if (want_rtv != rtv || want_dsv != dsv) {
            D3D12_CPU_DESCRIPTOR_HANDLE rh{};
            D3D12_CPU_DESCRIPTOR_HANDLE dh{};
            if (b.has_rt) rh = b.rtv;
            if (b.has_ds) dh = b.dsv;
            cl_->OMSetRenderTargets(b.has_rt ? 1 : 0, b.has_rt ? &rh : nullptr, FALSE, b.has_ds ? &dh : nullptr);
            rtv = want_rtv;
            dsv = want_dsv;
        }
        if (b.has_viewport) cl_->RSSetViewports(1, &b.viewport);
        if (b.has_scissor) cl_->RSSetScissorRects(1, &b.scissor);
        cl_->ExecuteBundle(b.list.Get());
        // The bundle changed PSO, root arguments and possibly primitive topology on the
        // calling list; the CPU-side cache no longer describes it.
        binds_ = {};
        defer_bundles_this_frame_++;
    };
    // Take the list first: an AO pass below that allocates descriptors or uploads may hit a
    // barrier, which must not flush these bundles a second time.
    std::vector<BundleDesc> bundles;
    bundles.swap(f.bundles);
    defer_reads_.clear();
    defer_writes_.clear();

    // 1. AO depth prepass: the receivers' depth-only copies into the AO depth target, so the
    //    AO input holds world geometry only, never light shafts, fog sheets or other effects.
    // 2. AO from that depth, blurred into the full-size occlusion target.
    // 3. Every bundle in its original order. Receivers' pixel shaders scale their colour by
    //    1 - occlusion; effects draw over the darkened scene exactly as the game ordered them.
    uint32_t receivers = 0;
    for (const BundleDesc& b : bundles)
        if (b.depth_list) receivers++;
    bool ao_bound = false;
    if (receivers && render_.ao != 0 && defer_rt_ && defer_ds_) {
        const auto depth = scene_depth_.find(defer_rt_);
        if (depth != scene_depth_.end() && depth->second.target == defer_ds_ && EnsureAoDepth(*defer_ds_)) {
            const D3D12_CPU_DESCRIPTOR_HANDLE dh = Cpu(dsv_heap_.Get(), ao_depth_view_, dsv_inc_);
            const D3D12_CPU_DESCRIPTOR_HANDLE distv = Cpu(rtv_heap_.Get(), kAoDistRtv, rtv_inc_);
            Transition(ao_depth_.Get(), ao_depth_state_, D3D12_RESOURCE_STATE_DEPTH_WRITE);
            Transition(ao_dist_.Get(), ao_dist_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
            // the prepass accumulates across a view's flushes; a scene clear starts it again
            if (std::exchange(ao_depth_clear_, false)) {
                cl_->ClearDepthStencilView(dh, D3D12_CLEAR_FLAG_DEPTH | D3D12_CLEAR_FLAG_STENCIL, 1.0f, 0, 0,
                                           nullptr);
                const float far_dist[4] = {kAoDistClear, 0, 0, 0};
                cl_->ClearRenderTargetView(distv, far_dist, 0, nullptr);
            }
            BindAoView(nullptr);  // the receivers' shaders declare it; the prepass does not read it
            cl_->OMSetRenderTargets(1, &distv, FALSE, &dh);
            for (const BundleDesc& b : bundles) {
                if (!b.depth_list) continue;
                if (b.has_viewport) cl_->RSSetViewports(1, &b.viewport);
                if (b.has_scissor) cl_->RSSetScissorRects(1, &b.scissor);
                cl_->ExecuteBundle(b.depth_list.Get());
            }
            Transition(ao_dist_.Get(), ao_dist_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            ao_bound = RenderAoOcclusion(*defer_rt_, 0.0f, kAoDistClear);  // far: the distance clear
            cl_->SetGraphicsRootSignature(root_.Get());
            if (ao_bound) BindAoView(ao_occ_.Get());
            rtv = dsv = ~SIZE_T(0);
            binds_ = {};
            if (!ao_bound) LOG_FIRST(4, "d3d12: deferred AO pass failed");
        } else {
            LOG_FIRST(4, "d3d12: deferred AO skipped on frame %llu, no scene depth for the display list target",
                      static_cast<unsigned long long>(frame_));
        }
    }
    if (!ao_bound) BindAoView(nullptr);  // receivers draw unchanged
    for (BundleDesc& b : bundles) execute(b);
    if (ao_bound) defer_ao_applied_ = true;
    Begin();
}

// Point the AO root table (parameter 4) at the occlusion target, or at a null view, which reads
// 0 occlusion. Set on the direct list; the receivers' bundles inherit it.
void D3D12Backend::BindAoView(ID3D12Resource* occlusion) {
    const uint32_t slot = AllocSrvSlots(1);
    D3D12_SHADER_RESOURCE_VIEW_DESC sv{};
    sv.Format = kAoFormat;
    sv.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
    sv.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
    sv.Texture2D.MipLevels = 1;
    dev_->CreateShaderResourceView(occlusion, &sv, Cpu(srv_heap_.Get(), slot, srv_inc_));
    cl_->SetGraphicsRootDescriptorTable(4, Gpu(srv_heap_.Get(), slot, srv_inc_));
}

// The AO depth target, matching the scene depth's size and sample count.
bool D3D12Backend::EnsureAoDepth(const Target& like) {
    const D3D12_RESOURCE_DESC want = like.res->GetDesc();
    if (ao_depth_) {
        const D3D12_RESOURCE_DESC have = ao_depth_->GetDesc();
        if (have.Width == want.Width && have.Height == want.Height &&
            have.SampleDesc.Count == want.SampleDesc.Count)
            return true;
        // the old one may still be in use by submitted work
        for (auto& fr : frames_)
            if (fr.fence && !WaitFence(fr.fence)) return false;
        ao_depth_.Reset();
        ao_dist_.Reset();
    }
    if (ao_depth_view_ == UINT32_MAX) {
        if (free_dsv_.empty()) return false;
        ao_depth_view_ = free_dsv_.back();
        free_dsv_.pop_back();
    }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(kDepthTypeless, uint32_t(want.Width), want.Height);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
    td.SampleDesc.Count = want.SampleDesc.Count;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = kDepthFormat;
    cv.DepthStencil.Depth = kDepthFar;
    ao_depth_state_ = D3D12_RESOURCE_STATE_DEPTH_WRITE;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, ao_depth_state_, &cv,
                                             IID_PPV_ARGS(&ao_depth_)))) {
        LOGE("d3d12: AO depth target could not be created");
        return false;
    }
    D3D12_DEPTH_STENCIL_VIEW_DESC dv{};
    dv.Format = kDepthFormat;
    dv.ViewDimension = td.SampleDesc.Count > 1 ? D3D12_DSV_DIMENSION_TEXTURE2DMS : D3D12_DSV_DIMENSION_TEXTURE2D;
    dev_->CreateDepthStencilView(ao_depth_.Get(), &dv, Cpu(dsv_heap_.Get(), ao_depth_view_, dsv_inc_));
    // the view distance target: same size and sample count, so it pairs with the depth
    D3D12_RESOURCE_DESC dd = Texture2DDesc(kAoDistFormat, uint32_t(want.Width), want.Height);
    dd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    dd.SampleDesc.Count = want.SampleDesc.Count;
    D3D12_CLEAR_VALUE dcv{};
    dcv.Format = kAoDistFormat;
    dcv.Color[0] = kAoDistClear;
    ao_dist_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &dd, ao_dist_state_, &dcv,
                                             IID_PPV_ARGS(&ao_dist_)))) {
        LOGE("d3d12: AO distance target could not be created");
        ao_depth_.Reset();
        return false;
    }
    dev_->CreateRenderTargetView(ao_dist_.Get(), nullptr, Cpu(rtv_heap_.Get(), kAoDistRtv, rtv_inc_));
    ao_depth_clear_ = true;
    LOGR("d3d12: AO depth prepass target %llux%u, %u sample(s)", static_cast<unsigned long long>(want.Width),
         want.Height, td.SampleDesc.Count);
    return true;
}

bool D3D12Backend::EnsureAoOcclusion(uint32_t w, uint32_t h) {
    if (ao_occ_ && ao_occ_w_ == w && ao_occ_h_ == h) return true;
    if (ao_occ_) {
        for (auto& fr : frames_)
            if (fr.fence && !WaitFence(fr.fence)) return false;
        ao_occ_.Reset();
    }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(kAoFormat, w, h);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ao_occ_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, ao_occ_state_, nullptr,
                                             IID_PPV_ARGS(&ao_occ_)))) {
        LOGE("d3d12: AO occlusion target could not be created");
        return false;
    }
    dev_->CreateRenderTargetView(ao_occ_.Get(), nullptr, Cpu(rtv_heap_.Get(), kAoOccRtv, rtv_inc_));
    ao_occ_w_ = w;
    ao_occ_h_ = h;
    return true;
}

// AO from the prepass depth (already readable), then the depth-aware blur and upsample into
// the full-size occlusion target, left readable for the receivers' pixel shaders.
bool D3D12Backend::RenderAoOcclusion(Target& rt, float near_z, float far_z) {
    dof_depth_ = ao_dist_.Get();
    dof_near_ = near_z;
    dof_far_ = far_z;
    RenderHalfAo(rt);
    dof_depth_ = nullptr;
    if (!ao_half_ready_ || !EnsureAoOcclusion(rt.w, rt.h)) return false;
    const bool depth_ms = ao_dist_->GetDesc().SampleDesc.Count > 1;
    const std::string shader = std::string("#define DEPTH_MS ") + (depth_ms ? "1\n" : "0\n") + kAoOcclusionHlsl;
    if (!EnsurePassPipeline(shader.c_str(), "psmain", kAoFormat, -1)) return false;
    Transition(ao_occ_.Get(), ao_occ_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12Resource* src[3] = {ao_half_.Get(), ao_dist_.Get(), nullptr};
    DXGI_FORMAT fmt[3] = {kAoFormat, kAoDistFormat, kBgra};
    // cbuffer { float4 src; float4 planes; }: texel size in src.xy, near/far in planes.xy
    const float p[8] = {1.0f / float(std::max(ao_half_w_, 1u)), 1.0f / float(std::max(ao_half_h_, 1u)), 0, 0,
                        near_z, far_z, 0, 0};
    Pass(shader.c_str(), "psmain", kAoFormat, Cpu(rtv_heap_.Get(), kAoOccRtv, rtv_inc_), ao_occ_w_, ao_occ_h_, src,
         fmt, p, -1);
    Transition(ao_occ_.Get(), ao_occ_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return true;
}

// Pending bundles must execute before an immediate command that reads or rewrites their
// targets. This is the normal materialisation path, not a failure: the frame keeps deferring
// after the command. Only a conflict while a bundle is open forces the old immediate fallback.
bool D3D12Backend::DeferredBarrier(const char* why) {
    if (!defer_scene_ || CurrentFrame().bundles.empty()) return false;
    if (bundle_) {
        DeferredFallback(why);
        return false;
    }
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 40)
        LOGR("d3d12: deferred barrier frame %llu: %s", static_cast<unsigned long long>(frame_), why);
    FlushDeferredScene();
    return true;
}

// Stop deferring this frame. Pending bundles are executed first so the immediate commands
// that follow keep the title's original order.
void D3D12Backend::DeferredFallback(const char* why) {
    if (!defer_scene_ || defer_abandoned_) return;
    defer_frame_active_ = true;
    if (!defer_reason_) defer_reason_ = why;
    FlushDeferredScene();
    defer_abandoned_ = true;
    static std::atomic<int> logged{0};
    if (logged.fetch_add(1, std::memory_order_relaxed) < 4)
        LOGR("d3d12: deferred scene fell back for frame %llu: %s",
             static_cast<unsigned long long>(frame_), why);
}

void D3D12Backend::ReportDeferred() {
    if (!defer_scene_ || !defer_frame_active_) return;
    if (!defer_bundles_this_frame_ && !defer_abandoned_) return;
    if (defer_abandoned_) {
        LOGR("deferred: frame %llu replayed %u bundle(s), %u flush(es), fell back to immediate: %s",
             static_cast<unsigned long long>(frame_), defer_bundles_this_frame_, defer_flushes_this_frame_,
             defer_reason_ ? defer_reason_ : "unknown");
    } else if (!defer_reported_) {
        LOGR("deferred: frame %llu replayed %u bundle(s), %u flush(es), AO %s, no fallback",
             static_cast<unsigned long long>(frame_), defer_bundles_this_frame_, defer_flushes_this_frame_,
             defer_ao_applied_ ? "applied" : "off");
    }
    defer_reported_ = true;
}

// a guest rect scaled to the target and clamped to it
D3D12_RECT ScaledRect(const Target& t, int x0, int y0, int x1, int y1) {
    float sx = float(t.w) / t.gw, sy = float(t.h) / t.gh;
    return {std::clamp<LONG>(LONG(x0 * sx), 0, t.w), std::clamp<LONG>(LONG(y0 * sy), 0, t.h),
            std::clamp<LONG>(LONG(x1 * sx), 0, t.w), std::clamp<LONG>(LONG(y1 * sy), 0, t.h)};
}

// rs: the rects buffer, reused across targets
void D3D12Backend::ClearTarget(Target& t, const State& s, const ClearArgs& c, std::vector<D3D12_RECT>& rs) {
    if (t.depth) t.depth_resolved = false;
    rs.clear();
    if (c.count && c.rects) {
        for (uint32_t i = 0; i < c.count; i++) {
            int32_t r[4];
            std::memcpy(r, c.rects + size_t(i) * sizeof(r), sizeof(r));
            rs.push_back(ScaledRect(t, r[0], r[1], r[2], r[3]));
        }
    } else {
        const auto& v = s.viewport;
        rs.push_back(ScaledRect(t, int(v.x), int(v.y), int(v.x + v.w), int(v.y + v.h)));
    }
    if (t.depth) {
        if (c.flags & kClearZ)
            std::erase_if(scene_depth_, [&](const auto& entry) { return entry.second.target == &t; });
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_DEPTH_WRITE);
        D3D12_CLEAR_FLAGS cf = D3D12_CLEAR_FLAGS(((c.flags & kClearZ) ? D3D12_CLEAR_FLAG_DEPTH : 0) |
                                                 ((c.flags & kClearStencil) ? D3D12_CLEAR_FLAG_STENCIL : 0));
        float reversed = 1 - std::clamp(c.z, 0.0f, 1.0f);
        cl_->ClearDepthStencilView(Cpu(dsv_heap_.Get(), t.view, dsv_inc_), cf, reversed, UINT8(c.stencil),
                                   UINT(rs.size()), rs.data());
    } else {
        // pixels the CPU wrote before this clear go in first, a later upload would paint over the clear
        if (LatchCpuWrite(t)) UploadTarget(t, s.rt);
        Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
        float col[4];
        Color(c.color, col);
        for (int i = 0; t.range != 1 && i < 3; i++) col[i] /= t.range;
        // D3DCLEAR_TARGET_R/G/B/A are bits 4 to 7, D3D12's write mask has the same order
        uint8_t mask = uint8_t((c.flags & kClearTarget) >> 4);
        scene_depth_.erase(&t);
        if (mask == D3D12_COLOR_WRITE_ENABLE_ALL) {
            cl_->ClearRenderTargetView(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), col, UINT(rs.size()), rs.data());
        } else {
            ClearChannels(t, col, mask, rs);
        }
    }
    t.drawn = true;
    t.scene_frame = frame_;
}

// ClearChannels' pipeline for the target format and channel mask, kept in blit_psos_; null when it can't be made
ID3D12PipelineState* D3D12Backend::EnsureClearPipeline(DXGI_FORMAT rtv, uint8_t mask, uint32_t samples) {
    uint64_t seed = uint64_t(samples) << 40 | uint64_t(rtv) << 8 | mask;
    auto& pso = blit_psos_[Hash(kClearHlsl, std::strlen(kClearHlsl), seed)];
    if (pso) return pso.Get();
    uint64_t h;
    ID3DBlob* vs = Compile(kClearHlsl, "vs", "vs_5_0", h);
    ID3DBlob* ps = Compile(kClearHlsl, "psmain", "ps_5_0", h);
    if (!vs || !ps) return nullptr;
    D3D12_GRAPHICS_PIPELINE_STATE_DESC d = FullScreenPipelineDesc(vs, ps, rtv);
    d.SampleDesc.Count = samples;
    d.BlendState.RenderTarget[0].RenderTargetWriteMask = mask;
    if (FAILED(CreatePipeline(d, pso))) return nullptr;
    return pso.Get();
}

// a clear of some colour channels: drawn over each rect writing only those
void D3D12Backend::ClearChannels(Target& t, const float col[4], uint8_t mask, const std::vector<D3D12_RECT>& rs) {
    ID3D12PipelineState* pso = EnsureClearPipeline(t.fmt, mask, t.res->GetDesc().SampleDesc.Count);
    if (!pso) {
        LOG_FIRST(4, "d3d12: channel clear pipeline failed, mask %X", mask);
        return;
    }
    SetBlitTarget(Cpu(rtv_heap_.Get(), t.view, rtv_inc_), t.w, t.h);
    Alloc cb = Upload(256, 256);
    if (!cb.res) return;
    std::memcpy(cb.cpu, col, 4 * sizeof(float));
    uint32_t slot = AllocSrvSlots(1);
    for (const D3D12_RECT& r : rs) {
        cl_->RSSetScissorRects(1, &r);
        binds_ = {};
        DrawFullScreen(pso, cb.gpu, slot);
    }
}

void D3D12Backend::Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                         uint32_t stencil) {
    PerfScope perf_clear{g_perf.clear};
    if (cap_) {
        CapStep(s.rt);
        fprintf(cap_, "%04u clear rt %08X ds %08X flags %X color %08X z %.3f\n", cap_i_++, s.rt.data, s.zs.data, flags,
                color, z);
    }
    Target* rt = (flags & kClearTarget) ? EnsureTarget(s.rt, false) : nullptr;
    Target* ds = (flags & (kClearZ | kClearStencil)) ? EnsureTarget(s.zs, true) : nullptr;
    const bool scene_clear = (rt && rt == defer_rt_) || (ds && ds == defer_ds_);
    if ((rt && DeferredPendingResource(rt->res.Get())) || (ds && DeferredPendingResource(ds->res.Get()))) {
        // The pending draws write this target; run them before the clear. The frame stays
        // deferred, and a clear of the scene targets starts a new scene for AO tracking.
        if (!bundle_) DeferredBarrier("clear touched a deferred target");
        else DeferredFallback("clear touched a deferred target");
    }
    // a full-size depth clear starts a new scene view: the AO depth prepass starts empty again
    if (ds && (flags & kClearZ) && ds->gw == guest_w_ && ds->gh == guest_h_) ao_depth_clear_ = true;
    if (scene_clear) {
        LOG_FIRST(4, "d3d12: deferred scene clear rt %08X ds %08X flags %X", rt ? rt->data : 0, ds ? ds->data : 0,
                  flags);
        defer_ao_applied_ = false;
        defer_rt_ = nullptr;
        defer_ds_ = nullptr;
    }
    uint32_t gw;
    uint32_t gh;
    SurfaceSize(rt ? s.rt : s.zs, gw, gh);
    std::vector<D3D12_RECT> rs;
    ClearArgs c{.count = count, .rects = rects, .flags = flags, .color = color, .z = z, .stencil = stencil};
    for (Target* t : {rt, ds}) {
        if (t) ClearTarget(*t, s, c, rs);
    }
}

// CopyRects' guest lists: src_rects RECTs, dst_points POINTs, all int32
constexpr uint32_t kGuestRectBytes = 16;
constexpr uint32_t kRectLeft = 0;
constexpr uint32_t kRectTop = 4;
constexpr uint32_t kRectRight = 8;
constexpr uint32_t kRectBottom = 12;
constexpr uint32_t kGuestPointBytes = 8;
constexpr uint32_t kPointX = 0;
constexpr uint32_t kPointY = 4;

struct GuestRect {
    int32_t left;
    int32_t top;
    int32_t right;
    int32_t bottom;
};

struct GuestPoint {
    int32_t x;
    int32_t y;
};

GuestRect ReadGuestRect(uint32_t rects, uint32_t i) {
    uint32_t r = rects + i * kGuestRectBytes;
    GuestRect g;
    g.left = int32_t(kernel::R32(r + kRectLeft));
    g.top = int32_t(kernel::R32(r + kRectTop));
    g.right = int32_t(kernel::R32(r + kRectRight));
    g.bottom = int32_t(kernel::R32(r + kRectBottom));
    return g;
}

GuestPoint ReadGuestPoint(uint32_t points, uint32_t i) {
    uint32_t p = points + i * kGuestPointBytes;
    GuestPoint g;
    g.x = int32_t(kernel::R32(p + kPointX));
    g.y = int32_t(kernel::R32(p + kPointY));
    return g;
}

// a drawn copy's rect in guest pixels: the source corners and where it lands
struct DrawnRect {
    float x0 = 0;
    float y0 = 0;
    float x1 = 0;
    float y1 = 0;
    float dx = 0;
    float dy = 0;
};

// without dst_points the rect lands where it is
DrawnRect ReadDrawnRect(uint32_t src_rects, uint32_t dst_points, uint32_t i) {
    GuestRect g = ReadGuestRect(src_rects, i);
    DrawnRect r;
    r.x0 = float(g.left);
    r.y0 = float(g.top);
    r.x1 = float(g.right);
    r.y1 = float(g.bottom);
    r.dx = r.x0;
    r.dy = r.y0;
    if (dst_points) {
        GuestPoint p = ReadGuestPoint(dst_points, i);
        r.dx = float(p.x);
        r.dy = float(p.y);
    }
    return r;
}

// a guest coordinate read as unsigned, times the host scale
UINT ScaleUnsigned(int32_t v, float sc) { return UINT(uint32_t(v) * sc); }

// a drawn copy's taps round down a ratio a hair over a whole number
constexpr float kTapsSlack = 1e-3f;

// one Blit per rect: for formats that differ
void D3D12Backend::CopyByDraw(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                              Target& dst_t, const CopyList& l) {
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), dst_t.view, rtv_inc_);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    float src_scale = float(src_t.w) / src_t.gw, src_scale_y = float(src_t.h) / src_t.gh;
    float dst_scale = float(dst_t.w) / dst_t.gw, dst_scale_y = float(dst_t.h) / dst_t.gh;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        float whole_w = float(std::min(src_t.gw, dst_t.gw));
        float whole_h = float(std::min(src_t.gh, dst_t.gh));
        DrawnRect r{.x0 = 0, .y0 = 0, .x1 = whole_w, .y1 = whole_h};
        if (listed) r = ReadDrawnRect(l.src_rects, l.dst_points, i);
        if (r.x1 <= r.x0 || r.y1 <= r.y0) continue;
        D3D12_VIEWPORT vp{r.dx * dst_scale, r.dy * dst_scale_y, (r.x1 - r.x0) * dst_scale,
                          (r.y1 - r.y0) * dst_scale_y, 0, 1};
        LONG right = std::min(LONG(vp.TopLeftX + vp.Width), LONG(dst_t.w));
        LONG bottom = std::min(LONG(vp.TopLeftY + vp.Height), LONG(dst_t.h));
        D3D12_RECT scissor{LONG(vp.TopLeftX), LONG(vp.TopLeftY), right, bottom};
        if (scissor.right <= scissor.left || scissor.bottom <= scissor.top) continue;
        cl_->RSSetViewports(1, &vp);
        cl_->RSSetScissorRects(1, &scissor);
        binds_ = {};
        float uv[4] = {r.x0 / src_t.gw, r.y0 / src_t.gh, (r.x1 - r.x0) / src_t.gw, (r.y1 - r.y0) / src_t.gh};
        float ratio = std::max({src_scale / dst_scale, src_scale_y / dst_scale_y, 1.0f});
        uint32_t taps = uint32_t(std::ceil(ratio - kTapsSlack));
        float mul = 1 / dst_t.range;
        BlitOptions o{.alpha = true, .taps = taps, .mul = mul, .rtv = dst_t.fmt,
                      .samples = dst_t.res->GetDesc().SampleDesc.Count};
        Blit(src_res, src_t.fmt, uv, o);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

// one CopyTextureRegion per rect, clamped to both targets
void D3D12Backend::CopyByRegion(const Target& src_t, ID3D12Resource* src_res, D3D12_RESOURCE_STATES& src_state,
                                Target& dst_t, const CopyList& l) {
    Transition(src_res, src_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    Transition(dst_t.res.Get(), dst_t.state, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{dst_t.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{src_res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    float dst_scale = float(dst_t.w) / dst_t.gw, dst_scale_y = float(dst_t.h) / dst_t.gh;
    bool listed = l.count && l.src_rects;
    uint32_t n = listed ? l.count : 1;
    for (uint32_t i = 0; i < n; i++) {
        D3D12_BOX b{0, 0, 0, std::min(src_t.w, dst_t.w), std::min(src_t.h, dst_t.h), 1};
        uint32_t dx = 0;
        uint32_t dy = 0;
        if (listed) {
            GuestRect r = ReadGuestRect(l.src_rects, i);
            b.left = ScaleUnsigned(r.left, dst_scale);
            b.top = ScaleUnsigned(r.top, dst_scale_y);
            b.right = std::min(ScaleUnsigned(r.right, dst_scale), src_t.w);
            b.bottom = std::min(ScaleUnsigned(r.bottom, dst_scale_y), src_t.h);
            dx = b.left;
            dy = b.top;
            if (l.dst_points) {
                GuestPoint p = ReadGuestPoint(l.dst_points, i);
                dx = ScaleUnsigned(p.x, dst_scale);
                dy = ScaleUnsigned(p.y, dst_scale_y);
            }
        }
        if (b.right <= b.left || b.bottom <= b.top || dx >= dst_t.w || dy >= dst_t.h) continue;
        b.right = std::min<UINT>(b.right, b.left + dst_t.w - dx);
        b.bottom = std::min<UINT>(b.bottom, b.top + dst_t.h - dy);
        cl_->CopyTextureRegion(&dl, dx, dy, 0, &sl, &b);
    }
    dst_t.drawn = true;
    bound_rt_ = bound_ds_ = nullptr;
}

void D3D12Backend::Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects,
                        uint32_t dst_points) {
    PerfScope perf_copy{g_perf.copy};
    Target* src_t = FindTarget(src, false);
    if ((src_t && DeferredPendingResource(src_t->res.Get())) ||
        [&] {
            Target* dst_t = FindTarget(dst, false);
            return dst_t && DeferredPendingResource(dst_t->res.Get());
        }())
        DeferredBarrier("copy touched a deferred target");
    if (cap_) {
        const char* from = "memory";
        if (src_t) from = src_t->drawn ? "target" : "idle target";
        fprintf(cap_, "%04u copy %08X %s to %08X\n", cap_i_++, src.data, from, dst.data);
    }
    // pixels the CPU made: the copy happens in memory, textures and targets pick it up from there
    if (!src_t || !src_t->drawn) {
        if (!CopySurfaceMemory(src, dst, count, src_rects, dst_points)) {
            LOG_FIRST(8, "d3d12: CopyRects %08X to %08X skipped", src.data, dst.data);
        } else if (Target* dst_t = FindTarget(dst, false)) {
            UploadTarget(*dst_t, dst);
        }
        return;
    }
    Resolve(*src_t);
    Target* dst_t = EnsureTarget(dst, false);
    if (!dst_t || src_t == dst_t) return;
    ID3D12Resource* copy_src = ResolveColor(*src_t);
    if (!copy_src) return;
    auto& copy_state = copy_src == src_t->res.Get() ? src_t->state : src_t->resolved_state;
    CopyList l{.count = count, .src_rects = src_rects, .dst_points = dst_points};
    if (src_t->fmt != dst_t->fmt || dst_t->res->GetDesc().SampleDesc.Count > 1)
        CopyByDraw(*src_t, copy_src, copy_state, *dst_t, l);
    else
        CopyByRegion(*src_t, copy_src, copy_state, *dst_t, l);

    scene_depth_.erase(dst_t);
    bool full = !count || !src_rects;
    if (count == 1 && src_rects) {
        const auto r = ReadDrawnRect(src_rects, dst_points, 0);
        full = r.x0 == 0 && r.y0 == 0 && r.dx == 0 && r.dy == 0 &&
               r.x1 == float(src_t->gw) && r.y1 == float(src_t->gh);
    }
    if (full && src_t->gw == dst_t->gw && src_t->gh == dst_t->gh) {
        dst_t->scene_frame = src_t->scene_frame;
        dst_t->cpu = src_t->cpu || src_t->picture;
        if (auto depth = scene_depth_.find(src_t); depth != scene_depth_.end()) {
            const SceneDepth source = depth->second;
            scene_depth_.insert_or_assign(dst_t, source);
        }
    }
    if (cap_) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_copy_%08X", cap_i_, dst_t->data);
        CapSave(*dst_t, name);
    }
}

bool D3D12Backend::ResizeSwap() {
    HWND hwnd = static_cast<HWND>(app::Window());
    RECT rc;
    GetClientRect(hwnd, &rc);
    uint32_t w = std::max<LONG>(rc.right - rc.left, 1);
    uint32_t h = std::max<LONG>(rc.bottom - rc.top, 1);
    if (w == bb_w_ && h == bb_h_) return std::all_of(std::begin(bb_), std::end(bb_), [](const auto& buffer) { return bool(buffer); });
    Flush();
    for (auto& f : frames_) {
        if (f.fence && !WaitFence(f.fence)) {
            LOGE("d3d12: resize could not drain frame fence %llu", static_cast<unsigned long long>(f.fence));
            return false;
        }
    }
    for (auto& b : bb_) b.Reset();
    const HRESULT hr =
        swap_->ResizeBuffers(kFrames, w, h, DXGI_FORMAT_B8G8R8A8_UNORM, tearing_ ? DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING : 0);
    if (FAILED(hr)) {
        // ResizeBuffers leaves the swap chain as it was when it fails; rebuild the views so the
        // frame can continue at the previous size, and report device loss explicitly.
        LOGE("d3d12: ResizeBuffers(%ux%u) failed: 0x%08lX (device removed: 0x%08lX)", w, h, hr,
             dev_->GetDeviceRemovedReason());
        if (FAILED(dev_->GetDeviceRemovedReason())) return false;
        return CreateBackBufferViews();
    }
    if (!CreateBackBufferViews()) {
        LOGE("d3d12: back buffer views failed after resize to %ux%u; rendering cannot continue", w, h);
        return false;
    }
    bb_w_ = w;
    bb_h_ = h;
    return true;
}

void D3D12Backend::FinishCapture(const Surface& front) {
    CapStep(Surface{});
    if (Target* f = FindTarget(front, false)) CapSave(*f, "final");
    for (auto& t : cap_tex_) {
        char name[32];
        snprintf(name, sizeof(name), "tex_%08X", t.data);
        D3D12_RESOURCE_STATES st = D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE;
        CapSave({.res = t.res.Get(), .state = &st, .fmt = t.fmt, .w = t.w, .h = t.h}, name);
    }
    fclose(cap_);
    cap_ = nullptr;
    cap_tex_.clear();
    LOGI("d3d12: frame captured to %s", cap_dir_.string().c_str());
}

void D3D12Backend::StartCapture() {
    SYSTEMTIME tm;
    GetLocalTime(&tm);
    char stamp[32];
    snprintf(stamp, sizeof(stamp), "%02u%02u%02u", tm.wHour, tm.wMinute, tm.wSecond);
    std::error_code ec;
    cap_dir_ = std::filesystem::weakly_canonical(app::ExeDir() / diag::CaptureDir() / stamp, ec);
    std::filesystem::create_directories(cap_dir_, ec);
    cap_ = _wfopen((cap_dir_ / "frame.txt").c_str(), L"w");
    cap_i_ = cap_rt_ = 0;
    cap_seen_.clear();
    cap_sh_.clear();
}

// ramp_ holds the red, green and blue ramps, 256 entries each
constexpr int kRampRed = 0;
constexpr int kRampGreen = 256;
constexpr int kRampBlue = 512;

void D3D12Backend::UploadGammaRamp() {
    constexpr uint32_t kRampBytes = 256 * 4;  // one BGRA texel per ramp entry
    ramp_dirty_ = false;
    Alloc a = Upload(kRampBytes + D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
    if (!a.res) return;
    bool identity = true;
    for (int i = 0; i < 256; i++) {
        uint8_t* p = a.cpu + i * 4;
        p[0] = ramp_[kRampBlue + i];
        p[1] = ramp_[kRampGreen + i];
        p[2] = ramp_[kRampRed + i];
        p[3] = 255;
        identity &= ramp_[kRampRed + i] == i && ramp_[kRampGreen + i] == i && ramp_[kRampBlue + i] == i;
    }
    gamma_ = !identity;
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dl{lut_.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    sl.PlacedFootprint.Offset = a.off;
    sl.PlacedFootprint.Footprint = {DXGI_FORMAT_B8G8R8A8_UNORM, 256, 1, 1, kRampBytes};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
}

void D3D12Backend::BlitFrame(Target& t, ID3D12Resource* aa) {
    // at the aspect setting (by default the output's shape); 4:3 when the console reports a 4:3 TV. A picture the
    // CPU wrote (movies, legal screens) was made for 16:9 and is shown that way in the middle of a wider frame.
    float aspect = app::Widescreen() ? app::AspectRatio() : 4.0f / 3.0f;
    const bool scene = t.scene_frame == frame_;
    if (t.cpu || scene) t.picture = t.cpu && !scene;
    t.cpu = false;
    t.scene_frame = ~0ull;
    if (t.picture && aspect > 16.0f / 9.0f) aspect = 16.0f / 9.0f;
    float w = float(bb_w_);
    float h = float(bb_h_);
    float vw = std::min(w, h * aspect);
    float vh = vw / aspect;
    float uv[4] = {0, 0, 1, 1};
    D3D12_VIEWPORT vp{(w - vw) / 2, (h - vh) / 2, vw, vh, 0, 1};
    D3D12_RECT scissor{0, 0, LONG(bb_w_), LONG(bb_h_)};
    cl_->RSSetViewports(1, &vp);
    cl_->RSSetScissorRects(1, &scissor);
    binds_ = {};
    const float post = !t.picture && render_.pc_contrast ? 1.0f : 0.0f;
    const float bloom = scene && render_.bloom ? 1.0f : 0.0f;
    const float dof = scene && render_.dof && dof_depth_ ? 1.0f : 0.0f;
    if (aa) {  // ApplySmaa left it readable
        Blit(aa, t.fmt, uv, {.gamma = gamma_, .post = post, .bloom = bloom, .src_w = float(t.w), .src_h = float(t.h),
                             .depth = dof_depth_, .dof = dof, .near_z = dof_near_, .far_z = dof_far_});
        return;
    }
    ID3D12Resource* color = ResolveColor(t);
    if (!color) return;
    auto& state = color == t.res.Get() ? t.state : t.resolved_state;
    Transition(color, state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Blit(color, t.fmt, uv, {.gamma = gamma_, .post = post, .bloom = bloom, .src_w = float(t.w), .src_h = float(t.h),
                            .depth = dof_depth_, .dof = dof, .near_z = dof_near_, .far_z = dof_far_});
}

void D3D12Backend::RenderUi() {
    std::lock_guard lock(app::UiLock());
    ImGui_ImplDX12_NewFrame();
    ImGui_ImplWin32_NewFrame();
    ImGui::NewFrame();
    if (app::kGame.ui) app::kGame.ui(fps_);
    ImGui::Render();
    ID3D12DescriptorHeap* uh[] = {ui_heap_.Get()};
    cl_->SetDescriptorHeaps(1, uh);
    ImGui_ImplDX12_RenderDrawData(ImGui::GetDrawData(), cl_);
}

// SMAA 1x (third_party/smaa, high preset) on the finished frame before it is shown: luma edges, blending weights
// from the reference area and search textures, then neighbourhood blending. The HUD is part of the frame and is
// smoothed with it. The macros map the reference's samplers onto the passes' s0 (linear) and s1 (point), clamped.
const char* kSmaaPrologue = R"(
cbuffer B : register(b1) { float4 metrics; float4 unused; };
SamplerState ls : register(s0);
SamplerState ps : register(s1);
#define SMAA_RT_METRICS metrics
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
#define SMAA_FLATTEN [flatten]
#define SMAA_BRANCH [branch]
#define SMAATexture2DMS2(tex) Texture2DMS<float4, 2> tex
#define SMAALoad(tex, pos, sample) tex.Load(pos, sample)
#define SMAAGather(tex, coord) tex.Gather(ls, coord, 0)
)";

// a full screen triangle; uv 0..1 over the target
const char* kSmaaTriangle = R"(
float2 FullScreen(uint id, out float4 pos) {
    float2 t = float2((id << 1) & 2, id & 2);
    pos = float4(t * float2(2, -2) + float2(-1, 1), 0, 1);
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

// a pass's whole source, built once; render thread only
const std::string& SmaaSource(const char* tail) {
    static std::unordered_map<const char*, std::string> sources;
    std::string& src = sources[tail];
    if (src.empty()) src = std::string(kSmaaPrologue) + kSmaaHlsl + kSmaaTriangle + tail;
    return src;
}

constexpr DXGI_FORMAT kSmaaEdges = DXGI_FORMAT_R8G8_UNORM;
constexpr DXGI_FORMAT kSmaaWeights = DXGI_FORMAT_R8G8B8A8_UNORM;
constexpr DXGI_FORMAT kSmaaArea = DXGI_FORMAT_R8G8_UNORM;
constexpr DXGI_FORMAT kSmaaSearch = DXGI_FORMAT_R8_UNORM;

// the lookups once; the three targets whenever the frame's size or format changes
bool D3D12Backend::EnsureSmaa(const Target& t) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    auto upload = [&](SmaaTexture& tex, DXGI_FORMAT fmt, uint32_t w, uint32_t h, uint32_t pitch,
                      const uint8_t* bytes) {
        D3D12_RESOURCE_DESC td = Texture2DDesc(fmt, w, h);
        tex.state = D3D12_RESOURCE_STATE_COPY_DEST;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, tex.state, nullptr,
                                                 IID_PPV_ARGS(&tex.res))))
            return false;
        const uint32_t up = UploadPitch(pitch);
        Alloc a = Upload(uint64_t(up) * h, D3D12_TEXTURE_DATA_PLACEMENT_ALIGNMENT);
        if (!a.res) return false;
        for (uint32_t y = 0; y < h; y++) std::memcpy(a.cpu + size_t(y) * up, bytes + size_t(y) * pitch, pitch);
        D3D12_TEXTURE_COPY_LOCATION dl{tex.res.Get(), D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
        D3D12_TEXTURE_COPY_LOCATION sl{a.res, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
        sl.PlacedFootprint.Offset = a.off;
        sl.PlacedFootprint.Footprint = {fmt, w, h, 1, up};
        cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
        Transition(tex.res.Get(), tex.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
        return true;
    };
    if (!smaa_.area.res) {
        if (!upload(smaa_.area, kSmaaArea, AREATEX_WIDTH, AREATEX_HEIGHT, AREATEX_PITCH, areaTexBytes) ||
            !upload(smaa_.search, kSmaaSearch, SEARCHTEX_WIDTH, SEARCHTEX_HEIGHT, SEARCHTEX_PITCH, searchTexBytes))
            return false;
    }
    if (smaa_.edges.res && smaa_.w == t.w && smaa_.h == t.h && smaa_.fmt == t.fmt) return true;
    // The three view slots are fixed, so no earlier frame may still reference
    // them when they are repointed. This only runs on a scale/format change.
    Flush();
    for (auto& f : frames_)
        if (f.fence && !WaitFence(f.fence)) return false;
    auto target = [&](SmaaTexture& tex, DXGI_FORMAT fmt, uint32_t index) {
        if (tex.res) Release(std::move(tex.res));
        D3D12_RESOURCE_DESC td = Texture2DDesc(fmt, t.w, t.h);
        td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        tex.state = D3D12_RESOURCE_STATE_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, tex.state, nullptr,
                                                 IID_PPV_ARGS(&tex.res))))
            return false;
        dev_->CreateRenderTargetView(tex.res.Get(), nullptr,
                                     Cpu(rtv_heap_.Get(), kRtvCount + kFrames + index, rtv_inc_));
        return true;
    };
    smaa_.w = smaa_.h = 0;
    if (!target(smaa_.edges, kSmaaEdges, 0) || !target(smaa_.weights, kSmaaWeights, 1) ||
        !target(smaa_.output, t.fmt, 2))
        return false;
    smaa_.w = t.w;
    smaa_.h = t.h;
    smaa_.fmt = t.fmt;
    LOGI("d3d12: SMAA targets %ux%u", t.w, t.h);
    return true;
}

ID3D12Resource* D3D12Backend::ApplySmaa(Target& t) {
    if (smaa_.failed || t.depth || !t.w || !t.h) return nullptr;
    if (!EnsureSmaa(t)) {
        smaa_.failed = true;
        LOGE("d3d12: SMAA resources could not be created, SMAA is off");
        return nullptr;
    }
    auto rtv = [&](uint32_t index) { return Cpu(rtv_heap_.Get(), kRtvCount + kFrames + index, rtv_inc_); };
    const float metrics[8] = {1.0f / float(t.w), 1.0f / float(t.h), float(t.w), float(t.h)};
    const float clear[4] = {0, 0, 0, 0};

    ID3D12Resource* color = ResolveColor(t);
    if (!color) return nullptr;
    auto& state = color == t.res.Get() ? t.state : t.resolved_state;
    Transition(color, state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(smaa_.edges.res.Get(), smaa_.edges.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl_->ClearRenderTargetView(rtv(0), clear, 0, nullptr);
    {
        ID3D12Resource* src[3] = {color, nullptr, nullptr};
        DXGI_FORMAT fmt[3] = {t.fmt, kBgra, kBgra};
        Pass(SmaaSource(kSmaaEdgesTail).c_str(), "edges", kSmaaEdges, rtv(0), t.w, t.h, src, fmt, metrics, -1);
    }
    Transition(smaa_.edges.res.Get(), smaa_.edges.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(smaa_.weights.res.Get(), smaa_.weights.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    cl_->ClearRenderTargetView(rtv(1), clear, 0, nullptr);
    {
        ID3D12Resource* src[3] = {smaa_.edges.res.Get(), smaa_.area.res.Get(), smaa_.search.res.Get()};
        DXGI_FORMAT fmt[3] = {kSmaaEdges, kSmaaArea, kSmaaSearch};
        Pass(SmaaSource(kSmaaWeightsTail).c_str(), "weights", kSmaaWeights, rtv(1), t.w, t.h, src, fmt, metrics,
             -1);
    }
    Transition(smaa_.weights.res.Get(), smaa_.weights.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(smaa_.output.res.Get(), smaa_.output.state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    {
        ID3D12Resource* src[3] = {color, smaa_.weights.res.Get(), nullptr};
        DXGI_FORMAT fmt[3] = {t.fmt, kSmaaWeights, kBgra};
        Pass(SmaaSource(kSmaaBlendTail).c_str(), "blend", t.fmt, rtv(2), t.w, t.h, src, fmt, metrics, -1);
    }
    Transition(smaa_.output.res.Get(), smaa_.output.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    return smaa_.output.res.Get();
}

bool D3D12Backend::EnsureAoHalf(uint32_t w, uint32_t h) {
    const uint32_t aw = render_.ao == 1 ? std::max(1u, w / 2) : std::max(1u, w);
    const uint32_t ah = render_.ao == 1 ? std::max(1u, h / 2) : std::max(1u, h);
    if (ao_half_ && ao_half_w_ == aw && ao_half_h_ == ah) return true;
    if (ao_half_) {
        // Called from the deferred flush, where Flush would re-enter it. Every earlier submit
        // is already on the queue, so waiting for the fences is enough to release the target.
        for (auto& f : frames_)
            if (f.fence && !WaitFence(f.fence)) return false;
        ao_half_.Reset();
        ao_half_w_ = ao_half_h_ = 0;
    }
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(kAoFormat, aw, ah);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    ao_half_state_ = D3D12_RESOURCE_STATE_RENDER_TARGET;
    if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td, ao_half_state_, nullptr,
                                             IID_PPV_ARGS(&ao_half_)))) {
        LOGE("d3d12: AO target could not be created");
        return false;
    }
    dev_->CreateRenderTargetView(ao_half_.Get(), nullptr, Cpu(rtv_heap_.Get(), kAoRtv, rtv_inc_));
    ao_half_w_ = aw;
    ao_half_h_ = ah;
    LOGR("d3d12: deferred AO target %ux%u", aw, ah);
    return true;
}

void D3D12Backend::RenderHalfAo(Target& t) {
    ao_half_ready_ = false;
    static std::atomic<int> logged{0};
    if (render_.ao == 0 || !dof_depth_) {
        if (logged.fetch_add(1, std::memory_order_relaxed) < 4)
            LOGR("d3d12: deferred AO skipped: ao %u depth %p", render_.ao, static_cast<void*>(dof_depth_));
        return;
    }
    if (!EnsureAoHalf(t.w, t.h)) {
        if (logged.fetch_add(1, std::memory_order_relaxed) < 4)
            LOGR("d3d12: deferred AO skipped: target creation failed");
        return;
    }
    Transition(ao_half_.Get(), ao_half_state_, D3D12_RESOURCE_STATE_RENDER_TARGET);
    const auto depth_desc = dof_depth_->GetDesc();
    const float src[4] = {1.0f / float(std::max<uint64_t>(depth_desc.Width, 1)),
                          1.0f / float(std::max<uint32_t>(depth_desc.Height, 1)), dof_near_, dof_far_};
    float aspect = app::Widescreen() ? app::AspectRatio() : 4.0f / 3.0f;
    float tan_half_fov = 0.577f;
    const float fov = game::g_camera_fov_degrees.load(std::memory_order_relaxed);
    if (fov > 1.0f && fov < 179.0f && aspect > 0.1f)
        tan_half_fov = std::tan(fov * 3.14159265f / 360.0f) / aspect;
    static const bool ao_ssao = [] {
        const char* e = std::getenv("TCNYC_AO_METHOD");
        return e && std::string(e) == "ssao";
    }();
    const std::string shader = std::string("#define DEPTH_MS ") +
                               (depth_desc.SampleDesc.Count > 1 ? "1\n" : "0\n") +
                               // GTAO by default; TCNYC_AO_METHOD=ssao keeps the earlier kernel to compare
                               (ao_ssao ? "#define AO_SSAO 1\n" : "") +
                               // GTAO cost: slices x steps per side, 3 x 6 at Half (a quarter of the
                               // pixels, so a third slice is cheap and 2 left visible grain), 3 x 8 at Full
                               (render_.ao == 1 ? "#define GTAO_SLICES 3\n#define GTAO_STEPS 6\n"
                                                : "#define GTAO_SLICES 3\n#define GTAO_STEPS 8\n") +
                               kAoHlsl;
    if (!EnsurePassPipeline(shader.c_str(), "psmain", kAoFormat, -1)) {
        static std::atomic<int> logged_pipe{0};
        if (logged_pipe.fetch_add(1, std::memory_order_relaxed) < 4)
            LOGR("d3d12: deferred AO skipped: pipeline failed");
        return;
    }
    ID3D12Resource* depth_src[3] = {dof_depth_, nullptr, nullptr};
    DXGI_FORMAT depth_fmt[3] = {kAoDistFormat, kBgra, kBgra};  // the prepass view distance
    const float p[8] = {src[0], src[1], src[2], src[3], kAoStrength, kAoRadius, tan_half_fov, aspect};
    Pass(shader.c_str(), "psmain", kAoFormat, Cpu(rtv_heap_.Get(), kAoRtv, rtv_inc_), ao_half_w_, ao_half_h_,
         depth_src, depth_fmt, p, -1);
    Transition(ao_half_.Get(), ao_half_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    ao_half_ready_ = true;
}

void D3D12Backend::DrawFrame(Target* t, uint32_t bi, ID3D12Resource* aa) {
    D3D12_RESOURCE_STATES bs = D3D12_RESOURCE_STATE_PRESENT;
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_RENDER_TARGET);
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), kRtvCount + bi, rtv_inc_);
    float black[4] = {0, 0, 0, 1};
    cl_->ClearRenderTargetView(rh, black, 0, nullptr);
    cl_->OMSetRenderTargets(1, &rh, FALSE, nullptr);
    bound_rt_ = bound_ds_ = nullptr;
    if (t) BlitFrame(*t, aa);
    RenderUi();
    Transition(bb_[bi].Get(), bs, D3D12_RESOURCE_STATE_PRESENT);
}

void D3D12Backend::ReleaseOldTextures() {
    constexpr uint64_t kSweepEvery = 60;    // presents between sweeps
    constexpr size_t kTexturesKept = 2048;  // no sweep below this many
    constexpr uint64_t kIdleFrames = 600;
    // textures unused for ten seconds go, levels stream theirs in and out
    if (presents_ % kSweepEvery != 0 || textures_.size() <= kTexturesKept) return;
    for (auto it = textures_.begin(); it != textures_.end();) {
        if (it->second.frame + kIdleFrames < presents_) {
            CurrentFrame().garbage.push_back(it->second.res);
            it = textures_.erase(it);
        } else {
            ++it;
        }
    }
}

void D3D12Backend::ResetFrame() {
    Frame& f = CurrentFrame();
    if (f.fence && fence_->GetCompletedValue() < f.fence) {
        PerfScope perf_wait{g_perf.wait};
        if (!WaitFence(f.fence)) {
            LOGE("d3d12: frame fence %llu did not complete; stopping",
                 static_cast<unsigned long long>(f.fence));
            kernel::Shutdown(1);
        }
    }
    RecycleFrameViews(f, uint32_t(frame_ % kFrames));
    f.alloc->Reset();
    f.garbage.clear();
    f.used = 0;
    f.srv = 0;
    f.smp = uint32_t(frame_ % kFrames) * kSamplersPerFrame;
    samplers_[frame_ % kFrames].clear();
    if (f.bundle_alloc && FAILED(f.bundle_alloc->Reset()))
        LOGE("d3d12: bundle allocator reset failed for frame slot %u", uint32_t(frame_ % kFrames));
    f.bundles.clear();
    f.bundle_next = 0;
    bundle_ = nullptr;
    bundle_desc_ = {};
    defer_reads_.clear();
    defer_writes_.clear();
    defer_frame_active_ = false;
    defer_abandoned_ = false;
    defer_reason_ = nullptr;
    defer_rt_ = nullptr;
    defer_ds_ = nullptr;
    defer_flush_conflict_ = false;
    defer_ao_applied_ = false;
    ao_half_ready_ = false;
    ao_depth_clear_ = true;
    depth_replay_ = {};
    defer_bundles_this_frame_ = 0;
    defer_flushes_this_frame_ = 0;
    cl_ = lists_[frame_ % kFrames].Get();
    cl_->Reset(f.alloc.Get(), nullptr);
    Begin();
}

void D3D12Backend::CountFps() {
    fps_n_++;
    auto now = std::chrono::steady_clock::now();
    double el = std::chrono::duration<double>(now - fps_t_).count();
    if (el >= 0.5) {
        fps_ = fps_n_ / el;
        fps_n_ = 0;
        fps_t_ = now;
    }
}

void LogPerf() {
    static PerfClock::time_point perf_start = PerfClock::now();
    static uint64_t perf_frames = 0;
    perf_frames++;
    double el = PerfMs(perf_start);
    if (el >= 1000.0) {
        Perf& p = g_perf;
        double n = double(perf_frames);
        double host = p.draw + p.clear + p.copy + p.present + p.sync;
        LOGI("perf: %.1f fps %.2f ms/frame | host %.2f (draw %.2f: assemble %.2f bind %.2f [check %.2f, upload %.2f] "
             "compile %.2f; clear %.2f copy %.2f sync %.2f present %.2f [swap %.2f wait %.2f]) other %.2f",
             n * 1000.0 / el, el / n, host / n, p.draw / n, p.assemble / n, p.bind / n, p.sig / n, p.texup / n,
             p.compile / n, p.clear / n, p.copy / n, p.sync / n, p.present / n, p.swap / n, p.wait / n,
             (el - host) / n);
        LOGI("perf: title thread per frame: recording %.2f ms, waiting %.2f ms (%.1f waits: read-back %.2f "
             "resource %.2f fence %.2f flush %.2f copy %.2f, next frame %.2f) | read-backs waited for: %.1f lock, "
             "%.1f NOOVERWRITE",
             g_perf_capture_ns.exchange(0) / n / 1e6, g_perf_wait_ns.exchange(0) / n / 1e6,
             g_perf_waits.exchange(0) / n, g_perf_wait_kind_ns[0].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[1].exchange(0) / n / 1e6, g_perf_wait_kind_ns[2].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[3].exchange(0) / n / 1e6, g_perf_wait_kind_ns[4].exchange(0) / n / 1e6,
             g_perf_wait_kind_ns[5].exchange(0) / n / 1e6, g_perf_sync_block.exchange(0) / n,
             g_perf_sync_late.exchange(0) / n);
        LOGI("perf: per frame %.0f draws (%.0f raw) %.0fk verts %.1f MB upload %.1f overflow | %.0f checks %.2f MB "
             "hashed | %.1f tex uploads %.2f MB, %.1f new tex, %.1f new targets, %.1f target uploads, %.1f shrinks, "
             "%.1f read-backs %.1f late | %llu shaders %llu psos %llu shader cache hits this second",
             p.draws / n, p.raw / n, p.verts / n / 1000.0, p.upload / n / 1048576.0, p.overflow / n, p.sigs / n,
             p.sig_bytes / n / 1048576.0, p.texups / n, p.texup_bytes / n / 1048576.0, p.newtex / n,
             p.targets / n, p.rtups / n, p.shrinks / n, p.syncs / n, p.lates / n,
             (unsigned long long)p.shaders, (unsigned long long)p.psos, (unsigned long long)p.shader_hits);
        LOGI("perf: pipeline creation %.3f ms/frame", p.pipeline / n);
        g_perf = Perf{};
        perf_start = PerfClock::now();
        perf_frames = 0;
    }
}

void D3D12Backend::Present(const Surface& front) {
    PerfScope perf_present{g_perf.present};
    if (defer_scene_) FlushDeferredScene();
    // Temporary diagnostic: TCNYC_CAPTURE_AT=<frame> starts an F3-style capture without input.
    static const long capture_at = [] {
        const char* e = std::getenv("TCNYC_CAPTURE_AT");
        return e ? std::atol(e) : 0;
    }();
    if (capture_at > 0 && frame_ == uint64_t(capture_at)) g_capture.store(true);
    if (cap_) {
        FinishCapture(front);
    } else if (g_capture.exchange(false) && CaptureOn()) {
        StartCapture();
    }
    if (!ResizeSwap()) {
        LOGE("d3d12: stopping because swap-chain back buffers are unavailable");
        kernel::Shutdown(1);
    }
    Target* t = EnsureTarget(front, false);
    if (t && LatchCpuWrite(*t)) UploadTarget(*t, front);
    if (t) Resolve(*t);
    if (ramp_dirty_) UploadGammaRamp();
    Transition(lut_.Get(), lut_state_, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);

    dof_depth_ = nullptr;
    const auto depth = scene_depth_.find(t);
    if (t && render_.dof && depth != scene_depth_.end()) {
        Target& ds = *depth->second.target;
        if (ds.res && ds.res->GetDesc().SampleDesc.Count > 1 && ds.w == t->w && ds.h == t->h) {
            dof_depth_ = ds.res.Get();
            Transition(dof_depth_, ds.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
            dof_near_ = depth->second.near_z;
            dof_far_ = depth->second.far_z;
        }
    }

    ID3D12Resource* aa = t && render_.smaa ? ApplySmaa(*t) : nullptr;
    uint32_t bi = swap_->GetCurrentBackBufferIndex();
    DrawFrame(t, bi, aa);
    scene_depth_.clear();
    dof_depth_ = nullptr;
    Submit(false);
    present_fence_[presents_ % (kFrames + 1)].store(fence_value_);
    DeliverReports();
    if (defer_scene_) ReportDeferred();
    bool vsync = render_.vsync;
    {
        PerfScope perf_swap{g_perf.swap};
        const HRESULT hr = swap_->Present(vsync ? 1 : 0, !vsync && tearing_ ? DXGI_PRESENT_ALLOW_TEARING : 0);
        if (hr == DXGI_ERROR_DEVICE_REMOVED || hr == DXGI_ERROR_DEVICE_RESET || hr == DXGI_ERROR_DEVICE_HUNG) {
            LOGE("d3d12: present lost the device (0x%08lX), device removed reason 0x%08lX", hr,
                 dev_->GetDeviceRemovedReason());
            kernel::Shutdown(1);
            return;
        }
        if (FAILED(hr) && hr != DXGI_STATUS_OCCLUDED) LOGE("d3d12: present failed (0x%08lX)", hr);
    }

    presents_++;
    presents_seen_.store(presents_);
    frame_++;
    ReleaseOldTextures();
    if (frame_ % 60 == 0) SweepTargets();
    ResetFrame();
    CountFps();
    if (diag::Perf()) LogPerf();
    ApplySettings();
    render_ = app::Snapshot();
}

// the slot is not reused before the caller's wait: the title runs at most one present ahead
void D3D12Backend::WaitGpuFrame(uint64_t n) {
    uint64_t fence = present_fence_[n % (kFrames + 1)].load();
    if (fence > fence_->GetCompletedValue() && !WaitFence(fence)) {
        LOGE("d3d12: present fence %llu did not complete; stopping", static_cast<unsigned long long>(fence));
        kernel::Shutdown(1);
    }
}

uint32_t D3D12Backend::SupportedMsaa(uint32_t samples) {
    if (!samples) return 0;
    for (DXGI_FORMAT format : {kBgra, kDepthFormat}) {
        D3D12_FEATURE_DATA_MULTISAMPLE_QUALITY_LEVELS levels{format, samples};
        if (FAILED(dev_->CheckFeatureSupport(D3D12_FEATURE_MULTISAMPLE_QUALITY_LEVELS, &levels,
                                             sizeof(levels))) || !levels.NumQualityLevels) {
            LOGR("d3d12: %ux MSAA unavailable for format %u, using one sample", samples, unsigned(format));
            return 0;
        }
    }
    return samples;
}

void D3D12Backend::ApplySettings() {
    if (texturedumper::Update()) ReleasePackTextures();
    // Ambient occlusion requires the deferred scene list. Toggle it live so the F10 row
    // works without a restart.
    const bool want_defer = defer_forced_ || app::Snapshot().ao != 0;
    if (want_defer != defer_scene_) {
        if (defer_scene_) FlushDeferredScene();
        defer_scene_ = want_defer;
        defer_abandoned_ = false;
        defer_ao_applied_ = false;
        defer_reported_ = false;
        LOGR("d3d12: deferred scene display list %s (%s)", want_defer ? "on" : "off",
             defer_forced_ ? "TCNYC_DEFER_SCENE" : "ambient occlusion setting");
    }
    // Shader microcode variants should not accumulate for the whole session.
    constexpr size_t kMaxDrawPipelines = 4096;
    if (psos_.size() > kMaxDrawPipelines) {
        Flush();
        psos_.clear();
        LOGW("d3d12: pipeline cache reached %zu entries, cleared it", kMaxDrawPipelines);
    }
    float want = app::RenderScale(guest_h_);
    float want_stretch = app::RenderStretch(guest_w_, guest_h_);
    uint32_t want_msaa = msaa_;
    const uint32_t requested = app::MsaaSamples();
    if (requested != requested_msaa_) {
        requested_msaa_ = requested;
        want_msaa = SupportedMsaa(requested);
    }
    bool remake = ext_ && ext_->Settings();
    if (std::fabs(want - scale_) > 1e-4f || std::fabs(want_stretch - stretch_) > 1e-4f || want_msaa != msaa_ || remake) {
        Flush();
        scene_depth_.clear();
        if (remake) {
            for (auto& [key, tg] : targets_)
                (tg.depth ? CurrentFrame().dsv_garbage : CurrentFrame().rtv_garbage).push_back(tg.view);
            targets_.clear();
        }
        if (want_msaa != msaa_) LOGR("d3d12: live MSAA set to %u samples", want_msaa ? want_msaa : 1);
        msaa_ = want_msaa;
        scale_ = want;
        stretch_ = want_stretch;
        if (ext_) ext_->Reset(*this);
        // Existing targets stay until they are next bound as render targets: a target the title
        // only samples (the HUD's small icon buffers) must keep its contents across the change.
        bound_rt_ = bound_ds_ = nullptr;
    }
}

// The caller chooses the read state after the hardware resolve.
ID3D12Resource* D3D12Backend::ResolveColor(Target& t) {
    if (defer_writes_.count(t.res.Get()) || (t.resolved && defer_writes_.count(t.resolved.Get())))
        DeferredBarrier("resolve of a deferred target");
    if (t.res->GetDesc().SampleDesc.Count == 1) return t.res.Get();
    if (t.depth) return nullptr;
    if (!t.resolved) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        const auto desc = Texture2DDesc(t.fmt, t.w, t.h);
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, t.resolved_state,
                                                 nullptr, IID_PPV_ARGS(&t.resolved))))
            return nullptr;
    }
    // any write to the samples moves them out of RESOLVE_SOURCE: still there, the last resolve is current (a target
    // read by several draws in a frame resolves once)
    if (t.state == D3D12_RESOURCE_STATE_RESOLVE_SOURCE) return t.resolved.Get();
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_RESOLVE_SOURCE);
    Transition(t.resolved.Get(), t.resolved_state, D3D12_RESOURCE_STATE_RESOLVE_DEST);
    cl_->ResolveSubresource(t.resolved.Get(), 0, t.res.Get(), 0, t.fmt);
    return t.resolved.Get();
}

// Use the nearest covered sample in reversed depth for guest shadow comparisons.
ID3D12Resource* D3D12Backend::ResolveDepth(Target& t) {
    if (defer_writes_.count(t.res.Get()) || (t.resolved && defer_writes_.count(t.resolved.Get())))
        DeferredBarrier("resolve of a deferred depth target");
    static constexpr char hlsl[] = R"(
Texture2DMS<float> depth : register(t0);
float4 vs(uint id : SV_VertexID) : SV_Position {
    return float4(float2((id << 1) & 2, id & 2) * float2(2, -2) + float2(-1, 1), 0, 1);
}
float ps(float4 pos : SV_Position) : SV_Target {
    int2 p = int2(pos.xy);
    uint width, height, samples;
    depth.GetDimensions(width, height, samples);
    float nearest = depth.Load(p, 0);
    for (uint i = 1; i < samples; ++i) nearest = max(nearest, depth.Load(p, i));
    return nearest;
}
)";
    constexpr auto format = DXGI_FORMAT_R32_FLOAT;
    if (t.depth_resolved && t.resolved) return t.resolved.Get();
    const uint32_t scratch = ScratchRtv();
    if (scratch == UINT32_MAX || !EnsurePassPipeline(hlsl, "ps", format, -1)) return nullptr;
    if (!t.resolved) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
        auto desc = Texture2DDesc(format, t.w, t.h);
        desc.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &desc, t.resolved_state,
                                                 nullptr, IID_PPV_ARGS(&t.resolved))))
            return nullptr;
    }
    const auto view = Cpu(rtv_heap_.Get(), scratch, rtv_inc_);
    dev_->CreateRenderTargetView(t.resolved.Get(), nullptr, view);
    Transition(t.res.Get(), t.state, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    Transition(t.resolved.Get(), t.resolved_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    ID3D12Resource* src[3] = {t.res.Get(), nullptr, nullptr};
    const DXGI_FORMAT fmt[3] = {kDepthRead, kBgra, kBgra};
    const float constants[8] = {};
    Pass(hlsl, "ps", format, view, t.w, t.h, src, fmt, constants, -1);
    t.depth_resolved = true;
    return t.resolved.Get();
}

// the target resolved, then shrunk with o when it is not guest sized BGRA; res null on failure
D3D12Backend::CopySource D3D12Backend::PrepareCopySource(Target& t, const ShrinkOptions& o) {
    if (DeferredPendingResource(t.res.Get())) DeferredBarrier("copy of a deferred target");
    Resolve(t);
    CopySource out{ResolveColor(t), nullptr};
    if (!out.res) return out;
    auto& state = out.res == t.res.Get() ? t.state : t.resolved_state;
    if (t.w != t.gw || t.h != t.gh || t.fmt != kBgra) {
        out.down = Shrink(out.res, state, t.fmt, t.gw, t.gh, o);
        out.res = out.down.Get();
    } else {
        Transition(out.res, state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }
    return out;
}

// long unused: the title freed it and other data is at this address now
bool D3D12Backend::TargetStale(uint32_t data) const {
    auto it = target_used_.find(data);
    uint64_t used = it == target_used_.end() ? 0 : it->second;
    return frame_ - used > kStaleFrames;
}

// long sessions create render targets at new guest addresses; keep only the live set
void D3D12Backend::SweepTargets() {
    for (auto it = targets_.begin(); it != targets_.end();) {
        const Target& t = it->second;
        auto used_it = target_used_.find(t.data);
        const uint64_t used = used_it == target_used_.end() ? 0 : used_it->second;
        const uint64_t keep = uint64_t(t.gw) * t.gh <= kSmallTargetPixels ? kSmallTargetFrames : kStaleFrames;
        if (frame_ - used > keep) {
            ReleaseTarget(it++);
        } else {
            ++it;
        }
    }
    // Late-lock readback buffers are keyed by guest address and never referenced
    // by the target map, so they need their own idle sweep.
    constexpr uint64_t kLateIdleFrames = 600;
    std::lock_guard lock(late_lock_);
    for (auto it = late_.begin(); it != late_.end();) {
        uint64_t newest = 0;
        for (uint64_t f : it->second.frame) newest = std::max(newest, f);
        if (frame_ <= newest || frame_ - newest <= kLateIdleFrames) {
            ++it;
            continue;
        }
        for (auto& r : it->second.rb)
            if (r) CurrentFrame().garbage.push_back(r);
        if (it->second.down) CurrentFrame().garbage.push_back(it->second.down);
        it = late_.erase(it);
    }
    // The use map is keyed by guest address and otherwise keeps every address
    // the title has ever rendered to. It has to outlive the small-target window
    // above or those targets would look unused at the next sweep.
    for (auto it = target_used_.begin(); it != target_used_.end();) {
        if (frame_ > it->second && frame_ - it->second > kSmallTargetFrames)
            it = target_used_.erase(it);
        else
            ++it;
    }
}

// the CPU reads a surface the GPU drew: bring the pixels back
void D3D12Backend::Sync(uint32_t data, uint32_t) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || !it->second.drawn || TargetStale(data)) return;
    PerfScope perf_sync{g_perf.sync};
    g_perf.syncs++;
    if (diag::Perf()) {
        LOG_FIRST(16, "perf: read-back of %08X (%ux%u guest, %ux%u host)", data, it->second.gw, it->second.gh,
                  it->second.w, it->second.h);
    }
    Target& t = it->second;
    if (DeferredPendingResource(t.res.Get())) DeferredBarrier("read-back of a deferred target");
    // src.down, when there is one, stays alive until ReadBack is done
    CopySource src = PrepareCopySource(t, {});
    if (!src.res) return;
    std::vector<uint8_t> px;
    if (!ReadBack(src.res, t.gw, t.gh, px)) return;
    // color targets are 32 bit linear in guest memory, the pitch rounds to 64 bytes
    uint32_t pitch = GuestPitch(t.gw);
    uint8_t* guest = static_cast<uint8_t*>(kernel::HostPtr(data));
    for (uint32_t y = 0; y < t.gh; y++) std::memcpy(guest + size_t(y) * pitch, &px[size_t(y) * t.gw * 4], t.gw * 4);
    t.read = std::move(px);
    t.drawn = false;
    t.sig = MemSig(t.data, t.bytes);
}

// A NOOVERWRITE lock: the console reads whatever the GPU has finished, so a surface locked every frame gets the newest
// finished copy (a frame or two old) and the CPU never waits on the GPU; one locked now and then still waits in Sync
void D3D12Backend::SyncLate(uint32_t data) {
    if (!DeliverLate(data)) Sync(data, 0);
    RecordLate(data);
}

bool D3D12Backend::DeliverLate(uint32_t data) {
    std::lock_guard lock(late_lock_);
    auto it = late_.find(data);
    if (it == late_.end()) return false;
    Late& l = it->second;
    uint64_t done = fence_->GetCompletedValue();
    uint64_t now = presents_seen_.load();
    int best = -1;
    for (int i = 0; i < kFrames; i++) {
        bool finished = l.fence[i] && l.fence[i] <= done;
        bool recent = now - l.frame[i] <= uint64_t(kFrames) + 1;
        bool newest = best < 0 || l.fence[i] > l.fence[best];
        if (finished && recent && newest) best = i;
    }
    if (best < 0) {
        if (diag::Perf()) {
            LOG_FIRST(16, "perf: late %08X nothing finished: done %llu now %llu, slots %llu/%llu %llu/%llu %llu/%llu",
                      data, (unsigned long long)done, (unsigned long long)now, (unsigned long long)l.fence[0],
                      (unsigned long long)l.frame[0], (unsigned long long)l.fence[1], (unsigned long long)l.frame[1],
                      (unsigned long long)l.fence[2], (unsigned long long)l.frame[2]);
        }
        return false;
    }
    if (l.fence[best] > l.delivered) {
        uint8_t* p = nullptr;
        // the last row is not padded to the pitch: a range to the end of the pitch fails to map
        D3D12_RANGE rr{0, size_t(l.row_pitch) * (l.gh - 1) + l.gw * 4};
        if (FAILED(l.rb[best]->Map(0, &rr, reinterpret_cast<void**>(&p)))) return false;
        uint32_t pitch = GuestPitch(l.gw);
        uint8_t* dst = static_cast<uint8_t*>(kernel::HostPtr(data));
        for (uint32_t y = 0; y < l.gh; y++) std::memcpy(dst + size_t(y) * pitch, p + size_t(y) * l.row_pitch, l.gw * 4);
        D3D12_RANGE none{0, 0};
        l.rb[best]->Unmap(0, &none);
        l.delivered = l.fence[best];
    }
    return true;
}

// a slot set for another size: its buffers go with the frame
void D3D12Backend::ResetLate(Late& l, const Target& t) {
    auto& garbage = CurrentFrame().garbage;
    for (auto& r : l.rb) {
        if (r) garbage.push_back(r);
    }
    if (l.down) garbage.push_back(l.down);
    l = Late{};
    l.gw = t.gw;
    l.gh = t.gh;
}

// the target as a guest sized BGRA copy source, shrunk into l.down when it is not one already
ID3D12Resource* D3D12Backend::PrepareLateSource(Target& t, Late& l) {
    ShrinkOptions reuse{.into = l.down.Get(), .into_state = l.down_state};
    CopySource src = PrepareCopySource(t, reuse);
    if (src.down) {
        l.down = src.down;
        l.down_state = D3D12_RESOURCE_STATE_COPY_SOURCE;
    }
    return src.res;
}

// src into the readback buffer of the slot, made on first use
void D3D12Backend::RecordLateCopy(Late& l, uint32_t slot, ID3D12Resource* src) {
    D3D12_RESOURCE_DESC rd = src->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    if (!l.rb[slot]) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&l.rb[slot])))) {
            return;
        }
    }
    D3D12_TEXTURE_COPY_LOCATION dl{l.rb[slot].Get(), D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    l.row_pitch = fp.Footprint.RowPitch;
    l.fence[slot] = fence_value_ + 1;  // the next Submit carries it
    l.frame[slot] = presents_;
    l.next = (slot + 1) % kFrames;
}

void D3D12Backend::RecordLate(uint32_t data) {
    auto it = targets_.find(TargetMapKey(data, false));
    if (it == targets_.end() || TargetStale(data)) return;
    Target& t = it->second;
    if (DeferredPendingResource(t.res.Get())) DeferredBarrier("late read-back of a deferred target");
    PerfScope perf_sync{g_perf.sync};
    g_perf.lates++;
    // what DeliverLate or Sync left in guest memory is no CPU write to upload back
    t.sig = MemSig(t.data, t.bytes);
    t.read.clear();
    std::lock_guard lock(late_lock_);
    Late& l = late_[data];
    if (l.gw != t.gw || l.gh != t.gh) {
        if (diag::Perf()) LOG_FIRST(16, "perf: late %08X ring reset, %ux%u -> %ux%u", data, l.gw, l.gh, t.gw, t.gh);
        ResetLate(l, t);
    }
    // a copy for a later lock, unless the GPU is a whole ring behind
    uint32_t slot = l.next;
    if (l.fence[slot] > fence_->GetCompletedValue()) return;
    ID3D12Resource* src = PrepareLateSource(t, l);
    if (!src) return;
    RecordLateCopy(l, slot, src);
}

// filters src into a new w x h target, left as a copy source
ComPtr<ID3D12Resource> D3D12Backend::Shrink(ID3D12Resource* src, D3D12_RESOURCE_STATES& st, DXGI_FORMAT fmt,
                                            uint32_t w, uint32_t h, const ShrinkOptions& o) {
    D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_DEFAULT};
    D3D12_RESOURCE_DESC td = Texture2DDesc(DXGI_FORMAT_B8G8R8A8_UNORM, w, h);
    td.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
    D3D12_CLEAR_VALUE cv{};
    cv.Format = td.Format;
    ComPtr<ID3D12Resource> down = o.into;
    D3D12_RESOURCE_STATES down_state = o.into ? o.into_state : D3D12_RESOURCE_STATE_RENDER_TARGET;
    g_perf.shrinks++;
    const uint32_t scratch = ScratchRtv();
    if (scratch == UINT32_MAX) return nullptr;
    if (down) {
        Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_RENDER_TARGET);
    } else if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &td,
                                                    D3D12_RESOURCE_STATE_RENDER_TARGET, &cv, IID_PPV_ARGS(&down)))) {
        return nullptr;
    }
    D3D12_CPU_DESCRIPTOR_HANDLE rh = Cpu(rtv_heap_.Get(), scratch, rtv_inc_);
    dev_->CreateRenderTargetView(down.Get(), nullptr, rh);
    Transition(src, st, D3D12_RESOURCE_STATE_PIXEL_SHADER_RESOURCE);
    SetBlitTarget(rh, w, h);
    float uv[4] = {0, 0, 1, 1};
    Blit(src, fmt, uv, {.alpha = true, .taps = o.taps, .mul = o.mul});
    Transition(down.Get(), down_state, D3D12_RESOURCE_STATE_COPY_SOURCE);
    return down;
}

// a BGRA copy source to the CPU, rows packed; waits for the GPU
bool D3D12Backend::ReadBack(ID3D12Resource* res, uint32_t w, uint32_t h, std::vector<uint8_t>& out) {
    D3D12_RESOURCE_DESC rd = res->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp;
    UINT rows;
    UINT64 row_size;
    UINT64 bytes;
    dev_->GetCopyableFootprints(&rd, 0, 1, 0, &fp, &rows, &row_size, &bytes);
    Frame& f = CurrentFrame();
    if (!f.readback || f.readback_bytes < bytes) {
        D3D12_HEAP_PROPERTIES hp{D3D12_HEAP_TYPE_READBACK};
        D3D12_RESOURCE_DESC bd = BufferDesc(bytes);
        ComPtr<ID3D12Resource> rb;
        if (FAILED(dev_->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST,
                                                 nullptr, IID_PPV_ARGS(&rb)))) {
            return false;
        }
        if (f.readback) f.garbage.push_back(std::move(f.readback));
        f.readback = std::move(rb);
        f.readback_bytes = bytes;
    }
    ID3D12Resource* rb = f.readback.Get();
    D3D12_TEXTURE_COPY_LOCATION dl{rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT};
    dl.PlacedFootprint = fp;
    D3D12_TEXTURE_COPY_LOCATION sl{res, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX};
    cl_->CopyTextureRegion(&dl, 0, 0, 0, &sl, nullptr);
    Flush();
    uint8_t* p = nullptr;
    if (FAILED(rb->Map(0, nullptr, reinterpret_cast<void**>(&p))) || !p) return false;
    out.resize(size_t(w) * h * 4);
    for (uint32_t y = 0; y < h; y++) std::memcpy(&out[size_t(y) * w * 4], p + size_t(y) * fp.Footprint.RowPitch, w * 4);
    rb->Unmap(0, nullptr);
    return true;
}

// BMP file and info header: byte offsets of the fields CapSave writes
constexpr int kBmpHeaderBytes = 54;
constexpr int kBmpFileSize = 2;
constexpr int kBmpPixelOffset = 10;
constexpr int kBmpInfoSize = 14;
constexpr int kBmpWidth = 18;
constexpr int kBmpHeight = 22;  // negative: rows top down
constexpr int kBmpPlanes = 26;
constexpr int kBmpBitCount = 28;
constexpr int kBmpImageSize = 34;
constexpr uint32_t kBmpInfoBytes = 40;

void D3D12Backend::CapSave(const CapImage& img, const char* name) {
    ComPtr<ID3D12Resource> down = Shrink(img.res, *img.state, img.fmt, img.w, img.h, {.mul = img.mul});
    std::vector<uint8_t> px;
    if (!down || !ReadBack(down.Get(), img.w, img.h, px)) return;
    FILE* f = _wfopen((cap_dir_ / (std::string(name) + ".bmp")).c_str(), L"wb");
    if (!f) return;
    uint8_t hd[kBmpHeaderBytes] = {'B', 'M'};  // BITMAPFILEHEADER and BITMAPINFOHEADER, top down 32 bit
    auto put = [&](int o, uint32_t v) { std::memcpy(hd + o, &v, 4); };
    put(kBmpFileSize, kBmpHeaderBytes + uint32_t(px.size()));
    put(kBmpPixelOffset, kBmpHeaderBytes);
    put(kBmpInfoSize, kBmpInfoBytes);
    put(kBmpWidth, img.w);
    put(kBmpHeight, uint32_t(-int32_t(img.h)));
    hd[kBmpPlanes] = 1;
    hd[kBmpBitCount] = 32;
    put(kBmpImageSize, uint32_t(px.size()));
    fwrite(hd, 1, kBmpHeaderBytes, f);
    fwrite(px.data(), 1, px.size(), f);
    fclose(f);
}

void D3D12Backend::CapSave(Target& t, const char* name) {
    const bool resolve_depth = t.depth && t.res->GetDesc().SampleDesc.Count > 1;
    ID3D12Resource* color = resolve_depth ? ResolveDepth(t) : ResolveColor(t);
    if (!color) return;
    auto& state = color == t.res.Get() ? t.state : t.resolved_state;
    DXGI_FORMAT fmt = resolve_depth ? DXGI_FORMAT_R32_FLOAT : t.depth ? kDepthRead : t.fmt;
    float mul = t.depth ? 1 : t.range;
    CapSave({.res = color, .state = &state, .fmt = fmt, .w = t.gw, .h = t.gh, .mul = mul}, name);
}

// the frame moves to another target: save the one it leaves
void D3D12Backend::CapStep(const Surface& rt) {
    uint32_t d = rt.header ? mem::VirtualFromPhysical(rt.data) : 0;
    // XBE_CAPTURE_EVERY=<n>: also save the current target every n draws, to find the draw that changes it
    static const uint32_t every = [] { const char* v = std::getenv("XBE_CAPTURE_EVERY"); return v ? uint32_t(std::atoi(v)) : 0u; }();
    if (every && d == cap_rt_ && cap_i_ && cap_i_ % every == 0) {
        auto cur = targets_.find(TargetMapKey(cap_rt_, false));
        if (cur != targets_.end()) {
            char name[32];
            snprintf(name, sizeof(name), "%04u_%08X_s", cap_i_, cap_rt_);
            CapSave(cur->second, name);
        }
    }
    if (d == cap_rt_) return;
    auto it = targets_.find(TargetMapKey(cap_rt_, false));
    if (cap_rt_ && it != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X", cap_i_, cap_rt_);
        CapSave(it->second, name);
    }
    auto z = targets_.find(TargetMapKey(cap_rt_, true));
    if (cap_rt_ && z != targets_.end()) {
        char name[32];
        snprintf(name, sizeof(name), "%04u_%08X_z", cap_i_, cap_rt_);
        CapSave(z->second, name);
    }
    cap_rt_ = d;
}

void D3D12Backend::Capture(Target& t, const char* tag, float mul) {
    if (!cap_) return;
    ID3D12Resource* color = ResolveColor(t);
    if (!color) return;
    auto& state = color == t.res.Get() ? t.state : t.resolved_state;
    char name[32];
    snprintf(name, sizeof(name), "%04u_%s_%08X", cap_i_, tag, t.data);
    CapSave({.res = color, .state = &state, .fmt = t.fmt, .w = t.gw, .h = t.gh, .mul = mul}, name);
}

void D3D12Backend::SetGamma(const uint8_t* ramp) {
    std::memcpy(ramp_, ramp, sizeof(ramp_));
    ramp_dirty_ = true;
}

}  // namespace

std::unique_ptr<Backend> CreateBackend() { return std::make_unique<D3D12Backend>(); }

}  // namespace gpu
