"""Exercise the actual swap-chain resize method with failing resources."""
from pathlib import Path
from native_test import run_native

code = (Path(__file__).resolve().parents[1] / 'sdk/runtime/gpu/d3d12.cpp').read_text()
start = code.index('bool D3D12Backend::ResizeSwap()')
method = code[start:code.index('\n}\n', start) + 3]
source = r'''
#include <algorithm>
#include <atomic>
#include <iterator>
#include <cstdint>
using HWND = void*;
using HMONITOR = void*;
using LONG = long;
using HRESULT = long;
struct RECT { LONG left,top,right,bottom; };
RECT client{0,0,1280,720};
void GetClientRect(HWND,RECT* r) { *r=client; }
HMONITOR MonitorFromWindow(HWND,unsigned) { return nullptr; }
uint64_t GetTickCount64() { return 0; }
#define MONITOR_DEFAULTTOPRIMARY 1
#define FAILED(hr) ((hr)<0)
#define LOGE(...) ((void)0)
constexpr int kFrames=3, DXGI_FORMAT_B8G8R8A8_UNORM=0, DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING=1;
using DXGI_COLOR_SPACE_TYPE = int;
constexpr DXGI_COLOR_SPACE_TYPE DXGI_COLOR_SPACE_RGB_FULL_G10_NONE_P709 = 1;
constexpr DXGI_COLOR_SPACE_TYPE DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 = 2;
enum class HdrOutput { Off, ScRgb, Hdr10 };
namespace app {
 struct Settings { bool hdr=false; };
 Settings Snapshot() { return {}; }
 void* Window() { return nullptr; }
}
inline std::atomic<bool> g_hdr_available{false};
inline std::atomic<uint32_t> g_hdr_display_peak_nits{0};
struct Factory { void* Get() { return nullptr; } };
HdrOutput DetectHdrOutput(HWND, void*, uint32_t&) { return HdrOutput::Off; }
struct Buffer { bool valid=true; explicit operator bool() const { return valid; } void Reset() { valid=false; } };
struct Swap {
 HRESULT result=0; unsigned calls=0; bool valid=true;
 HRESULT ResizeBuffers(int,unsigned,unsigned,int,int) { ++calls; return result; }
 HRESULT SetColorSpace1(int) { return 0; }
};
struct Device { HRESULT removed=0; HRESULT GetDeviceRemovedReason() { return removed; } };
struct D3D12Backend {
 Buffer bb_[3];
 struct Frame { uint64_t fence=0; } frames_[3];
 Swap swap; Swap* swap_=&swap;
 Device device; Device* dev_=&device;
 Factory factory_;
 uint32_t bb_w_=640,bb_h_=480;
 bool tearing_=false, views=true; unsigned view_calls=0;
 bool hdr_setting_=false;
 HdrOutput hdr_output_=HdrOutput::Off;
 uint32_t display_peak_nits_=1000;
 HMONITOR hdr_monitor_=nullptr;
 HdrOutput detected_hdr_=HdrOutput::Off;
 uint64_t hdr_check_ms_=0;
 void Flush() {}
 bool WaitFence(uint64_t) { return true; }
 bool CreateBackBufferViews() {
  ++view_calls;
  for(auto& b:bb_)b.valid=views;
  return views;
 }
 int SwapFormat() const { return DXGI_FORMAT_B8G8R8A8_UNORM; }
 bool RefreshFactory(bool = false) { return true; }
 bool CreateSwapChain(HWND) { return true; }
 bool ResizeSwap();
};
''' + method + r'''
extern "C" __declspec(dllexport) int run() {
 D3D12Backend b;
 if(!b.ResizeSwap() || b.bb_w_!=1280 || b.bb_h_!=720)return 1;
 b.swap.result=-1; client={0,0,1920,1080};
 if(!b.ResizeSwap() || b.bb_w_!=1280 || b.bb_h_!=720)return 2;
 b.views=false;
 if(b.ResizeSwap())return 3;
 b.views=true;b.device.removed=-1;
 unsigned calls=b.view_calls;
 if(b.ResizeSwap() || b.view_calls!=calls)return 4;
 b.device.removed=0;b.swap.result=0;b.views=false;
 if(b.ResizeSwap() || b.bb_w_!=1280 || b.bb_h_!=720)return 5;
 client={0,0,1280,720};
 if(b.ResizeSwap())return 6;
 b.views=true;client={0,0,1920,1080};
 if(!b.ResizeSwap())return 7;
 b.bb_[2].valid=false;
 if(b.ResizeSwap())return 8;
 return 0;
}
'''
result = run_native(source, prefix='tcnyc-resize-', needs_crt=True)
assert result == 0, f'back-buffer recovery regression {result}'
print('PASS: resize recovery preserves dimensions, rejects missing buffers and stops on device loss')
