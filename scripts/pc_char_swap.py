"""Swap the PC character assets into the Xbox data folders, or restore them.

The PC and Xbox character containers share the same layout; this replaces a
chosen set of body/head files so the port can be tested against the higher
detail PC meshes and textures. Originals are copied once into
out/asset-backup before anything is overwritten.

    python scripts/pc_char_swap.py apply LResChar
    python scripts/pc_char_swap.py apply Char
    python scripts/pc_char_swap.py restore
    python scripts/pc_char_swap.py status
"""

import shutil
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
PC_DATA = Path(r"D:\Old PC Games\True Crime - New York City\Data")
XB_DATA = ROOT / "assets" / "data"
BACKUP = ROOT / "out" / "asset-backup"
SETS = ("LResChar", "Char")
SUBS = ("Bodies", "Heads")


def pairs(set_name, sub):
    pc_dir = PC_DATA / set_name / sub
    xb_dir = XB_DATA / set_name / sub
    if not pc_dir.is_dir() or not xb_dir.is_dir():
        return
    for pc in sorted(pc_dir.iterdir()):
        if pc.suffix.lower() not in (".pcf", ".pc2"):
            continue
        xb = xb_dir / (pc.stem + ".xb2")
        if xb.is_file():
            yield pc, xb


def apply(set_names):
    n = 0
    for set_name in set_names:
        for sub in SUBS:
            for pc, xb in pairs(set_name, sub):
                backup = BACKUP / set_name / sub / xb.name
                if not backup.is_file():
                    backup.parent.mkdir(parents=True, exist_ok=True)
                    shutil.copy2(xb, backup)
                shutil.copy2(pc, xb)
                n += 1
    print(f"replaced {n} character assets from the PC install")


def restore():
    n = 0
    if not BACKUP.is_dir():
        print("no backup to restore")
        return
    for set_name in SETS:
        for sub in SUBS:
            src = BACKUP / set_name / sub
            if not src.is_dir():
                continue
            dst = XB_DATA / set_name / sub
            for f in src.iterdir():
                shutil.copy2(f, dst / f.name)
                n += 1
    print(f"restored {n} character assets")


def status():
    for set_name in SETS:
        for sub in SUBS:
            pc = list(pairs(set_name, sub))
            swapped = 0
            for p, xb in pc:
                backup = BACKUP / set_name / sub / xb.name
                if backup.is_file() and xb.stat().st_size != backup.stat().st_size:
                    swapped += 1
            if pc:
                print(f"{set_name}/{sub}: {swapped}/{len(pc)} swapped")


if __name__ == "__main__":
    cmd = sys.argv[1] if len(sys.argv) > 1 else "status"
    if cmd == "apply":
        apply(sys.argv[2:] or list(SETS))
    elif cmd == "restore":
        restore()
    elif cmd == "status":
        status()
    else:
        print(__doc__)
        sys.exit(2)
