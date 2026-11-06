// Native Linux host: a software renderer that runs the title's D3D8 calls on the CPU
// (the SDK's reference renderer) and hands finished frames to the SDL window thread.
// This is the bring-up path: it needs no GPU API, so the whole host can be tested
// while the Vulkan backend is written.
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

#include "app/app.h"
#include "core/log.h"
#include "gpu/backend.h"
#include "reference_renderer.h"

namespace app {
// app_sdl.cpp: the window thread picks the newest finished frame up from here
void SubmitFrame(const uint8_t* rgba, int width, int height);
}

namespace gpu {

namespace {

class SoftwareBackend final : public Backend {
public:
    bool Init(uint32_t w, uint32_t h) override {
        width_ = w;
        height_ = h;
        reference::BeginFrame();
        LOGI("renderer: software %ux%u", w, h);
        return true;
    }

    void Clear(const State& s, uint32_t n, const uint8_t* rects, uint32_t flags, uint32_t color, float z,
               uint32_t stencil) override {
        reference::Clear(s, n, rects, flags, color, z, stencil);
    }

    void Draw(const State& s, uint32_t prim, uint32_t start, uint32_t count, const uint8_t* indices) override {
        draws_++;
        reference::Draw(s, prim, start, count, indices);
    }

    void Copy(const Surface& src, const Surface& dst, uint32_t n, uint32_t rects, uint32_t points) override {
        reference::Copy(src, dst, n, rects, points);
    }

    void Present(const Surface& front) override {
        uint32_t w = 0, h = 0;
        if (reference::ReadRgba(front, frame_, w, h)) {
            app::SubmitFrame(frame_.data(), int(w), int(h));
        }
        // TC_SW_CAP=N: save every Nth frame for a look at the software path's output
        static const long capture = [] {
            const char* e = std::getenv("TC_SW_CAP");
            return e ? std::atol(e) : 0;
        }();
        if (capture > 0 && frames_ % uint64_t(capture) == 0) {
            const std::string path =
                (app::ExeDir() / ("cap/sw_" + std::to_string(frames_) + ".tga")).string();
            std::error_code ec;
            std::filesystem::create_directories(app::ExeDir() / "cap", ec);
            reference::WriteTga(front, path.c_str());
        }
        reference::BeginFrame();
        if (++frames_ % 60 == 0) {
            LOGI("renderer: frame %llu, %llu draws", frames_, draws_);
        }
        draws_ = 0;
    }

    void Sync(uint32_t, uint32_t) override {}
    void SyncLate(uint32_t) override {}
    void Flush() override {}
    void SetGamma(const uint8_t*) override {}

private:
    uint32_t width_ = 0, height_ = 0;
    unsigned long long frames_ = 0, draws_ = 0;
    std::vector<uint8_t> frame_;
};

} // namespace

std::unique_ptr<Backend> CreateSoftwareBackend() { return std::make_unique<SoftwareBackend>(); }

} // namespace gpu
