"""Run all port regression checks without launching the game or modifying saves."""
from pathlib import Path
import os
import subprocess
import sys

# A single check may compile one or more harnesses; allow a slow CI machine some
# headroom without letting a hung test block the job forever.
TIMEOUT_SECONDS = int(os.environ.get('TCNYC_CHECK_TIMEOUT', '300'))


def _text(value):
    if value is None:
        return ''
    if isinstance(value, bytes):
        return value.decode('utf-8', 'replace')
    return value


def main():
    root = Path(__file__).resolve().parent
    checks = sorted(root.glob('test_*.py'))
    if not checks:
        print(f'No test_*.py checks found in {root}', file=sys.stderr)
        return 1

    failed = []
    for check in checks:
        print(f'Running {check.name}', flush=True)
        try:
            result = subprocess.run(
                [sys.executable, str(check)],
                cwd=str(root.parent),
                capture_output=True,
                text=True,
                encoding='utf-8',
                errors='replace',
                timeout=TIMEOUT_SECONDS,
            )
        except subprocess.TimeoutExpired as error:
            failed.append(check.name)
            print(f'{check.name} timed out after {TIMEOUT_SECONDS} s', file=sys.stderr)
            output = _text(error.stdout) + _text(error.stderr)
            if output:
                print(output, file=sys.stderr, end='' if output.endswith('\n') else '\n')
            continue

        if result.returncode:
            failed.append(check.name)
            print(f'{check.name} failed with exit code {result.returncode}', file=sys.stderr)
            output = result.stdout + result.stderr
            if output:
                print(output, file=sys.stderr, end='' if output.endswith('\n') else '\n')

    if failed:
        print('Failed: ' + ', '.join(failed), file=sys.stderr)
        return 1
    print(f'All {len(checks)} regression checks passed.')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
