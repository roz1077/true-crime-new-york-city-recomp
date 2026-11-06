"""Regress mouse yaw ordering, stalled-camera counts, and pad/mouse handoff.

Compile the actual hooks and input routines; guest update doubles model the
confirmed yaw overwrite followed by the shared orientation builder. No game or
save files are opened.
"""
from pathlib import Path
import sys
from native_test import run_native

root = Path(__file__).resolve().parents[1]
old = '--baseline' in sys.argv
if old:
    import subprocess
    def read(path):
        return subprocess.check_output(['git', 'show', 'cb2adb23cf01d1fa107bf293402bc882f856e46a:' + path], cwd=root, text=True)
else:
    def read(path):
        return (root / path).read_text()

hooks = read('src/game/mouse_aim.cpp')
hooks = '\n'.join(line for line in hooks.splitlines() if not line.startswith('#include "'))
source = r'''
#include <cstdint>
#include <cstring>
#include <cmath>
struct X86Context { uint32_t ecx = 0; };
#define X86_FUNC(name) void name(X86Context& ctx, uint8_t* base)
#define X86_EXTERN_FUNC(name) X86_FUNC(name)
#define X86_MEM32(a) (*reinterpret_cast<uint32_t*>(base + (a)))
#define X86_MEM8(a) base[a]
namespace mem { bool IsCommitted(uint32_t a) { return a && a < 0x500000; } }
namespace input {
bool g_direct_mouse_aim = true, recent = false;
uint32_t g_mouse_sensitivity_percent = 100, g_mouse_vertical_sensitivity_percent = 100;
float pending_x = 0, pending_y = 0;
void UseDirectMouseAim() {}
bool DirectAimRecent() { return recent; }
bool ConsumeMouseAim(float& x, float& y) {
    x = pending_x; y = pending_y; pending_x = pending_y = 0;
    if (x || y) { recent = true; return true; }
    return false;
}
}
''' + hooks + r'''
constexpr uint32_t camera = 0x11000, controller = 0x12000;
float built_yaw = 0, replacement = 0;
bool no_target = false, inline_view = false;
X86_FUNC(__imp__sub_0001FD40) { built_yaw = RdF(base, kYaw); }
X86_FUNC(__imp__sub_0002E200) {
    if (no_target) return;
    if (!inline_view) std::memcpy(base + kYaw, &replacement, 4);
    if (inline_view) built_yaw = RdF(base, kYaw);
    else { ctx.ecx = controller; sub_0001FD40(ctx, base); }
}
void SetFloat(uint8_t* base, uint32_t a, float v) { std::memcpy(base+a, &v, 4); }
bool Close(float a, float b) { return std::fabs(a-b) < 0.00001f; }
void Update(uint8_t* base, uint8_t mode, float dx) {
    base[camera + kMode] = mode;
    input::pending_x = dx;
    X86Context ctx{camera};
    sub_0002E200(ctx, base);
}
extern "C" __declspec(dllexport) int run() {
    static uint8_t storage[0x500000]{};
    uint8_t* base = storage;
    X86_MEM32(kGame) = 0x10000;
    X86_MEM32(0x10000+kCamera) = camera;
    X86_MEM32(camera+kActive) = controller;
    X86_MEM32(camera+kTarget) = 0x13000;
    X86_MEM32(controller+kOwner) = camera;
    base[camera+kActiveFlag] = 1;
    SetFloat(base, controller+0x20, -0.7f);
    SetFloat(base, controller+0x24, 1.0f);
    // Transition controller overwrites the pre-step yaw. The view built in
    // THIS update must still receive even one mouse count.
    SetFloat(base, kYaw, 1.0f); replacement = -0.5f;
    Update(base, 23, 1);
    if (!Close(built_yaw, 1.004f)) return 1;
    // No repeated offset accumulation while the mouse is idle.
    for (int i=0; i<10; ++i) Update(base, 23, 0);
    if (!Close(built_yaw, 1.004f)) return 2;
    // A missing target must preserve each delta exactly once.
    no_target = true;
    Update(base, 23, 10); Update(base, 23, -2);
    no_target = false;
    Update(base, 23, 0);
    if (!Close(built_yaw, 1.036f)) return 3;
    // Other transition modes share the builder; mode changes do not reset yaw.
    Update(base, 5, 2);
    if (!Close(built_yaw, 1.044f)) return 4;
    // Controller takes ownership; the native replacement is now authoritative.
    input::recent = false; replacement = 2.0f;
    Update(base, 23, 0);
    if (!Close(built_yaw, 2.0f)) return 5;
    Update(base, 23, 1);
    if (!Close(built_yaw, 2.004f)) return 6;
    // Inline builders still see the pre-step and do not double the delta.
    inline_view = true;
    Update(base, 6, 3);
    if (!Close(built_yaw, 2.016f)) return 7;
    // Scoped override cannot leak into unrelated builder calls.
    SetFloat(base, kYaw, -1.0f);
    X86Context ctx{controller}; sub_0001FD40(ctx, base);
    if (!Close(built_yaw, -1.0f)) return 8;
    if (!Close(RdF(base, controller+0x20), -0.7f)) return 9;
    // Vehicle hands the held heading back immediately and reseeds on mouse use.
    inline_view = false; base[camera+kMode] = 13;
    SetFloat(base,kYaw,0.5f); input::pending_x=10;
    sub_0001FD40(ctx,base);
    if (!Close(built_yaw,0.54f)) return 10;
    input::recent=false; SetFloat(base,kYaw,1.5f);
    sub_0001FD40(ctx,base);
    if (!Close(built_yaw,1.5f)) return 11;
    input::pending_x=1; sub_0001FD40(ctx,base);
    if (!Close(built_yaw,1.504f)) return 12;
    return 0;
}
'''
result = run_native(source, prefix='mouse-camera-', needs_crt=True)
camera_result = result
if result and not old:
    raise SystemExit(f'Mouse camera regression {result} failed')

code = read('sdk/runtime/core/input.cpp')
# Keep actual state, fallback sampling and raw/direct routines. SDL device
# enumeration is irrelevant to these deterministic clock/focus fixtures.
state = code[code.index('uint32_t g_mouse_sensitivity_percent'):code.index('\nvoid AddPad(')]
methods = code[code.index('void ApplyMouseDelta('):code.index('\nvoid Init()')]
arbitration = code[code.index('    if (port == 0) {', code.index('Pad Poll(')):code.index('    if (app::kGame.pad)', code.index('Pad Poll('))]
source = r'''
#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <iterator>
#include <mutex>
#define LOGR(...) ((void)0)
#define LOGE(...) ((void)0)
using HWND=void*; using HRAWINPUT=void*; using UINT=unsigned; using LONG=long;
constexpr int VK_UP=1,VK_DOWN=2,VK_LEFT=3,VK_RIGHT=4,VK_ESCAPE=5,VK_TAB=6,
 VK_SPACE=7,VK_RBUTTON=8,VK_LBUTTON=9,VK_RETURN=10,VK_BACK=11,
 VK_MBUTTON=12,VK_XBUTTON1=13,VK_XBUTTON2=14,VK_CONTROL=15,VK_SHIFT=16,
 VK_LCONTROL=23,VK_RCONTROL=25,VK_LMENU=24,VK_RMENU=26,VK_RSHIFT=27,VK_NUMPAD7=17,VK_NUMPAD9=18,VK_NUMPAD5=19,VK_CAPITAL=20,VK_ADD=21,VK_OEM_PLUS=22;
uint64_t now=1000;
uint64_t GetTickCount64() { return now; }
bool focus=true;
void* GetForegroundWindow() { return focus ? reinterpret_cast<void*>(1) : nullptr; }
bool keys[256]{};
short GetAsyncKeyState(int key) { return key >= 0 && key < 256 && keys[key] ? short(0x8000) : 0; }
namespace app { struct { bool (*keyboard_driving)() = nullptr; } kGame; void* Window() { return reinterpret_cast<void*>(1); } }
struct RAWINPUTDEVICE { unsigned short usUsagePage, usUsage; unsigned long dwFlags; HWND hwndTarget; };
bool RegisterRawInputDevices(const RAWINPUTDEVICE*, unsigned, unsigned) { return true; }
struct RAWINPUTHEADER { unsigned dwType; };
struct RAWINPUT { RAWINPUTHEADER header; struct { struct { unsigned usFlags; LONG lLastX,lLastY; unsigned short usButtonFlags,usButtonData; } mouse; } data; };
constexpr unsigned RID_INPUT=1,RIM_TYPEMOUSE=0,MOUSE_MOVE_ABSOLUTE=1,RI_MOUSE_WHEEL=0x0800;
UINT GetRawInputData(HRAWINPUT r, UINT, RAWINPUT* p, UINT*, UINT) { *p=*static_cast<RAWINPUT*>(r); return sizeof(*p); }
struct SDL_Gamepad {};
enum SDL_GamepadType { SDL_GAMEPAD_TYPE_UNKNOWN,SDL_GAMEPAD_TYPE_PS3,SDL_GAMEPAD_TYPE_PS4,SDL_GAMEPAD_TYPE_PS5,SDL_GAMEPAD_TYPE_XBOX360,SDL_GAMEPAD_TYPE_XBOXONE };
SDL_GamepadType SDL_GetRealGamepadType(SDL_Gamepad*) { return SDL_GAMEPAD_TYPE_XBOXONE; }
namespace input {
''' + (root/'sdk/runtime/core/input.h').read_text().split('namespace input {',1)[1].rsplit('}',1)[0] + state + '\n} // anonymous namespace\n' + methods + r'''
Pad device;
Pad Poll(int port) {
    Pad p=device;
''' + arbitration + r'''
    return p;
}
void Move(int x, int y=0) { RAWINPUT r{}; r.data.mouse.lLastX=x; r.data.mouse.lLastY=y; ReadMouse(&r); }
void Reset() {
    SetUiActive(true); SetUiActive(false);
    g_direct_confirmed=false; g_direct_at=0;
    device=Pad{}; device.connected=true; device.rx=16000;
    now+=1000;
}
} // input
extern "C" __declspec(dllexport) int run() {
    using namespace input;
    float x=0,y=0;
    Reset(); UseDirectMouseAim(); Move(12);
    // A pad poll after heartbeat expiry used to throw away the first motion.
    now+=120; Poll(0);
    UseDirectMouseAim();
    if (!ConsumeMouseAim(x,y) || x!=12) return 20;
    if (ConsumeMouseAim(x,y)) return 21;
    // No polls during the load: stale direct counts also enter the buffer.
    Reset(); UseDirectMouseAim(); Move(45);
    now+=600; UseDirectMouseAim();
    float total=0;
    while (ConsumeMouseAim(x,y)) { if(x>20) return 22; total+=x; }
    if(total!=45) return 23;
    // Motion received after the heartbeat expires includes the earlier counts.
    Reset(); UseDirectMouseAim(); Move(10);
    now+=120; Move(25);
    UseDirectMouseAim(); total=0;
    while(ConsumeMouseAim(x,y)) total+=x;
    if(total!=35) return 24;
    // Raw one-count input selects mouse even after the camera consumes it.
    Reset(); UseDirectMouseAim(); Move(1); ConsumeMouseAim(x,y);
    Pad p=Poll(0);
    if(p.rx!=0 || !g_keyboard_mouse_active || !DirectAimRecent()) return 25;
    // Physical stick regains control promptly and invalidates the mouse hold.
    now+=200; UseDirectMouseAim(); p=Poll(0);
    if(p.rx!=16000 || g_keyboard_mouse_active || DirectAimRecent()) return 26;
    Move(1); p=Poll(0);
    if(p.rx!=0 || !g_keyboard_mouse_active) return 27;
    // One-count motion during the fallback window also retains mouse ownership.
    Reset(); UseDirectMouseAim(); now+=120; Move(1); p=Poll(0);
    if(p.rx!=0 || !g_keyboard_mouse_active) return 30;
    UseDirectMouseAim();
    if(!ConsumeMouseAim(x,y) || x!=1) return 31;
    // Focus/UI transitions discard counts and the old held heading.
    Move(50); SetUiActive(true); SetUiActive(false);
    if(ConsumeMouseAim(x,y) || DirectAimRecent()) return 28;
    Move(30); focus=false; Poll(0); focus=true;
    if(ConsumeMouseAim(x,y)) return 29;
    // Actual keyboard polling must translate actions to the Xbox layout.
    Reset();
    const int keycodes[] = {VK_SPACE, VK_LCONTROL, VK_LMENU, 'R', 'E', 'G', 'V', VK_RETURN, VK_BACK, VK_XBUTTON1,
                            VK_XBUTTON2, VK_RCONTROL, VK_RMENU};
    const int buttons[] = {3, 0, 2, 4, 5, 3, 1, 0, 1, 0, 2, 0, 2};
    for (unsigned i=0;i<sizeof(keycodes)/sizeof(keycodes[0]);++i) {
        keys[keycodes[i]]=true;
        Pad report; ReadKeyboardMouse(report);
        keys[keycodes[i]]=false;
        for(int button=0;button<8;++button)
            if(report.analog[button] != (button==buttons[i] ? 255 : 0))return 40+int(i);
    }
    keys['F']=true; Pad unused;ReadKeyboardMouse(unused);keys['F']=false;
    for(auto value:unused.analog) if(value)return 55;
    keys['Q']=true; Pad badge;ReadKeyboardMouse(badge);keys['Q']=false;
    if(!(badge.buttons&(1<<5)) || badge.analog[1])return 50;
    keys[VK_SHIFT]=true;keys[VK_RBUTTON]=true;
    Pad aim;ReadKeyboardMouse(aim);
    keys[VK_SHIFT]=keys[VK_RBUTTON]=false;
    if(!(aim.buttons&(1<<7)) || aim.analog[6]!=255)return 51;
    app::kGame.keyboard_driving=+[] { return true; };
    keys['W']=true;keys['S']=true;keys[VK_SPACE]=true;
    Pad driving;ReadKeyboardMouse(driving);
    keys['W']=keys['S']=keys[VK_SPACE]=false;
    app::kGame.keyboard_driving=nullptr;
    if(driving.analog[0]!=255 || driving.analog[2]!=255 || driving.analog[1]!=255 ||
       driving.analog[3] || driving.ly)return 52;

    return 0;
}
'''
result=run_native(source, prefix='mouse-input-', needs_crt=True)
if old:
    if not camera_result or not result:
        raise SystemExit('Baseline unexpectedly passed a regression harness')
    print(f'Baseline reproduced camera failure {camera_result} and input failure {result}.')
    raise SystemExit(0)
if result:
    raise SystemExit(f'Mouse input regression {result} failed')
print('Mouse transition and controller handoff regressions passed.')


# Exercise the actual window-message filter used while Alt is held for combat.
window_source = read('src/common/graphics_options.cpp')
start = window_source.index('bool GraphicsMenuMessage(')
handler = window_source[start:window_source.index('\n}\n', start) + 3]
window_check = r"""
#include <windows.h>
#include <cstdint>
""" + handler + r"""
extern "C" __declspec(dllexport) int run() {
 intptr_t result=123;
 if(!GraphicsMenuMessage(nullptr,WM_SYSKEYUP,VK_MENU,0,&result)||result!=0)return 1;
 if(!GraphicsMenuMessage(nullptr,WM_SYSCOMMAND,SC_KEYMENU,0,&result))return 2;
 if(GraphicsMenuMessage(nullptr,WM_SYSCOMMAND,SC_CLOSE,0,&result))return 3;
 if(GraphicsMenuMessage(nullptr,WM_SYSKEYDOWN,VK_F4,0,&result))return 4;
 return 0;
}
"""
assert run_native(window_check, prefix='combat-alt-', needs_crt=True) == 0
