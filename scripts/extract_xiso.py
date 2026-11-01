"""Extract files from an original Xbox disc image (Redump full dump or plain XISO).

Usage: python extract_xiso.py <image.iso> <out_dir> [--list]
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Game partition offsets used by Redump full dumps (XGD1/XGD2/XGD3) and plain XISO.
PARTITION_OFFSETS = [0x18300000, 0xFD90000, 0x2080000, 0]


def find_partition(f):
    for off in PARTITION_OFFSETS:
        f.seek(off + 32 * SECTOR)
        if f.read(len(MAGIC)) == MAGIC:
            return off
    raise SystemExit("No XDVDFS volume found")


def walk(f, base, sector, size, path, out):
    """Walk the directory's binary tree; yields (path, start, size, is_dir)."""
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    stack = [0]
    seen = set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(data):
            continue
        seen.add(off)
        left, right, start, fsize, attr, nlen = struct.unpack_from("<HHIIBB", data, off)
        if left == 0xFFFF and right == 0xFFFF and start == 0xFFFFFFFF:
            continue
        name = data[off + 14: off + 14 + nlen].decode("latin-1")
        full = f"{path}/{name}" if path else name
        if attr & 0x10:
            out.append((full, start, fsize, True))
            if fsize:
                walk(f, base, start, fsize, full, out)
        else:
            out.append((full, start, fsize, False))
        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)


def main():
    iso, out_dir = sys.argv[1], sys.argv[2]
    list_only = "--list" in sys.argv
    with open(iso, "rb") as f:
        base = find_partition(f)
        f.seek(base + 32 * SECTOR + 20)
        root_sector, root_size = struct.unpack("<II", f.read(8))
        entries = []
        walk(f, base, root_sector, root_size, "", entries)
        entries.sort()
        total = 0
        for path, start, size, is_dir in entries:
            dest = os.path.join(out_dir, *path.split("/"))
            if is_dir:
                if not list_only:
                    os.makedirs(dest, exist_ok=True)
                continue
            total += size
            if list_only:
                print(f"{size:12d}  {path}")
                continue
            os.makedirs(os.path.dirname(dest), exist_ok=True)
            if os.path.exists(dest) and os.path.getsize(dest) == size:
                continue
            f.seek(base + start * SECTOR)
            with open(dest, "wb") as o:
                left = size
                while left:
                    chunk = f.read(min(left, 1 << 22))
                    o.write(chunk)
                    left -= len(chunk)
        print(f"partition 0x{base:X}: {sum(not e[3] for e in entries)} files, {total / 2**20:.1f} MiB")


if __name__ == "__main__":
    main()
