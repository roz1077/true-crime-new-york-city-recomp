"""Build a hybrid character body: PC mesh/texture data with the Xbox runtime
fields copied into the texture table.

The PC model containers leave the per-texture runtime fields as 0xCD, which
makes the Xbox loader follow a bad pointer. This copies the small runtime
fields from the Xbox file of the same name while keeping all PC offsets and
sizes, so the engine can be tested with the PC mesh.

    python scripts/pc_hybrid_body.py Char/Bodies/Body_F_Casual_Heavy
    python scripts/pc_hybrid_body.py --restore
"""

import argparse
import shutil
import struct
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PC_DATA = Path(r"D:\Old PC Games\True Crime - New York City\Data")
XB_DATA = ROOT / "assets" / "data"
BACKUP = ROOT / "out" / "asset-backup" / "hybrid"


def tex_table(data):
    for i in range(12):
        kind, count, off = struct.unpack_from("<HHI", data, 0x0C + i * 8)
        if kind == 2:
            return count, off
    return 0, 0


def hybrid(rel):
    pc_path = PC_DATA / (rel + ".pcf")
    xb_path = XB_DATA / (rel + ".xb2")
    if not pc_path.is_file() or not xb_path.is_file():
        print(f"missing {pc_path} or {xb_path}")
        return False
    pc = bytearray(pc_path.read_bytes())
    xb = xb_path.read_bytes()
    pc_count, pc_off = tex_table(pc)
    xb_count, xb_off = tex_table(xb)
    if not pc_count or pc_count != xb_count:
        print(f"{rel}: texture counts differ (pc {pc_count}, xbox {xb_count})")
        return False
    for i in range(pc_count):
        p = pc_off + i * 0x28
        x = xb_off + i * 0x24
        pc[p + 0x08:p + 0x0C] = xb[x + 0x08:x + 0x0C]      # runtime id / format
        pc[p + 0x1C:p + 0x24] = xb[x + 0x1C:x + 0x24]      # 8 byte runtime id
        pc[p + 0x24:p + 0x28] = b"\0\0\0\0"                # padding was 0xCD
    backup = BACKUP / (rel + ".xb2")
    backup.parent.mkdir(parents=True, exist_ok=True)
    if not backup.is_file():
        shutil.copy2(xb_path, backup)
    xb_path.write_bytes(pc)
    print(f"{rel}: wrote hybrid {len(pc)} bytes (xbox was {len(xb)})")
    return True


def restore():
    n = 0
    for f in BACKUP.rglob("*.xb2"):
        rel = f.relative_to(BACKUP)
        shutil.copy2(f, XB_DATA / rel)
        n += 1
    print(f"restored {n} hybrid files")


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="*")
    ap.add_argument("--restore", action="store_true")
    a = ap.parse_args()
    if a.restore:
        restore()
    else:
        ok = all(hybrid(p.replace("\\", "/")) for p in a.paths)
        sys.exit(0 if ok else 1)
