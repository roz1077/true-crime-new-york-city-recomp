"""Make the city-cell grid index divisions use a non-negative remainder.

The grid routines around 0x146CA0..0x147308, 0x138B90, 0x13F090 and 0x14A240 map a
cell coordinate with (coordinate + 1000) / 3, signed `idiv`, and use the division
remainders (expected 0..2) to index a 3x3 table of 16-byte records. A negative
coordinate makes `idiv` return a negative remainder; index -1 then writes the
record 16 bytes before the table, over the end of cell slot 8 (+0x70/+0x74), and
the renderer crashes following 0xFFFFFFF6 (the watch run showed index 0xFFFFFFFF
and remainder -1).

Only the two divisions whose dividend is `coordinate + 1000` feed that index. For
those, after `ctx.edx = n % d`, add d when the remainder is negative. This is the
mathematical modulo: identical to the original for every non-negative dividend,
and the intended 0..d-1 bucket for a negative one. The quotient is discarded in
these routines. Divisions that do not have the +1000 dividend (the sub-position
division, or `value * 1000`) are left untouched.

Run this after a clean recompile of default.xbe (which regenerates generated/game)
and before building; the checked-in sources are already patched.
"""
from pathlib import Path
import re

root = Path(__file__).resolve().parents[1]
PLAIN = "ctx.edx = (uint32_t)(n % d); }"
FIXED = "ctx.edx = (uint32_t)(n % d); if ((int32_t)ctx.edx < 0) ctx.edx += (uint32_t)d; }"

changed = []
for path in sorted((root / "generated" / "game").glob("x86_recomp.*.cpp")):
    text = path.read_text()
    parts = re.split(r"(?=X86_FUNC\()", text)
    dirty = False
    for i, part in enumerate(parts):
        if "sub_00137330(ctx, base)" not in part:
            continue
        lines = part.splitlines(keepends=True)
        fixed = 0
        for n, line in enumerate(lines):
            if "ctx.edx = (uint32_t)(n % d);" not in line:
                continue
            dividend = ""
            for k in range(n - 1, max(-1, n - 20), -1):
                if "ctx.eax =" in lines[k]:
                    dividend = lines[k]
                    break
            grid_index = "0x3E8u" in dividend and "*" not in dividend
            want = FIXED if grid_index else PLAIN
            line = line.replace(FIXED, want).replace(PLAIN, want)
            lines[n] = line
            fixed += int(grid_index)
        new = "".join(lines)
        if new != part:
            parts[i] = new
            dirty = True
            m = re.match(r"X86_FUNC\((__imp__)?(sub_[0-9A-F]+)\)", part)
            changed.append((path.name, m.group(2) if m else "?", fixed))
    if dirty:
        path.write_text("".join(parts))

for name, func, sites in changed:
    print(f"{name} {func}: {sites} index division(s)")
print(f"{sum(s for _, _, s in changed)} index division(s) corrected")
