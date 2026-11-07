# Flat PE of an XBE for Ghidra: the image at its load address, recompiler functions and config names as exports.
# usage, from the game repo after a build: python sdk/tools/symbols/xbe2pe.py config/game.toml game.exe
import re
import struct
import sys
import tomllib
from pathlib import Path


def load(path, cfg, open_paths=None):
    open_paths = set() if open_paths is None else open_paths
    key = path.resolve()
    if key in open_paths:
        raise SystemExit(f"{path}: include cycle")
    open_paths.add(key)
    try:
        t = tomllib.loads(path.read_text())
        for inc in t.get('includes', []):
            load(path.parent / inc, cfg, open_paths)
    finally:
        open_paths.remove(key)
    for k in ('xbe', 'functions', 'symbols'):
        cfg.setdefault(k, {}).update(t.get(k, {}))
    return cfg


def u32(data, offset):
    if offset < 0 or offset + 4 > len(data):
        raise SystemExit(f"XBE is truncated at 0x{offset:X}")
    return struct.unpack_from('<I', data, offset)[0]


cfg_path, out = Path(sys.argv[1]), Path(sys.argv[2])
cfg = load(cfg_path, {})
root = cfg_path.parent.parent
d = (root / cfg['xbe']['path']).read_bytes()
if len(d) < 0x180 or d[:4] != b'XBEH':
    raise SystemExit("not an XBE")
# XBE header: base, headers size, image size; the entry is xored with the retail key, else the debug key
base, hsize, isize = u32(d, 0x104), u32(d, 0x108), u32(d, 0x10C)
if not base or hsize < 0x178 or hsize > len(d) or isize < hsize or base + isize > 0x100000000:
    raise SystemExit("XBE header sizes are invalid")
# a flat image much larger than its file is a truncated or hostile header, not a game
if isize > min(512 << 20, max(64 << 20, len(d) * 8)):
    raise SystemExit(f"XBE image size 0x{isize:X} is too large for its 0x{len(d):X} byte file")
entry_raw = u32(d, 0x128)
entry = entry_raw ^ 0xA8FC57AB
if not base <= entry < base + isize:
    entry = entry_raw ^ 0x94859D4B
if not base <= entry < base + isize:
    raise SystemExit("entry point key not recognised")

img = bytearray(isize)
img[:hsize] = d[:hsize]
# section headers are 56 bytes, virtual address, virtual size, raw address and raw size from +4
nsec = u32(d, 0x11C)
sec_addr = u32(d, 0x120)
if nsec > 1024 or (nsec and sec_addr < base):
    raise SystemExit("section table is invalid")
sec_off = sec_addr - base if nsec else 0
if nsec and (sec_off + nsec * 56 > len(d) or sec_off + nsec * 56 > hsize):
    raise SystemExit("section table is outside the header")
for i in range(nsec):
    off = sec_off + i * 56
    va, vs, raw, rs = struct.unpack_from('<IIII', d, off + 4)
    if rs > vs:
        raise SystemExit(f"section {i} raw data is larger than its virtual size")
    if rs:
        if va < base or va + rs > base + isize or raw + rs > len(d):
            raise SystemExit(f"section {i} data is outside the image")
        img[va - base:va - base + rs] = d[raw:raw + rs]
    elif vs and (va < base or va + vs > base + isize):
        raise SystemExit(f"section {i} range is outside the image")

names = {}
mapping = root / cfg['xbe']['out'] / 'x86_func_mapping.cpp'
if mapping.exists():
    for a in re.findall(r'\{0x([0-9A-F]+)u, sub_', mapping.read_text()):
        names[int(a, 16)] = 'sub_%08X' % int(a, 16)
for va, name in cfg['functions'].items():
    names[int(va, 0)] = name.replace('::', '_')
for name, va in cfg['symbols'].items():
    if va:
        names[va] = name

exp = sorted(names)
nm = [names[a].encode() for a in exp]
order = sorted(range(len(exp)), key=lambda i: nm[i])
# export directory (40 bytes), address table, name pointers, ordinals and names, page aligned after the image
ert = (isize + 0xFFF) & ~0xFFF
eat = ert + 40
npt = eat + 4 * len(exp)
ot = npt + 4 * len(exp)
st = ot + 2 * len(exp)
strs, off = bytearray(b'image.exe\0'), {}
for i in range(len(exp)):
    off[i] = st + len(strs)
    strs += nm[i] + b'\0'
edata = struct.pack('<IIHHIIIIIII', 0, 0, 0, 0, st, 1, len(exp), len(exp), eat, npt, ot)
edata += b''.join(struct.pack('<I', a - base) for a in exp)
edata += b''.join(struct.pack('<I', off[i]) for i in order)
edata += b''.join(struct.pack('<H', i) for i in order)
edata += strs
esz = (len(edata) + 0xFFF) & ~0xFFF

opt = struct.pack('<HBBIIIIIIIIIHHHHHHIIIIHHIIIIII', 0x10B, 14, 0, isize, 0, 0, entry - base, 0x1000, 0x1000, base,
                  0x1000, 0x1000, 4, 0, 0, 0, 4, 0, 0, ert + esz, 0x1000, 0, 2, 0, 0x100000, 0x1000, 0x100000, 0x1000,
                  0, 16)
opt += struct.pack('<II', ert, len(edata)) + bytes(8 * 15)
coff = struct.pack('<HHIIIHH', 0x14C, 2, 0, 0, 0, len(opt), 0x0102)
flat = struct.pack('<8sIIIIIIHHI', b'.flat', isize - 0x1000, 0x1000, isize - 0x1000, 0x1000, 0, 0, 0, 0, 0xE0000060)
exps = struct.pack('<8sIIIIIIHHI', b'.edata', len(edata), ert, esz, ert, 0, 0, 0, 0, 0x40000040)
dos = bytearray(0x40)
dos[:2] = b'MZ'
struct.pack_into('<I', dos, 0x3C, 0x40)
head = bytes(dos) + b'PE\0\0' + coff + opt + flat + exps

f = bytearray(ert + esz)
f[0x1000:isize] = img[0x1000:]
f[:len(head)] = head
f[ert:ert + len(edata)] = edata
out.write_bytes(f)
print(out, len(exp), 'exports')
