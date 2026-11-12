#pragma once
#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>

#include "gpu/state.h"

namespace gpu {

// D3DCLEAR flags as the XDK defines them
enum : uint32_t {
    kClearTarget = 0xF0,  // ARGB channels
    kClearZ = 0x01,
    kClearStencil = 0x02,
};

class Backend {
public:
    virtual ~Backend() = default;
    // the title's back buffer size, the output window may differ
    virtual bool Init(uint32_t width, uint32_t height) = 0;
    // rects: count D3DRECTs (4 x int32), null for the whole viewport
    virtual void Clear(const State& s, uint32_t count, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
                       uint32_t stencil) = 0;
    // indices: the title's 16 bit indices, null for non indexed draws, then start is the first vertex
    virtual void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) = 0;
    virtual void Copy(const Surface& src, const Surface& dst, uint32_t count, uint32_t src_rects_va,
                      uint32_t dst_points_va) = 0;
    virtual void Present(const Surface& front) = 0;
    // the CPU is about to touch this resource's memory: write GPU results back first
    virtual void Sync(uint32_t data_va, uint32_t size) = 0;
    // a lock that does not wait for the GPU (NOOVERWRITE): a result a frame or two old is what the console gives too
    virtual void SyncLate(uint32_t data_va) = 0;
    // SyncLate in two for the render thread: DeliverLate (any thread) writes the newest finished copy to guest memory,
    // false when there is none recent; RecordLate (in order with the draws) starts a copy for a later lock
    virtual bool DeliverLate(uint32_t) { return false; }
    virtual void RecordLate(uint32_t) {}
    virtual void Flush() = 0;
    virtual void SetGamma(const uint8_t* ramp /*3 x 256*/) = 0;
    // visibility tests: draws count passing pixels while simple[kMCountEnable / 4] is set, ClearReport zeroes the
    // count, GetReport writes it to a 16 byte report once the GPU is past those draws; without them it stays pending
    virtual void ClearReport() {}
    virtual void GetReport(uint32_t /*report*/) {}

    // the XDK's fences and busy checks against the render thread; a backend that draws at once is never behind
    virtual uint32_t Fence() { return ++fence_; }
    virtual bool FencePending(uint32_t) { return false; }
    virtual void WaitFence(uint32_t) {}
    virtual bool Busy(uint32_t /*resource*/) { return false; }
    virtual void WaitResource(uint32_t /*resource*/) {}
    // until the GPU has drawn present number n, counted from 0; any thread
    virtual void WaitGpuFrame(uint64_t /*n*/) {}
    // a CPU-written front buffer: wait until the renderer has read it, so the
    // guest cannot decode the next movie frame into it mid-read
    virtual void WaitPresent() {}

private:
    uint32_t fence_ = 0;
};

std::unique_ptr<Backend> CreateBackend();
// the same backend driven from a thread of its own: calls return once recorded
std::unique_ptr<Backend> CreateThreadedBackend(std::unique_ptr<Backend> inner);

// perf diagnostic: the title's thread recording for the render thread, and waiting on it
inline std::atomic<uint64_t> g_perf_capture_ns{0}, g_perf_wait_ns{0}, g_perf_waits{0};
inline std::atomic<uint64_t> g_perf_wait_kind_ns[6];  // read-back, resource, fence, flush, copy, next frame
inline std::atomic<uint64_t> g_perf_sync_late{0}, g_perf_sync_block{0};  // read-backs the title waited for, by cause

// F3: save the next frame, every draw and target, under diag::CaptureDir
inline std::atomic<bool> g_capture{false};

// HDR output chosen for the swapchain: linear scRGB/FP16, PQ HDR10, or the SDR path.
enum class HdrOutput : uint32_t { Off, ScRgb, Hdr10 };
// The display the window is on reports HDR, and its peak luminance in nits (0 when unknown).
// Backends refresh these when they (re)create the swapchain; the Graphics menu reads them.
inline std::atomic<bool> g_hdr_available{false};
inline std::atomic<uint32_t> g_hdr_display_peak_nits{0};
// Set by the window thread on WM_DISPLAYCHANGE (an HDR toggle in Windows, a display added or
// removed); the renderer re-reads the display's HDR capability once and clears it.
inline std::atomic<bool> g_display_changed{false};
// The gameplay camera, for TAA's reprojection: the view matrix (D3D rows, world to view) and the projection's x and
// y scales, written by the game hooks (src/game/camera.cpp, night_shadows.cpp) on the title's thread. Each present
// snapshots it into g_camera_frames[present % 8], so the renderer reads the camera of the frame it draws.
struct CameraFrame {
    float view[16] = {};
    float xs = 0, ys = 0;  // clip x = view x * xs / view z, likewise y
    float near_z = 0, far_z = 0;  // the projection's planes in view units (far 0: infinite)
    uint64_t serial = 0;   // the game's frame it was written in, 0 before any
    uint32_t world_generation = 0;
};
inline std::mutex g_camera_mutex;
inline CameraFrame g_camera_now;            // under g_camera_mutex
inline CameraFrame g_camera_frames[8];      // under g_camera_mutex
inline void SnapshotCamera(uint64_t present) {
    std::lock_guard lock(g_camera_mutex);
    g_camera_frames[present % 8] = g_camera_now;
}
inline CameraFrame CameraOf(uint64_t present) {
    std::lock_guard lock(g_camera_mutex);
    return g_camera_frames[present % 8];
}


// DLSS availability, published by the D3D12 backend after its NGX probe. The menu reads it without
// calling into the renderer; Unknown means the probe has not run yet.
enum class DlssState : uint32_t { Unknown, Available, Unavailable, NotBuilt };
inline std::atomic<DlssState> g_dlss_state{DlssState::Unknown};

// AMD FSR 3.1 / 4.1 availability, published by the D3D12 backend when it loads FidelityFX and enumerates its
// providers. NoDll: no FidelityFX DLL beside the game. No4: the DLLs or driver offer only FSR 3.1.
enum class FsrState : uint32_t { Unknown, Available, No4, NoDll };
inline std::atomic<FsrState> g_fsr_state{FsrState::Unknown};

}  // namespace gpu
