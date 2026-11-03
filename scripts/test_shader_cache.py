"""Exercise actual DXBC cache IO, corruption rejection and recovery using real compiled bytecode."""
from pathlib import Path
import sys
from native_test import run_native

root = Path(__file__).resolve().parents[1]
source = (root / 'sdk/runtime/gpu/d3d12.cpp').read_text()
start = source.index('constexpr uint64_t kShaderCacheMagic')
cache = source[start:source.index('const std::filesystem::path& ShaderCacheDirectory()', start)]
if '--baseline-fingerprint' in sys.argv:
    cache = cache.replace('HMODULE compiler = GetModuleHandleW(D3DCOMPILER_DLL_W);',
        'HMODULE compiler = nullptr;\n'
        '    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,\n'
        '        reinterpret_cast<LPCWSTR>(&D3DCompile), &compiler);')
source = (root / 'sdk/runtime/gpu/shaders.cpp').read_text()
start = source.index('uint64_t Hash(')
hash_code = source[start:source.index('\n}\n', start) + 3]

test = r'''
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <d3dcompiler.h>
#include <d3d11shader.h>
#include <wrl/client.h>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>
#include <vector>
#pragma comment(lib,"d3dcompiler.lib")
#pragma comment(lib,"dxguid.lib")
uint64_t Hash(const void*,size_t,uint64_t=1469598103934665603ull);
''' + hash_code + cache + r'''
extern "C" __declspec(dllexport) int run() {
 namespace fs=std::filesystem;
 using Microsoft::WRL::ComPtr;
 auto dir=fs::temp_directory_path()/std::to_string(GetCurrentProcessId());
 dir += "-tcnyc-cache-check";
 auto file=dir/"shader.dxbc",blocked=dir/"blocked";
 // The fingerprint must belong to the compiler DLL, not the harness/executable's import thunk.
 wchar_t compiler_path[MAX_PATH];
 auto module=GetModuleHandleW(D3DCOMPILER_DLL_W);
 if(!module||!GetModuleFileNameW(module,compiler_path,MAX_PATH))return 13;
 std::ifstream compiler(std::filesystem::path(compiler_path),std::ios::binary|std::ios::ate);
 if(!compiler)return 14;
 std::vector<uint8_t> compiler_bytes(static_cast<size_t>(compiler.tellg()));compiler.seekg(0);
 compiler.read(reinterpret_cast<char*>(compiler_bytes.data()),std::streamsize(compiler_bytes.size()));
 uint64_t expected=Hash(compiler_bytes.data(),compiler_bytes.size());
 expected=Hash(&kShaderCompileFlags,sizeof(kShaderCompileFlags),expected);
 if(ShaderCompilerFingerprint()!=expected)return 15;
 struct Cleanup { fs::path file,blocked,dir; ~Cleanup(){ std::error_code ec;
  fs::remove(file,ec);fs::remove(blocked,ec);fs::remove(dir,ec); }
 } cleanup{file,blocked,dir};
 if(!ReadShaderCache(file,42).empty())return 1;
 const char* shader="float4 main():SV_Target{return float4(.25,.5,.75,1);}";
 ComPtr<ID3DBlob> blob,error;
 if(FAILED(D3DCompile(shader,std::strlen(shader),nullptr,nullptr,nullptr,"main","ps_5_0",
                     kShaderCompileFlags,0,&blob,&error)))return 2;
 WriteShaderCache(file,42,blob->GetBufferPointer(),blob->GetBufferSize());
 auto bytes=ReadShaderCache(file,42);
 if(bytes.size()!=blob->GetBufferSize()||std::memcmp(bytes.data(),blob->GetBufferPointer(),bytes.size()))return 3;
 ComPtr<ID3D11ShaderReflection> reflection;
 if(FAILED(D3DReflect(bytes.data(),bytes.size(),IID_PPV_ARGS(&reflection))))return 4;
 if(!ReadShaderCache(file,43).empty())return 5;
 { std::fstream f(file,std::ios::binary|std::ios::in|std::ios::out);
   f.seekp(sizeof(ShaderCacheHeader)+20);char corrupt=42;f.write(&corrupt,1); }
 if(!ReadShaderCache(file,42).empty())return 6;
 // Recovery replaces the corrupt entry atomically rather than failing on an existing file.
 WriteShaderCache(file,42,blob->GetBufferPointer(),blob->GetBufferSize());
 if(ReadShaderCache(file,42).size()!=bytes.size())return 7;
 { std::ofstream f(file,std::ios::binary|std::ios::app);f.put(0); }
 if(!ReadShaderCache(file,42).empty())return 8;
 { std::ofstream f(file,std::ios::binary|std::ios::trunc);f.put(0); }
 if(!ReadShaderCache(file,42).empty())return 9;
 { ShaderCacheHeader h{kShaderCacheMagic,42,kShaderCacheMaxBytes+1,0};
   std::ofstream f(file,std::ios::binary|std::ios::trunc);f.write(reinterpret_cast<char*>(&h),sizeof(h)); }
 if(!ReadShaderCache(file,42).empty())return 10;
 { std::ofstream f(blocked);f.put(0); }
 WriteShaderCache(blocked/"shader.dxbc",42,blob->GetBufferPointer(),blob->GetBufferSize());
 if(fs::exists(blocked/"shader.dxbc"))return 11;
 for(auto& e:fs::directory_iterator(dir))if(e.path().extension()==".tmp")return 12;
 return 0;
}
'''
rc = run_native(test, prefix='tcnyc-shader-cache-', needs_crt=True)
assert rc == 0, f'Shader cache regression {rc}'
print('PASS: real DXBC round-trip/reflection, key checks, corrupt/truncated/oversized rejection and write recovery')
