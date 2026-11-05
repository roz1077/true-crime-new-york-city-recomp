"""Check the guest frame-chain walker against the real guest stack range.

Guest stacks are allocated around 0xD0000000, outside the user-image range, so the original
kUserHigh check rejected every normal frame (see the review finding). The fixture uses the actual
0xD0... range and also checks that unaligned, out-of-range, non-monotonic and non-code chains stop.
"""
from pathlib import Path
from native_test import run_native

root = Path(__file__).resolve().parents[1]
source = (root / 'sdk/runtime/cpu/dispatch.cpp').read_text()
start = source.index('void LogGuestFrames(')
frames = source[start:source.index('\n}\n', start) + 3]

prefix = r'''
#include <cstdarg>
#include <cstdio>
#include <cstring>
using uint32_t = unsigned int;
struct X86Context { uint32_t eax, ebx, ecx, edx, esp, ebp, esi, edi; };
struct Thread { uint32_t stack_top = 0, stack_size = 0; };
Thread g_thread;
Thread* Current() { return nullptr; } // F3 runs outside the inspected guest thread
struct Word { uint32_t address, value; };
Word g_words[32];
int g_word_count = 0;
void Set(uint32_t address, uint32_t value) {
    if (g_word_count < 32) g_words[g_word_count++] = {address, value};
}
uint32_t R32(uint32_t address) {
    for (int i = 0; i < g_word_count; ++i)
        if (g_words[i].address == address) return g_words[i].value;
    return 0;
}
namespace mem {
constexpr uint32_t kUserLow = 0x00010000;
constexpr uint32_t kUserHigh = 0x7FFE0000;
bool IsCommitted(uint32_t address) {
    return (address >= 0xD0000000u && address < 0xD0001000u) ||
           (address >= 0x00100000u && address < 0x00101000u);
}
}
namespace dispatch {
bool InCode(uint32_t address) { return address >= 0x00100000u && address < 0x00200000u; }
}
int g_logged = 0;
bool g_none = false;
void LogLine(const char* format, ...) {
    char text[256];
    va_list args;
    va_start(args, format);
    vsnprintf(text, sizeof(text), format, args);
    va_end(args);
    if (text[0] == ' ' && text[1] == ' ' && text[2] == 'f' && text[3] == 'r' && text[4] == 'a' &&
        text[5] == 'm' && text[6] == 'e' && text[7] == ' ' && text[8] >= '0' && text[8] <= '9')
        ++g_logged;
    if (strstr(text, "frame chain: none")) g_none = true;
}
#define LOGE(...) LogLine(__VA_ARGS__)
'''

check = r'''
void Reset() { g_word_count = 0; g_logged = 0; g_none = false; g_thread = Thread{}; }
bool Walk(uint32_t ebp, int expected_frames, bool expected_none) {
    X86Context ctx{};
    ctx.ebp = ebp;
    LogGuestFrames(ctx, &g_thread);
    if (g_logged != expected_frames || g_none != expected_none) {
        fprintf(stderr, "ebp %08X: logged %d (expected %d), none %d (expected %d)\n",
                ebp, g_logged, expected_frames, int(g_none), int(expected_none));
        return false;
    }
    return true;
}
extern "C" __declspec(dllexport) int run() {
    // Normal chain at 0xD0...: two frames, then the sentinel stops it.
    Reset();
    g_thread.stack_top = 0xD0001000u;
    g_thread.stack_size = 0x1000u;
    Set(0xD0000100u, 0xD0000200u); Set(0xD0000104u, 0x00123456u);
    Set(0xD0000200u, 0xD0000300u); Set(0xD0000204u, 0x00123457u);
    Set(0xD0000300u, 0x00000000u); Set(0xD0000304u, 0x00000000u);
    if (!Walk(0xD0000100u, 2, false)) return 1;

    // The old kUserHigh check rejected this address; it must now be accepted.
    Reset();
    g_thread.stack_top = 0xD0001000u;
    g_thread.stack_size = 0x1000u;
    Set(0xD0000400u, 0xD0000500u); Set(0xD0000404u, 0x00123459u);
    Set(0xD0000500u, 0xD0000500u); Set(0xD0000504u, 0x0012345Au);
    if (!Walk(0xD0000400u, 2, false)) return 2;

    // A non-monotonic link must stop the walk even though the return address is valid code.
    // The frame that was already validated is still reported, then the walk ends.
    Reset();
    g_thread.stack_top = 0xD0001000u;
    g_thread.stack_size = 0x1000u;
    Set(0xD0000600u, 0xD0000500u); Set(0xD0000604u, 0x0012345Bu);
    if (!Walk(0xD0000600u, 1, false)) return 3;

    // Below the allocated stack.
    Reset();
    g_thread.stack_top = 0xD0001000u;
    g_thread.stack_size = 0x1000u;
    if (!Walk(0xCFFFFFF0u, 0, true)) return 4;

    // At the top and too close to it for a complete frame read.
    if (!Walk(0xD0001000u, 0, true)) return 5;
    if (!Walk(0xD0000FFCu, 0, true)) return 6;

    // Unaligned frame pointer.
    if (!Walk(0xD0000101u, 0, true)) return 7;

    // Valid stack word but the return address is not recompiled code.
    Reset();
    g_thread.stack_top = 0xD0001000u;
    g_thread.stack_size = 0x1000u;
    Set(0xD0000700u, 0xD0000800u); Set(0xD0000704u, 0x00300000u);
    if (!Walk(0xD0000700u, 0, true)) return 8;

    // No current thread: fall back to the user-image range as before.
    Reset();
    g_thread.stack_top = 0;
    g_thread.stack_size = 0;
    Set(0x00100000u, 0x00100010u); Set(0x00100004u, 0x0012345Cu);
    Set(0x00100010u, 0x00000000u); Set(0x00100014u, 0x0012345Du);
    if (!Walk(0x00100000u, 2, false)) return 9;
    return 0;
}
'''

result = run_native(prefix + frames + check, prefix='tcnyc-guest-frames-', needs_crt=True)
assert result == 0, f'guest frame-chain regression {result}'
print('PASS: frame chain walks guest 0xD0... stacks and rejects invalid links')
