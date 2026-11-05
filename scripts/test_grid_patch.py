"""Fixture test for the shared city-cell grid patcher.

The recompiler's generated sources are git-ignored, so this builds the exact instruction blocks
from the confirmed guest sites and checks the shared implementation used by both entry points: the
CMake generation tool and the first-launch compiler. It verifies the patch is explicit (unrelated
+1000 divisions are untouched), idempotent, and fails on every kind of drift.
"""
from pathlib import Path
import subprocess
import tempfile

from native_test import run_native

root = Path(__file__).resolve().parents[1]
header = (root / 'sdk/runtime/xbe/grid_patch.h').read_text()

# address, idiv operand, occurrences across the generated partitions
SITES = [
    (0x00138BCD, 'ecx', 1),
    (0x00138BE4, 'edi', 1),
    (0x0013F212, 'ecx', 1),
    (0x0013F229, 'edi', 1),
    (0x00146D30, 'esi', 3),
    (0x00146D7D, 'ecx', 3),
    (0x00147053, 'esi', 2),
    (0x0014709D, 'ecx', 2),
    (0x00147373, 'edi', 2),
    (0x001473BD, 'ecx', 2),
    (0x0014A5D1, 'ecx', 1),
    (0x0014A5F8, 'esi', 1),
]
PARTS = 3
PLAIN = 'ctx.edx = (uint32_t)(n % d); }'
FIXED = 'ctx.edx = (uint32_t)(n % d); if ((int32_t)ctx.edx < 0) ctx.edx += (uint32_t)d; }'


def block(address, operand, fixed):
    short = f'{address:X}'
    remainder = ('ctx.edx = (uint32_t)(n % d);'
                 + (' if ((int32_t)ctx.edx < 0) ctx.edx += (uint32_t)d;' if fixed else '')
                 + ' }')
    return (f'\t// {address:08X} idiv {operand}\n'
            f'\t{{ int64_t n = (int64_t)(((uint64_t)ctx.edx << 32) | ctx.eax); '
            f'int32_t d = (int32_t)ctx.{operand}; if (!d) x86_trap(ctx, 0x{short}, "divide error"); '
            f'if (n == INT64_MIN && d == -1) x86_trap(ctx, 0x{short}, "divide error"); '
            f'int64_t q = n / d; if (q != (int32_t)q) x86_trap(ctx, 0x{short}, "divide error"); '
            f'ctx.eax = (uint32_t)q; {remainder}\n')


def unrelated():
    # Same +1000 dividend and signed remainder shape, but not one of the confirmed sites. The old
    # runtime scanner matched this; the explicit patcher must leave it alone.
    return block(0x0099ABCD, 'ecx', False)


def fixture(fixed=False, skip=None):
    parts = [[] for _ in range(PARTS)]
    for i, (address, operand, count) in enumerate(SITES):
        for j in range(count):
            if skip == (i, j):
                continue
            parts[(i + j) % PARTS].append(block(address, operand, fixed))
    for part in parts:
        part.append(unrelated())
    return [''.join(part) for part in parts]


def write(directory, files):
    directory.mkdir(parents=True, exist_ok=True)
    for index, text in enumerate(files):
        (directory / f'x86_recomp.{index}.cpp').write_text(text, encoding='utf-8')


def replace_first(directory, old, new):
    for path in sorted(directory.glob('x86_recomp.*.cpp')):
        text = path.read_text(encoding='utf-8')
        if old in text:
            path.write_text(text.replace(old, new), encoding='utf-8')
            return
    raise AssertionError(f'fixture does not contain {old!r}')


def replace_block_remainder(directory, address):
    for path in sorted(directory.glob('x86_recomp.*.cpp')):
        lines = path.read_text(encoding='utf-8').splitlines(keepends=True)
        for i, line in enumerate(lines[:-1]):
            if f'// {address:08X} idiv' in line and PLAIN in lines[i + 1]:
                lines[i + 1] = lines[i + 1].replace(PLAIN, 'ctx.edx = (uint32_t)(n % d); ctx.edx |= 0; }', 1)
                path.write_text(''.join(lines), encoding='utf-8')
                return
    raise AssertionError(f'fixture does not contain a block for {address:08X}')


source = tempfile.TemporaryDirectory(prefix='tcnyc-grid-patch-')
base = Path(source.name)
try:
    good = base / 'good'
    write(good, fixture())

    missing = base / 'missing'
    write(missing, fixture()[:2])

    operand = base / 'operand'
    write(operand, fixture())
    replace_first(operand, '// 00138BCD idiv ecx', '// 00138BCD idiv edx')

    trap = base / 'trap'
    write(trap, fixture())
    replace_first(trap, 'x86_trap(ctx, 0x13F212, "divide error")',
                  'x86_trap(ctx, 0x13F213, "divide error")')

    count = base / 'count'
    write(count, fixture(skip=(4, 0)))

    remainder = base / 'remainder'
    write(remainder, fixture())
    replace_block_remainder(remainder, 0x00138BCD)

    cases = [(good, 3, True), (missing, 3, False), (operand, 3, False), (trap, 3, False),
             (count, 3, False), (remainder, 3, False), (good, 0, False)]
    # Embed the temporary paths and expected outcomes in the harness.
    entries = ',\n'.join(
        '    {L"%s", %d, %s}' % (str(path).replace('\\', '\\\\'), partitions, 'true' if ok else 'false')
        for path, partitions, ok in cases)
    check = r'''
struct Case { const wchar_t* directory; unsigned partitions; bool expected; };
static const Case kCases[] = {
%s
};
extern "C" __declspec(dllexport) int run() {
    for (unsigned i = 0; i < sizeof(kCases) / sizeof(kCases[0]); ++i) {
        const xbe::GridPatchResult result = xbe::PatchGridSources(kCases[i].directory, kCases[i].partitions);
        if (result.ok() != kCases[i].expected) {
            std::fprintf(stderr, "case %%u: expected %%s, got: %%s\n", i,
                         kCases[i].expected ? "ok" : "failure", result.error.c_str());
            return int(i) + 1;
        }
        if (kCases[i].expected && (result.files != %d || result.occurrences != 20)) return 100 + int(i);
    }
    // The first case is patched above; a second run must accept the already-corrected sources.
    const xbe::GridPatchResult again = xbe::PatchGridSources(L"%s", %d);
    if (!again.ok() || again.files != %d || again.occurrences != 20) return 200;
    return 0;
}
''' % (entries, PARTS, str(good).replace('\\', '\\\\'), PARTS, PARTS)

    result = run_native(header + check, prefix='tcnyc-grid-patch-native-', needs_crt=True)
    assert result == 0, f'grid patch fixture regression {result}'

    expected = fixture(fixed=True)
    for index, text in enumerate(expected):
        actual = (good / f'x86_recomp.{index}.cpp').read_text(encoding='utf-8')
        assert actual == text, f'partition {index} does not match the expected corrected sources'

    # When a developer build is present, run the CMake generation tool over an identical fixture and
    # require byte-identical output: both entry points must agree.
    tools = sorted(root.glob('out/build/*/compiler/grid_patch.exe'))
    if tools:
        tool_dir = base / 'tool'
        write(tool_dir, fixture())
        completed = subprocess.run([str(tools[-1]), str(tool_dir), str(PARTS)],
                                   capture_output=True, text=True)
        assert completed.returncode == 0, f'grid_patch tool failed: {completed.stdout}{completed.stderr}'
        for index in range(PARTS):
            assert ((tool_dir / f'x86_recomp.{index}.cpp').read_bytes() ==
                    (good / f'x86_recomp.{index}.cpp').read_bytes()), \
                f'grid_patch tool differs from the runtime implementation on partition {index}'
finally:
    source.cleanup()

print('PASS: grid patcher matches only confirmed sites, is idempotent and rejects drift')
