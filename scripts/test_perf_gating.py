"""Exercise actual render-thread Draw/Wait methods with a counted clock and command recorder."""
from pathlib import Path
import sys
from native_test import run_native

source = (Path(__file__).resolve().parents[1] / 'sdk/runtime/gpu/render_thread.cpp').read_text()


def method(signature):
    start = source.index(signature)
    return source[start:source.index('\n    }', start) + 6]


draw = method('    void Draw(const State& s,')
wait = method('    void Wait(uint64_t id,')
if '--baseline' in sys.argv:
    draw = draw.replace('perf ? Clock::now() : Clock::time_point{}', 'Clock::now()')
    draw = draw.replace('if (perf) g_perf_capture_ns', 'g_perf_capture_ns')

test = r'''
#include <array>
#include <atomic>
#include <chrono>
#include <cstdint>
#include <mutex>
#include <vector>
unsigned clock_calls=0,logs=0;
namespace diag { bool enabled=false; bool Perf() { return enabled; } }
struct Clock {
 using time_point=std::chrono::steady_clock::time_point;
 static time_point now() { return time_point(std::chrono::nanoseconds(++clock_calls*10)); }
};
uint64_t Ns(Clock::time_point t) {
 return std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now()-t).count();
}
std::atomic<uint64_t> g_perf_capture_ns{0},g_perf_wait_ns{0},g_perf_waits{0};
std::atomic<uint64_t> g_perf_wait_kind_ns[6]{};
#define LOG_FIRST(...) (++logs)
enum class WaitFor { ReadBack, Resource, Fence, Flush, Copy, Frame };
enum class Op { Draw };
struct Cmd { Op op; uint32_t a=0,b=0,c=0,d=0,data=0; };
struct Batch { uint64_t id=10; std::vector<Cmd> cmds; };
struct State {
 std::array<uint32_t,4> textures{},palettes{};
 struct Stream { uint32_t vb=0; }; std::array<Stream,16> streams{};
 struct Surface { uint32_t header=0; } rt,zs;
};
struct Backend { virtual void Draw(const State&,uint32_t,uint32_t,uint32_t,const uint8_t*)=0; };
struct WaitCv {
 std::atomic<uint64_t>* done; unsigned calls=0; uint64_t target=5;
 template<class Lock,class Predicate> void wait(Lock&,Predicate predicate) {
  ++calls; done->store(target); if(!predicate()) throw 1;
 }
};
constexpr uint32_t kBatchCommands=64;
struct Threaded:Backend {
 Batch batch; Batch* cur_=&batch; unsigned states=0,uses=0,writes=0,submits=0,puts=0;
 std::mutex m_; std::atomic<uint64_t> done_{0}; WaitCv done_cv_{&done_};
 void SendState(const State&) { ++states; }
 void Use(uint32_t) { ++uses; }
 void Written(const State::Surface&) { ++writes; }
 uint64_t Submit() { ++submits; return 9; }
 uint32_t Put(const uint8_t*,size_t n) { puts+=unsigned(n); return 12; }
''' + draw + wait + r'''
};
extern "C" __declspec(dllexport) int run() {
 State s; Threaded t;
 uint8_t indices[6]{};
 t.Draw(s,6,2,3,indices);
 if(clock_calls || g_perf_capture_ns.load())return 1;
 if(t.states!=1||t.uses!=26||t.writes!=1||t.puts!=6||t.batch.cmds.size()!=1)return 2;
 auto c=t.batch.cmds[0]; if(c.a!=6||c.b!=2||c.c!=3||c.d!=1||c.data!=12)return 3;
 t.Wait(5,WaitFor::Resource,"resource");
 if(t.done_cv_.calls!=1||t.done_.load()!=5)return 4;
 if(clock_calls||logs||g_perf_wait_ns.load()||g_perf_waits.load())return 5;
 diag::enabled=true;
 t.Draw(s,6,2,3,nullptr);
 if(clock_calls!=2||g_perf_capture_ns.load()!=10)return 6;
 if(t.states!=2||t.uses!=52||t.writes!=2||t.puts!=6)return 7;
 t.done_=0;t.Wait(5,WaitFor::Resource,"resource");
 if(clock_calls!=4||g_perf_wait_ns.load()!=10||g_perf_waits.load()!=1||logs!=1)return 8;
 if(g_perf_wait_kind_ns[size_t(WaitFor::Resource)].load()!=10)return 9;
 t.Wait(0,WaitFor::Resource,nullptr);t.Wait(5,WaitFor::Resource,nullptr);
 if(clock_calls!=4||t.done_cv_.calls!=2)return 10;
 return 0;
}
'''
rc = run_native(test, prefix='tcnyc-perf-gating-', needs_crt=True)
assert rc == 0, f'Profiler gating regression {rc} (1 = disabled Draw still collects timings)'
print('PASS: disabled profiling skips clocks/counters; enabled diagnostics and command/wait behavior survive')
