"""Keep the developer debug tools out of release builds (docs/debug-menu-plan.md).

Always checks that both build systems leave src/dev/ out unless asked. When a
release build exists (out/build/release), also checks its cache has the option
off and its executable lacks the marker string that only dev builds contain.
"""
from pathlib import Path
import re
import sys

ROOT = Path(__file__).resolve().parents[1]
MARKER = b'TCNYC-DEV-TOOLS-BUILD'


def main():
    failures = []

    windows = (ROOT / 'CMakeLists.txt').read_text(encoding='utf-8')
    if not re.search(r'option\(TCNYC_DEV_TOOLS\b[^)]*\bOFF\)', windows):
        failures.append('CMakeLists.txt: TCNYC_DEV_TOOLS must default to OFF')
    if not re.search(r'if\(NOT TCNYC_DEV_TOOLS\)\s*list\(FILTER XBE_HOST_SOURCES EXCLUDE REGEX "/src/dev/"\)',
                     windows):
        failures.append('CMakeLists.txt: src/dev/ must be filtered out when TCNYC_DEV_TOOLS is off')

    linux = (ROOT / 'platform/linux/CMakeLists.txt').read_text(encoding='utf-8')
    if 'list(FILTER GAME_SRC EXCLUDE REGEX "/src/dev/")' not in linux:
        failures.append('platform/linux/CMakeLists.txt: src/dev/ must be left out of the Linux build')

    menu = (ROOT / 'src/dev/dev_menu.cpp').read_text(encoding='utf-8')
    if MARKER.decode() not in menu:
        failures.append('src/dev/dev_menu.cpp: the dev build marker string is missing')
    for script in ('scripts/package-release.ps1',):
        if MARKER.decode() not in (ROOT / script).read_text(encoding='utf-8'):
            failures.append(f'{script}: must refuse executables with the dev build marker')

    build = ROOT / 'out/build/release'
    cache = build / 'CMakeCache.txt'
    if cache.exists() and re.search(r'^TCNYC_DEV_TOOLS:BOOL=(ON|TRUE|1|YES)\s*$',
                                    cache.read_text(encoding='utf-8', errors='replace'), re.M):
        failures.append(f'{cache}: the release build directory has TCNYC_DEV_TOOLS=ON')
    exe = build / 'TrueCrimeNYC.exe'
    checked = ''
    if exe.exists():
        if MARKER in exe.read_bytes():
            failures.append(f'{exe}: contains the developer debug tools')
        checked = ' and the release executable'

    if failures:
        for failure in failures:
            print('FAIL: ' + failure, file=sys.stderr)
        return 1
    print(f'PASS: dev tools stay out of release builds (build rules{checked})')
    return 0


if __name__ == '__main__':
    sys.exit(main())
