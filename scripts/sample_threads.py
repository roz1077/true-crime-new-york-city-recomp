"""Sample where a running recompiled game spends its time, per thread.

Usage: python sample_threads.py <exe name> <game dll path> [seconds=5] [rate_hz=200]

Suspends each thread briefly, reads RIP and scans the top of its host stack for return addresses inside the game
DLL, and maps them to the DLL's exported names (sub_XXXXXXXX = the guest function at that Xbox address).
Prints, per thread, the most common innermost function and the most common call chains.
"""
import collections
import ctypes
import ctypes.wintypes as wt
import struct
import sys
import time

k32 = ctypes.WinDLL("kernel32", use_last_error=True)
psapi = ctypes.WinDLL("psapi", use_last_error=True)

TH32CS_SNAPPROCESS, TH32CS_SNAPTHREAD = 0x2, 0x4
THREAD_ALL = 0x001A | 0x0040 | 0x0008  # suspend/resume, get context, query
PROCESS_READ = 0x0010 | 0x0400  # vm read, query info
CONTEXT_AMD64, CONTEXT_CONTROL, CONTEXT_INTEGER = 0x100000, 0x1, 0x2


class PROCESSENTRY32W(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ProcessID", wt.DWORD),
                ("th32DefaultHeapID", ctypes.c_void_p), ("th32ModuleID", wt.DWORD), ("cntThreads", wt.DWORD),
                ("th32ParentProcessID", wt.DWORD), ("pcPriClassBase", ctypes.c_long), ("dwFlags", wt.DWORD),
                ("szExeFile", ctypes.c_wchar * 260)]


class THREADENTRY32(ctypes.Structure):
    _fields_ = [("dwSize", wt.DWORD), ("cntUsage", wt.DWORD), ("th32ThreadID", wt.DWORD),
                ("th32OwnerProcessID", wt.DWORD), ("tpBasePri", ctypes.c_long), ("tpDeltaPri", ctypes.c_long),
                ("dwFlags", wt.DWORD)]


def find_pid(name):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0)
    e = PROCESSENTRY32W()
    e.dwSize = ctypes.sizeof(e)
    ok = k32.Process32FirstW(snap, ctypes.byref(e))
    while ok:
        if e.szExeFile.lower() == name.lower():
            k32.CloseHandle(snap)
            return e.th32ProcessID
        ok = k32.Process32NextW(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    raise SystemExit(f"{name} is not running")


def threads(pid):
    snap = k32.CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0)
    e = THREADENTRY32()
    e.dwSize = ctypes.sizeof(e)
    out = []
    ok = k32.Thread32First(snap, ctypes.byref(e))
    while ok:
        if e.th32OwnerProcessID == pid:
            out.append(e.th32ThreadID)
        ok = k32.Thread32Next(snap, ctypes.byref(e))
    k32.CloseHandle(snap)
    return out


def module_base(hproc, dll_name):
    mods = (ctypes.c_void_p * 1024)()
    need = wt.DWORD()
    psapi.EnumProcessModulesEx(hproc, mods, ctypes.sizeof(mods), ctypes.byref(need), 3)
    buf = ctypes.create_unicode_buffer(260)
    for m in mods[: need.value // ctypes.sizeof(ctypes.c_void_p)]:
        psapi.GetModuleBaseNameW(hproc, ctypes.c_void_p(m), buf, 260)
        if buf.value.lower() == dll_name.lower():
            return m
    raise SystemExit(f"{dll_name} is not loaded")


def exports(path):
    """(sorted rvas, names) of the DLL's named exports, plus its image size."""
    d = open(path, "rb").read()
    pe = struct.unpack_from("<I", d, 0x3C)[0]
    nsec = struct.unpack_from("<H", d, pe + 6)[0]
    opt = pe + 24
    size_image = struct.unpack_from("<I", d, opt + 56)[0]
    exp_rva = struct.unpack_from("<I", d, opt + 112)[0]
    secs = []
    sh = opt + struct.unpack_from("<H", d, pe + 20)[0]
    for i in range(nsec):
        vs, va, rs, ra = struct.unpack_from("<IIII", d, sh + i * 40 + 8)
        secs.append((va, max(vs, rs), ra))

    def off(rva):
        for va, sz, ra in secs:
            if va <= rva < va + sz:
                return ra + rva - va
        raise ValueError(hex(rva))

    e = off(exp_rva)
    nfuncs, nnames, afuncs, anames, aords = struct.unpack_from("<IIIII", d, e + 20)
    pairs = []
    for i in range(nnames):
        name_rva = struct.unpack_from("<I", d, off(anames) + i * 4)[0]
        ordinal = struct.unpack_from("<H", d, off(aords) + i * 2)[0]
        rva = struct.unpack_from("<I", d, off(afuncs) + ordinal * 4)[0]
        n = d[off(name_rva): d.index(b"\0", off(name_rva))].decode()
        pairs.append((rva, n))
    pairs.sort()
    return [p[0] for p in pairs], [p[1] for p in pairs], size_image


class CONTEXT(ctypes.Structure):
    _pack_ = 16
    _fields_ = [("P", ctypes.c_uint64 * 6), ("ContextFlags", wt.DWORD), ("MxCsr", wt.DWORD),
                ("Seg", ctypes.c_uint16 * 6), ("EFlags", wt.DWORD), ("Dr", ctypes.c_uint64 * 6),
                ("Rax", ctypes.c_uint64), ("Rcx", ctypes.c_uint64), ("Rdx", ctypes.c_uint64), ("Rbx", ctypes.c_uint64),
                ("Rsp", ctypes.c_uint64), ("Rbp", ctypes.c_uint64), ("Rsi", ctypes.c_uint64), ("Rdi", ctypes.c_uint64),
                ("R", ctypes.c_uint64 * 8), ("Rip", ctypes.c_uint64), ("Rest", ctypes.c_byte * 976)]


def main():
    exe, dll = sys.argv[1], sys.argv[2]
    seconds = float(sys.argv[3]) if len(sys.argv) > 3 else 5
    rate = float(sys.argv[4]) if len(sys.argv) > 4 else 200
    rvas, names, size = exports(dll)
    import bisect
    import os

    pid = find_pid(exe)
    hproc = k32.OpenProcess(PROCESS_READ, False, pid)
    base = module_base(hproc, os.path.basename(dll))

    def name(addr):
        if not (base <= addr < base + size):
            return None
        i = bisect.bisect_right(rvas, addr - base) - 1
        return names[i] if i >= 0 else None

    # host module ranges, to say where a thread outside game code is
    mods = (ctypes.c_void_p * 1024)()
    need = wt.DWORD()
    psapi.EnumProcessModulesEx(hproc, mods, ctypes.sizeof(mods), ctypes.byref(need), 3)

    class MODULEINFO(ctypes.Structure):
        _fields_ = [("base", ctypes.c_void_p), ("size", wt.DWORD), ("entry", ctypes.c_void_p)]

    ranges = []
    nbuf = ctypes.create_unicode_buffer(260)
    for m in mods[: need.value // ctypes.sizeof(ctypes.c_void_p)]:
        mi = MODULEINFO()
        psapi.GetModuleInformation(hproc, ctypes.c_void_p(m), ctypes.byref(mi), ctypes.sizeof(mi))
        psapi.GetModuleBaseNameW(hproc, ctypes.c_void_p(m), nbuf, 260)
        ranges.append((m, m + mi.size, nbuf.value))

    def host_where(addr):
        for lo, hi, n in ranges:
            if lo <= addr < hi:
                return f"{n}+{addr - lo:X}" if n.lower().startswith("truecrime") else n
        return "?"

    # host symbols from the exe's PDB through dbghelp, for runtime frames
    dbg = ctypes.WinDLL("dbghelp")
    dbg.SymSetOptions(0x2 | 0x4)  # undecorate, deferred loads
    dbg.SymInitializeW(hproc, ctypes.c_wchar_p(os.path.dirname(os.path.abspath(dll))), True)

    class SYMBOL_INFO(ctypes.Structure):
        _fields_ = [("SizeOfStruct", wt.ULONG), ("TypeIndex", wt.ULONG), ("Reserved", ctypes.c_uint64 * 2),
                    ("Index", wt.ULONG), ("Size", wt.ULONG), ("ModBase", ctypes.c_uint64), ("Flags", wt.ULONG),
                    ("Value", ctypes.c_uint64), ("Address", ctypes.c_uint64), ("Register", wt.ULONG),
                    ("Scope", wt.ULONG), ("Tag", wt.ULONG), ("NameLen", wt.ULONG), ("MaxNameLen", wt.ULONG),
                    ("Name", ctypes.c_char * 256)]

    sym_cache = {}

    def host_sym(addr):
        if addr in sym_cache:
            return sym_cache[addr]
        s = SYMBOL_INFO()
        s.SizeOfStruct = ctypes.sizeof(SYMBOL_INFO) - 256 + 1
        s.MaxNameLen = 255
        disp = ctypes.c_uint64()
        r = s.Name.decode(errors="replace") if dbg.SymFromAddr(hproc, ctypes.c_uint64(addr), ctypes.byref(disp),
                                                               ctypes.byref(s)) else None
        sym_cache[addr] = r
        return r

    exe_ranges = [(lo, hi) for lo, hi, n in ranges if n.lower().endswith(".exe")]
    host_chains = collections.defaultdict(collections.Counter)
    rip_mod = collections.defaultdict(collections.Counter)
    handles = {}
    inner = collections.defaultdict(collections.Counter)
    chains = collections.defaultdict(collections.Counter)
    total = collections.Counter()
    ctx = CONTEXT()
    stack = (ctypes.c_uint64 * 16384)()
    got = ctypes.c_size_t()
    end = time.time() + seconds
    while time.time() < end:
        for tid in threads(pid):
            h = handles.get(tid)
            if h is None:
                h = handles[tid] = k32.OpenThread(THREAD_ALL, False, tid)
            if not h or k32.SuspendThread(h) == 0xFFFFFFFF:
                continue
            ctx.ContextFlags = CONTEXT_AMD64 | CONTEXT_CONTROL | CONTEXT_INTEGER
            ok = k32.GetThreadContext(h, ctypes.byref(ctx))
            frames = []
            if ok:
                frames.append(name(ctx.Rip) or "host")
                rip_mod[tid][host_where(ctx.Rip)] += 1
                # read page by page: one unreadable page fails a whole ReadProcessMemory
                page = 4096 - (ctx.Rsp & 4095)
                addr, words = ctx.Rsp, []
                while len(words) < 16384:
                    if not k32.ReadProcessMemory(hproc, ctypes.c_void_p(addr), stack, page, ctypes.byref(got)):
                        break
                    words.extend(stack[: got.value // 8])
                    addr += page
                    page = 4096
                hframes = []
                for v in words[:4096]:
                    if any(lo <= v < hi for lo, hi in exe_ranges):
                        hs = host_sym(v)
                        if hs and (not hframes or hframes[-1] != hs):
                            hframes.append(hs)
                        if len(hframes) >= 8:
                            break
                host_chains[tid][" < ".join(hframes)] += 1
                for v in words:
                    n = name(v)
                    if n and (not frames or frames[-1] != n):
                        frames.append(n)
                    if len(frames) >= 10:
                        break
            k32.ResumeThread(h)
            if not ok:
                continue
            total[tid] += 1
            guest = [f for f in frames if f != "host"]
            inner[tid][guest[0] if guest else "host only"] += 1
            chains[tid][" < ".join(guest[:6]) if guest else "host only"] += 1
        time.sleep(1 / rate)
    # one raw look at the busiest game thread: every stack value inside a module, nearest first
    for busiest in (list(total) if os.environ.get("SAMPLE_ALL_RAW") else [max(total, key=lambda t: sum(c for f, c in inner[t].items() if f != "host only"), default=None)]):
      if busiest is not None and busiest in handles:
        h = handles[busiest]
        k32.SuspendThread(h)
        ctx.ContextFlags = CONTEXT_AMD64 | CONTEXT_CONTROL | CONTEXT_INTEGER
        k32.GetThreadContext(h, ctypes.byref(ctx))
        raw = []
        if k32.ReadProcessMemory(hproc, ctypes.c_void_p(ctx.Rsp), stack, 4096 - (ctx.Rsp & 4095), ctypes.byref(got)):
            raw = list(stack[: got.value // 8])
        k32.ResumeThread(h)
        print(f"== raw stack of thread {busiest}: RIP {host_where(ctx.Rip)} {host_sym(ctx.Rip) or ''}")
        shown = 0
        for i, v in enumerate(raw):
            w = host_where(v)
            if w != "?":
                print(f"  rsp+{i * 8:04X}  {v:016X}  {w}  {name(v) or host_sym(v) or ''}")
                shown += 1
                if shown >= 30:
                    break
    for tid, n in total.most_common():
        print(f"\n== thread {tid}: {n} samples, RIP in " +
              ", ".join(f"{m} {100 * c / n:.0f}%" for m, c in rip_mod[tid].most_common(3)))
        for f, c in inner[tid].most_common(6):
            print(f"  {100 * c / n:5.1f}%  {f}")
        print("  chains:")
        for f, c in chains[tid].most_common(5):
            print(f"  {100 * c / n:5.1f}%  {f}")
        print("  runtime (exe) frames:")
        for f, c in host_chains[tid].most_common(4):
            print(f"  {100 * c / n:5.1f}%  {f}")


if __name__ == "__main__":
    main()
