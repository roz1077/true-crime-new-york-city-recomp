"""Check actual RunApcs/CallGuest code with callbacks that clobber CPU state.

Windows + LLVM required. Queue/locking stubs isolate the processor-state contract;
this is not a scheduler/concurrency test.
"""
from pathlib import Path
from native_test import run_native

root = Path(__file__).resolve().parents[1]


def function(path, signature):
    source = (root / path).read_text()
    start = source.index(signature)
    return source[start:source.index("\n}\n", start) + 3]


context = (root / "sdk/runtime/cpu/x86_context.h").read_text()
context = context[context.index("union X86Xmm"):context.index("using X86Func =")]
call = function("sdk/runtime/cpu/dispatch.cpp", "uint32_t CallGuest(")
pump = function("sdk/runtime/kernel/threads.cpp", "bool RunApcs(")
prefix = r'''
using uint8_t = unsigned char;
using uint16_t = unsigned short;
using uint32_t = unsigned int;
using int32_t = int;
using uint64_t = unsigned long long;
using size_t = unsigned long long;
extern "C" { int _fltused = 0; }
extern "C" void* memcpy(void* dst, const void* src, size_t n) {
    auto d = static_cast<volatile uint8_t*>(dst);
    auto s = static_cast<const volatile uint8_t*>(src);
    for (size_t i = 0; i < n; ++i) d[i] = s[i];
    return dst;
}
extern "C" void* memset(void* dst, int value, size_t n) {
    auto d = static_cast<volatile uint8_t*>(dst);
    for (size_t i = 0; i < n; ++i) d[i] = uint8_t(value);
    return dst;
}
namespace std {
struct mutex {};
template<class T> struct lock_guard { lock_guard(T&) {} };
template<class T, size_t N> struct array {
    T data[N];
    T& operator[](size_t i) { return data[i]; }
};
template<class T> class initializer_list {
    const T* data_; size_t count_;
public:
    const T* begin() const { return data_; }
    size_t size() const { return count_; }
};
}
'''
setup = r'''
using Apc = std::array<uint32_t, 4>;
struct Queue {
    Apc data[4];
    unsigned first = 0, last = 0;
    bool empty() const { return first == last; }
    Apc front() { return data[first]; }
    void pop_front() { ++first; }
    void push_back(Apc a) { data[last++] = a; }
};
struct Thread { X86Context* ctx; std::mutex apc_lock; Queue apcs; };
uint8_t memory[4096];
uint8_t* g_base = memory;
void W32(uint32_t a, uint32_t v) { memcpy(memory + a, &v, 4); }
uint32_t R32(uint32_t a) { uint32_t v; memcpy(&v, memory + a, 4); return v; }
Thread* active;
X86Context expected;
int calls = 0, callback_error = 0;
void x86_icall(X86Context& ctx, uint8_t*, uint32_t fn) {
    // Floating-point values are live on the interrupted thread, including between callbacks.
    if (ctx.xmm[3].q[1] != expected.xmm[3].q[1] || ctx.st[4] != expected.st[4] ||
        ctx.top != expected.top || ctx.fcw != expected.fcw || ctx.df != expected.df)
        callback_error = 1;
    if (R32(ctx.esp + 4) != fn + 100 || R32(ctx.esp + 8) != fn + 200 ||
        R32(ctx.esp + 12) != fn + 300) callback_error = 2;
    ++calls;
    W32(fn * 4, 0xC0FFEE00 + fn);  // callback memory effects must survive restoration
    auto bytes = reinterpret_cast<volatile uint8_t*>(&ctx);
    for (size_t i = 0; i < sizeof(ctx); ++i) bytes[i] = 0xA5;
    if (fn == 1) active->apcs.push_back({{2, 102, 202, 302}});
}
'''
overload = r'''
inline uint32_t CallGuest(X86Context& ctx, uint32_t fn, std::initializer_list<uint32_t> args) {
    return CallGuest(ctx, fn, args.begin(), args.size());
}
'''
checks = r'''
bool equal(const X86Context& a, const X86Context& b) {
    if (a.eax != b.eax || a.ebx != b.ebx || a.ecx != b.ecx || a.edx != b.edx || a.esp != b.esp ||
        a.ebp != b.ebp || a.esi != b.esi || a.edi != b.edi || a.cf != b.cf || a.zf != b.zf ||
        a.sf != b.sf || a.of != b.of || a.pf != b.pf || a.df != b.df || a.xflags != b.xflags ||
        a.fs_base != b.fs_base || a.top != b.top || a.fcw != b.fcw || a.fsw != b.fsw ||
        a.mxcsr != b.mxcsr || a.thread_handle != b.thread_handle || a.last_icall != b.last_icall || a.host != b.host)
        return false;
    for (int i = 0; i < 8; ++i) {
        if (a.st[i] != b.st[i] || a.mm[i] != b.mm[i] || a.xmm[i].q[0] != b.xmm[i].q[0] ||
            a.xmm[i].q[1] != b.xmm[i].q[1]) return false;
    }
    return true;
}
extern "C" __declspec(dllexport) int run() {
    X86Context ctx{};
    auto p = reinterpret_cast<uint8_t*>(&ctx);
    for (size_t i = 0; i < sizeof(ctx); ++i) p[i] = uint8_t(i * 13 + 7);
    ctx.esp = 3000;
    ctx.st[4] = 12.5;
    expected = ctx;
    Thread t{&ctx}; active = &t;
    if (RunApcs(t) || !equal(ctx, expected)) return 1;
    t.apcs.push_back({{1, 101, 201, 301}});
    if (!RunApcs(t)) return 2;
    if (!equal(ctx, expected)) return 3;
    if (callback_error || calls != 2 || !t.apcs.empty()) return 4;
    if (R32(4) != 0xC0FFEE01 || R32(8) != 0xC0FFEE02) return 5;
    if (RunApcs(t) || !equal(ctx, expected)) return 6;
    return 0;
}
'''
result = run_native(prefix + context + setup + call + overload + pump + checks, prefix="tcnyc-apc-")
assert result == 0, f"APC context regression {result} (3 = interrupted context corrupted)"
print("PASS: APCs preserve full CPU context; queued callbacks and guest memory effects survive")
