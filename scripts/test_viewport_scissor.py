"""Exercise the actual D3D12 viewport/scissor setter with a recording command list."""
from pathlib import Path
import sys
from native_test import run_native

root = Path(__file__).resolve().parents[1]
code = (root / 'sdk/runtime/gpu/d3d12.cpp').read_text()
start = code.index('void D3D12Backend::SetViewportScissor(')
method = code[start:code.index('\n}\n', start) + 3]
if '--baseline' in sys.argv:
    for i, name, op, axis in [(0, 'x0', 'max', 'sx'), (1, 'y0', 'max', 'sy'), (2, 'x1', 'min', 'sx'),
                              (3, 'y1', 'min', 'sy')]:
        method = method.replace(f'{name} = LONG(s.scissors[{i}] * {axis});',
                                f'{name} = std::{op}({name}, LONG(s.scissors[{i}] * {axis}));')

source = r'''
#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
using LONG = long;
struct D3D12_VIEWPORT { float x,y,w,h,z0,z1; };
struct D3D12_RECT { LONG left,top,right,bottom; };
struct State {
 struct { uint32_t x,y,w,h; } viewport;
 uint32_t scissor_count=0, scissors[4]{};
};
struct Target { uint32_t w,h; };
struct Commands {
 D3D12_RECT rect{}; D3D12_VIEWPORT viewport{}; unsigned calls=0;
 void RSSetViewports(unsigned,const D3D12_VIEWPORT* p) { viewport=*p; }
 void RSSetScissorRects(unsigned,const D3D12_RECT* p) { rect=*p; ++calls; }
};
struct D3D12Backend {
 Target* bound_rt_=nullptr; Target* bound_ds_=nullptr; Commands* cl_;
 // a deferred scene bundle: viewport and scissor are kept for the direct list at replay
 Commands* bundle_=nullptr;
 struct { D3D12_VIEWPORT viewport{}; D3D12_RECT scissor{}; bool has_viewport=false, has_scissor=false; } bundle_desc_;
 struct { bool rasterizer=false; D3D12_VIEWPORT viewport{}; D3D12_RECT scissor{}; } binds_;
 void SetViewportScissor(const State&,uint32_t,uint32_t,float = 1.0f);
};
''' + method + r'''
bool equal(D3D12_RECT a,D3D12_RECT b) { return !std::memcmp(&a,&b,sizeof(a)); }
extern "C" __declspec(dllexport) int run() {
 Target rt{640,480}; Commands cmd; D3D12Backend b; b.bound_rt_=&rt; b.cl_=&cmd;
 State s{{358,216,244,264}};
 b.SetViewportScissor(s,640,480);
 if(!equal(cmd.rect,{358,216,602,480}))return 1;
 // The title explicitly opens the surface clip above the locker viewport.
 s.scissor_count=1; s.scissors[2]=639; s.scissors[3]=479;
 b.SetViewportScissor(s,640,480);
 if(!equal(cmd.rect,{0,0,639,479}))return 2;
 // A narrower explicit clip still works, including a nonzero origin.
 s.scissors[0]=400;s.scissors[1]=230;s.scissors[2]=500;s.scissors[3]=350;
 b.SetViewportScissor(s,640,480);
 if(!equal(cmd.rect,{400,230,500,350}))return 3;
 unsigned calls=cmd.calls;b.SetViewportScissor(s,640,480);if(cmd.calls!=calls)return 4;
 // Upscaling scales the explicit clip; hardware viewport remains the full target.
 rt={1280,960};s.scissors[0]=0;s.scissors[1]=0;s.scissors[2]=639;s.scissors[3]=479;
 b.SetViewportScissor(s,640,480);
 if(!equal(cmd.rect,{0,0,1278,958})||cmd.viewport.w!=1280||cmd.viewport.h!=960)return 5;
 s.scissors[2]=1000;s.scissors[3]=1000;b.SetViewportScissor(s,640,480);
 if(!equal(cmd.rect,{0,0,1280,960}))return 6;
 // SetScissors(0) restores the viewport clip rather than retaining the override.
 s.scissor_count=0;b.SetViewportScissor(s,640,480);
 if(!equal(cmd.rect,{716,432,1204,960}))return 7;
 // a frame wider than the console's: x and y scale apart (640x480 drawn 2240x960, 3.5 and 2 host pixels each)
 rt={2240,960};s.scissor_count=1;s.scissors[0]=0;s.scissors[1]=0;s.scissors[2]=639;s.scissors[3]=479;
 b.SetViewportScissor(s,640,480);
 if(!equal(cmd.rect,{0,0,2236,958}))return 8;
 // Squeezed HUD preserves its guest clip, hiding sprites outside the 16:9 area.
 b.SetViewportScissor(s,640,480,0.75f);
 if(!equal(cmd.rect,{280,0,1957,958}))return 9;
 // While a bundle records, the direct list is left alone and the clip waits in the bundle's state.
 Commands bundle; b.bundle_=&bundle; calls=cmd.calls;
 s.scissors[0]=10;s.scissors[1]=20;s.scissors[2]=110;s.scissors[3]=220;
 b.SetViewportScissor(s,640,480);
 if(cmd.calls!=calls||bundle.calls||!b.bundle_desc_.has_viewport||!b.bundle_desc_.has_scissor)return 10;
 if(!equal(b.bundle_desc_.scissor,{35,40,385,440}))return 11;
 return 0;
}
'''
rc = run_native(source, prefix="tcnyc-viewport_scissor-", needs_crt=True)
assert rc == 0, f"Viewport/scissor regression {rc} (2 = locker clipped to viewport)"
print('PASS: explicit scissor overrides viewport, default clip, upscaling, wide frames, target bounds, state cache and bundle recording')
