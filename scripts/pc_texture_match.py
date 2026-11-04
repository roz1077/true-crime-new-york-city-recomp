"""Match dumped Xbox character textures against the PC character atlases.

The PC character containers store every texture as 256x256 DXT1. The Xbox ones
are the same atlases at a much lower resolution. This tool decodes both,
compares a downscaled signature, and reports which PC texture each dump came
from. With --write it also puts the full resolution PC image into
textures/replacement under the dump's key, which the port loads as a custom
texture.

    python scripts/pc_texture_match.py [--sets LResChar] [--write]
"""

import argparse
import struct
import sys
from pathlib import Path

import numpy as np
from PIL import Image

ROOT = Path(__file__).resolve().parent.parent
PC_DATA = Path(r"D:\Old PC Games\True Crime - New York City\Data")
DUMPS = ROOT / "out" / "build" / "release" / "textures" / "dumps"
REPLACE = ROOT / "out" / "build" / "release" / "textures" / "replacement"

SIG = 32          # signature side
MIN_SCORE = 0.010  # mean squared error of the signature, lower is better


def dxt1(data, w=256, h=256):
    a = np.frombuffer(data, dtype=np.uint8)[: w * h // 2].reshape(-1, 8)
    c0 = a[:, 0].astype(np.uint32) | (a[:, 1].astype(np.uint32) << 8)
    c1 = a[:, 2].astype(np.uint32) | (a[:, 3].astype(np.uint32) << 8)
    idx = (a[:, 4].astype(np.uint32) | (a[:, 5].astype(np.uint32) << 8)
           | (a[:, 6].astype(np.uint32) << 16) | (a[:, 7].astype(np.uint32) << 24))

    def rgb(c):
        return np.stack([((c >> 11) & 31) * 255 // 31, ((c >> 5) & 63) * 255 // 63,
                         (c & 31) * 255 // 31], -1)

    a0, a1 = rgb(c0), rgb(c1)
    cols = np.stack([a0, a1, (2 * a0 + a1) // 3, (a0 + 2 * a1) // 3], 1)
    rows = np.stack([cols[np.arange(len(a)), (idx >> (2 * px)) & 3] for px in range(16)], 1)
    out = rows.reshape(h // 4, w // 4, 4, 4, 3).transpose(0, 2, 1, 3, 4).reshape(h, w, 3)
    return Image.fromarray(out.astype(np.uint8))


def pc_textures(path):
    data = path.read_bytes()
    count = off = 0
    for i in range(12):
        kind, n, o = struct.unpack_from("<HHI", data, 0x0C + i * 8)
        if kind == 2:
            count, off = n, o
            break
    if not count:
        return
    for i in range(count):
        rec = off + i * 0x28
        rel = struct.unpack_from("<I", data, rec + 0x0C)[0]
        size = struct.unpack_from("<I", data, rec + 0x18)[0]
        if not rel or size < 0x800:
            continue
        blob = data[off + rel: off + rel + size]
        if len(blob) >= 0x8000:
            yield i, dxt1(blob[:0x8000])


def sig(im, n=SIG):
    return np.asarray(im.convert("RGB").resize((n, n), Image.BOX), dtype=np.float32) / 255.0


def dump_key(name):
    for part in name.split("_"):
        if len(part) == 16 and all(c in "0123456789abcdefABCDEF" for c in part):
            return part
    return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--sets", default="LResChar,Char")
    ap.add_argument("--write", action="store_true")
    ap.add_argument("--limit", type=int, default=40)
    args = ap.parse_args()

    dumps = sorted(p for p in DUMPS.glob("*.png"))
    if not dumps:
        print(f"no dumps in {DUMPS}")
        return 1
    dsig = []
    for p in dumps:
        try:
            im = Image.open(p).convert("RGB")
        except OSError:
            continue
        if max(im.size) > 128 or min(im.size) < 16:
            continue
        if "P8" in p.stem or "A8" in p.stem:
            continue
        a = np.asarray(im, dtype=np.float32) / 255.0
        if a.std() < 0.04:  # flat utility maps match everything
            continue
        dsig.append((p, sig(im)))
    print(f"{len(dumps)} dumps, {len(dsig)} candidates")

    best = {}  # dump path -> (score, pc file, index, image)
    paths = [p for p, _ in dsig]
    mat = np.stack([d.reshape(-1) for _, d in dsig]) if dsig else None
    mat_sq = (mat * mat).sum(1) if mat is not None else None
    for set_name in args.sets.split(","):
        for sub in ("Bodies", "Heads"):
            folder = PC_DATA / set_name / sub
            if not folder.is_dir():
                continue
            for pc in sorted(folder.glob("*.pcf")):
                for index, img in pc_textures(pc):
                    s = sig(img).reshape(-1)
                    if mat is None:
                        break
                    d2 = mat_sq + float((s * s).sum()) - 2.0 * (mat @ s)
                    j = int(np.argmin(d2))
                    e = float(d2[j]) / s.size
                    old = best.get(paths[j])
                    if old is None or e < old[0]:
                        best[paths[j]] = (e, pc, index, img)
            print(f"scanned {set_name}/{sub}")
    ranked = sorted(best.items(), key=lambda kv: kv[1][0])
    strong = [(p, v) for p, v in ranked if v[0] <= MIN_SCORE]
    print(f"\n{len(strong)} matches at or below {MIN_SCORE}:")
    for path, (e, pc, index, img) in strong[: args.limit]:
        print(f"  {e:.5f}  {path.name}  <- {pc.parent.parent.name}/{pc.parent.name}/{pc.name} #{index}")

    if args.write:
        REPLACE.mkdir(parents=True, exist_ok=True)
        written = 0
        for path, (e, pc, index, img) in strong:
            key = dump_key(path.stem)
            if not key:
                continue
            out = REPLACE / f"{img.width}x{img.height}_{key}_DXT1.png"
            img.save(out)
            written += 1
        print(f"wrote {written} replacement images into {REPLACE}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
