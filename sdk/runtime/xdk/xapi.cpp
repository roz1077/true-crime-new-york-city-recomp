// XAPI functions that talk to hardware: controller ports and the hard disk cache partition
#include <chrono>

#include "core/input.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

// configs written before the SDK knew the name
#ifndef SYM_XInputGetCapabilities
#define SYM_XInputGetCapabilities FFFFFC00
#define HAS_XInputGetCapabilities 0
#endif

using namespace kernel;

namespace {

// XPP device type records: CurrentConnected, ChangeConnected, PreviousConnected
constexpr uint32_t kGamepadType = SYM(XPP_GamepadType);
constexpr uint32_t kMemoryUnitType = SYM(XPP_MemoryUnitType);

std::mutex g_ports_lock;
bool g_poller = false;

void UpdatePorts() {
    std::lock_guard<std::mutex> lock(g_ports_lock);
    uint32_t cur = 0;
    for (int p = 0; p < 4; p++)
        if (input::Connected(p)) cur |= 1u << p;
    uint32_t old = R32(kGamepadType + XDK_XPP_Current);
    if (cur != old) {
        W32(kGamepadType + XDK_XPP_Changed, R32(kGamepadType + XDK_XPP_Changed) | (cur ^ old));
        W32(kGamepadType + XDK_XPP_Current, cur);
        LOGI("xapi: controller ports now %X", cur);
    }
}

struct Handle {
    uint32_t port;
    uint32_t packet;
    // closes and reopens can reuse the same guest address; the generation tells
    // a lookup that started before the close to report an invalid handle
    uint64_t generation;
    input::Pad last;
};
std::unordered_map<uint32_t, Handle> g_handles;
uint64_t g_handle_generation = 0;  // changed under g_ports_lock

}  // namespace

HLE_STDCALL(XMountUtilityDrive, 1) {
    CreateSymbolicLink("\\??\\Z:", "\\Device\\Harddisk0\\Partition5");
    return 1;
}

HLE_STDCALL(XInitDevices, 2) {
    XDK_REQUIRE("xapi", XDK_KEY(XPP_Current), XDK_KEY(XPP_Changed), XDK_KEY(XPP_Previous), XDK_DATA(XPP_GamepadType));
    W32(kGamepadType + XDK_XPP_Current, 0);
    W32(kGamepadType + XDK_XPP_Changed, 0);
    W32(kGamepadType + XDK_XPP_Previous, 0);
    if constexpr (HAS_XPP_MemoryUnitType != 0) {  // titles without memory unit support have no record
        W32(kMemoryUnitType + XDK_XPP_Current, 0);
        W32(kMemoryUnitType + XDK_XPP_Changed, 0);
        W32(kMemoryUnitType + XDK_XPP_Previous, 0);
    }
    UpdatePorts();
    if (!g_poller) {
        g_poller = true;
        std::thread([] {
            for (;;) {
                std::this_thread::sleep_for(std::chrono::milliseconds(250));
                UpdatePorts();
            }
        }).detach();
    }
    return 0;
}

HLE_STDCALL(XInputOpen, 4) {
    uint32_t type = a[0], port = a[1];
    if (type != kGamepadType || port > 3) return 0;
    uint32_t h = mem::AllocSystem(16);
    if (!h) return 0;
    std::lock_guard<std::mutex> lock(g_ports_lock);
    g_handles[h] = {port, 0, ++g_handle_generation, {}};
    return h;
}

HLE_STDCALL(XInputClose, 1) {
    std::lock_guard<std::mutex> lock(g_ports_lock);
    if (g_handles.erase(a[0])) mem::FreeSystem(a[0]);
    return 0;
}

HLE_STDCALL(XInputGetState, 2) {
    uint32_t port;
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(g_ports_lock);
        auto it = g_handles.find(a[0]);
        if (it == g_handles.end()) return 6;  // ERROR_INVALID_HANDLE
        port = it->second.port;
        generation = it->second.generation;
    }
    input::Pad p = input::Poll(int(port));
    std::lock_guard<std::mutex> lock(g_ports_lock);
    auto it = g_handles.find(a[0]);
    if (it == g_handles.end() || it->second.generation != generation) return 6;
    if (!p.connected) return 1167;  // ERROR_DEVICE_NOT_CONNECTED
    Handle& h = it->second;
    if (p != h.last) h.packet++;
    h.last = p;
    uint32_t s = a[1];
    W32(s, h.packet);  // XINPUT_STATE: packet, buttons, 8 analog buttons, thumbs
    W16(s + 4, p.buttons);
    std::memcpy(HostPtr(s + 6), p.analog, 8);
    W16(s + 14, uint16_t(p.lx));
    W16(s + 16, uint16_t(p.ly));
    W16(s + 18, uint16_t(p.rx));
    W16(s + 20, uint16_t(p.ry));
    return 0;
}

// XINPUT_CAPABILITIES is 26 bytes in the original Xbox XDK: Type, SubType,
// Flags, then XINPUT_GAMEPAD (a WORD of digital buttons, 8 analog button
// bytes and four SHORT thumb axes), then XINPUT_VIBRATION (two WORD motors).
// The host implements a wired rumble-capable gamepad.
HLE_STDCALL(XInputGetCapabilities, 2) {
    constexpr uint32_t kCapsBytes = 26;
    constexpr uint8_t kTypeGamepad = 1;       // XINPUT_DEVTYPE_GAMEPAD
    constexpr uint8_t kSubtypeGamepad = 1;    // XINPUT_DEVSUBTYPE_GC_GAMEPAD
    constexpr uint16_t kFlags = 0x0001;       // vibration / force feedback supported
    uint32_t port;
    uint64_t generation;
    {
        std::lock_guard<std::mutex> lock(g_ports_lock);
        auto it = g_handles.find(a[0]);
        if (it == g_handles.end()) return 6;  // ERROR_INVALID_HANDLE
        port = it->second.port;
        generation = it->second.generation;
    }
    const bool connected = input::Connected(int(port));
    std::lock_guard<std::mutex> lock(g_ports_lock);
    auto it = g_handles.find(a[0]);
    if (it == g_handles.end() || it->second.generation != generation) return 6;
    if (!connected) return 1167;  // ERROR_DEVICE_NOT_CONNECTED
    uint32_t c = a[1];
    std::memset(HostPtr(c), 0, kCapsBytes);
    G<uint8_t>(c + 0) = kTypeGamepad;
    G<uint8_t>(c + 1) = kSubtypeGamepad;
    W16(c + 2, kFlags);
    W16(c + 4, 0x00FF);                   // the 8 digital buttons
    std::memset(HostPtr(c + 6), 0xFF, 8); // every analog button
    W16(c + 14, 0x7FFF);                  // thumb X/Y axes, full resolution
    W16(c + 16, 0x7FFF);
    W16(c + 18, 0x7FFF);
    W16(c + 20, 0x7FFF);
    W16(c + 22, 0xFFFF);                  // left and right motors
    W16(c + 24, 0xFFFF);
    return 0;
}

HLE_STDCALL(XInputSetState, 2) {
    uint32_t port;
    {
        std::lock_guard<std::mutex> lock(g_ports_lock);
        auto it = g_handles.find(a[0]);
        if (it == g_handles.end()) return 6;
        port = it->second.port;
    }
    uint32_t fb = a[1];
    // XINPUT_FEEDBACK_HEADER is dwStatus, hEvent and 58 reserved bytes, the rumble follows
    input::Rumble(int(port), R16(fb + 66), R16(fb + 68));
    W32(fb, 0);
    if (uint32_t ev = R32(fb + 4))
        if (ObjectRef o = FromHandle(ev)) SetEvent(*o);
    return 997;  // ERROR_IO_PENDING, already complete
}

// A recompiled image launches through the XAPI, then a process restart into its executable; any other
// (the dashboard, updates) is refused and the title stays in its menu.
X86_EXTERN_FUNC(GUEST_ORIG(XLaunchNewImageA));
X86_FUNC(GUEST(XLaunchNewImageA)) {
    std::string path = ReadCString(R32(ctx.esp + 4));
    if (ImageExe(path)) {
        GUEST_ORIG(XLaunchNewImageA)(ctx, base);
        return;
    }
    LOGW("xapi: launch of %s refused, it is not recompiled", path.c_str());
    Return(ctx, 2, 2);  // ERROR_FILE_NOT_FOUND
}
