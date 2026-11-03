"""Compare allocation-free audible lanes and ramped output with the previous vector implementation."""
from pathlib import Path
from native_test import run_native

code = (Path(__file__).resolve().parents[1] / 'sdk/runtime/xdk/dsound.cpp').read_text()
start = code.index('struct Lane {')
lane = code[start:code.index('\n};', start) + 3]
start = code.index('std::span<Lane> AudibleLanes(')
function = code[start:code.index('\n}\n', start) + 3]

test = r'''
#include <array>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <new>
#include <span>
#include <vector>
unsigned allocations=0;
void* operator new(size_t n) { ++allocations; if(auto p=std::malloc(n))return p; throw std::bad_alloc(); }
void operator delete(void* p) noexcept { std::free(p); }
void operator delete(void* p,size_t) noexcept { std::free(p); }
constexpr uint32_t kRows=10,kBins=32;
struct Gains { float bin[kRows][kBins]{}; };
''' + lane + function + r'''
std::vector<Lane> Previous(const Gains& from,const Gains& to,uint32_t frames) {
 std::vector<Lane> lanes;
 for(uint32_t r=0;r<kRows;++r)for(uint32_t b=0;b<kBins;++b) {
  float a=from.bin[r][b],z=to.bin[r][b];
  if(a!=0||z!=0)lanes.push_back({r,b,a,(z-a)/float(frames)});
 }
 return lanes;
}
extern "C" __declspec(dllexport) int run() {
 uint32_t rng=123456;
 for(uint32_t scenario=0;scenario<64;++scenario) {
  Gains from,to;
  uint32_t frames=scenario%2?480:96;
  for(uint32_t r=0;r<kRows;++r)for(uint32_t b=0;b<kBins;++b) {
   rng=rng*1664525+1013904223;
   from.bin[r][b]=scenario==0||rng%5==0?0:float(int32_t(rng%2001)-1000)/1024;
   rng=rng*1664525+1013904223;
   to.bin[r][b]=scenario==0||rng%7==0?0:float(int32_t(rng%2001)-1000)/1024;
  }
  if(scenario==1)for(auto& row:to.bin)for(float& v:row)v=0; // fade out
  if(scenario==2)from={}; // fade in
  auto old=Previous(from,to,frames);
  std::array<Lane,kRows*kBins> scratch;
  unsigned before=allocations;
  auto now=AudibleLanes(from,to,frames,scratch);
  if(allocations!=before)return 1;
  if(now.size()!=old.size())return 2;
  if(std::memcmp(now.data(),old.data(),now.size_bytes()))return 3;
  // Same ramp updates and bin-add order as MixVoice; include dense routing and fades.
  for(uint32_t i=0;i<frames;++i) {
   float a[kBins]{},b[kBins]{};
   for(auto& lane:old) { lane.gain+=lane.step;a[lane.bin]+=float(int((i+lane.row)%17)-8)*lane.gain; }
   for(auto& lane:now) { lane.gain+=lane.step;b[lane.bin]+=float(int((i+lane.row)%17)-8)*lane.gain; }
   if(std::memcmp(a,b,sizeof(a)))return 4;
  }
 }
 if(!allocations)return 5; // The oracle really exercised the previous allocations.
 return 0;
}
'''
rc = run_native(test, prefix='tcnyc-audio-lanes-', needs_crt=True)
assert rc == 0, f'Audio lane regression {rc}'
print('PASS: no lane-list allocations; lane order, gains and ramped bin output match across 64 scenarios')
