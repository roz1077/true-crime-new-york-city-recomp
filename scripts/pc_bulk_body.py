"""Build PC-mesh hybrids for every character body and head.

The Xbox and PC character containers have the same layout. The PC files leave
the small per-texture runtime fields as 0xCD, which the Xbox loader treats as a
bad pointer. This copies those fields from the same-named Xbox file and keeps
all PC offsets/sizes, producing a loadable higher-detail character.

Originals are copied to out/asset-backup/bulk before anything is replaced.

    python scripts/pc_bulk_body.py apply
    python scripts/pc_bulk_body.py restore
    python scripts/pc_bulk_body.py status
"""

import argparse
import json
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PC_DATA = Path(r"D:\Old PC Games\True Crime - New York City\Data")
XB_DATA = ROOT / "assets" / "data"
BACKUP = ROOT / "out" / "asset-backup" / "bulk"
MANIFEST = ROOT / "out" / "asset-backup" / "bulk-manifest.json"
SETS = ("LResChar", "Char")
SUBS = ("Bodies", "Heads")


def tex_table(data):
    for i in range(12):
        kind, count, off = struct.unpack_from("<HHI", data, 0x0C + i * 8)
        if kind == 2:
            return count, off
    return 0, 0


def sub_blocks(data):
    for i in range(12):
        kind, count, off = struct.unpack_from("<HHI", data, 0x0C + i * 8)
        if kind == 0:
            return count
    return 0


def hybrid(pc_path, xb_path):
    pc = bytearray(pc_path.read_bytes())
    xb = xb_path.read_bytes()
    pc_count, pc_off = tex_table(pc)
    xb_count, xb_off = tex_table(xb)
    if not pc_count or pc_count != xb_count:
        return None, f"texture counts {pc_count} vs {xb_count}"
    if sub_blocks(pc) != sub_blocks(xb):
        return None, f"sub-blocks {sub_blocks(pc)} vs {sub_blocks(xb)}"
    for i in range(pc_count):
        p = pc_off + i * 0x28
        x = xb_off + i * 0x24
        pc[p + 0x08:p + 0x0C] = xb[x + 0x08:x + 0x0C]
        pc[p + 0x1C:p + 0x24] = xb[x + 0x1C:x + 0x24]
        pc[p + 0x24:p + 0x28] = b"\0\0\0\0"
    return bytes(pc), None


def apply(sets, subs):
    done, skipped = [], []
    for set_name in sets:
        for sub in subs:
            pc_dir = PC_DATA / set_name / sub
            xb_dir = XB_DATA / set_name / sub
            if not pc_dir.is_dir() or not xb_dir.is_dir():
                continue
            for pc in sorted(pc_dir.glob("*.pcf")):
                xb = xb_dir / (pc.stem + ".xb2")
                if not xb.is_file():
                    skipped.append((set_name, sub, pc.stem, "no xbox file"))
                    continue
                data, why = hybrid(pc, xb)
                if data is None:
                    skipped.append((set_name, sub, pc.stem, why))
                    continue
                rel = Path(set_name) / sub / xb.name
                backup = BACKUP / rel
                backup.parent.mkdir(parents=True, exist_ok=True)
                if not backup.is_file():
                    shutil.copy2(xb, backup)
                xb.write_bytes(data)
                done.append(str(rel))
    MANIFEST.parent.mkdir(parents=True, exist_ok=True)
    MANIFEST.write_text(json.dumps({"files": done}, indent=1))
    print(f"converted {len(done)} files, skipped {len(skipped)}")
    for s in skipped:
        print("  skipped", s)


def restore():
    if not MANIFEST.is_file():
        print("no manifest to restore")
        return
    files = json.loads(MANIFEST.read_text())["files"]
    n = 0
    for rel in files:
        src = BACKUP / rel
        if src.is_file():
            shutil.copy2(src, XB_DATA / rel)
            n += 1
    print(f"restored {n} files")


def status():
    if not MANIFEST.is_file():
        print("nothing converted")
        return
    files = json.loads(MANIFEST.read_text())["files"]
    changed = sum(1 for rel in files if (XB_DATA / rel).stat().st_size != (BACKUP / rel).stat().st_size)
    print(f"{changed}/{len(files)} converted files are in place")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=("apply", "restore", "status"))
    ap.add_argument("--sets", default=",".join(SETS))
    ap.add_argument("--subs", default=",".join(SUBS))
    a = ap.parse_args()
    if a.cmd == "apply":
        apply(a.sets.split(","), a.subs.split(","))
    elif a.cmd == "restore":
        restore()
    else:
        status()
