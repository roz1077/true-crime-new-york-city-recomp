"""Exercise the SDK's actual ApplyImmediate implementation without starting D3D12.

Windows + LLVM required. Compiles an isolated DLL in a temporary directory.
"""
from pathlib import Path
from native_test import run_native


root = Path(__file__).resolve().parents[1]
source = (root / "sdk/runtime/xdk/d3d8.cpp").read_text()
start = source.index("bool ApplyImmediate(")
end = source.index("\n// Vertex constants", start)
implementation = source[start:end]
vertex_source = (root / "sdk/runtime/gpu/vertex.cpp").read_text()
convert = vertex_source[vertex_source.index("void Convert("):vertex_source.index("// A/B switch")]
retain = vertex_source[vertex_source.index("void RetainVertexAttributes("):vertex_source.index("bool Assemble(const")]
harness = r'''
using uint32_t = unsigned int;
extern "C" { int _fltused = 0; }
extern "C" void* memcpy(void* dst, const void* src, unsigned long long n) {
    auto d = static_cast<volatile unsigned char*>(dst);
    auto s = static_cast<const volatile unsigned char*>(src);
    for (unsigned long long i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}
namespace std { using ::memcpy; }
extern "C" void* memset(void* dst, int v, unsigned long long n) {
    auto d = static_cast<volatile unsigned char*>(dst);
    for (unsigned long long i = 0; i < n; ++i) d[i] = static_cast<unsigned char>(v);
    return dst;
}
struct Vertices {
    void clear() {}
    int end() { return 0; }
    void insert(int, float*, float*) {}
} g_im_vertices;
struct { float vdef[16][4] = {}; } g_state;
uint32_t g_im_prim = 0, g_im_mask = 0;
float g_im_attr[64] = {};
void EndPrimitive() { g_im_prim = 0; }
'''
checks = r'''
unsigned bits(float f) { unsigned u; memcpy(&u, &f, 4); return u; }
extern "C" __declspec(dllexport) int run() {
    // An outside tint, followed by a white colour inside an immediate draw.
    ApplyImmediate(0x194C, 0x40302010);
    ApplyImmediate(0x17FC, 5);
    ApplyImmediate(0x194C, 0xFFFFFFFF);
    ApplyImmediate(0x17FC, 0);
    for (int i = 0; i < 4; ++i)
        if (g_state.vdef[3][i] != 1.0f) return 1;
    // A partial four-float setter must update x without waiting for w.
    ApplyImmediate(0x1A30, bits(0.25f));
    if (g_state.vdef[3][0] != 0.25f || g_state.vdef[3][3] != 1.0f) return 2;
    g_state.vdef[3][3] = 0.5f; // a streamed draw changed the current alpha
    ApplyImmediate(0x1A30, bits(0.75f));
    if (g_state.vdef[3][0] != 0.75f || g_state.vdef[3][3] != 0.5f) return 8;
    // Two-float inputs retain their implicit z/w, inside and after Begin/End.
    ApplyImmediate(0x17FC, 5);
    ApplyImmediate(0x18C8, bits(0.5f));
    ApplyImmediate(0x18CC, bits(0.75f));
    ApplyImmediate(0x17FC, 0);
    if (g_state.vdef[9][0] != 0.5f || g_state.vdef[9][1] != 0.75f ||
        g_state.vdef[9][2] != 0 || g_state.vdef[9][3] != 1) return 3;
    return run_streamed();
}
'''
streamed = r'''
using size_t = unsigned long long;
using uint8_t = unsigned char;
using uint16_t = unsigned short;
using int16_t = short;
using int32_t = int;
namespace std {
template<class T> T min(T a, T b) { return a < b ? a : b; }
template<class T> T max(T a, T b) { return a > b ? a : b; }
}
namespace carry {
constexpr unsigned kTypeFloat2H = 0x72, kTypeNone = 2, kResData = 4;
unsigned char memory[256] = {};
namespace mem { unsigned VirtualFromPhysical(unsigned v) { return v; } }
namespace kernel {
unsigned R32(unsigned) { return 32; }
void* HostPtr(unsigned v) { return memory + v; }
}
struct VertexShader {
    struct Input { int stream = -1; unsigned type = 0, offset = 0; } inputs[16];
} layout;
struct State {
    struct Stream { unsigned vb = 0, stride = 0; } streams[16];
    unsigned base_vertex = 0;
    float vdef[16][4] = {};
};
const VertexShader* PickLayout(const State&, VertexShader&) { return &layout; }
unsigned StrideOf(const State& s, const VertexShader&, const VertexShader::Input& in) {
    return s.streams[in.stream].stride;
}
'''+convert+retain+r'''
int check() {
    State s;
    layout.inputs[3] = {0, 0x40, 0};
    s.streams[0] = {16, 4};
    for (int i = 0; i < 4; ++i) memory[32 + 8 + i] = 255;
    RetainVertexAttributes(s, 0, 3, nullptr);
    if (s.vdef[3][0] != 1 || s.vdef[3][3] != 1) return 4;
    uint16_t indices[2] = {2, 0};
    RetainVertexAttributes(s, 0, 2, reinterpret_cast<uint8_t*>(indices));
    if (s.vdef[3][0] != 0) return 5;
    s.base_vertex = 2;
    RetainVertexAttributes(s, 0, 2, reinterpret_cast<uint8_t*>(indices));
    if (s.vdef[3][0] != 1) return 6;
    RetainVertexAttributes(s, 0, 0, nullptr);
    if (s.vdef[3][0] != 1) return 7;
    return 0;
}
}
int run_streamed() { return carry::check(); }
'''
result = run_native(harness + implementation + streamed + checks, prefix="tcnyc-attributes-")
assert result == 0, f"attribute regression {result}"
print("PASS: immediate and streamed carryover, partial writes, defaults, indices, base vertex, empty draw")
