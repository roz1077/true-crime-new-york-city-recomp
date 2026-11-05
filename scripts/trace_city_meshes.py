"""Locate the packed city vertices printed by a frame capture in the retail assets."""

import argparse
from pathlib import Path
import re
import struct


CONTAINER_HEADER = bytes.fromhex("09000000060000006c000000")
DRAW = re.compile(r"^(\d+) draw ", re.MULTILINE)
VERTEX = re.compile(r"\[ ([^\]]+)\]")


def draw_positions(frame, wanted):
    current = None
    for line in frame.open(encoding="utf-8", errors="replace"):
        match = DRAW.match(line)
        if match:
            current = int(match[1])
        if current not in wanted or not line.lstrip().startswith("verts "):
            continue
        positions = {}
        for match in VERTEX.finditer(line):
            xyz = tuple(float(value) for value in match[1].split()[:3])
            if len(xyz) != 3 or not all(-1 <= value <= 1 for value in xyz):
                continue
            packed = tuple(round(value * 32767) for value in xyz)
            # Capture output rounds its decimal floats. Allow only that rounding.
            if any(abs(value * 32767 - integer) > 0.04 for value, integer in zip(xyz, packed)):
                continue
            positions[packed] = struct.pack("<3h", *packed)
        if len(positions) >= 3:
            yield current, tuple(positions.values())


def find_containers(data, positions):
    common = None
    for position in positions:
        hits = {}
        offset = data.find(position)
        while offset != -1:
            container = data.rfind(CONTAINER_HEADER, 0, offset)
            if container != -1:
                hits.setdefault(container, offset)
            offset = data.find(position, offset + 1)
        if common is None:
            common = {container: [offset] for container, offset in hits.items()}
        else:
            common = {
                container: offsets + [hits[container]]
                for container, offsets in common.items()
                if container in hits
            }
        if not common:
            break
    return common or {}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("frame", type=Path)
    parser.add_argument("draw", nargs="+", type=int)
    parser.add_argument("--city", type=Path, default=Path("assets/data/City"))
    args = parser.parse_args()
    positions = dict(draw_positions(args.frame, set(args.draw)))
    results = {draw: [] for draw in positions}
    if positions:
        assets = sorted(path for path in args.city.rglob("*") if path.suffix.lower() in {".exm", ".xb2"})
        for path in assets:
            data = path.read_bytes()
            for draw, vertices in positions.items():
                for container, offsets in find_containers(data, vertices).items():
                    results[draw].append((path, container, offsets))
    for draw in args.draw:
        if draw not in positions:
            print(f"{draw}: fewer than three distinct packed positions in the capture")
        elif not results[draw]:
            print(f"{draw}: no matching city container")
        else:
            for path, container, offsets in results[draw]:
                vertices = ", ".join(hex(offset) for offset in offsets)
                print(f"{draw}: {path}, container {container:#x}, vertices {vertices}")


if __name__ == "__main__":
    main()
