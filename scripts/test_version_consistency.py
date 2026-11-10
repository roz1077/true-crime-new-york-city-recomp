"""Keep the release version the same everywhere it is written.

The Windows version resource (numeric and string fields), the Linux package's
BUILD.txt line, the README release badge and the newest CHANGELOG heading must
all name the same version, so a release cannot ship half-renumbered.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]


def read(path):
    return (ROOT / path).read_text(encoding='utf-8')


def main():
    found = {}
    rc = read('launcher/version.rc')
    for key in ('FILEVERSION', 'PRODUCTVERSION'):
        m = re.search(rf'^\s*{key}\s+(\d+),(\d+),(\d+),(\d+)', rc, re.M)
        found[f'version.rc {key}'] = '.'.join(m.groups()) if m else None
    for key in ('FileVersion', 'ProductVersion'):
        m = re.search(rf'VALUE\s+"{key}",\s+"([\d.]+)"', rc)
        found[f'version.rc "{key}"'] = m.group(1) if m else None

    m = re.search(r'echo "Version: ([\d.]+)"', read('scripts/package-linux.sh'))
    found['package-linux.sh BUILD.txt'] = m.group(1) if m else None

    # shields.io escapes '-' as '--': release-v0.9.1--beta.2 is v0.9.1-beta.2, four-part 0.9.1.0
    m = re.search(r'badge/release-v(\d+)\.(\d+)\.(\d+)--', read('README.md'))
    found['README badge'] = f'{m.group(1)}.{m.group(2)}.{m.group(3)}.0' if m else None

    m = re.search(r'^## (?!Unreleased)(\d+\.\d+\.\d+\.\d+)', read('CHANGELOG.md'), re.M)
    found['CHANGELOG newest heading'] = m.group(1) if m else None

    missing = [k for k, v in found.items() if v is None]
    versions = {v for v in found.values() if v}
    if missing or len(versions) != 1:
        for k, v in found.items():
            print(f'  {k}: {v}', file=sys.stderr)
        print('FAIL: the release version differs between files (or one is missing)', file=sys.stderr)
        return 1
    print(f'PASS: every release version field says {versions.pop()}')
    return 0


if __name__ == '__main__':
    sys.exit(main())
