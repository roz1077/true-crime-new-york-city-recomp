"""Relink existing guest partitions after a host-only build, without recompiling them."""
import hashlib
from pathlib import Path
import subprocess
import tomllib


def main():
    root = Path(__file__).resolve().parents[1]
    config = tomllib.loads((root / 'config/game.toml').read_text())['xbe']
    output = root / 'out/build/release'
    compiler = output / 'compiler'
    library = Path(config['library'])
    name = library.stem
    work = compiler / 'work' / name
    executable = output / f'{name}.exe'
    objects = ['x86_func_mapping.obj'] + [f'x86_recomp.{i}.obj' for i in range(config['partitions'])]
    required = [executable, compiler / 'lld-link.exe', compiler / f'{name}.lib', work / 'exports.def']
    required += [work / obj for obj in objects]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise RuntimeError('Build the port and guest code first. Missing:\n' + '\n'.join(missing))
    command = [str(compiler / 'lld-link.exe'), '/dll', '/noentry', '/nodefaultlib', '/machine:x64',
               '/opt:noicf', '/def:exports.def', '/implib:code.lib', f'/out:{output / library}',
               str(compiler / f'{name}.lib'), *objects]
    subprocess.run(command, cwd=work, check=True)
    # Match the runtime compiler's host timestamp and original-image fingerprint.
    info = executable.stat()
    image_hash = hashlib.sha1((root / config['path']).read_bytes()).hexdigest()
    timestamp = info.st_mtime_ns // 100 + 116444736000000000
    (work / 'built.txt').write_text(f'{info.st_size} {timestamp} {image_hash}\n')
    print(f'Relinked {library.name} against the current executable.')


if __name__ == '__main__':
    main()
