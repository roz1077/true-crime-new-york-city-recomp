"""Splice the PC character mesh into the Xbox character container.

Instead of replacing the whole PC file (which carries uninitialised runtime
fields and crashes the loader), keep the Xbox container and its texture tables
intact and replace only the mesh chunk (id 0) with the PC one. The chunk table
offsets after the mesh are shifted by the size difference.

    python scripts/pc_mesh_splice.py Char/Bodies/Body_M_Casual
    python scripts/pc_mesh_splice.py --restore
"""

import argparse
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PC_DATA = Path(r"D:\Old PC Games\True Crime - New York City\Data")
XB_DATA = ROOT / "assets" / "data"
BACKUP = ROOT / "out" / "asset-backup" / "mesh-splice"


def entries(data):
    out = []
    for i in range(12):
        kind, count, off = struct.unpack_from("<HHI", data, 0x0C + i * 8)
        out.append((i, kind, count, off))
    return out


def mesh_span(data):
    ents = entries(data)
    mesh = next((e for e in ents if e[1] == 0), None)
    if not mesh:
        return None
    off = mesh[3]
    later = [e[3] for e in ents if e[3] > off and e[0] != mesh[0]]
    if not later:
        return None
    return off, min(later)


def splice(rel):
    pc_path = PC_DATA / (rel + ".pcf")
    xb_path = XB_DATA / (rel + ".xb2")
    if not pc_path.is_file() or not xb_path.is_file():
        print(f"missing {pc_path} or {xb_path}")
        return False
    pc = pc_path.read_bytes()
    xb = bytearray(xb_path.read_bytes())
    pspan = mesh_span(pc)
    xspan = mesh_span(xb)
    if not pspan or not xspan:
        print(f"{rel}: no mesh chunk")
        return False
    poff, pend = pspan
    xoff, xend = xspan
    mesh = pc[poff:pend]
    delta = len(mesh) - (xend - xoff)
    out = bytearray()
    out += xb[:xoff]
    out += mesh
    out += xb[xend:]
    for i, kind, count, off in entries(xb):
        if off > xoff:
            struct.pack_into("<I", out, 0x0C + i * 8 + 4, off + delta)
    backup = BACKUP / (rel + ".xb2")
    backup.parent.mkdir(parents=True, exist_ok=True)
    if not backup.is_file():
        shutil.copy2(xb_path, backup)
    xb_path.write_bytes(out)
    print(f"{rel}: mesh {xend - xoff} -> {len(mesh)} bytes, file {len(xb)} -> {len(out)}")
    return True


def restore():
    n = 0
    for f in BACKUP.rglob("*.xb2"):
        rel = f.relative_to(BACKUP)
        shutil.copy2(f, XB_DATA / rel)
        n += 1
    print(f"restored {n} files")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--restore", action="store_true")
    a = ap.parse_args()
    if a.restore:
        restore()
    else:
        ok = all(splice(p.replace("\\", "/")) for p in a.paths)
        sys.exit(0 if ok else 1)
