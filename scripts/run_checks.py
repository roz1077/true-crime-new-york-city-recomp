"""Run all port regression checks without launching the game or modifying saves."""
from pathlib import Path
import subprocess
import sys


def main():
    root = Path(__file__).resolve().parent
    checks = sorted(root.glob('test_*.py'))
    failed = []
    for check in checks:
        print(f'Running {check.name}', flush=True)
        result = subprocess.run([sys.executable, str(check)], cwd=root.parent)
        if result.returncode:
            failed.append(check.name)
    if failed:
        print('Failed: ' + ', '.join(failed), file=sys.stderr)
        return 1
    print(f'All {len(checks)} regression checks passed.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
