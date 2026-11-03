"""Exercise the actual D3D12 viewport/scissor setter with a recording command list."""
from pathlib import Path
import sys
from native_test import run_native

root = Path(__file__).resolve().parents[1]
code = (root / 'sdk/runtime/gpu/d3d12.cpp').read_text()
start = code.index('void D3D12Backend::SetViewportScissor(')
method = code[start:code.index('\n}\n', start) + 3]
if '--baseline' in sys.argv:
    for i, name, op in [(0, 'x0', 'max'), (1, 'y0', 'max'), (2, 'x1', 'min'), (3, 'y1', 'min')]:
        method = method.replace(f'{name} = LONG(s.scissors[{i}] * sc);',
                                f'{name} = std::{op}({name}, LONG(s.scissors[{i}] * sc));')

source = r'''
#include <algorithm>
#include <cstdint>
#include <cstring>
using LONG = int32_t;
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
 struct { bool rasterizer=false; D3D12_VIEWPORT viewport{}; D3D12_RECT scissor{}; } binds_;
 void SetViewportScissor(const State&,uint32_t);
};
''' + method + r'''
bool equal(D3D12_RECT a,D3D12_RECT b) { return !std::memcmp(&a,&b,sizeof(a)); }
extern "C" __declspec(dllexport) int run() {
 Target rt{640,480}; Commands cmd; D3D12Backend b; b.bound_rt_=&rt; b.cl_=&cmd;
 State s{{358,216,244,264}};
 b.SetViewportScissor(s,640);
 if(!equal(cmd.rect,{358,216,602,480}))return 1;
 // The title explicitly opens the surface clip above the locker viewport.
 s.scissor_count=1; s.scissors[2]=639; s.scissors[3]=479;
 b.SetViewportScissor(s,640);
 if(!equal(cmd.rect,{0,0,639,479}))return 2;
 // A narrower explicit clip still works, including a nonzero origin.
 s.scissors[0]=400;s.scissors[1]=230;s.scissors[2]=500;s.scissors[3]=350;
 b.SetViewportScissor(s,640);
 if(!equal(cmd.rect,{400,230,500,350}))return 3;
 unsigned calls=cmd.calls;b.SetViewportScissor(s,640);if(cmd.calls!=calls)return 4;
 // Upscaling scales the explicit clip; hardware viewport remains the full target.
 rt={1280,960};s.scissors[0]=0;s.scissors[1]=0;s.scissors[2]=639;s.scissors[3]=479;
 b.SetViewportScissor(s,640);
 if(!equal(cmd.rect,{0,0,1278,958})||cmd.viewport.w!=1280||cmd.viewport.h!=960)return 5;
 s.scissors[2]=1000;s.scissors[3]=1000;b.SetViewportScissor(s,640);
 if(!equal(cmd.rect,{0,0,1280,960}))return 6;
 // SetScissors(0) restores the viewport clip rather than retaining the override.
 s.scissor_count=0;b.SetViewportScissor(s,640);
 if(!equal(cmd.rect,{716,432,1204,960}))return 7;
 return 0;
}
'''
rc = run_native(source, prefix="tcnyc-viewport_scissor-", needs_crt=True)
assert rc == 0, f"Viewport/scissor regression {rc} (2 = locker clipped to viewport)"
print('PASS: explicit scissor overrides viewport, default clip, upscaling, target bounds and state cache')
