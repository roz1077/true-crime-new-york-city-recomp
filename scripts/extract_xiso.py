"""Extract files from an original Xbox disc image (Redump full dump or plain XISO).

Usage: python extract_xiso.py <image.iso> <out_dir> [--list]

Disc names are untrusted input: every entry is validated as a single path
component and every extent is checked against the image before it is read, so a
malformed image cannot write outside <out_dir> or make the extractor hang.
"""
import os
import struct
import sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Game partition offsets used by Redump full dumps (XGD1/XGD2/XGD3) and plain XISO.
PARTITION_OFFSETS = [0x18300000, 0xFD90000, 0x2080000, 0]
MAX_DIR_DEPTH = 64
_WINDOWS_RESERVED = {"CON", "PRN", "AUX", "NUL", "CLOCK$"}
_WINDOWS_RESERVED.update(f"COM{i}" for i in range(1, 10))
_WINDOWS_RESERVED.update(f"LPT{i}" for i in range(1, 10))


def component(name):
    """True when name is safe as one file or directory component."""
    if not name or name in (".", ".."):
        return False
    if any(c in name for c in ("/", "\\", ":", "\0")):
        return False
    if any(ord(c) < 0x20 for c in name):
        return False
    # Windows strips trailing dots and spaces, which can change the target.
    if name[-1] in " .":
        return False
    if name.split(".", 1)[0].upper() in _WINDOWS_RESERVED:
        return False
    return True


def within(root, path):
    try:
        return os.path.commonpath([root, path]) == root
    except ValueError:
        return False


def find_partition(f):
    for off in PARTITION_OFFSETS:
        f.seek(off + 32 * SECTOR)
        if f.read(len(MAGIC)) == MAGIC:
            return off
    raise SystemExit("No XDVDFS volume found")


def walk(f, image_size, base, sector, size, path, out, visited, depth=0):
    """Walk one directory table, appending (path, start, size, is_dir) to out."""
    if depth > MAX_DIR_DEPTH:
        raise ValueError("directory nesting is too deep")
    start = base + sector * SECTOR
    if sector == 0 or start < base or size > image_size or start + size > image_size:
        raise ValueError(f"directory at sector {sector} lies outside the image")
    key = (sector, size)
    if key in visited:
        return  # a cycle in a malformed table
    visited.add(key)

    f.seek(start)
    data = f.read(size)
    if len(data) != size:
        raise ValueError(f"short read of directory at sector {sector}")

    stack = [0]
    seen = set()
    while stack:
        off = stack.pop()
        if off in seen or off + 14 > len(data):
            continue
        seen.add(off)
        left, right, child, fsize, attr, nlen = struct.unpack_from("<HHIIBB", data, off)
        if left == 0xFFFF and right == 0xFFFF and child == 0xFFFFFFFF:
            continue
        if off + 14 + nlen > len(data):
            raise ValueError("entry name extends past its directory table")
        name = data[off + 14: off + 14 + nlen].decode("latin-1")
        if not component(name):
            raise ValueError(f"unsafe name on disc: {name!r}")
        full = f"{path}/{name}" if path else name

        if child == 0xFFFFFFFF:
            if fsize:
                raise ValueError(f"entry {full!r} has no data sector but a size")
        else:
            end = base + child * SECTOR + fsize
            if end < base or end > image_size:
                raise ValueError(f"entry {full!r} lies outside the image")

        if attr & 0x10:
            out.append((full, child, fsize, True))
            if fsize:
                walk(f, image_size, base, child, fsize, full, out, visited, depth + 1)
        else:
            out.append((full, child, fsize, False))

        if left:
            stack.append(left * 4)
        if right:
            stack.append(right * 4)


def main():
    if len(sys.argv) < 3:
        raise SystemExit("usage: extract_xiso.py <image.iso> <out_dir> [--list]")
    iso, out_dir = sys.argv[1], sys.argv[2]
    list_only = "--list" in sys.argv

    with open(iso, "rb") as f:
        image_size = os.fstat(f.fileno()).st_size
        base = find_partition(f)
        f.seek(base + 32 * SECTOR + 20)
        header = f.read(8)
        if len(header) != 8:
            raise SystemExit("Short XDVDFS volume header")
        root_sector, root_size = struct.unpack("<II", header)
        entries = []
        walk(f, image_size, base, root_sector, root_size, "", entries, set())
        entries.sort()
        total = 0

        root = os.path.realpath(out_dir)
        for path, start, size, is_dir in entries:
            parts = path.split("/")
            dest = os.path.join(root, *parts)
            parent = os.path.dirname(dest)

            if is_dir:
                if list_only:
                    continue
                os.makedirs(dest, exist_ok=True)
                if not within(root, os.path.realpath(dest)):
                    raise SystemExit(f"refusing to write outside {root}: {path!r}")
                continue

            total += size
            if list_only:
                print(f"{size:12d}  {path}")
                continue

            os.makedirs(parent, exist_ok=True)
            if not within(root, os.path.realpath(parent)):
                raise SystemExit(f"refusing to write outside {root}: {path!r}")
            if os.path.exists(dest) and not os.path.islink(dest) and os.path.getsize(dest) == size:
                continue

            f.seek(base + start * SECTOR)
            tmp = dest + ".part"
            try:
                with open(tmp, "wb") as o:
                    left = size
                    while left:
                        chunk = f.read(min(left, 1 << 22))
                        if not chunk:
                            raise ValueError(f"short read of {path!r}")
                        o.write(chunk)
                        left -= len(chunk)
                os.replace(tmp, dest)
            finally:
                if os.path.exists(tmp):
                    os.remove(tmp)

        print(f"partition 0x{base:X}: {sum(not e[3] for e in entries)} files, {total / 2**20:.1f} MiB")


if __name__ == "__main__":
    try:
        main()
    except (ValueError, OSError) as e:
        raise SystemExit(f"extract_xiso: {e}")
