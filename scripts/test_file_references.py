"""Exercise actual file-object bindings and pointer-reference lifetime (Windows/LLVM/MSVC)."""
from pathlib import Path
from native_test import run_native
root=Path(__file__).resolve().parents[1]
obj=(root/'sdk/runtime/kernel/objects.cpp').read_text();files=(root/'sdk/runtime/kernel/files.cpp').read_text();kh=(root/'sdk/runtime/kernel/kernel.h').read_text()
def fn(sig):
 a=obj.find(sig)
 return obj[a:obj.index('\n}\n',a)+3] if a>=0 else ''
model=kh[kh.index('enum class ObjType'):kh.index('uint32_t NewHandle(')]
globals_=obj[obj.index('std::mutex g_handles_lock;'):obj.index('std::mutex g_names_lock;')]
fileclass=files[files.index('struct File : Object {'):files.index('\nstruct Link : Object')]
sync=files[files.index('bool SyncStream'):files.index('\n}\n',files.index('bool SyncStream'))+3]
open_host=files[files.index('FILE* OpenHostFile'):files.index('\n}\n',files.index('FILE* OpenHostFile'))+3]
registry=files[files.index('std::mutex g_open_lock;'):files.index('void RegisterFile')]
register=files[files.index('void RegisterFile'):files.index('\n}\n',files.index('void RegisterFile'))+3]
flush=files[files.index('void FlushOpenFiles()'):files.index('\n}\n',files.index('void FlushOpenFiles'))+3]
source=r'''
#include <array>
#include <atomic>
#include <cstdint>
#include <cstring>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <io.h>
#include <iterator>
#include <memory>
#include <mutex>
#include <string>
#include <vector>
#include <unordered_map>
namespace fs=std::filesystem;
struct Waiter {};
struct Disc { struct Entry {}; };
unsigned freed=0;
uint8_t memory[1<<20]{};
namespace mem { void FreeSystem(uint32_t) { ++freed; }
 bool IsCommitted(uint32_t) { return true; } }
template<class T> T& G(uint32_t a) { return *reinterpret_cast<T*>(memory+a); }
void W32(uint32_t a,uint32_t v) { G<uint32_t>(a)=v; }
uint32_t R32(uint32_t a) { return G<uint32_t>(a); }
#define LOGW(...) ((void)0)
struct Thread { int* ctx; };
Thread* Current() { return nullptr; }
void LogGuestStack(int&,int) {}
'''+model+r'''
ObjectRef CurrentRef() { return {}; }
'''+globals_+fn('static void Stamp(')+fn('uint32_t NewHandle(')+fn('ObjectRef FromHandle(')+fn('bool CloseHandle(')+fn('uint32_t DuplicateHandle(')+fn('void Bind(')+fn('ObjectRef FromPointer(')
for sig,name in [('void ReferenceObject(','ReferenceObject'),('void DereferenceObject(','DereferenceObject'),('void Unbind(','Unbind')]:
 source+=fn(sig) or f'void {name}(uint32_t) {{}}\n'
source+=fileclass+sync+registry+register+open_host+flush+r'''
int destroyed=0;
struct TrackedFile:File { ~TrackedFile() { ++destroyed; } };
ObjectRef make_file(unsigned address) {
 auto f=std::make_shared<TrackedFile>();f->fp=std::tmpfile();
 if (!f->fp) return {};
 RegisterFile(f);Bind(f,address);return f;
}
extern "C" __declspec(dllexport) int run() {
 auto f=make_file(64); if(!f)return 1;
 std::weak_ptr<Object> weak=f;
 auto h=NewHandle(f), h2=DuplicateHandle(h);
 ReferenceObject(64);ReferenceObject(64);f.reset();
 CloseHandle(h);CloseHandle(h2);
 if(weak.expired())return 2;
 DereferenceObject(64);if(weak.expired())return 3;
 DereferenceObject(64);if(!weak.expired())return 4;
 if(destroyed!=1 || freed!=1)return 5;
 // A handle still owns the file after the final pointer reference is dropped.
 f=make_file(128);if(!f)return 6;h=NewHandle(f);weak=f;
 ReferenceObject(128);f.reset();DereferenceObject(128);
 if(weak.expired() || !FromPointer(128))return 7;
 CloseHandle(h);if(!weak.expired())return 8;
 // Exceed the CRT's default 512-stream limit through sequential balanced use.
 for(unsigned i=0;i<1100;++i) {
  unsigned a=256+i*32;f=make_file(a);if(!f)return 9;
  weak=f;h=NewHandle(f);ReferenceObject(a);f.reset();
  CloseHandle(h);DereferenceObject(a);if(!weak.expired())return 10;
 }
 if(destroyed!=1102 || freed!=1102)return 11;
 // A writable stream is committed by FlushOpenFiles without closing it. The
 // bytes must already be visible to a second reader when the function returns.
 std::error_code ec;
 auto path=std::filesystem::temp_directory_path()/"tcnyc-flush-test.bin";
 std::filesystem::remove(path,ec);
 auto durable=std::make_shared<TrackedFile>();
 durable->host=path;
 durable->writable=true;
 durable->fp=OpenHostFile(path,"wb");
 if(!durable->fp)return 12;
 RegisterFile(durable);
 const char text[]="durable";
 if(std::fwrite(text,1,sizeof(text)-1,durable->fp)!=sizeof(text)-1)return 13;
 FlushOpenFiles();
 std::ifstream in(path,std::ios::binary);
 std::string got((std::istreambuf_iterator<char>(in)),{});
 if(got!="durable")return 14;
 durable.reset();
 std::filesystem::remove(path,ec);
 return 0;
}
'''
rc = run_native(source, prefix="tcnyc-file_references-", needs_crt=True)
assert rc == 0, f"File reference regression {rc} (4 = closed file retained by binding)"
print('PASS: balanced references, duplicate handles, 1100 open/close cycles and unclosed save flush')
