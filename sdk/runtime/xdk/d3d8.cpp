// Direct3D 8 (XDK 4831 and 5849) entry points the title calls, implemented on the host renderer.
// Resource creation, locking and the XG helpers stay recompiled: they only manage memory.
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <map>
#include <thread>
#include <unordered_map>
#include <unordered_set>
#include <vector>

#include "app/app.h"
#include "core/diag.h"
#include "gpu/backend.h"
#include "gpu/shaders.h"
#include "gpu/simple_dirty.h"
#include "gpu/texture.h"
#include "gpu/vertex.h"
#include "xdk/d3d8.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;

namespace dispatch {
bool InCode(uint32_t va);  // cpu/dispatch.cpp
}

// configs written before the SDK knew the name
#ifndef SYM_D3DDevice_BeginPush
#define SYM_D3DDevice_BeginPush FFFFFE00
#define HAS_D3DDevice_BeginPush 0
#endif
#ifndef SYM_D3DDevice_EndPush
#define SYM_D3DDevice_EndPush FFFFFE01
#define HAS_D3DDevice_EndPush 0
#endif
#ifndef HAS_D3D_FvfShader
#define HAS_D3D_FvfShader 0
#endif
#ifndef SYM_D3DDevice_SetPalette
#define SYM_D3DDevice_SetPalette FFFFFE02
#define HAS_D3DDevice_SetPalette 0
#endif
#ifndef SYM_D3DDevice_GetVertexShaderFunction
#define SYM_D3DDevice_GetVertexShaderFunction FFFFFE03
#define HAS_D3DDevice_GetVertexShaderFunction 0
#endif
#ifndef SYM_D3DDevice_GetTile
#define SYM_D3DDevice_GetTile FFFFFE04
#define HAS_D3DDevice_GetTile 0
#endif
#ifndef SYM_D3DDevice_SetRenderTargetFast
#define SYM_D3DDevice_SetRenderTargetFast FFFFFE05
#define HAS_D3DDevice_SetRenderTargetFast 0
#endif
#ifndef SYM_D3DDevice_LoadVertexShaderProgram
#define SYM_D3DDevice_LoadVertexShaderProgram FFFFFE06
#define HAS_D3DDevice_LoadVertexShaderProgram 0
#endif
#ifndef SYM_D3DDevice_GetRasterStatus
#define SYM_D3DDevice_GetRasterStatus FFFFFE07
#define HAS_D3DDevice_GetRasterStatus 0
#endif
#ifndef SYM_D3DDevice_Reset
#define SYM_D3DDevice_Reset FFFFFE08
#define HAS_D3DDevice_Reset 0
#endif
#ifndef SYM_D3DDevice_RunPushBuffer
#define SYM_D3DDevice_RunPushBuffer FFFFFE09
#define HAS_D3DDevice_RunPushBuffer 0
#endif
#ifndef SYM_D3D_BlockOnNonSurfaceResource
#define SYM_D3D_BlockOnNonSurfaceResource FFFFFE0A
#define HAS_D3D_BlockOnNonSurfaceResource 0
#endif

namespace d3d {

namespace {

// A/B switch: 0 draws on the title's thread, as before
#ifndef XBE_RENDER_THREAD
#define XBE_RENDER_THREAD 1
#endif
constexpr bool kRenderThread = XBE_RENDER_THREAD;

// D3D globals of this build
constexpr uint32_t kDevice = SYM(D3D_Device);        // the static CDevice, g_pDevice points at it
constexpr uint32_t kDevicePtr = SYM(D3D_pDevice);
constexpr uint32_t kIndexData = SYM(D3D_IndexData);  // index data base the inline draws read
constexpr uint32_t kDirtyFlags = SYM(D3D_DirtyFlags);
constexpr uint32_t kTextureState = SYM(D3D_DeferredTextureState);  // [4][32]
constexpr uint32_t kRenderState = SYM(D3D_RenderState);            // [166]
constexpr uint32_t kStreams = SYM(D3D_Streams);                    // [16] stride, offset, buffer
constexpr uint32_t kSimpleEncode = SYM(D3D_SimpleRenderStateEncode);

// CDevice fields mirrored so the recompiled getters and resource code see what the XDK would
constexpr uint32_t kDevPush = XDK_CDevice_Push, kDevThreshold = XDK_CDevice_PushThreshold,
                   kDevBaseVertex = XDK_CDevice_BaseVertex, kDevIndexBuffer = XDK_CDevice_IndexBuffer,
                   kDevTextures = XDK_CDevice_Textures, kDevRenderTarget = XDK_CDevice_RenderTarget,
                   kDevDepth = XDK_CDevice_Depth, kDevSwapCount = XDK_CDevice_SwapCount,
                   kDevBackBuffer = XDK_CDevice_BackBuffer, kDevFrontBuffer = XDK_CDevice_FrontBuffer,
                   kDevZScale = XDK_CDevice_ZScale, kDevSuperX = XDK_CDevice_SuperSampleX,
                   kDevSuperY = XDK_CDevice_SuperSampleY, kDevTransforms = XDK_CDevice_Transforms,
                   kDevPalettes = XDK_CDevice_Palettes;
constexpr uint32_t kStageStates = XDK_TextureState_Stage, kStreamSize = XDK_Stream_Size;

gpu::State g_state;
std::unique_ptr<gpu::Backend> g_backend;
std::recursive_mutex g_lock;

// every write to g_state.simple goes through here or marks it
void SetSimple(uint32_t method, uint32_t v) {
    g_state.simple[method >> 2] = v;
    gpu::MarkSimple(method >> 2);
}

std::unordered_map<uint32_t, std::shared_ptr<gpu::VertexShader>> g_vs;
std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> g_ps;
std::array<uint32_t, 3> g_buffers{};  // front, back surfaces
uint32_t g_buffer_count = 2;
uint32_t g_depth = 0;
uint32_t g_scratch_push = 0, g_scratch_size = 0;
uint32_t g_walk = 0;  // first scratch command not read yet

// guest shader objects the title owns; the cap keeps a corrupt stream of creates from exhausting host memory
constexpr size_t kMaxGuestShaders = 4096;
constexpr uint32_t kInvalidCall = 0x8876086C;  // D3DERR_INVALIDCALL

// whether a guest range may be read before a length from an untrusted header is used
bool GuestReadable(uint32_t va, uint64_t bytes) {
    if (!va || !bytes) return false;
    if (uint32_t alloc = mem::AllocationSize(va)) return bytes <= alloc;
    const uint64_t end = uint64_t(va) + bytes;
    if (end > 0x100000000ull) return false;
    for (uint64_t p = uint64_t(va) & ~uint64_t(mem::kPage - 1); p < end; p += mem::kPage) {
        if (!mem::IsCommitted(uint32_t(p))) return false;
    }
    return true;
}

uint32_t Dev(uint32_t off) { return R32(kDevice + off); }
void SetDev(uint32_t off, uint32_t v) { W32(kDevice + off, v); }

// The device's own references: a count in bits 19 to 22 of the resource's first dword. A surface's first one
// also holds its parent. The library's functions do it when the image has them, inlined ones are done here.
void AddRef(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalAddRef
    GUEST_CALL(D3D_InternalAddRef, ctx, {res});
#else
    (void)ctx;
    uint32_t common = R32(res);
    if (!(common & 0x780000) && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) W32(parent, R32(parent) + 0x80000);
    W32(res, R32(res) + 0x80000);
#endif
}
void Release(X86Context& ctx, uint32_t res) {
    if (!res) return;
#if HAS_D3D_InternalRelease
    GUEST_CALL(D3D_InternalRelease, ctx, {res});
#else
    (void)ctx;
    // the library frees a resource nothing holds any more; without its function the memory stays
    auto drop = [](uint32_t r) {
        W32(r, R32(r) - 0x80000);
        if (!(R32(r) & 0x78FFFF)) LOG_FIRST(8, "d3d: resource %08X is not freed, the image has no D3D_InternalRelease", r);
    };
    uint32_t common = R32(res);
    if ((common & 0x780000) == 0x80000 && (common & 0x70000) == 0x50000)
        if (uint32_t parent = R32(res + 0x14)) drop(parent);
    drop(res);
#endif
}

void Walk(uint32_t end);

// Hardware commands a caller still pushes go to a scratch buffer, rewound at every request once Walk has read it
uint32_t ScratchPush(uint32_t bytes = 0) {
    Walk(Dev(kDevPush));
    const uint64_t rounded = (uint64_t(bytes) + 0x1FFF) & ~uint64_t(0xFFF);
    const uint64_t need64 = std::max<uint64_t>(0x10000, rounded);
    if (need64 > UINT32_MAX) {
        LOGE("d3d: push buffer of %u bytes is too large", bytes);
        if (g_scratch_push) mem::FreeContiguous(g_scratch_push);
        g_scratch_push = g_scratch_size = g_walk = 0;
        SetDev(kDevPush, 0);
        SetDev(kDevThreshold, 0);
        return 0;
    }
    const uint32_t need = uint32_t(need64);
    if (need > g_scratch_size) {
        if (g_scratch_push) mem::FreeContiguous(g_scratch_push);
        g_scratch_push = mem::AllocContiguous(need, 0, 0xFFFFFFFF, 0x1000);
        if (!g_scratch_push) {
            LOGE("d3d: push buffer of %u bytes could not be allocated", need);
            g_scratch_size = 0;
            SetDev(kDevPush, 0);
            SetDev(kDevThreshold, 0);
            g_walk = 0;
            return 0;
        }
        g_scratch_size = need;
    }
    SetDev(kDevPush, g_scratch_push);
    SetDev(kDevThreshold, g_scratch_push + g_scratch_size - 0x1000);
    g_walk = g_scratch_push;
    return g_scratch_push;
}

gpu::Surface SurfaceOf(uint32_t header) {
    gpu::Surface s;
    s.header = header;
    if (header) {
        s.data = R32(header + 4);
        s.format = R32(header + 12);
        s.size = R32(header + 16);
    }
    return s;
}

// a surface this file made: its contiguous data and its system header
void FreeSurface(uint32_t hdr) {
    if (!hdr) return;
    const uint32_t data = mem::VirtualFromPhysical(R32(hdr + 4));
    if (data) mem::FreeContiguous(data);
    mem::FreeSystem(hdr);
}

uint32_t NewSurface(uint32_t w, uint32_t h, uint32_t fmt, uint32_t bpp) {
    constexpr uint32_t kMaxSurfaceSide = 4096;  // the header stores w - 1 and h - 1 in 12 bits
    if (!w || !h || w > kMaxSurfaceSide || h > kMaxSurfaceSide || (bpp != 1 && bpp != 2 && bpp != 4)) {
        LOG_FIRST(8, "d3d: surface %ux%u bpp %u rejected", w, h, bpp);
        return 0;
    }
    const uint64_t pitch = (uint64_t(w) * bpp + 63) & ~uint64_t(63);
    const uint64_t bytes = pitch * h;
    if (pitch / 64 > 256 || bytes > UINT32_MAX) {  // pitch/64 - 1 is stored in 8 bits
        LOG_FIRST(8, "d3d: surface %ux%u bpp %u is too large", w, h, bpp);
        return 0;
    }
    const uint32_t data = mem::AllocContiguous(uint32_t(bytes), 0, 0xFFFFFFFF, 0x1000);
    if (!data) return 0;
    const uint32_t hdr = mem::AllocSystem(24);
    if (!hdr) {
        mem::FreeContiguous(data);
        return 0;
    }
    W32(hdr + 0, 0x000D0001);  // surface, one reference, one internal reference held by the device
    W32(hdr + 4, mem::PhysicalFromVirtual(data));
    W32(hdr + 8, 0);
    W32(hdr + 12, 0x00010021 | (fmt << 8));  // dma A, 2D, one level
    W32(hdr + 16, (w - 1) | (h - 1) << 12 | (uint32_t(pitch / 64) - 1) << 24);
    W32(hdr + 20, 0);
    return hdr;
}

void SyncDeferred() {
    // deferred states live in the title's memory, the inline SetRenderState and SetTextureStageState write them
    for (uint32_t i = gpu::kRsDeferredFirst; i < gpu::kRsComplexFirst; i++) g_state.rs[i] = R32(kRenderState + i * 4);
    for (int s = 0; s < 4; s++)
        for (uint32_t i = 0; i < gpu::kTssBumpEnvMat00; i++) g_state.tss[s][i] = R32(kTextureState + (s * kStageStates + i) * 4);
}

float ZScale(const gpu::Surface& zs) {
    uint32_t f = zs.header ? (zs.format >> 8) & 0xFF : 0x2E;
    return f == 0x2C || f == 0x30 ? 65535.0f : 16777215.0f;
}

bool g_own_constants = false;  // D3DSCM_NORESERVEDCONSTANTS: c-38 and c-37 are the title's

// c-38 and c-37 hold the viewport scale and offset, as D3D_GetViewportOffsetAndScale computes them
void UpdateViewportConstants() {
    auto& v = g_state.viewport;
    g_state.zscale = ZScale(g_state.zs);
    float hw = v.w * 0.5f, hh = v.h * 0.5f;
    if (!g_own_constants) {
        g_state.vsc[58] = {hw, -hh, (v.max_z - v.min_z) * g_state.zscale, 0};
        g_state.vsc[59] = {v.x + g_state.ss_offset_x + hw, v.y + g_state.ss_offset_y + hh, v.min_z * g_state.zscale, 0};
    }
    // the viewport as the library's SetViewport stores it, for inlined readers
    if (XDK_CDevice_Viewport != xdk::kUnknown) {
        uint32_t p = kDevice + XDK_CDevice_Viewport;
        W32(p, v.x);
        W32(p + 4, v.y);
        W32(p + 8, v.w);
        W32(p + 12, v.h);
        std::memcpy(HostPtr(p + 16), &v.min_z, 4);
        std::memcpy(HostPtr(p + 20), &v.max_z, 4);
    }
}

void SetComplex(uint32_t rs, uint32_t v) {
    g_state.rs[rs] = v;
    W32(kRenderState + rs * 4, v);
}

// The library's default render and texture stage states; its pushes land in the scratch buffer
void ApplyDefaultStates(X86Context& ctx) {
#if HAS_D3D_InitDefaultState
    GUEST_CALL(D3D_InitDefaultState, ctx, {});
#else
    (void)ctx;
    LOG_FIRST(1, "d3d: the image has no D3D_InitDefaultState, default states were not applied");
#endif
}

// The frame buffers a D3DPRESENT_PARAMETERS asks for become the device's targets, the viewport covers them. The
// renderer is made at the first ones' size.
void SetFrameBuffers(const char* caller, uint32_t pp) {
    const uint64_t count64 = std::max<uint64_t>(R32(pp + 12), 1);
    const uint32_t count = uint32_t(std::min<uint64_t>(count64, UINT32_MAX));
    uint32_t w = R32(pp), h = R32(pp + 4), fmt = R32(pp + 8);
    uint32_t auto_depth = R32(pp + 0x20), depth_fmt = R32(pp + 0x24), interval = R32(pp + 0x30);
    if (!w) w = 640;
    if (!h) h = 480;
    LOGI("d3d: %s %ux%u format %X buffers %u depth %u format %X interval %X flags %X", caller, w, h, fmt, count,
         auto_depth, depth_fmt, interval, R32(pp + 0x28));
    uint32_t color_fmt = gpu::LinearColorFormat(fmt);
    uint32_t zf = auto_depth ? gpu::LinearDepthFormat(depth_fmt) : 0;
    // surfaces of the same shape are kept: a title wraps their memory in textures of its own across a Reset
    static std::array<uint32_t, 5> made{};
    static bool depth_ours = false;
    const uint64_t buffered = std::clamp<uint64_t>(count64 + 1, 2, 3);  // one front plus the back buffers
    std::array<uint32_t, 5> asked{w, h, color_fmt, uint32_t(buffered), zf};
    // the title's own frame buffers: D3DPRESENT_PARAMETERS BufferSurfaces[3] (+0x34, front first) and
    // DepthStencilSurface (+0x40). True Crime NYC keeps one object per buffer and finds the back buffer by its memory,
    // so the device must draw into and present these surfaces, not ones of its own.
    uint32_t own[3] = {R32(pp + 0x34), R32(pp + 0x38), R32(pp + 0x3C)}, own_depth = R32(pp + 0x40);
    auto drop_made = [&] {
        if (made[0])
            for (uint32_t i = 0; i < made[3]; i++) FreeSurface(g_buffers[i]);
        if (depth_ours) FreeSurface(g_depth);
        g_depth = 0;
        depth_ours = false;
        made = {};
    };
    if (own[0]) {
        uint32_t n = own[2] ? 3 : own[1] ? 2 : 1;
        LOGI("d3d: %s uses the title's buffers %08X %08X %08X depth %08X", caller, own[0], own[1], own[2], own_depth);
        const uint32_t kept_depth = depth_ours ? 0 : g_depth;  // a title-owned depth may outlive a Reset
        drop_made();
        g_buffer_count = std::max<uint32_t>(n, 2);
        g_buffers[0] = own[0];
        g_buffers[1] = n > 1 ? own[1] : own[0];
        g_buffers[2] = n > 2 ? own[2] : 0;
        depth_ours = false;
        if (own_depth) {
            g_depth = own_depth;
        } else if (kept_depth) {
            g_depth = kept_depth;
        } else if (zf) {
            g_depth = NewSurface(w, h, zf, zf == 0x30 || zf == 0x31 ? 2 : 4);
            depth_ours = g_depth != 0;
        }
        made = {};  // a later Reset without own buffers makes the host's again
    } else if (asked != made) {
        // make the replacement first: a failed allocation keeps the buffers that are already bound
        std::array<uint32_t, 3> next{};
        bool ok = true;
        for (uint32_t i = 0; i < asked[3]; i++) {
            next[i] = NewSurface(w, h, color_fmt, 4);
            if (!next[i]) ok = false;
        }
        const uint32_t next_depth = zf ? NewSurface(w, h, zf, zf == 0x30 || zf == 0x31 ? 2 : 4) : 0;
        if (zf && !next_depth) ok = false;
        if (ok) {
            drop_made();
            g_buffer_count = asked[3];
            for (uint32_t i = 0; i < 3; i++) g_buffers[i] = next[i];
            g_depth = next_depth;  // 0x30 LIN_D16, 0x31 LIN_F16
            depth_ours = zf != 0;
            made = asked;
        } else {
            for (uint32_t surface : next) FreeSurface(surface);
            FreeSurface(next_depth);
            LOGE("d3d: %s could not make %ux%u frame buffers, keeping the current ones", caller, w, h);
        }
    }
    if (!g_backend) {
        g_backend = gpu::CreateBackend();
        if (!g_backend->Init(w, h)) {
            LOGE("d3d: renderer init failed");
#ifdef _WIN32
            // no owner window: this runs on the title's thread and the window may already be fullscreen over the box
            MessageBoxW(nullptr,
                        L"Direct3D 12 could not be started. Make sure your graphics card supports it and its driver "
                        L"is up to date.",
                        app::kGame.short_title, MB_ICONERROR | MB_TOPMOST);
#else
            LOGE("d3d: the renderer could not be started; see the log");
#endif
            kernel::Shutdown(1);
        }
        if (kRenderThread) g_backend = gpu::CreateThreadedBackend(std::move(g_backend));
    }
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    SetDev(kDevRenderTarget, g_buffers[1]);
    SetDev(kDevDepth, g_depth);
    SetDev(kDevBackBuffer, g_buffers[1]);
    SetDev(kDevFrontBuffer, g_buffers[1]);
    float one = 1.0f, zs = g_depth && (R32(g_depth + 12) >> 8 & 0xFF) == 0x30 ? 65535.0f : 16777215.0f;
    std::memcpy(HostPtr(kDevice + kDevSuperX), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevSuperY), &one, 4);
    std::memcpy(HostPtr(kDevice + kDevZScale), &zs, 4);
    // fields the library's init sets that inlined library code and the default states read
    if (XDK_CDevice_AutoDepth != xdk::kUnknown) SetDev(XDK_CDevice_AutoDepth, g_depth);
    if (XDK_CDevice_BackBufferScale != xdk::kUnknown) {
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale), &one, 4);
        std::memcpy(HostPtr(kDevice + XDK_CDevice_BackBufferScale + 4), &one, 4);
    }
    g_state.rt = SurfaceOf(g_buffers[1]);
    g_state.zs = SurfaceOf(g_depth);
    UpdateViewportConstants();
}

constexpr bool kVBlank = XDK_CDevice_VBlankCount != xdk::kUnknown && XDK_CDevice_VBlankEvent != xdk::kUnknown &&
                         XDK_CDevice_VBlankCallback != xdk::kUnknown;

// what the library's vertical blank interrupt leaves for the title: count, event, callback
void VerticalBlank(X86Context& ctx) {
    using clock = std::chrono::steady_clock;
    uint32_t info = mem::AllocSystem(12), shown = 0;
    auto next = clock::now();
    for (;;) {
        next = std::max(next + std::chrono::microseconds(16667), clock::now());
        std::this_thread::sleep_until(next);
        uint32_t count = Dev(XDK_CDevice_VBlankCount) + 1, swaps = Dev(kDevSwapCount);
        SetDev(XDK_CDevice_VBlankCount, count);
        ObjectRef event = FromPointer(kDevice + XDK_CDevice_VBlankEvent);
        SetEvent(*event);
        if (uint32_t callback = Dev(XDK_CDevice_VBlankCallback)) {
            // blank count, swaps shown, 1 when this blank showed one
            W32(info, count);
            W32(info + 4, swaps);
            W32(info + 8, swaps != shown);
            CallGuest(ctx, callback, {info});
        }
        shown = swaps;
        // the title clears the event in its own memory before a wait, which the host object does not see
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
        ResetEvent(*event);
    }
}

}  // namespace

gpu::State& State() { return g_state; }
std::recursive_mutex& Lock() { return g_lock; }

}  // namespace d3d

using namespace d3d;

// device

HLE_STDCALL(Direct3D_CreateDevice, 6) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_Device), XDK_DATA(D3D_pDevice), XDK_DATA(D3D_DirtyFlags),
                XDK_DATA(D3D_DeferredTextureState), XDK_DATA(D3D_RenderState), XDK_DATA(D3D_Streams),
                XDK_DATA(D3D_SimpleRenderStateEncode));
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Push), XDK_KEY(CDevice_PushThreshold), XDK_KEY(CDevice_BaseVertex),
                XDK_KEY(CDevice_IndexBuffer), XDK_KEY(CDevice_ZScale), XDK_KEY(CDevice_SuperSampleX),
                XDK_KEY(CDevice_SuperSampleY), XDK_KEY(CDevice_Transforms), XDK_KEY(CDevice_Textures),
                XDK_KEY(CDevice_RenderTarget), XDK_KEY(CDevice_Depth), XDK_KEY(CDevice_BackBuffer),
                XDK_KEY(CDevice_FrontBuffer), XDK_KEY(CDevice_SwapCount), XDK_KEY(CDevice_Size),
                XDK_KEY(DirtyFlags_Texture), XDK_KEY(TextureState_Stage), XDK_KEY(Stream_Size),
                XDK_KEY(Stream_Stride), XDK_KEY(Stream_Buffer));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    std::memset(HostPtr(kDevice), 0, XDK_CDevice_Size);
    W32(kDevicePtr, kDevice);
    ScratchPush();
    g_state = gpu::State{};
    gpu::MarkAllSimple();
    gpu::g_simple_tracked = true;
    SetFrameBuffers("CreateDevice", a[4]);
    // 60 Hz, widescreen
    if (XDK_CDevice_DisplayFlags != xdk::kUnknown) SetDev(XDK_CDevice_DisplayFlags, 0x400000 | (app::Widescreen() ? 0x10000 : 0));
    // the miniport's hardware init leaves an identity ramp in slot 0, which GetGammaRamp reads until one is set
    if (XDK_CDevice_GammaRamps != xdk::kUnknown) {
        uint8_t* ramp = static_cast<uint8_t*>(HostPtr(kDevice + XDK_CDevice_GammaRamps));  // red, green, blue
        for (uint32_t i = 0; i < 256; i++) ramp[i] = ramp[256 + i] = ramp[512 + i] = uint8_t(i);
    }
#if HAS_D3D_FvfShader
    if (XDK_CDevice_VertexShader != xdk::kUnknown) SetDev(XDK_CDevice_VertexShader, SYM(D3D_FvfShader));
#endif
    if (kVBlank) {
        // notification event, signalled
        SetDev(XDK_CDevice_VBlankEvent, 0x00040000);
        SetDev(XDK_CDevice_VBlankEvent + 4, 1);
        static bool started = false;
        if (!started) StartHostThread("vertical blank", VerticalBlank);
        started = true;
    }
    ApplyDefaultStates(ctx);
    // InitDefaultState restores defaults; creation's requested interval takes precedence.
    if (a[4]) W32(kRenderState + gpu::kRsPresentationInterval * 4, R32(a[4] + 0x30));
    if (a[5]) W32(a[5], kDevice);
    return 0;
}

HLE_STDCALL(D3DDevice_Release, 0) { return 1; }

// synchronisation, the renderer never runs behind

HLE_STDCALL(D3D_BlockOnResource, 1) {
    uint32_t res = a[0];
    if (res && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        uint32_t type = R32(res) & 0x70000;  // 0x40000 texture, 0x50000 surface
        if (type == 0x40000 || type == 0x50000) {
            uint32_t data = mem::VirtualFromPhysical(R32(res + 4));
            // perf diagnostic: who blocks on which surface, once each
            static std::unordered_set<uint32_t> seen;
            if (diag::Perf() && seen.insert(data).second) {
                uint32_t code[4] = {}, n = 0;
                for (uint32_t i = 0; i < 32 && n < 4; i++) {
                    uint32_t v = R32(ctx.esp + i * 4);
                    if (dispatch::InCode(v)) code[n++] = v;
                }
                LOGI("d3d: BlockOnResource on surface %08X, stack %08X %08X %08X %08X", data, code[0], code[1], code[2], code[3]);
            }
            g_backend->Sync(data, 0);
        } else {
            g_backend->WaitResource(res);
        }
    }
    gpu::MarkDirty(res);
    return 0;
}
// lock(resource, face, level, locked, rect, flags): D3DLOCK_NOOVERWRITE skips the block above, yet a
// title may read what the GPU drew
HLE_WRAP(Lock2DSurface, 6) {
    uint32_t res = a[0];
    if ((a[5] & 0x20) && res && g_backend) {  // D3DLOCK_NOOVERWRITE
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->SyncLate(mem::VirtualFromPhysical(R32(res + 4)));
    }
}
HLE_STDCALL(D3DResource_BlockUntilNotBusy, 1) {
    if (a[0] && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->WaitResource(a[0]);
    }
    gpu::MarkDirty(a[0]);
    return 0;
}
// (resource): Lock2 of a vertex buffer or palette. The library waits on GPU time stamps, which the host does not keep
// (BlockOnTime returns at once), so a title refilling a buffer would overwrite draws not yet taken.
HLE_STDCALL(D3D_BlockOnNonSurfaceResource, 1) {
    if (a[0] && g_backend) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_backend->WaitResource(a[0]);
    }
    gpu::MarkDirty(a[0]);
    return 0;
}
HLE_STDCALL(D3DResource_IsBusy, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t res = a[0];
    if (!res) return 0;
    // set on the device, a surface also through its texture
    uint32_t common = R32(res), parent = (common & 0x70000) == 0x50000 ? R32(res + 0x14) : 0;
    if ((common & 0x780000) || (parent && (R32(parent) & 0x780000))) return 1;  // internal reference count
    return g_backend && g_backend->Busy(res) ? 1 : 0;
}
HLE_STDCALL(D3D_BlockOnTime, 2) { return 0; }
HLE_STDCALL(D3DDevice_BlockOnFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->WaitFence(a[0]);
    return 0;
}
HLE_STDCALL(D3DDevice_InsertFence, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3DDevice_IsFencePending, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend && g_backend->FencePending(a[0]) ? 1 : 0;
}
HLE_STDCALL(D3D_SetFence, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return g_backend ? g_backend->Fence() : 0;
}
HLE_STDCALL(D3D_KickOffAndWaitForIdle, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Flush();
    return 0;
}
HLE_FASTCALL(CDevice_KickOff, 0) { return 0; }

HLE_STDCALL(D3DDevice_MakeSpace, 0) {
    LOG_FIRST(16, "d3d: MakeSpace from %08X, some caller still pushes hardware commands", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}
HLE_STDCALL(D3D_MakeRequestedSpace, 2) {
    LOG_FIRST(16, "d3d: MakeRequestedSpace from %08X", R32(ctx.esp));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    return ScratchPush();
}

HLE_STDCALL(D3DDevice_BlockUntilVerticalBlank, 0) {
    std::this_thread::sleep_for(std::chrono::milliseconds(16));
    return 0;
}
// (status): D3DRASTER_STATUS {InVBlank, ScanLine} on a 60 Hz frame clock. The library reads the GPU's scanline,
// which counts the front buffer's lines, then the blank: 45 lines per 480 shown, as on NTSC.
HLE_STDCALL(D3DDevice_GetRasterStatus, 1) {
    constexpr int64_t kFrameMicroseconds = 16667;
    constexpr uint32_t kBlankLines = 45;
    constexpr uint32_t kShownLines = 480;
    uint32_t shown = g_buffers[0] ? (R32(g_buffers[0] + 0x10) >> 12 & 0xFFF) + 1 : kShownLines;
    uint32_t lines = shown + shown * kBlankLines / kShownLines;
    auto now = std::chrono::steady_clock::now().time_since_epoch();
    int64_t into_frame = std::chrono::duration_cast<std::chrono::microseconds>(now).count() % kFrameMicroseconds;
    uint32_t line = uint32_t(into_frame * lines / kFrameMicroseconds);
    W32(a[0], line >= shown);
    W32(a[0] + 4, line < shown ? line : 0);
    return 0;
}
HLE_STDCALL(D3DDevice_SetFlickerFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_SetSoftDisplayFilter, 1) { return 0; }
HLE_STDCALL(D3DDevice_EnableOverlay, 1) { return 0; }
// the front buffer, read back at the guest size, is kept for the next instance after a quick reboot, which may
// fade from it at its first frames
HLE_STDCALL(D3DDevice_PersistDisplay, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const uint32_t front = g_buffers[0];
    if (!front) return 0;
    uint32_t size = R32(front + 0x10);
    uint32_t pitch = ((size >> 24 & 0xFF) + 1) * 64, height = (size >> 12 & 0xFFF) + 1, bytes = pitch * height;
    uint32_t data = mem::VirtualFromPhysical(R32(front + 4));
    if (g_backend) g_backend->Sync(data, bytes);
    kernel::SavePersistedDisplay(R32(front), R32(front + 0xC), size, HostPtr(data), bytes);
    LOGI("d3d: PersistDisplay, %u bytes", bytes);
    return 0;
}
HLE_STDCALL(D3DDevice_FlushVertexCache, 0) { return 0; }

// (near, far, flags): depth clip planes in depth buffer units. Flags 1 sets the vertex program planes, 2 the fixed
// function ones, 3 and 4 return them to the default (the whole range). The library keeps them in the device, where
// GetDepthClipPlanes reads them; the renderer clips vertex program draws outside [near, far] of their screen z.
HLE_STDCALL(D3DDevice_SetDepthClipPlanes, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    float near_z = ArgF(a, 0), far_z = ArgF(a, 1);
    uint32_t base = XDK_CDevice_DepthClip, mode = base != xdk::kUnknown ? Dev(base + 0x10) : 0;
    switch (a[2]) {
        case 1:
            if (base != xdk::kUnknown) SetDev(base + 8, a[0]), SetDev(base + 0xC, a[1]);
            mode |= 2;
            g_state.zclip[0] = near_z;
            g_state.zclip[1] = far_z;
            break;
        case 2:
            if (base != xdk::kUnknown) SetDev(base, a[0]), SetDev(base + 4, a[1]);
            mode |= 1;
            LOG_FIRST(1, "d3d: fixed function depth clip planes are kept but not applied");
            break;
        case 3: mode &= ~2u; break;
        case 4: mode &= ~1u; break;
        default: break;
    }
    if (base != xdk::kUnknown) SetDev(base + 0x10, mode);
    g_state.zclip_on = (mode & 2) ? 1 : 0;
    return 0;
}

// The library keeps the ramp in one of two device slots for GetGammaRamp: both get it, whichever one its index names
HLE_STDCALL(D3DDevice_SetGammaRamp, 2) {
    uint8_t ramp[768];
    std::memcpy(ramp, HostPtr(a[1]), 768);
    LOG_FIRST(8, "d3d: gamma ramp flags %X, red at 32/64/128/192/255: %u %u %u %u %u", a[0], ramp[32], ramp[64], ramp[128],
              ramp[192], ramp[255]);
    if (XDK_CDevice_GammaRamps != xdk::kUnknown)
        for (uint32_t slot = 0; slot < 2; slot++)
            std::memcpy(HostPtr(kDevice + XDK_CDevice_GammaRamps + slot * sizeof(ramp)), ramp, sizeof(ramp));
    if (g_backend) g_backend->SetGamma(ramp);
    return 0;
}

// targets and presentation

namespace {
// the device's targets become rt and zs, the references move from the old ones
void ApplyRenderTarget(X86Context& ctx, uint32_t rt, uint32_t zs) {
    AddRef(ctx, rt);
    uint32_t old_rt = Dev(kDevRenderTarget);
    uint32_t old_zs = Dev(kDevDepth);
    SetDev(kDevRenderTarget, rt);
    SetDev(kDevDepth, zs);
    AddRef(ctx, zs);
    Release(ctx, old_rt);
    Release(ctx, old_zs);
    g_state.rt = SurfaceOf(rt);
    g_state.zs = SurfaceOf(zs);
}
}  // namespace

HLE_STDCALL(D3DDevice_SetRenderTarget, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t rt = a[0] ? a[0] : Dev(kDevRenderTarget);
    ApplyRenderTarget(ctx, rt, a[1]);
    // SetRenderTarget resets the viewport to the whole target
    uint32_t w, h;
    gpu::SurfaceSize(g_state.rt, w, h);
    g_state.viewport = {0, 0, w, h, 0.0f, 1.0f};
    // The regular target bind rebuilds the hardware surface clip as well as the
    // viewport. A single inclusive SetScissors uses that same surface clip; it
    // must not leak into the next target (in particular smaller blur targets).
    g_state.scissor_count = 0;
    UpdateViewportConstants();
    return 0;
}

// (rt, zs, flags): the surfaces alone, the viewport stays as it was
HLE_STDCALL(D3DDevice_SetRenderTargetFast, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    ApplyRenderTarget(ctx, a[0], a[1]);
    return 0;
}

HLE_STDCALL(D3DDevice_GetBackBuffer2, 1) {
    int32_t i = int32_t(a[0]);
    uint32_t s = i < 0 ? g_buffers[0] : g_buffers[std::min<uint32_t>(uint32_t(i) + 1, g_buffer_count - 1)];
    W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetRenderTarget2, 0) {
    uint32_t s = Dev(kDevRenderTarget);
    if (s) W32(s, R32(s) + 1);
    return s;
}

HLE_STDCALL(D3DDevice_GetDepthStencilSurface2, 0) {
    uint32_t s = Dev(kDevDepth);
    if (s) W32(s, R32(s) + 1);
    return s;
}

namespace {
std::atomic<uint32_t> g_presents{0};
uint32_t g_frame_draws = 0;  // draws since the last Swap: a frame with none presents a CPU-written buffer
}  // namespace

uint32_t d3d::PresentCount() { return g_presents.load(); }

HLE_STDCALL(D3DDevice_Swap, 1) {
    // XBE_CAPTURE_AT=<seconds>[,<seconds>...]: frame captures that long after the first present, as F3 takes them;
    // p<number> instead of seconds captures that presented picture (replays line up by picture, not by time)
    {
        static std::vector<double> at, pictures;
        static bool parsed = [] {
            for (const char* v = std::getenv("XBE_CAPTURE_AT"); v && *v;) {
                const bool pic = *v == 'p';
                char* end;
                double s = std::strtod(v + (pic ? 1 : 0), &end);
                if (end == v + (pic ? 1 : 0)) break;
                (pic ? pictures : at).push_back(s);
                v = *end == ',' ? end + 1 : end;
            }
            std::sort(at.begin(), at.end(), std::greater<>());  // the next one last
            std::sort(pictures.begin(), pictures.end(), std::greater<>());
            return true;
        }();
        (void)parsed;
        static const auto start = std::chrono::steady_clock::now();
        if (!at.empty() && std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count() >= at.back()) {
            at.pop_back();
            gpu::g_capture = true;
        }
        if (!pictures.empty() && double(g_presents.load()) >= pictures.back()) {
            pictures.pop_back();
            gpu::g_capture = true;
        }
    }
    app::SplashFirstFrame();
    // handed over from the previous instance: the window opens on the second frame, the first picture with
    // anything drawn over it right after
    if (app::HandingOver() && g_presents >= 2) {
        {
            std::lock_guard<std::recursive_mutex> lock(g_lock);
            if (g_backend) g_backend->Flush();
        }
        app::FinishHandover();
    }
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_presents++;
    // the kept display is shown until the title presents its own
    kernel::SetSavedDataAddress(0);
    // the ramp an inlined SetGammaRamp left for the vertical blank that shows this swap
    if (g_backend && XDK_CDevice_GammaPending != xdk::kUnknown && XDK_CDevice_GammaRamps != xdk::kUnknown) {
        uint32_t i = Dev(kDevSwapCount) & 1;
        if (Dev(XDK_CDevice_GammaPending + i * 4) == 1) {
            SetDev(XDK_CDevice_GammaPending + i * 4, 0);
            g_backend->SetGamma(static_cast<const uint8_t*>(HostPtr(kDevice + XDK_CDevice_GammaRamps + i * 0x300)));
        }
    }
    Walk(Dev(kDevPush));  // reports asked for at the end of the frame
    // back buffer scale on (CDevice flag 0x4000, set by SetBackBufferScale with a scale other than 1): the library's
    // Swap draws the device's back buffer (+BackBuffer, a title may point it at its own surface) as a texture onto
    // the front buffer; present that surface instead of the host's own back buffer
    uint32_t shown = g_buffers[1] ? g_buffers[1] : g_buffers[0];
    if (XDK_CDevice_Flags != xdk::kUnknown && (Dev(XDK_CDevice_Flags) & 0x4000) && Dev(kDevBackBuffer))
        shown = Dev(kDevBackBuffer);
    if (g_backend) {
        gpu::Surface picture = SurfaceOf(shown);
        // one look at the open files per present, so the offset and the picture kind agree
        const bool movie = !g_frame_draws && kernel::MovieOpen();
#ifndef _WIN32
        // The legal screen is written at its buffer's base; the console presents that base.
        // The Linux surface data field starts 0x54 bytes (its frame header) into that
        // buffer, so a CPU-written frame (the legal screen) is presented
        // 0x54 bytes late -- a 21 pixel left shift at 640 pixels wide. The Windows renderer
        // finds targets by this address, so it moves the legal screen in BlitFrame instead.
        const uint32_t saved_data = picture.header ? R32(picture.header + 4) : 0;
        // Bink movie frames are written at the normal position: moving them puts the picture 21 pixels right
        if (!g_frame_draws && picture.data >= 0x54 && !movie) {
            picture.data -= 0x54;
            if (picture.header) W32(picture.header + 4, picture.data);
        }
#endif
        // a frame with no draws is a picture the title wrote: a movie frame while a Bink file is open, else a page
        if (!g_frame_draws) picture.picture = movie ? gpu::Picture::Movie : gpu::Picture::Still;
        g_backend->Present(picture);
#ifndef _WIN32
        if (saved_data) W32(picture.header + 4, saved_data);
#endif
        // a frame with no draws presents a CPU-written buffer (movies, legal screen):
        // wait for the renderer to finish reading it before the title writes the next one
        if (!g_frame_draws) g_backend->WaitPresent();
    }
    g_frame_draws = 0;
    uint32_t interval = R32(kRenderState + gpu::kRsPresentationInterval * 4);
    if (app::kGame.swap_interval) interval = app::kGame.swap_interval(interval);
    app::Pace(interval >= 1 && interval <= 4 ? 60 / interval : interval == 0 ? 60 : 0);
    // like the XDK, the surfaces stay put and their memory rotates: titles keep back buffer pointers and copy its data
    const uint32_t buffers = std::clamp<uint32_t>(g_buffer_count, 2, 3);
    if (g_buffers[0] && g_buffers[buffers - 1]) {
        uint32_t last = g_buffers[buffers - 1], data = R32(last + 4), lk = R32(last + 8);
        for (uint32_t i = buffers - 1; i > 0; i--) {
            W32(g_buffers[i] + 4, R32(g_buffers[i - 1] + 4));
            W32(g_buffers[i] + 8, R32(g_buffers[i - 1] + 8));
        }
        W32(g_buffers[0] + 4, data);
        W32(g_buffers[0] + 8, lk);
    }
    g_state.rt = SurfaceOf(Dev(kDevRenderTarget));
    uint32_t n = Dev(kDevSwapCount) + 1;
    SetDev(kDevSwapCount, n);
    // the console's present waits alertably for the vertical blank, so I/O completion routines (ReadFileEx) queued
    // to the presenting thread run once a frame. The host paces with a plain sleep: run them here instead, or a
    // title whose main thread issues streaming reads waits for their completions forever (True Crime NYC).
    if (kernel::Thread* t = kernel::Current()) kernel::RunApcs(*t);
    return n;
}

HLE_STDCALL(D3DDevice_Clear, 6) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const uint8_t* rects = a[0] && a[1] ? static_cast<const uint8_t*>(HostPtr(a[1])) : nullptr;
    if (g_backend) g_backend->Clear(g_state, a[0], rects, a[2], a[3], ArgF(a, 4), a[5]);
    return 0;
}

// (pp): the frame buffers pp asks for, the targets on them and cleared, as the library's Reset leaves the device.
// The device's references move back to the frame buffers first, so kept ones hold theirs as CreateDevice made them.
HLE_STDCALL(D3DDevice_Reset, 1) {
    constexpr uint32_t kClearTargetAndZ = 3;
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const uint32_t back = g_buffer_count > 1 ? g_buffers[1] : g_buffers[0];
    ApplyRenderTarget(ctx, back, g_depth);
    SetFrameBuffers("Reset", a[0]);
    if (a[0]) W32(kRenderState + gpu::kRsPresentationInterval * 4, R32(a[0] + 0x30));
    if (g_backend) g_backend->Clear(g_state, 0, nullptr, kClearTargetAndZ, 0, 1.0f, 0);
    return 0;
}

HLE_STDCALL(D3DDevice_CopyRects, 5) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_backend) g_backend->Copy(SurfaceOf(a[0]), SurfaceOf(a[3]), a[2], a[1], a[4]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetViewport, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t v = a[0];
    g_state.viewport = {R32(v), R32(v + 4), R32(v + 8), R32(v + 12), GF(v + 16), GF(v + 20)};
    g_state.scissor_count = 0;
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_SetScissors, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.scissor_count = std::min<uint32_t>(a[0], 8);
    for (uint32_t i = 0; i < g_state.scissor_count * 4; i++) g_state.scissors[i] = R32(a[2] + i * 4);
    return 0;
}

HLE_STDCALL(D3DDevice_SetScreenSpaceOffset, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.ss_offset_x = ArgF(a, 0);
    g_state.ss_offset_y = ArgF(a, 1);
    UpdateViewportConstants();
    return 0;
}

// render states

HLE_FASTCALL(D3DDevice_SetRenderState_Simple, 0) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t method = a[0] & 0x1FFC, value = a[1];
    SetSimple(method, value);
    return 0;
}

HLE_STDCALL(D3DDevice_SetRenderStateNotInline, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t rs = a[0], v = a[1];
    if (rs < gpu::kRsDeferredFirst) {
        uint32_t method = R32(kSimpleEncode + rs * 4) & 0x1FFC;
        SetSimple(method, v);
        W32(kRenderState + rs * 4, v);  // titles read their states back from the library's table
    } else if (rs < gpu::kRsMax) {
        SetComplex(rs, v);
    }
    return 0;
}

#define COMPLEX_STATE(name, rs)                       \
    HLE_STDCALL(D3DDevice_SetRenderState_##name, 1) { \
        std::lock_guard<std::recursive_mutex> lock(g_lock); \
        SetComplex(rs, a[0]);                         \
        return 0;                                     \
    }
COMPLEX_STATE(PSTextureModes, gpu::kRsPsTextureModes)
COMPLEX_STATE(VertexBlend, gpu::kRsVertexBlend)
COMPLEX_STATE(FogColor, gpu::kRsFogColor)
COMPLEX_STATE(FillMode, gpu::kRsFillMode)
COMPLEX_STATE(BackFillMode, gpu::kRsBackFillMode)
COMPLEX_STATE(TwoSidedLighting, gpu::kRsTwoSidedLighting)
COMPLEX_STATE(NormalizeNormals, gpu::kRsNormalizeNormals)
COMPLEX_STATE(ZEnable, gpu::kRsZEnable)
COMPLEX_STATE(StencilEnable, gpu::kRsStencilEnable)
COMPLEX_STATE(StencilFail, gpu::kRsStencilFail)
COMPLEX_STATE(FrontFace, gpu::kRsFrontFace)
COMPLEX_STATE(CullMode, gpu::kRsCullMode)
COMPLEX_STATE(TextureFactor, gpu::kRsTextureFactor)
COMPLEX_STATE(LogicOp, gpu::kRsLogicOp)
COMPLEX_STATE(EdgeAntiAlias, gpu::kRsEdgeAntiAlias)
COMPLEX_STATE(MultiSampleAntiAlias, gpu::kRsMultiSampleAntiAlias)
COMPLEX_STATE(MultiSampleMask, gpu::kRsMultiSampleMask)
COMPLEX_STATE(MultiSampleMode, gpu::kRsMultiSampleMode)
COMPLEX_STATE(MultiSampleRenderTargetMode, gpu::kRsMultiSampleRenderTargetMode)
COMPLEX_STATE(ShadowFunc, gpu::kRsShadowFunc)
COMPLEX_STATE(LineWidth, gpu::kRsLineWidth)
COMPLEX_STATE(SampleAlpha, gpu::kRsSampleAlpha)
COMPLEX_STATE(Dxt1NoiseEnable, gpu::kRsDxt1NoiseEnable)
COMPLEX_STATE(YuvEnable, gpu::kRsYuvEnable)
COMPLEX_STATE(OcclusionCullEnable, gpu::kRsOcclusionCullEnable)
COMPLEX_STATE(StencilCullEnable, gpu::kRsStencilCullEnable)
COMPLEX_STATE(RopZCmpAlwaysRead, gpu::kRsRopZCmpAlwaysRead)
COMPLEX_STATE(RopZRead, gpu::kRsRopZRead)
COMPLEX_STATE(DoNotCullUncompressed, gpu::kRsDoNotCullUncompressed)

HLE_STDCALL(D3DDevice_SetTextureState_TexCoordIndex, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.tss[a[0] & 3][gpu::kTssTexCoordIndex] = a[1];
    W32(kTextureState + ((a[0] & 3) * kStageStates + gpu::kTssTexCoordIndex) * 4, a[1]);
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BumpEnv, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (a[1] >= gpu::kTssBumpEnvMat00 && a[1] <= gpu::kTssBumpEnvLOffset) g_state.tss[a[0] & 3][a[1]] = a[2];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_BorderColor, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.tss[a[0] & 3][gpu::kTssBorderColor] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureState_ColorKeyColor, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.tss[a[0] & 3][gpu::kTssColorKeyColor] = a[1];
    return 0;
}
HLE_STDCALL(D3DDevice_SetTextureStageStateNotInline, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = a[0] & 3, type = a[1], v = a[2];
    if (type < gpu::kTssCount) {
        g_state.tss[stage][type] = v;
        W32(kTextureState + (stage * kStageStates + type) * 4, v);
    }
    return 0;
}

// the XDK's transform types: view 0, projection 1, texture 2-5, world 6-9; the renderer keeps Direct3D 8's numbers
// (view 2, projection 3, texture 16, world 256). The device copy is what GetTransform reads.
HLE_STDCALL(D3DDevice_SetTransform, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t t = a[0];
    if (t >= 10) return 0;
    std::memcpy(HostPtr(kDevice + kDevTransforms + t * 64), HostPtr(a[1]), 64);
    uint32_t pc = t < 2 ? t + 2 : t < 6 ? t + 14 : t + 250;
    std::memcpy(g_state.transforms[pc].data(), HostPtr(a[1]), 64);
    return 0;
}

HLE_STDCALL(D3DDevice_SetMaterial, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    std::memcpy(g_state.material.data(), HostPtr(a[0]), 17 * 4);
    return 0;
}

// the XDK keeps its own light lists, the renderer gets a copy
HLE_WRAP(D3DDevice_SetLight, 2) {
    uint32_t i = a[0], l = a[1];
    if (i < 8 && l) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        std::memcpy(g_state.lights[i].data(), HostPtr(l), 26 * 4);
    }
}
HLE_WRAP(D3DDevice_LightEnable, 2) {
    uint32_t i = a[0], on = a[1];
    if (i < 8) {
        std::lock_guard<std::recursive_mutex> lock(g_lock);
        g_state.light_enable = on ? g_state.light_enable | 1u << i : g_state.light_enable & ~(1u << i);
    }
}

// resources in use

HLE_STDCALL(D3DDevice_SetTexture, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = a[0] & 3, tex = a[1];
    uint32_t old = Dev(kDevTextures + stage * 4);
    // bound again: the AddRef and Release would cancel out
    bool same = tex == old;
    if (!same) AddRef(ctx, tex);
    SetDev(kDevTextures + stage * 4, tex);
    if (!same) Release(ctx, old);
    g_state.textures[stage] = tex;
    W32(kDirtyFlags, R32(kDirtyFlags) | XDK_DirtyFlags_Texture);
    return 0;
}

// SetPalette(stage, palette): the colours a P8 texture of the stage looks up
HLE_STDCALL(D3DDevice_SetPalette, 2) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Palettes));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t stage = a[0] & 3, pal = a[1];
    uint32_t old = Dev(kDevPalettes + stage * 4);
    AddRef(ctx, pal);
    SetDev(kDevPalettes + stage * 4, pal);
    Release(ctx, old);
    g_state.palettes[stage] = pal;
    return 0;
}

// SwitchTexture(method, data, format), fastcall: a swizzled texture of the bound one's shape replaces it. It is
// bound through a header of the stage's own holding the bound texture's fields with the new data and format.
HLE_FASTCALL(D3DDevice_SwitchTexture, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    static uint32_t headers[4];
    // texture methods from 0x1B00, 0x40 per stage
    uint32_t stage = (((a[0] & 0x1FFC) - 0x1B00) >> 6) & 3, cur = g_state.textures[stage];
    uint32_t& h = headers[stage];
    if (!h) h = mem::AllocSystem(20);
    if (cur && cur != h) std::memcpy(HostPtr(h), HostPtr(cur), 20);
    W32(h + 4, a[1]);
    W32(h + 12, a[2]);
    g_state.textures[stage] = h;
    return 0;
}

HLE_STDCALL(D3DDevice_SetStreamSource, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t n = a[0] & 15, vb = a[1], stride = a[2];
    uint32_t old = R32(kStreams + n * kStreamSize + XDK_Stream_Buffer);
    bool same = vb == old;
    if (!same) AddRef(ctx, vb);
    W32(kStreams + n * kStreamSize + XDK_Stream_Stride, stride);
    W32(kStreams + n * kStreamSize + XDK_Stream_Buffer, vb);
    if (!same) Release(ctx, old);
    g_state.streams[n] = {vb, stride};
    return 0;
}

HLE_STDCALL(D3DDevice_SetIndices, 2) {
    XDK_REQUIRE("d3d", XDK_DATA(D3D_IndexData));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t ib = a[0];
    uint32_t old = Dev(kDevIndexBuffer);
    bool same = ib == old;
    if (!same) AddRef(ctx, ib);
    W32(kIndexData, ib ? R32(ib + 4) : 0);
    SetDev(kDevIndexBuffer, ib);
    SetDev(kDevBaseVertex, a[1]);
    if (!same) Release(ctx, old);
    g_state.index_buffer = ib;
    g_state.index_data = ib ? R32(ib + 4) : 0;
    g_state.base_vertex = a[1];
    return 0;
}

// shaders

// TODO(pre-beta): a shader that fails validation still fails the create. A basic logged fallback program would let the
// draw through white instead of disappearing; changing program matching is too invasive for this pass.
HLE_STDCALL(D3DDevice_CreateVertexShader, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t decl = a[0], func = a[1], out = a[2];
    if (g_vs.size() >= kMaxGuestShaders) {
        LOGE("d3d: too many vertex shaders, %08X not made", decl);
        return kInvalidCall;
    }
    auto vs = std::make_shared<gpu::VertexShader>();
    if (decl) {
        bool ended = false;
        for (uint32_t i = 0; i < 1024; i++) {  // the console's declaration limit
            const uint64_t at = uint64_t(decl) + uint64_t(i) * 4;
            if (at + 4 > 0x100000000ull || !GuestReadable(uint32_t(at), 4)) break;
            uint32_t t = R32(uint32_t(at));
            vs->decl.push_back(t);
            if (t == 0xFFFFFFFF) {
                ended = true;
                break;
            }
        }
        if (!ended) {
            LOGE("d3d: vertex shader declaration %08X is not readable or has no end token", decl);
            return kInvalidCall;
        }
    }
    if (func) {
        if (!GuestReadable(func, 4)) {
            LOGE("d3d: vertex shader function %08X is not readable", func);
            return kInvalidCall;
        }
        uint32_t hdr = R32(func);
        vs->program_type = hdr & 0xFFFF;
        uint32_t n = hdr >> 16;
        const uint64_t words = uint64_t(n) * 4;
        if (words > 512 || uint64_t(func) + 4 + words * 4 > 0x100000000ull ||
            (words && !GuestReadable(func + 4, words * 4))) {  // 128 instructions, the console's limit
            LOGE("d3d: vertex shader function %08X has %llu dwords, not readable", func,
                 static_cast<unsigned long long>(words));
            return kInvalidCall;
        }
        for (uint64_t i = 0; i < words; i++) vs->program.push_back(R32(uint32_t(uint64_t(func) + 4 + i * 4)));
    }
    gpu::ParseDeclaration(*vs);
    uint32_t rec = mem::AllocSystem(16, 16);
    if (!rec) return kInvalidCall;
    vs->handle = rec | 1;
    g_vs[vs->handle] = vs;
    W32(out, vs->handle);
    LOG_FIRST(64, "d3d: vertex shader %08X, %zu decl tokens, %zu instructions, type %X", vs->handle, vs->decl.size(),
              vs->program.size() / 4, vs->program_type);
    return 0;
}

HLE_STDCALL(D3DDevice_DeleteVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    if (it != g_vs.end()) {
        mem::FreeSystem(a[0] & ~1u);
        g_vs.erase(it);
    }
    return 0;
}

// (handle, data, size in and out): the program as it was given, the size alone without data or room
HLE_STDCALL(D3DDevice_GetVertexShaderFunction, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    uint32_t words = it == g_vs.end() ? 0 : uint32_t(it->second->program.size());
    uint32_t need = words ? 4 + words * 4 : 0, room = R32(a[2]);
    W32(a[2], need);
    if (need > room) return a[1] ? 0x88760867 : 0;  // D3DERR_MOREDATA
    if (!a[1] || !need) return 0;
    W32(a[1], it->second->program_type | words / 4 << 16);
    for (uint32_t i = 0; i < words; i++) W32(a[1] + 4 + i * 4, it->second->program[i]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetVertexShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    uint32_t h = a[0];
    g_state.vs_handle = h;
    g_state.vs = nullptr;
    if (h & 1) {
        auto it = g_vs.find(h);
        if (it != g_vs.end()) {
            g_state.vs = it->second;
            // constants declared with the shader load when it is selected
            for (auto& [reg, v] : it->second->constants)
                if (reg < 192) g_state.vsc[reg] = v;
        } else {
            LOG_FIRST(8, "d3d: SetVertexShader with unknown handle %08X", h);
        }
    }
    // The XDK binds default current colours for declaration inputs that are
    // absent (5849: 0x2BBA57..0x2BBAA7). Later SetVertexData calls may override
    // these, including when an FVF omits DIFFUSE/SPECULAR.
    gpu::VertexShader fvf;
    if (!(h & 1)) gpu::FvfInputs(h, fvf);
    const auto* layout = h & 1 ? g_state.vs.get() : &fvf;
    if (layout) {
        for (int slot : {3, 4, 7, 8}) {
            const auto& input = layout->inputs[slot];
            if (input.stream >= 0 && input.type && input.type != 0x02) continue;
            float value = slot == 3 || slot == 7 ? 1.0f : 0.0f;
            for (float& component : g_state.vdef[slot]) component = value;
        }
    }
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1Fast, 0) {
    uint32_t reg = a[0];
    if (reg < 192) std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant1, 0) {
    uint32_t reg = a[0];
    if (reg < 192) std::memcpy(g_state.vsc[reg].data(), HostPtr(a[1]), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstant4, 0) {
    uint32_t reg = a[0], src = a[1];
    for (uint32_t i = 0; i < 4 && reg + i < 192; i++) std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    return 0;
}

HLE_FASTCALL(D3DDevice_SetVertexShaderConstantNotInlineFast, 1) {
    uint32_t reg = a[0], src = a[1], dwords = a[2];
    for (uint32_t i = 0; i < dwords / 4 && reg + i < 192; i++) std::memcpy(g_state.vsc[reg + i].data(), HostPtr(src + i * 16), 16);
    return 0;
}

// Vertex program memory: Load writes instructions at a slot, Select runs the program that starts at one
namespace {
constexpr uint32_t kProgramSlots = 136;
constexpr uint32_t kInstructionDwords = 4;
constexpr uint32_t kLastInstruction = 1;  // bit 0 of an instruction's fourth dword
constexpr uint32_t kVertexProgramType = 0x2078;
constexpr uint32_t kInputRegisters = 16;

// guest vertex shader object, the record a handle points at
constexpr uint32_t kShaderInputs = 0x14;  // 16 inputs of {stream, offset, D3DVSDT type, unused}
constexpr uint32_t kShaderInputBytes = 16;
constexpr uint32_t kTypeNone = 0x02;  // D3DVSDT_NONE

std::array<uint32_t, kProgramSlots * kInstructionDwords> g_program_memory{};
std::map<uint64_t, std::shared_ptr<gpu::VertexShader>> g_selected;  // kept alive, the render thread keys by pointer
constexpr size_t kMaxSelected = 1024;  // bound the derived-program cache; entries are rebuilt on demand

// bytes of one D3DVSDT element: kind in the low nibble, count in the next
uint32_t ElementBytes(uint32_t type) {
    uint32_t kind = type & 15;
    uint32_t count = (type >> 4) & 15;
    if (kind == 0 || kind == 6) return 4;          // D3DCOLOR, packed 11:11:10
    if (kind == 1 || kind == 5) return 2 * count;  // shorts
    if (kind == 4) return count;                   // bytes
    return 4 * count;                              // floats
}

void StoreProgram(uint32_t slot, const std::vector<uint32_t>& words) {
    if (slot >= kProgramSlots) return;
    size_t room = g_program_memory.size() - slot * kInstructionDwords;
    std::copy_n(words.begin(), std::min(words.size(), room), g_program_memory.begin() + slot * kInstructionDwords);
}

// a {type | count << 16, instructions} function's instructions
std::vector<uint32_t> FunctionWords(uint32_t function) {
    if (!GuestReadable(function, 4)) {
        LOGE("d3d: vertex shader function %08X is not readable", function);
        return {};
    }
    uint32_t count = R32(function) >> 16;
    const uint64_t words = uint64_t(count) * kInstructionDwords;
    if (words > 512 || uint64_t(function) + 4 + words * 4 > 0x100000000ull ||
        (words && !GuestReadable(function + 4, words * 4))) {  // 128 instructions, the console's limit
        LOGE("d3d: vertex shader function %08X has %llu dwords, not readable", function,
             static_cast<unsigned long long>(words));
        return {};
    }
    std::vector<uint32_t> out(static_cast<size_t>(words));
    for (uint64_t i = 0; i < words; i++) out[i] = R32(uint32_t(uint64_t(function) + 4 + i * 4));
    return out;
}

// the instructions from slot to the last one
std::vector<uint32_t> ProgramAt(uint32_t slot) {
    std::vector<uint32_t> program;
    for (; slot < kProgramSlots; slot++) {
        const uint32_t* insn = &g_program_memory[slot * kInstructionDwords];
        program.insert(program.end(), insn, insn + kInstructionDwords);
        if (insn[3] & kLastInstruction) break;
    }
    return program;
}

// a guest made shader's inputs, as its object holds them
void ApplyGuestInputs(uint32_t handle, gpu::VertexShader& vs) {
    uint32_t inputs = (handle & ~1u) + kShaderInputs;
    for (uint32_t reg = 0; reg < kInputRegisters; reg++) {
        uint32_t entry = inputs + reg * kShaderInputBytes;
        uint32_t stream = R32(entry);
        uint32_t offset = R32(entry + 4);
        uint32_t type = R32(entry + 8);
        if (type == kTypeNone || stream >= vs.stream_stride.size()) continue;
        vs.inputs[reg] = {int(stream), offset, type};
        uint32_t end = offset + ElementBytes(type);
        vs.stream_stride[stream] = std::max(vs.stream_stride[stream], end);
    }
}

// the vertex inputs a selected program reads: a host made shader's, else the guest object's
void ApplyShaderInputs(uint32_t handle, gpu::VertexShader& vs) {
    auto it = g_vs.find(handle);
    if (it == g_vs.end()) {
        ApplyGuestInputs(handle, vs);
        return;
    }
    vs.inputs = it->second->inputs;
    vs.stream_stride = it->second->stream_stride;
}

// one shader object per program and input layout; handle 0 keeps the bound shader's inputs
std::shared_ptr<gpu::VertexShader> SelectedShader(uint32_t handle, uint32_t slot) {
    gpu::VertexShader vs;
    vs.program = ProgramAt(slot);
    vs.program_type = kVertexProgramType;
    if (handle) {
        ApplyShaderInputs(handle, vs);
    } else {
        vs.inputs = g_state.vs->inputs;
        vs.stream_stride = g_state.vs->stream_stride;
    }
    uint64_t key = gpu::Hash(vs.program.data(), vs.program.size() * 4);
    key = gpu::Hash(vs.inputs.data(), sizeof(vs.inputs), key);
    key = gpu::Hash(vs.stream_stride.data(), sizeof(vs.stream_stride), key);
    if (g_selected.size() >= kMaxSelected && !g_selected.contains(key)) g_selected.clear();
    auto& kept = g_selected[key];
    if (!kept) kept = std::make_shared<gpu::VertexShader>(std::move(vs));
    return kept;
}
}  // namespace

// (handle, slot): the program a shader was made with, stored at the slot
HLE_STDCALL(D3DDevice_LoadVertexShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    auto it = g_vs.find(a[0]);
    if (it == g_vs.end()) {
        LOG_FIRST(8, "d3d: LoadVertexShader with unknown handle %08X", a[0]);
        return 0;
    }
    StoreProgram(a[1], it->second->program);
    return 0;
}

// (function, slot)
HLE_STDCALL(D3DDevice_LoadVertexShaderProgram, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    StoreProgram(a[1], FunctionWords(a[0]));
    return 0;
}

// (handle, slot): the program at the slot runs with the handle's inputs; handle 0 only moves the start
HLE_STDCALL(D3DDevice_SelectVertexShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t handle = a[0];
    bool program_bound = (g_state.vs_handle & 1) && g_state.vs;
    if (!handle && !program_bound) {
        LOG_FIRST(8, "d3d: SelectVertexShader at slot %u without a bound vertex shader", a[1]);
        return 0;
    }
    g_state.vs = SelectedShader(handle, a[1]);
    if (handle) g_state.vs_handle = handle;
    return 0;
}
HLE_STDCALL(D3DDevice_SetShaderConstantMode, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_own_constants = a[0] & 0x10;  // D3DSCM_NORESERVEDCONSTANTS
    UpdateViewportConstants();
    return 0;
}

HLE_STDCALL(D3DDevice_CreatePixelShader, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_ps.size() >= kMaxGuestShaders || !GuestReadable(a[0], 0xF0 / 4 * 4)) {
        LOGE("d3d: pixel shader definition %08X is not readable, not made", a[0]);
        return kInvalidCall;
    }
    auto ps = std::make_shared<gpu::PixelShader>();
    std::memcpy(ps->def, HostPtr(a[0]), sizeof(ps->def));
    uint32_t rec = mem::AllocSystem(16, 16);
    if (!rec) return kInvalidCall;
    ps->handle = rec;
    g_ps[rec] = ps;
    W32(a[1], rec);
    return 0;
}

HLE_STDCALL(D3DDevice_DeletePixelShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_ps.erase(a[0])) mem::FreeSystem(a[0]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShader, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    g_state.ps = nullptr;
    if (!a[0]) return 0;
    auto it = g_ps.find(a[0]);
    if (it != g_ps.end()) {
        g_state.ps = it->second;
    } else if (XDK_PixelShader_Definition != xdk::kUnknown) {
        // made by library code the title inlined: the object points at its definition, handles are reused
        static std::unordered_map<uint32_t, std::shared_ptr<gpu::PixelShader>> made;
        constexpr size_t kMaxMade = 256;  // bound the cache a corrupt stream of handles can grow
        const uint64_t field = uint64_t(a[0]) + XDK_PixelShader_Definition;
        if (field + 4 > 0x100000000ull || !GuestReadable(a[0] + XDK_PixelShader_Definition, 4)) {
            LOG_FIRST(8, "d3d: pixel shader %08X has no readable definition pointer", a[0]);
            return 0;
        }
        const uint32_t def_va = R32(a[0] + XDK_PixelShader_Definition);
        if (!GuestReadable(def_va, 0xF0 / 4 * 4)) {
            LOG_FIRST(8, "d3d: pixel shader %08X definition %08X is not readable", a[0], def_va);
            return 0;
        }
        const void* def = HostPtr(def_va);
        if (made.size() >= kMaxMade && !made.contains(a[0])) made.clear();
        auto& ps = made[a[0]];
        if (!ps || std::memcmp(ps->def, def, sizeof(ps->def))) {
            ps = std::make_shared<gpu::PixelShader>();
            ps->handle = a[0];
            std::memcpy(ps->def, def, sizeof(ps->def));
        }
        g_state.ps = ps;
    } else {
        LOG_FIRST(8, "d3d: SetPixelShader with unknown handle %08X", a[0]);
        return 0;
    }
    // the definition loads the combiner state like the XDK pushes it
    const uint32_t* d = g_state.ps->def;
    for (int i = 0; i < 8; i++) {
        SetSimple(gpu::kMPsAlphaInputs + i * 4, d[0 + i]);
        SetSimple(gpu::kMPsConstant0 + i * 4, d[10 + i]);
        SetSimple(gpu::kMPsConstant1 + i * 4, d[18 + i]);
        SetSimple(gpu::kMPsAlphaOutputs + i * 4, d[26 + i]);
        SetSimple(gpu::kMPsRgbInputs + i * 4, d[34 + i]);
        SetSimple(gpu::kMPsRgbOutputs + i * 4, d[45 + i]);
    }
    SetSimple(gpu::kMPsFinalAbcd, d[8]);
    SetSimple(gpu::kMPsFinalEfgh, d[9]);
    SetSimple(gpu::kMPsCompareMode, d[42]);
    SetSimple(gpu::kMPsFinalConstant0, d[43]);
    SetSimple(gpu::kMPsFinalConstant1, d[44]);
    SetSimple(gpu::kMPsCombinerCount, d[53]);
    SetSimple(gpu::kMPsDotMapping, d[55]);
    SetSimple(gpu::kMPsInputTexture, d[56]);
    SetComplex(gpu::kRsPsTextureModes, d[54]);
    return 0;
}

HLE_STDCALL(D3DDevice_SetPixelShaderConstant, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t reg = a[0], src = a[1], n = a[2];
    for (uint32_t i = 0; i < n && reg + i < 16; i++) std::memcpy(g_state.psc[reg + i].data(), HostPtr(src + i * 16), 16);
    gpu::ApplyPixelConstants(g_state, reg, n);
    gpu::MarkAllSimple();  // it writes the combiner constants into simple
    return 0;
}

// drawing

HLE_STDCALL(D3DDevice_DrawVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));  // a visibility test begun since the last draw counts this one
    SyncDeferred();
    if (g_backend) g_backend->Draw(g_state, a[0], a[1], a[2], nullptr);
    g_frame_draws++;
    gpu::RetainVertexAttributes(g_state, a[1], a[2], nullptr);
    return 0;
}

HLE_STDCALL(D3DDevice_DrawIndexedVertices, 3) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    SyncDeferred();
    const uint8_t* indices = a[2] ? static_cast<const uint8_t*>(HostPtr(a[2])) : nullptr;
    if (g_backend) g_backend->Draw(g_state, a[0], 0, a[1], indices);
    g_frame_draws++;
    gpu::RetainVertexAttributes(g_state, 0, a[1], indices);
    return 0;
}

// DrawVerticesUP(prim, count, vertices, stride): the vertices are copied to a ring the render thread reads, stream 0
// points there through a vertex buffer header for the draw. A half of the ring is written again once the draws
// that read it are done.
namespace {
constexpr uint32_t kUpRing = 4u << 20, kUpHalf = kUpRing / 2;
uint32_t g_up_ring = 0, g_up_pos = 0, g_up_header = 0, g_up_fence[2] = {};

uint32_t UpVertices(const void* src, uint32_t bytes) {
    if (!g_up_ring) {
        g_up_ring = mem::AllocContiguous(kUpRing, 0, 0xFFFFFFFF, 0x1000);
        g_up_header = mem::AllocSystem(20);
        std::memset(HostPtr(g_up_header), 0, 20);
        W32(g_up_header, 1);  // a vertex buffer, one reference
    }
    bytes = std::min(bytes, kUpHalf);
    uint32_t room = (bytes + 63) & ~63u;
    uint32_t pos = g_up_pos + room > kUpRing ? 0 : g_up_pos;
    uint32_t from = g_up_pos ? (g_up_pos - 1) / kUpHalf : 1, to = (pos + room - 1) / kUpHalf;
    if (to != from) {
        g_up_fence[from] = g_backend->Fence();
        g_backend->WaitFence(g_up_fence[to]);
    }
    std::memcpy(HostPtr(g_up_ring + pos), src, bytes);
    g_up_pos = pos + room;
    W32(g_up_header + 4, mem::PhysicalFromVirtual(g_up_ring + pos));
    return g_up_header;
}

void DrawUp(uint32_t prim, uint32_t count, const void* vertices, uint32_t stride) {
    if (!g_backend || !count || !stride) return;
    if (uint64_t(count) * stride > kUpHalf) {
        count = kUpHalf / stride;
        if (!count) return;
    }
    SyncDeferred();
    auto stream = g_state.streams[0];
    g_state.streams[0] = {UpVertices(vertices, count * stride), stride};
    g_backend->Draw(g_state, prim, 0, count, nullptr);
    g_frame_draws++;
    gpu::RetainVertexAttributes(g_state, 0, count, nullptr);
    g_state.streams[0] = stream;
}

// bytes per vertex of the bound format: stream 0 of the FVF or of the shader's declaration
uint32_t VertexBytes() {
    if (g_state.vs_handle & 1) return g_state.vs ? g_state.vs->stream_stride[0] : 0;
    gpu::VertexShader fvf;
    gpu::FvfInputs(g_state.vs_handle, fvf);
    return fvf.stream_stride[0];
}

uint32_t g_push_begin = 0;
}  // namespace

// Begin/End immediate mode. Titles inline Begin and the vertex data calls, so their commands are read from the scratch buffer.
namespace d3d {
namespace {
uint32_t g_im_prim = 0, g_im_mask = 0;  // open primitive, slots it wrote
float g_im_attr[64] = {0, 0, 0, 1, 0, 0, 0, 1, 0, 0, 0, 1, 1, 1, 1, 1};  // 16 slots of x y z w
std::vector<float> g_im_vertices;       // every slot per vertex

// visibility tests: depth passing pixels are counted while enabled, a report gets the count since the clear,
// at a physical address in the first 16 MB
void ApplyVisibilityMethod(uint32_t m, uint32_t v) {
    constexpr uint32_t kReportAddressMask = 0xFFFFFF;
    if (m == gpu::kMCountEnable) SetSimple(gpu::kMCountEnable, v);
    if (!g_backend) return;
    if (m == gpu::kMClearReport) g_backend->ClearReport();
    if (m == gpu::kMGetReport) g_backend->GetReport(mem::VirtualFromPhysical(v & kReportAddressMask));
}

// draws the open primitive: the slots it wrote are FLOAT4 inputs of the bound program, or of the fixed pipeline
void EndPrimitive() {
    struct Made {
        std::shared_ptr<gpu::VertexShader> from, vs;
    };
    // the render thread tells shaders apart by pointer: one object per program and slot set, the program kept alive
    static std::map<std::pair<gpu::VertexShader*, uint32_t>, Made> made;
    static std::vector<float> packed;
    uint32_t prim = g_im_prim, count = uint32_t(g_im_vertices.size() / 64), handle = g_state.vs_handle;
    g_im_prim = 0;
    if (!prim || !count) return;
    // pretransformed FVF (D3DFVF_XYZRHW, as front ends and movies draw their quads): pack the slots into the FVF's own
    // layout and draw with the FVF handle, as DrawVerticesUP does, so the positions stay in screen space
    if (!(handle & 1) && (handle & 0xE) == 4) {
        gpu::VertexShader fvf;
        gpu::FvfInputs(handle, fvf);
        uint32_t stride = fvf.stream_stride[0];
        static std::vector<uint8_t> bytes;
        bytes.assign(size_t(count) * stride, 0);
        bool ok = stride != 0;
        for (uint32_t v = 0; ok && v < count; v++)
            for (uint32_t s = 0; s < 16; s++) {
                const auto& in = fvf.inputs[s];
                if (in.stream < 0) continue;
                const float* src = &g_im_vertices[size_t(v) * 64 + s * 4];
                uint8_t* dst = &bytes[size_t(v) * stride + in.offset];
                auto byte = [](float x) { return uint32_t(std::clamp(x, 0.0f, 1.0f) * 255.0f + 0.5f); };
                switch (in.type) {
                    case 0x12: std::memcpy(dst, src, 4); break;   // FLOAT1
                    case 0x22: std::memcpy(dst, src, 8); break;   // FLOAT2
                    case 0x32: std::memcpy(dst, src, 12); break;  // FLOAT3
                    case 0x42: std::memcpy(dst, src, 16); break;  // FLOAT4
                    case 0x40: {  // D3DCOLOR: the slot holds R G B A, memory is B G R A
                        uint32_t c = byte(src[2]) | byte(src[1]) << 8 | byte(src[0]) << 16 | byte(src[3]) << 24;
                        std::memcpy(dst, &c, 4);
                        break;
                    }
                    default:
                        LOG_FIRST(4, "d3d: pretransformed immediate mode input type %02X not packed", in.type);
                        ok = false;
                        break;
                }
            }
        if (ok) {
            DrawUp(prim, count, bytes.data(), stride);
            return;
        }
    }
    auto bound = g_state.vs, from = handle & 1 ? bound : nullptr;
    Made& m = made[{from.get(), g_im_mask}];
    if (!m.vs) {
        m.from = from;
        m.vs = std::make_shared<gpu::VertexShader>();
        if (from) {
            m.vs->program = from->program;
            m.vs->program_type = from->program_type;
        }
        for (uint32_t s = 0; s < 16; s++)
            if (g_im_mask >> s & 1) {
                m.vs->inputs[s] = {0, m.vs->stream_stride[0], 0x42};
                m.vs->stream_stride[0] += 16;
            }
    }
    packed.clear();
    for (uint32_t v = 0; v < count; v++)
        for (uint32_t s = 0; s < 16; s++)
            if (g_im_mask >> s & 1) packed.insert(packed.end(), &g_im_vertices[v * 64 + s * 4], &g_im_vertices[v * 64 + s * 4] + 4);
    g_state.vs = m.vs;
    g_state.vs_handle = handle | 1;
    DrawUp(prim, count, packed.data(), m.vs->stream_stride[0]);
    g_state.vs = bound;
    g_state.vs_handle = handle;
}

// a begin/end or vertex data method opens, feeds or draws the immediate primitive; false for any other method
bool ApplyImmediate(uint32_t m, uint32_t v) {
    // Array draws can have changed the current attributes since the last setter.
    std::memcpy(g_im_attr, g_state.vdef, sizeof(g_im_attr));
    float f;
    float* a = g_im_attr;
    bool last = true;  // the slot's last component
    std::memcpy(&f, &v, 4);
    if (m == 0x17FC) {  // begin/end, 0 ends
        if (v) {
            g_im_prim = v;
            g_im_mask = 0;
            g_im_vertices.clear();
        } else {
            EndPrimitive();
        }
        return true;
    } else if (m >= 0x1880 && m < 0x1900) {  // 2 floats, z = 0 and w = 1
        a += (m - 0x1880) / 8 * 4;
        last = (m - 0x1880) / 4 & 1;
        a[last] = f;
        a[2] = 0;
        a[3] = 1;
    } else if (m >= 0x1940 && m < 0x1980) {  // 4 bytes
        a += (m - 0x1940) / 4 * 4;
        for (int c = 0; c < 4; c++) a[c] = (v >> c * 8 & 0xFF) / 255.0f;
    } else if (m >= 0x1A00 && m < 0x1B00) {  // 4 floats
        a += (m - 0x1A00) / 16 * 4;
        last = ((m - 0x1A00) / 4 & 3) == 3;
        a[(m - 0x1A00) / 4 & 3] = f;
    } else if (m >= 0x1500 && m < 0x150C) {  // position, 3 floats and w = 1
        last = m == 0x1508;
        a[(m - 0x1500) / 4] = f;
        a[3] = 1;
    } else if (m >= 0x1518 && m < 0x1528) {  // position, 4 floats
        last = m == 0x1524;
        a[(m - 0x1518) / 4] = f;
    } else {
        return false;
    }
    // Immediate setters and array draws share the current vertex attributes on NV2A.
    // Keep each component write, including writes inside Begin/End, for later draws
    // whose declaration does not supply this input. Waiting for the last component
    // also loses legal partial updates of a four-component attribute.
    std::memcpy(g_state.vdef[(a - g_im_attr) / 4], a, 16);
    if (!g_im_prim) return true;
    g_im_mask |= 1u << (a - g_im_attr) / 4;
    if (a == g_im_attr && last) g_im_vertices.insert(g_im_vertices.end(), g_im_attr, g_im_attr + 64);
    return true;
}

// Vertex constants a title pushes itself (007 does a skinned mesh's ambient and material colour so): a load
// register, then four dwords per register from it on
constexpr uint32_t kConstantLoad = 0x1EA4;
constexpr uint32_t kConstantData = 0x0B80;
constexpr uint32_t kConstantDataEnd = 0x0C00;
constexpr uint32_t kConstantRegisters = 192;
uint32_t g_constant_register = 0;
uint32_t g_constant_component = 0;

// a constant load or data method updates the vertex constants; false for any other method
bool ApplyConstant(uint32_t m, uint32_t v) {
    if (m == kConstantLoad) {
        g_constant_register = v;
        g_constant_component = 0;
        return true;
    }
    if (m < kConstantData || m >= kConstantDataEnd) return false;
    if (g_constant_register < kConstantRegisters) {
        std::memcpy(&g_state.vsc[g_constant_register][g_constant_component], &v, 4);
    }
    if (++g_constant_component == 4) {
        g_constant_component = 0;
        g_constant_register++;
    }
    return true;
}

// reads the commands pushed since the last call: vertex data between a begin and an end is drawn, visibility tests
// are run, constants are stored, the rest is skipped
void Walk(uint32_t end) {
    if (!g_walk || end < g_walk || end > g_scratch_push + g_scratch_size) return;
    for (uint32_t p = g_walk; p + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || n * 4 > end - p) break;  // anything but a method header
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4;
            uint32_t v = R32(p);
            ApplyVisibilityMethod(m, v);
            if (!ApplyConstant(m, v)) ApplyImmediate(m, v);
        }
    }
    g_walk = end;
}
}  // namespace
}  // namespace d3d

HLE_STDCALL(D3DDevice_DrawVerticesUP, 4) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    DrawUp(a[0], a[1], HostPtr(a[2]), a[3]);
    return 0;
}

// BeginPush(dwords) hands out room for raw hardware commands, EndPush(end) runs what the title wrote there:
// a primitive between two begin/end writes with its vertices inline or as vertex data, other methods as simple states.
HLE_STDCALL(D3DDevice_BeginPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    const uint64_t bytes = uint64_t(a[0]) * 4;
    if (bytes > UINT32_MAX) {
        LOGE("d3d: BeginPush of %u dwords is too large", a[0]);
        g_push_begin = 0;
        return 0;
    }
    g_push_begin = ScratchPush(uint32_t(bytes));
    return g_push_begin;
}

// a primitive whose vertices came as an inline array, in the bound vertex format
static void DrawInline(uint32_t prim, const std::vector<uint32_t>& vertices) {
    if (vertices.empty()) return;
    uint32_t stride = VertexBytes();
    uint32_t bytes = uint32_t(vertices.size()) * 4;
    if (stride && bytes % stride == 0) DrawUp(prim, bytes / stride, vertices.data(), stride);
    else LOG_FIRST(8, "d3d: EndPush primitive %u dropped, %u bytes of vertices of %u", prim, bytes, stride);
}

static void RunCommands(uint32_t begin, uint32_t end) {
    constexpr uint32_t kBeginEnd = 0x17FC, kInlineArray = 0x1818;
    // vertex array offsets and formats, then index elements and draw arrays up to the inline array
    constexpr uint32_t kArrayOffsets = 0x1720, kArrayFormatsEnd = 0x17A0, kArrayDraws = 0x1800;
    constexpr size_t kMaxInlineVertices = 4u << 20;  // 16 MB of pushed vertex dwords
    static std::vector<uint32_t> vertices;
    uint32_t prim = 0;
    if (!begin || end <= begin) return;
    for (uint32_t p = begin; uint64_t(p) + 4 <= end;) {
        // count in bits 18 to 28, bit 30 keeps the method for every value
        uint32_t head = R32(p), method = head & 0x1FFC, n = (head >> 18) & 0x7FF;
        p += 4;
        if ((head & 0xA0030003) || uint64_t(n) * 4 > uint64_t(end) - p) {  // anything but a method header
            LOG_FIRST(8, "d3d: pushed commands stop at %08X, %u bytes before the end", head, end - p);
            break;
        }
        for (uint32_t i = 0; i < n; i++, p += 4) {
            uint32_t m = head & 0x40000000 ? method : method + i * 4, v = R32(p);
            ApplyVisibilityMethod(m, v);
            if (m == kInlineArray) {
                if (prim) {
                    if (vertices.size() < kMaxInlineVertices) {
                        vertices.push_back(v);
                    } else if (vertices.size() == kMaxInlineVertices) {
                        LOG_FIRST(4, "d3d: pushed inline vertices over %zu dwords are dropped", kMaxInlineVertices);
                    }
                }
            } else if (m == kBeginEnd) {
                if (!v && prim) DrawInline(prim, vertices);
                prim = v;
                vertices.clear();
                ApplyImmediate(m, v);
            } else if ((m >= kArrayOffsets && m < kArrayFormatsEnd) || (m >= kArrayDraws && m < kInlineArray)) {
                LOG_FIRST(16, "d3d: pushed commands draw from vertex arrays (method %04X), which is not run", m);
            } else if (!ApplyConstant(m, v) && !ApplyImmediate(m, v) && m >> 2 < g_state.simple.size()) {
                SetSimple(m, v);
            }
        }
    }
}

HLE_STDCALL(D3DDevice_EndPush, 1) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    if (g_push_begin && g_scratch_push && g_scratch_size) {
        uint64_t end = a[0];
        const uint64_t limit = uint64_t(g_scratch_push) + g_scratch_size;
        if (end < g_push_begin) end = g_push_begin;
        if (end > limit) end = limit;
        RunCommands(g_push_begin, uint32_t(end));
    }
    g_push_begin = 0;
    ScratchPush();
    return a[0];
}

// (push buffer, fixup): its commands run as EndPush runs a title's, after the ones pushed before it. D3DPushBuffer is
// Common, Data, Lock, Size; the library runs Size less its last dword.
HLE_STDCALL(D3DDevice_RunPushBuffer, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t data = mem::VirtualFromPhysical(R32(a[0] + 4)), size = R32(a[0] + 12);
    const uint32_t alloc = mem::AllocationSize(data);
    if (size > alloc) size = alloc;
    LOG_FIRST(8, "d3d: RunPushBuffer %08X, %u bytes, first command %08X", data, size, size ? R32(data) : 0);
    if (a[1]) LOG_FIRST(1, "d3d: RunPushBuffer fixups are not applied");
    Walk(Dev(kDevPush));
    const uint64_t finish = uint64_t(data) + size;
    if (size >= 4 && finish <= 0x100000000ull) RunCommands(data, uint32_t(finish - 4));
    return 0;
}

// frame buffer compression tiles are the console's memory controller's business
HLE_STDCALL(D3DDevice_SetTile, 2) { return 0; }

// D3DTILE: flags, memory, size, pitch, z start tag, z offset
constexpr uint32_t kTileMemory = 4;
constexpr uint32_t kTileSize = 8;
constexpr uint32_t kTilePitch = 12;
constexpr uint32_t kTileBytes = 24;
// tiles the library's CreateDevice sets
constexpr uint32_t kBackBufferTile = 0;
constexpr uint32_t kDepthTile = 1;
// surface header fields
constexpr uint32_t kSurfaceData = 4;
constexpr uint32_t kSurfaceSize = 16;

// (index, tile): the frame buffer memory a title wraps in its own textures, as 007 does for a depth texture
HLE_STDCALL(D3DDevice_GetTile, 2) {
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    uint32_t tile = a[1];
    std::memset(HostPtr(tile), 0, kTileBytes);
    uint32_t surface = 0;
    if (a[0] == kBackBufferTile) surface = g_buffers[1];
    if (a[0] == kDepthTile) surface = g_depth;
    if (!surface) return 0;
    uint32_t size = R32(surface + kSurfaceSize);
    uint32_t pitch = ((size >> 24 & 0xFF) + 1) * 64;
    uint32_t height = (size >> 12 & 0xFFF) + 1;
    W32(tile + kTileMemory, mem::VirtualFromPhysical(R32(surface + kSurfaceData)));
    W32(tile + kTileSize, pitch * height);
    W32(tile + kTilePitch, pitch);
    return 0;
}

// the library's Begin pushes the start of the primitive, here it is opened
HLE_STDCALL(D3DDevice_Begin, 1) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    g_im_prim = a[0];
    std::memcpy(g_im_attr, g_state.vdef, sizeof(g_im_attr));
    g_im_mask = 0;
    g_im_vertices.clear();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) | 0x800);
    return 0;
}
// the library's End pushes the end of the primitive, here it is drawn
HLE_STDCALL(D3DDevice_End, 0) {
    XDK_REQUIRE("d3d", XDK_KEY(CDevice_Flags));
    std::lock_guard<std::recursive_mutex> lock(g_lock);
    Walk(Dev(kDevPush));
    EndPrimitive();
    SetDev(XDK_CDevice_Flags, Dev(XDK_CDevice_Flags) & ~0x1800u);
    return 0;
}
