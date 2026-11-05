"""Install the real watchpoint on live and exiting Windows worker threads."""
from pathlib import Path
from native_test import run_native

code = (Path(__file__).resolve().parents[1] / 'sdk/runtime/kernel/threads.cpp').read_text()
start = code.index('void ApplyWatchToHostThread(')
method = code[start:code.index('\n}\n', start) + 3]
source = r'''
#include <windows.h>
#include <atomic>
#include <memory>
#include <thread>
#include <cstdint>
struct Thread { std::atomic<uint32_t> host_tid{0}; };
using ThreadRef=std::shared_ptr<Thread>;
alignas(8) uint64_t watched=0;
uint8_t* g_base=reinterpret_cast<uint8_t*>(uintptr_t(&watched)-0x1000);
''' + method + r'''
extern "C" __declspec(dllexport) int run() {
 for(unsigned iteration=0;iteration<8;++iteration) {
  auto t=std::make_shared<Thread>();
  HANDLE ready=CreateEventW(nullptr,TRUE,FALSE,nullptr);
  HANDLE finish=CreateEventW(nullptr,TRUE,FALSE,nullptr);
  if(!ready || !finish)return 1;
  std::thread worker([&] {
   t->host_tid.store(GetCurrentThreadId());
   SetEvent(ready);
   WaitForSingleObject(finish,INFINITE);
   t->host_tid.store(0);
  });
  WaitForSingleObject(ready,INFINITE);
  ApplyWatchToHostThread(t,0x1000,8);
  HANDLE h=OpenThread(THREAD_SUSPEND_RESUME|THREAD_GET_CONTEXT,FALSE,t->host_tid.load());
  CONTEXT c{};c.ContextFlags=CONTEXT_DEBUG_REGISTERS;
  bool valid=h && SuspendThread(h)!=DWORD(-1);
  if(valid) {
   valid=GetThreadContext(h,&c) && c.Dr0==uintptr_t(&watched) && (c.Dr7&1) &&
         ((c.Dr7>>18)&3)==2;
   ResumeThread(h);
  }
  if(h)CloseHandle(h);
  SetEvent(finish);
  worker.join();
  ApplyWatchToHostThread(t,0x1000,8);
  CloseHandle(ready);CloseHandle(finish);
  if(!valid)return 2;
 }
 return 0;
}
'''
result=run_native(source, prefix='tcnyc-watch-', needs_crt=True)
assert result == 0, f'watchpoint installation regression {result}'
print('PASS: watchpoint installs with 8-byte coverage on live workers and skips exited workers')
