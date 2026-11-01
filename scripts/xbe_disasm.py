"""Disassemble code from an XBE by virtual address.

Usage: python xbe_disasm.py <default.xbe> <va_hex> [count=40]   (stops after `count` instructions or at int3 padding)
       python xbe_disasm.py <default.xbe> --callers <va_hex>   (lists direct call sites of a function)
"""
import struct
import sys

import capstone


def load(path):
    d = open(path, "rb").read()
    base = struct.unpack_from("<I", d, 0x104)[0]
    ns, sh = struct.unpack_from("<II", d, 0x11C)
    secs = []
    for i in range(ns):
        _, va, vs, ra, rs, _ = struct.unpack_from("<IIIIII", d, sh - base + i * 56)
        secs.append((va, vs, ra, rs))
    return d, secs


def read(d, secs, va, n):
    for sva, vs, ra, rs in secs:
        if sva <= va < sva + rs:
            off = ra + va - sva
            return d[off: off + min(n, sva + rs - va)]
    raise SystemExit(f"{va:#x} is not in a section")


def main():
    d, secs = load(sys.argv[1])
    md = capstone.Cs(capstone.CS_ARCH_X86, capstone.CS_MODE_32)
    md.syntax = capstone.CS_OPT_SYNTAX_INTEL
    if sys.argv[2] == "--targets":  # direct call targets in [lo, hi) with their call counts
        lo, hi = int(sys.argv[3], 16), int(sys.argv[4], 16)
        counts = {}
        for sva, vs, ra, rs in secs:
            code = d[ra: ra + rs]
            for i in range(len(code) - 5):
                if code[i] == 0xE8:
                    t = (sva + i + 5 + struct.unpack_from("<i", code, i + 1)[0]) & 0xFFFFFFFF
                    if lo <= t < hi:
                        counts[t] = counts.get(t, 0) + 1
        for t in sorted(counts):
            print(f"{t:08X} {counts[t]}")
        return
    if sys.argv[2] == "--callers":
        target = int(sys.argv[3], 16)
        for sva, vs, ra, rs in secs:
            code = d[ra: ra + rs]
            for i in range(len(code) - 5):
                if code[i] == 0xE8 and (sva + i + 5 + struct.unpack_from("<i", code, i + 1)[0]) & 0xFFFFFFFF == target:
                    print(f"{sva + i:08X}")
        return
    va = int(sys.argv[2], 16)
    count = int(sys.argv[3]) if len(sys.argv) > 3 else 40
    for n, ins in enumerate(md.disasm(read(d, secs, va, count * 15), va)):
        if n >= count or ins.mnemonic == "int3":
            break
        print(f"{ins.address:08X}  {ins.mnemonic:8s} {ins.op_str}")


if __name__ == "__main__":
    main()
