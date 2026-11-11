// Portable texture packs and dumps: reads PNG (stb_image) and DDS, writes PNG (stb_image_write)
// or DDS, and watches textures/replacement with a cheap directory signature poll. The key and
// the file names match the Windows texturedumper exactly, so the same pack works on both.
#include "texturedumper_linux.h"

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <cstdio>
#include <cstring>
#include <deque>
#include <filesystem>
#include <fstream>
#include <functional>
#include <iterator>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <unordered_set>

#define STB_IMAGE_IMPLEMENTATION
#include "stb/stb_image.h"
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "stb/stb_image_write.h"

#include "app/app.h"
#include "core/log.h"
#include "gpu/mip_filter.h"
#include "gpu/shaders.h"

namespace gpu::texturedumper {

namespace {

namespace fs = std::filesystem;

constexpr size_t kKeyDigits = 16;
constexpr uint32_t kMaxSide = 16384;  // the same limit the Windows path uses
constexpr uint32_t kDumpDds = 1;      // texture_dump_format
constexpr size_t kPaletteBytes = 256 * 4;
constexpr auto kSettle = std::chrono::milliseconds(500);  // files still being copied in
constexpr uint32_t kWatchRetry = 120;  // presents between directory checks

using Clock = std::chrono::steady_clock;

// the settings as Update latched them, the render thread's
struct Latched {
    bool replacing = false;
    bool dumping = false;
    uint32_t format = 0;
    bool operator==(const Latched&) const = default;
};

struct Pack {
    Latched set;
    uint32_t generation = 1;
    fs::path pack_dir;
    fs::path dump_dir;
    std::unordered_map<uint64_t, fs::path> files;  // by the key in their names; written by Update alone
    uint64_t dir_sig = 0;
    uint32_t watch_tries = 0;
    std::optional<Clock::time_point> changed;  // the last change seen, not listed yet
    std::once_flag worker;
    std::mutex lock;  // everything below
    uint32_t revision = 0;  // a load started before the pack was listed again is dropped
    std::condition_variable wake;
    std::deque<std::function<void()>> jobs;
    std::unordered_set<uint64_t> asked;
    std::unordered_map<uint64_t, std::optional<Image>> loaded;  // empty: the file did not load
    std::unordered_set<uint64_t> dumped;
};

// the detached worker waits on these for the process lifetime: never run static destructors under it
Pack& g = *new Pack();

// Worker

void Work() {
    for (;;) {
        std::function<void()> job;
        {
            std::unique_lock l(g.lock);
            g.wake.wait(l, [] { return !g.jobs.empty(); });
            job = std::move(g.jobs.front());
            g.jobs.pop_front();
        }
        job();
    }
}

void Post(std::function<void()> job) {
    std::call_once(g.worker, [] { std::thread(Work).detach(); });
    {
        std::lock_guard l(g.lock);
        g.jobs.push_back(std::move(job));
    }
    g.wake.notify_one();
}

// Formats

VkFormat UploadFormat(Host h) {
    switch (h) {
        case Host::BGRA8: return VK_FORMAT_B8G8R8A8_UNORM;
        case Host::BC1: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        case Host::BC2: return VK_FORMAT_BC2_UNORM_BLOCK;
        case Host::BC3: return VK_FORMAT_BC3_UNORM_BLOCK;
        default: return VK_FORMAT_UNDEFINED;
    }
}

bool Uncompressed(VkFormat f) {
    return f == VK_FORMAT_B8G8R8A8_UNORM || f == VK_FORMAT_R8G8B8A8_UNORM;
}

struct LevelShape {
    uint32_t pitch;
    uint32_t rows;
};

// pitch 0: a format packs cannot use
LevelShape Shape(VkFormat f, uint32_t w, uint32_t h) {
    uint32_t blocks_x = std::max(1u, (w + 3) / 4);
    uint32_t blocks_y = std::max(1u, (h + 3) / 4);
    switch (f) {
        case VK_FORMAT_BC1_RGBA_UNORM_BLOCK: return {blocks_x * 8, blocks_y};
        case VK_FORMAT_BC2_UNORM_BLOCK:
        case VK_FORMAT_BC3_UNORM_BLOCK:
        case VK_FORMAT_BC7_UNORM_BLOCK: return {blocks_x * 16, blocks_y};
        case VK_FORMAT_B8G8R8A8_UNORM:
        case VK_FORMAT_R8G8B8A8_UNORM: return {w * 4, h};
        default: return {0, 0};
    }
}

uint32_t FullChain(uint32_t w, uint32_t h) {
    uint32_t n = 1;
    while ((std::max(w, h) >> n) > 0) n++;
    return n;
}

// Names

std::string LowerExtension(const fs::path& p) {
    std::string ext = p.extension().string();
    for (char& c : ext) c = char(std::tolower(uint8_t(c)));
    return ext;
}

// the 16 hex digit part of a name such as 256x256_0123456789abcdef_DXT1, 0 without one
uint64_t KeyInName(const std::string& stem) {
    size_t start = 0;
    while (start < stem.size()) {
        size_t end = std::min(stem.find('_', start), stem.size());
        std::string part = stem.substr(start, end - start);
        if (part.size() == kKeyDigits && part.find_first_not_of("0123456789abcdefABCDEF") == std::string::npos)
            return std::strtoull(part.c_str(), nullptr, 16);
        start = end + 1;
    }
    return 0;
}

std::string DumpName(uint64_t key, const TextureDesc& d, const char* extension) {
    char name[96];
    std::snprintf(name, sizeof(name), "%ux%u_%016llx_%s.%s", d.width, d.height,
                  static_cast<unsigned long long>(key), Format(d.format).name, extension);
    return name;
}

void ListPack(const fs::path& dir) {
    g.files.clear();
    std::error_code ec;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        const fs::path& p = it->path();
        const std::string ext = LowerExtension(p);
        if (ext != ".dds" && ext != ".png") continue;
        uint64_t key = KeyInName(p.stem().string());
        if (key) g.files.emplace(key, p);
    }
    LOGR("texture pack: %zu images in %s", g.files.size(), dir.string().c_str());
}

// DDS files

constexpr uint32_t FourCC(const char (&s)[5]) {
    return uint32_t(uint8_t(s[0])) | uint32_t(uint8_t(s[1])) << 8 | uint32_t(uint8_t(s[2])) << 16 |
           uint32_t(uint8_t(s[3])) << 24;
}

// "DDS " then DDS_HEADER, byte offsets from the file start; DDS_HEADER_DXT10 follows when the FourCC is DX10
constexpr uint32_t kDdsMagic = FourCC("DDS ");
constexpr size_t kDdsSize = 4;
constexpr size_t kDdsFlags = 8;
constexpr size_t kDdsHeight = 12;
constexpr size_t kDdsWidth = 16;
constexpr size_t kDdsPitch = 20;
constexpr size_t kDdsMipCount = 28;
constexpr size_t kDdsPixelSize = 76;
constexpr size_t kDdsPixelFlags = 80;
constexpr size_t kDdsFourCC = 84;
constexpr size_t kDdsBitCount = 88;
constexpr size_t kDdsRedMask = 92;
constexpr size_t kDdsGreenMask = 96;
constexpr size_t kDdsBlueMask = 100;
constexpr size_t kDdsAlphaMask = 104;
constexpr size_t kDdsCaps = 108;
constexpr size_t kDdsHeaderBytes = 128;
constexpr size_t kDx10Format = 128;
constexpr size_t kDx10HeaderBytes = 148;

// DDS_HEADER field values
constexpr uint32_t kDdsHeaderSize = 124;
constexpr uint32_t kDdsPixelFormatSize = 32;
constexpr uint32_t kDdsdRequired = 0x1007;  // caps, height, width, pixel format
constexpr uint32_t kDdsdPitch = 0x8;
constexpr uint32_t kDdsdMipCount = 0x20000;
constexpr uint32_t kDdsdLinearSize = 0x80000;
constexpr uint32_t kDdpfAlphaPixels = 0x1;
constexpr uint32_t kDdpfFourCC = 0x4;
constexpr uint32_t kDdpfRgb = 0x40;
constexpr uint32_t kCapsComplex = 0x8;
constexpr uint32_t kCapsTexture = 0x1000;
constexpr uint32_t kCapsMipmap = 0x400000;
constexpr uint32_t kRedOfBgra = 0x00FF0000;
constexpr uint32_t kGreenMask = 0x0000FF00;
constexpr uint32_t kBlueOfBgra = 0x000000FF;
constexpr uint32_t kRedOfRgba = 0x000000FF;
constexpr uint32_t kAlphaMask = 0xFF000000;

uint32_t Read32(const std::vector<uint8_t>& b, size_t at) {
    uint32_t v;
    std::memcpy(&v, &b[at], 4);
    return v;
}

void Write32(std::vector<uint8_t>& b, size_t at, uint32_t v) { std::memcpy(&b[at], &v, 4); }

// DXGI_FORMAT in a DDS DX10 header to the Vulkan format the renderer uploads
VkFormat DxgiFormat(uint32_t dxgi) {
    switch (dxgi) {
        case 71: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;  // BC1_UNORM
        case 72: return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;  // BC1_UNORM_SRGB
        case 74: return VK_FORMAT_BC2_UNORM_BLOCK;
        case 75: return VK_FORMAT_BC2_UNORM_BLOCK;
        case 77: return VK_FORMAT_BC3_UNORM_BLOCK;
        case 78: return VK_FORMAT_BC3_UNORM_BLOCK;
        case 87: return VK_FORMAT_B8G8R8A8_UNORM;  // B8G8R8A8_UNORM
        case 88: return VK_FORMAT_B8G8R8A8_UNORM;  // B8G8R8X8_UNORM
        case 91: return VK_FORMAT_B8G8R8A8_UNORM;  // B8G8R8A8_SRGB
        case 28: return VK_FORMAT_R8G8B8A8_UNORM;
        case 29: return VK_FORMAT_R8G8B8A8_UNORM;
        case 98: return VK_FORMAT_BC7_UNORM_BLOCK;
        case 99: return VK_FORMAT_BC7_UNORM_BLOCK;
        default: return VK_FORMAT_UNDEFINED;
    }
}

// data: where level 0 starts
VkFormat DdsFormat(const std::vector<uint8_t>& f, size_t& data) {
    data = kDdsHeaderBytes;
    uint32_t flags = Read32(f, kDdsPixelFlags);
    uint32_t cc = Read32(f, kDdsFourCC);
    if (flags & kDdpfFourCC) {
        if (cc == FourCC("DXT1")) return VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
        if (cc == FourCC("DXT3")) return VK_FORMAT_BC2_UNORM_BLOCK;
        if (cc == FourCC("DXT5")) return VK_FORMAT_BC3_UNORM_BLOCK;
        if (cc != FourCC("DX10") || f.size() < kDx10HeaderBytes) return VK_FORMAT_UNDEFINED;
        data = kDx10HeaderBytes;
        return DxgiFormat(Read32(f, kDx10Format));
    }
    if (!(flags & kDdpfRgb) || Read32(f, kDdsBitCount) != 32) return VK_FORMAT_UNDEFINED;
    uint32_t red = Read32(f, kDdsRedMask);
    if (red == kRedOfBgra) return VK_FORMAT_B8G8R8A8_UNORM;
    return red == kRedOfRgba ? VK_FORMAT_R8G8B8A8_UNORM : VK_FORMAT_UNDEFINED;
}

// X8R8G8B8 and the like: alpha bytes left undefined
void MakeOpaque(Image& img) {
    for (Level& l : img.levels) {
        for (size_t i = 3; i < l.px.size(); i += 4) l.px[i] = 0xFF;
    }
}

bool ReadDds(const fs::path& path, Image& img) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> f{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (f.size() < kDdsHeaderBytes || Read32(f, 0) != kDdsMagic) return false;
    size_t at = 0;
    img.format = DdsFormat(f, at);
    img.width = Read32(f, kDdsWidth);
    img.height = Read32(f, kDdsHeight);
    if (img.format == VK_FORMAT_UNDEFINED || !img.width || !img.height) return false;
    if (img.width > kMaxSide || img.height > kMaxSide) return false;
    uint32_t mips = Read32(f, kDdsFlags) & kDdsdMipCount ? Read32(f, kDdsMipCount) : 1;
    uint32_t levels = std::clamp(mips, 1u, FullChain(img.width, img.height));
    // uncompressed DDS rows may carry padding; the header pitch is the top level's real stride
    const uint32_t header_pitch = Read32(f, kDdsPitch);
    for (uint32_t l = 0; l < levels; l++) {
        LevelShape s = Shape(img.format, std::max(img.width >> l, 1u), std::max(img.height >> l, 1u));
        uint32_t pitch = s.pitch;
        if (l == 0 && Uncompressed(img.format) && header_pitch >= s.pitch) pitch = header_pitch;
        size_t bytes = size_t(pitch) * s.rows;
        if (at + bytes > f.size()) break;  // short file: the levels it holds
        img.levels.push_back({std::vector<uint8_t>(f.begin() + at, f.begin() + at + bytes), pitch});
        at += bytes;
    }
    uint32_t flags = Read32(f, kDdsPixelFlags);
    if (!(flags & kDdpfFourCC) && !(flags & kDdpfAlphaPixels)) MakeOpaque(img);
    return !img.levels.empty();
}

std::vector<uint8_t> DdsHeader(const Image& img) {
    std::vector<uint8_t> h(kDdsHeaderBytes);
    bool bc = !Uncompressed(img.format);
    uint32_t levels = uint32_t(img.levels.size());
    const Level& top = img.levels[0];
    Write32(h, 0, kDdsMagic);
    Write32(h, kDdsSize, kDdsHeaderSize);
    Write32(h, kDdsFlags, kDdsdRequired | kDdsdMipCount | (bc ? kDdsdLinearSize : kDdsdPitch));
    Write32(h, kDdsHeight, img.height);
    Write32(h, kDdsWidth, img.width);
    Write32(h, kDdsPitch, bc ? uint32_t(top.px.size()) : top.pitch);
    Write32(h, kDdsMipCount, levels);
    Write32(h, kDdsPixelSize, kDdsPixelFormatSize);
    Write32(h, kDdsCaps, kCapsTexture | (levels > 1 ? kCapsComplex | kCapsMipmap : 0));
    if (bc) {
        uint32_t cc = FourCC("DXT1");
        if (img.format == VK_FORMAT_BC2_UNORM_BLOCK) cc = FourCC("DXT3");
        if (img.format == VK_FORMAT_BC3_UNORM_BLOCK) cc = FourCC("DXT5");
        Write32(h, kDdsPixelFlags, kDdpfFourCC);
        Write32(h, kDdsFourCC, cc);
        return h;
    }
    Write32(h, kDdsPixelFlags, kDdpfRgb | kDdpfAlphaPixels);
    Write32(h, kDdsBitCount, 32);
    Write32(h, kDdsRedMask, kRedOfBgra);
    Write32(h, kDdsGreenMask, kGreenMask);
    Write32(h, kDdsBlueMask, kBlueOfBgra);
    Write32(h, kDdsAlphaMask, kAlphaMask);
    return h;
}

void WriteDds(const fs::path& path, const Image& img) {
    std::error_code ec;
    if (fs::exists(path, ec)) return;
    std::ofstream out(path, std::ios::binary);
    std::vector<uint8_t> header = DdsHeader(img);
    out.write(reinterpret_cast<const char*>(header.data()), std::streamsize(header.size()));
    for (const Level& l : img.levels)
        out.write(reinterpret_cast<const char*>(l.px.data()), std::streamsize(l.px.size()));
    if (!out) LOGE("texture dump: %s not written", path.string().c_str());
}

// PNG files (stb_image / stb_image_write)

void SwapRedBlue(std::vector<uint8_t>& px) {
    for (size_t i = 0; i + 3 < px.size(); i += 4) std::swap(px[i], px[i + 2]);
}

// DXT blocks to RGBA, for PNG dumps of the BC formats (the renderer uploads BC1/2/3 only)
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
        a[3] = 0;  // the transparent BC1 texel
    }
    for (int i = 0; i < 16; i++) {
        const int index = (src[4 + i / 4] >> ((i % 4) * 2)) & 3;
        px[i][0] = r[index];
        px[i][1] = g[index];
        px[i][2] = b[index];
        px[i][3] = a[index];
    }
}

void DecodeBcLevel(const Image& img, std::vector<uint8_t>& rgba) {
    if (img.format != VK_FORMAT_BC1_RGBA_UNORM_BLOCK && img.format != VK_FORMAT_BC2_UNORM_BLOCK &&
        img.format != VK_FORMAT_BC3_UNORM_BLOCK)
        return;
    rgba.assign(size_t(img.width) * img.height * 4, 0);
    const Level& top = img.levels[0];
    const uint32_t blocks_x = (img.width + 3) / 4, blocks_y = (img.height + 3) / 4;
    const bool bc1 = img.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK;
    const bool bc2 = img.format == VK_FORMAT_BC2_UNORM_BLOCK;
    const uint32_t stride = bc1 ? 8 : 16;
    for (uint32_t by = 0; by < blocks_y; by++)
        for (uint32_t bx = 0; bx < blocks_x; bx++) {
            const uint8_t* block = &top.px[(size_t(by) * blocks_x + bx) * stride];
            uint8_t px[16][4];
            if (bc1) {
                DecodeColourBlock(block, true, px);
            } else {
                uint8_t alpha[16];
                if (bc2) {
                    for (int i = 0; i < 16; i++)
                        alpha[i] = uint8_t(((block[i / 2] >> ((i & 1) * 4)) & 0xF) * 255 / 15);
                } else {  // BC3
                    const int a0 = block[0], a1 = block[1];
                    uint8_t pal[8] = {uint8_t(a0), uint8_t(a1)};
                    if (a0 > a1) {
                        for (int i = 0; i < 6; i++)
                            pal[2 + i] = uint8_t(((6 - i) * a0 + (1 + i) * a1) / 7);
                    } else {
                        for (int i = 0; i < 4; i++)
                            pal[2 + i] = uint8_t(((4 - i) * a0 + (1 + i) * a1) / 5);
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
                if (x >= img.width || y >= img.height) continue;
                std::memcpy(&rgba[(size_t(y) * img.width + x) * 4], px[i], 4);
            }
        }
}

bool ReadPng(const fs::path& path, Image& img) {
    std::ifstream in(path, std::ios::binary);
    std::vector<uint8_t> f{std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>()};
    if (f.empty()) return false;
    int w = 0, h = 0, channels = 0;
    stbi_uc* px = stbi_load_from_memory(f.data(), int(f.size()), &w, &h, &channels, 4);
    if (!px || w <= 0 || h <= 0 || w > int(kMaxSide) || h > int(kMaxSide)) {
        stbi_image_free(px);
        return false;
    }
    Level top{std::vector<uint8_t>(px, px + size_t(w) * h * 4), uint32_t(w) * 4};
    stbi_image_free(px);
    SwapRedBlue(top.px);  // the renderer's BGRA upload order, as the Windows WIC path gives
    img.format = VK_FORMAT_B8G8R8A8_UNORM;
    img.width = uint32_t(w);
    img.height = uint32_t(h);
    img.levels.push_back(std::move(top));
    return true;
}

void WritePng(const fs::path& path, const Image& img) {
    std::error_code ec;
    if (fs::exists(path, ec)) return;
    std::vector<uint8_t> rgba;
    if (Uncompressed(img.format)) {
        rgba = img.levels[0].px;
        SwapRedBlue(rgba);
    } else if (img.format == VK_FORMAT_BC1_RGBA_UNORM_BLOCK || img.format == VK_FORMAT_BC2_UNORM_BLOCK ||
               img.format == VK_FORMAT_BC3_UNORM_BLOCK) {
        DecodeBcLevel(img, rgba);
    } else {
        LOGE("texture dump: %s not written (no PNG decoder for its BC format)", path.string().c_str());
        return;
    }
    if (!stbi_write_png(path.string().c_str(), int(img.width), int(img.height), 4, rgba.data(),
                        int(img.width) * 4))
        LOGE("texture dump: %s not written", path.string().c_str());
}

// Mipmaps

// a level with only level 0 gets a chain (mip_filter.h), so a bigger image does not shimmer at a distance
void AddMipmaps(Image& img) {
    if (img.levels.size() > 1) return;
    if (!Uncompressed(img.format)) {
        // a single BC level has no chain: decode once so the box filter below can build one
        std::vector<uint8_t> rgba;
        DecodeBcLevel(img, rgba);
        if (rgba.empty()) return;
        img.levels[0].px = std::move(rgba);
        img.levels[0].pitch = img.width * 4;
        img.format = VK_FORMAT_B8G8R8A8_UNORM;
    }
    mips::BuildChain(img.levels, img.width, img.height);
}

// Loading

void Load(uint64_t key, const fs::path& path, uint32_t revision) {
    Image img;
    const bool dds = LowerExtension(path) == ".dds";
    bool ok = dds ? ReadDds(path, img) : ReadPng(path, img);
    // a BC texture's top level is whole blocks
    if (ok && !Uncompressed(img.format) && ((img.width | img.height) & 3)) ok = false;
    if (ok) {
        AddMipmaps(img);
    } else {
        LOGE("texture pack: %s did not load", path.string().c_str());
    }
    std::lock_guard l(g.lock);
    if (revision != g.revision) return;
    if (ok) {
        g.loaded[key] = std::move(img);
    } else {
        g.loaded[key] = std::nullopt;
    }
}

Image Decode(const TextureDesc& d, const uint8_t* guest, const uint32_t* palette) {
    Image img;
    img.format = UploadFormat(Format(d.format).host);
    img.width = d.width;
    img.height = d.height;
    img.levels.resize(d.levels);
    for (uint32_t l = 0; l < d.levels; l++) {
        Level& level = img.levels[l];
        DecodeLevel(d, l, 0, guest, palette, level.px, level.pitch);
    }
    return img;
}

// the settings into g.set; false when none changed
bool LatchSettings() {
    const app::Settings s = app::Snapshot();
    Latched now{s.custom_textures, s.dump_textures, s.texture_dump_format};
    if (now == g.set) return false;
    const bool was_replacing = g.set.replacing;
    g.set = now;
    fs::path root = app::ExeDir() / "textures";
    g.pack_dir = root / "replacement";
    if (now.dumping) {
        g.dump_dir = root / "dumps";
        std::error_code ec;
        fs::create_directories(g.dump_dir, ec);
        fs::create_directories(g.pack_dir, ec);  // ready for the edited images
        LOGR("texture dump: %s into %s", now.format == kDumpDds ? "DDS" : "PNG",
             g.dump_dir.string().c_str());
    }
    // a texture dumped in the other format is dumped again
    std::lock_guard l(g.lock);
    g.dumped.clear();
    if (!now.replacing) {
        // files removed while replacement was off must not stay selectable
        g.files.clear();
        g.loaded.clear();
        g.asked.clear();
        g.revision++;
    }
    // list the pack at once when replacement is turned on
    g.changed = Clock::time_point{};
    g.dir_sig = 0;
    g.watch_tries = 0;
    (void)was_replacing;
    return true;
}

// count, total size and newest write time: enough to notice a pack being added to or edited
uint64_t DirSignature(const fs::path& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return 0;
    uint64_t count = 0, bytes = 0, newest = 0;
    fs::recursive_directory_iterator it(dir, fs::directory_options::skip_permission_denied, ec);
    for (; !ec && it != fs::recursive_directory_iterator(); it.increment(ec)) {
        if (!it->is_regular_file(ec)) continue;
        count++;
        bytes += it->file_size(ec);
        const auto t = it->last_write_time(ec).time_since_epoch().count();
        newest = std::max(newest, uint64_t(t));
    }
    return count * 1000003ull ^ (bytes << 1) ^ newest;
}

// lists the pack once its files changed and settled; true when listed
bool UpdatePackFiles() {
    const Clock::time_point now = Clock::now();
    if (g.watch_tries++ % kWatchRetry == 0) {
        const uint64_t sig = DirSignature(g.pack_dir);
        if (sig != g.dir_sig) {
            g.dir_sig = sig;
            g.changed = now;
        }
    }
    if (!g.changed || now - *g.changed < kSettle) return false;
    g.changed.reset();
    ListPack(g.pack_dir);
    std::lock_guard l(g.lock);
    g.revision++;
    g.asked.clear();
    g.loaded.clear();
    return true;
}

}  // namespace

bool Update() {
    const bool changed = LatchSettings();
    const bool reloaded = g.set.replacing && UpdatePackFiles();
    if (changed || reloaded) g.generation++;
    return reloaded;
}

uint32_t Generation() { return g.generation; }

bool Replacing() { return g.set.replacing && !g.files.empty(); }

uint64_t Key(const TextureDesc& d, const FormatInfo& fi, const uint8_t* guest, const uint32_t* palette) {
    if (!Replacing() && !g.set.dumping) return 0;
    // cube maps, volumes and depth stay the console's; linear ones are movie frames and the game's own copies
    if (d.cube || d.depth > 1 || d.linear || UploadFormat(fi.host) == VK_FORMAT_UNDEFINED) return 0;
    uint32_t shape[] = {d.format, d.width, d.height};
    uint64_t key = Hash(guest, LevelSize(d, 0), Hash(shape, sizeof(shape)));
    if (palette) key = Hash(palette, kPaletteBytes, key);
    return key ? key : 1;
}

void Dump(uint64_t key, const TextureDesc& d, const uint8_t* guest, const uint32_t* palette) {
    if (!key || !g.set.dumping || g.files.count(key)) return;
    {
        std::lock_guard l(g.lock);
        if (!g.dumped.insert(key).second) return;
    }
    const bool dds = g.set.format == kDumpDds;
    fs::path path = g.dump_dir / DumpName(key, d, dds ? "dds" : "png");
    Post([path, dds, img = Decode(d, guest, palette)] {
        if (dds) {
            WriteDds(path, img);
        } else {
            WritePng(path, img);
        }
    });
}

Lookup Take(uint64_t key, Image& out) {
    auto file = g.files.find(key);
    if (!g.set.replacing || file == g.files.end()) return Lookup::None;
    uint32_t revision = 0;
    {
        std::lock_guard l(g.lock);
        auto it = g.loaded.find(key);
        if (it != g.loaded.end()) {
            if (!it->second) return Lookup::None;
            out = std::move(*it->second);
            g.loaded.erase(it);
            return Lookup::Ready;
        }
        if (!g.asked.insert(key).second) return Lookup::Loading;
        revision = g.revision;
    }
    Post([key, path = file->second, revision] { Load(key, path, revision); });
    return Lookup::Loading;
}

}  // namespace gpu::texturedumper
