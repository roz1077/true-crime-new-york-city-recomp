"""Compare actual vertex assembly with the previous per-draw index-vector implementation."""
from pathlib import Path
import sys

root = Path(__file__).resolve().parents[1]
from native_test import run_native

state = (root / 'sdk/runtime/gpu/state.h').read_text().replace('#pragma once', '')
header = (root / 'sdk/runtime/gpu/vertex.h').read_text().replace('#pragma once', '').replace('#include "gpu/state.h"', '')
implementation = (root / 'sdk/runtime/gpu/vertex.cpp').read_text()
implementation = implementation[implementation.index('namespace gpu {'):]
previous = implementation.replace(
    'const bool native_strip = !force_converted && d.prim == 6 && s.simple[kMShadeMode >> 2] != 0x1D00;',
    'const bool native_strip = !force_converted && false;')
assert previous != implementation
previous = previous.replace('if (!ranged || (!native_strip && d.ip)) {', 'if (true) {')

test = r'''
#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <cstdlib>
#include <new>
#include <chrono>
#include <cstdio>
#include <dxgiformat.h>
unsigned allocations=0;
void* operator new(size_t n) { ++allocations;if(auto p=std::malloc(n))return p;throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p,size_t) noexcept { std::free(p); }
uint8_t memory[4<<20]{};
namespace kernel {
 uint32_t R32(uint32_t address) { uint32_t v;std::memcpy(&v,memory+address,4);return v; }
 void* HostPtr(uint32_t address) { return memory+address; }
}
namespace mem { uint32_t VirtualFromPhysical(uint32_t address) { return address; } }
#define LOG_FIRST(...) ((void)0)
''' + state + header + implementation
test += (state + header + previous).replace('namespace gpu {', 'namespace previous {')
test += r'''
template<class State,class Shader> void setup(State& s) {
 s.vs_handle=1;s.vs=std::make_shared<Shader>();
 s.vs->inputs[0]={0,1,0x32};s.vs->inputs[3]={0,13,0x40};
 s.vs->stream_stride[0]=17;s.streams[0]={64,17};s.base_vertex=3;
}
template<class Assembly> std::vector<uint32_t> triangles(const Assembly& draw) {
    auto index = [&](uint32_t i) {
        if (!draw.index_count) return i;
        uint32_t value = 0;
        const size_t size = draw.index16 ? 2 : 4;
        std::memcpy(&value, static_cast<const uint8_t*>(draw.index_src) + i * size, size);
        return value + draw.base_vertex;
    };
    std::vector<uint32_t> result;
    const uint32_t count = draw.index_count ? draw.index_count : draw.draw_count;
    if (draw.strip) {
        for (uint32_t i = 0; i + 2 < count; ++i) {
            result.push_back(index(i + (i & 1)));
            result.push_back(index(i + 1 - (i & 1)));
            result.push_back(index(i + 2));
        }
    } else {
        for (uint32_t i = 0; i < count; ++i) result.push_back(index(i));
    }
    return result;
}

bool same(const gpu::Assembled& a, const previous::Assembled& b) {
    if (a.regs != b.regs || a.vertex_count != b.vertex_count || int(a.topology) != int(b.topology) ||
        a.vertices.size() != b.vertices.size() || triangles(a) != triangles(b)) return false;
 if(std::memcmp(a.vertices.data(),b.vertices.data(),a.vertices.size()*sizeof(float)))return false;
 for(unsigned r=0;r<16;++r)if(a.regs>>r&1) {
  if(a.elements[r].format!=b.elements[r].format||a.elements[r].buffer!=b.elements[r].buffer||
     a.elements[r].offset!=b.elements[r].offset)return false;
 }
 for(unsigned i=0;i<a.buffer_count;++i)if(a.buffers[i].bytes!=b.buffers[i].bytes||
     a.buffers[i].stride!=b.buffers[i].stride)return false;
 return true;
}
extern "C" __declspec(dllexport) int run() {
 uint32_t address=1024;std::memcpy(memory+68,&address,4);
 for(unsigned i=0;i<1024;++i) {
  float xyz[3]={float(i)*.25f,float(i)*-.125f,float(i)*.0625f};
  std::memcpy(memory+1024+i*17+1,xyz,sizeof(xyz));
  uint32_t colour=0x80402010u+i;std::memcpy(memory+1024+i*17+13,&colour,4);
 }
 gpu::State s;previous::State old;setup<gpu::State,gpu::VertexShader>(s);
 setup<previous::State,previous::VertexShader>(old);
 gpu::Assembled a;previous::Assembled b;
 const unsigned counts[]={0,1,2,3,4,8,17,64};
 for(unsigned mode=0;mode<4;++mode)for(unsigned prim=1;prim<=10;++prim)for(unsigned n:counts) {
  uint16_t indices[64];for(unsigned i=0;i<n;++i)indices[i]=uint16_t(mode==2?(i%2?700-i:i):i);
  const auto* ip=mode?reinterpret_cast<const uint8_t*>(indices):nullptr;
  s.streams[0].stride=old.streams[0].stride=mode==3?0:17; // declaration stride fallback
  bool ok=gpu::Assemble(s,prim,5,n,ip,a),was=previous::Assemble(old,prim,5,n,ip,b);
  if (ok != was || (ok && !same(a,b))) return 1;
  if (ok && prim == 6 && !a.strip) return 4;
 }
 s.simple[gpu::kMShadeMode >> 2] = 0x1D00;
 if (!gpu::Assemble(s,6,0,8,nullptr,a) || a.strip || a.index_count != 18) return 5;
 s.simple[gpu::kMShadeMode >> 2] = 0;
 // Grow every retained output once. Subsequent equal-size draws must allocate nothing.
 gpu::Assemble(s,6,0,256,nullptr,a);previous::Assemble(old,6,0,256,nullptr,b);
 unsigned before=allocations;
 for(unsigned i=0;i<100;++i)gpu::Assemble(s,6,0,256,nullptr,a);
 if(allocations!=before)return 2;
 before=allocations;
 for(unsigned i=0;i<100;++i)previous::Assemble(old,6,0,256,nullptr,b);
 if(allocations-before!=100)return 3;
#ifdef BENCHMARK
 auto measure=[&](bool current) {
  auto start=std::chrono::steady_clock::now();
  for(unsigned i=0;i<50000;++i) {
   if(current)gpu::Assemble(s,6,0,256,nullptr,a);
   else previous::Assemble(old,6,0,256,nullptr,b);
  }
  return std::chrono::duration<double,std::milli>(std::chrono::steady_clock::now()-start).count();
 };
 double was[7],now[7];
 for(unsigned i=0;i<7;++i) {
  if(i%2) { now[i]=measure(true);was[i]=measure(false); }
  else { was[i]=measure(false);now[i]=measure(true); }
 }
 std::sort(was,was+7);std::sort(now,now+7);
 std::printf("Vertex assembly microbenchmark (50000 draws, O2): previous %.3f ms, direct %.3f ms, ratio %.4f\n",
             was[3],now[3],now[3]/was[3]);
 std::fflush(stdout);
#endif
 return 0;
}
'''
benchmark = '--benchmark' in sys.argv
if benchmark:
    test = '#define BENCHMARK\n' + test
rc = run_native(test, prefix='tcnyc-converted-strips-', needs_crt=True,
                compiler_flags=['-O2'] if benchmark else [])
assert rc == 0, f'Vertex assembly regression {rc}'
print('PASS: 320 assembly cases preserve vertex data and primitive order, including indexed strips and flat-shading fallback')
