"""Relink existing guest partitions after a host-only build, without recompiling them."""
import filecmp
import hashlib
from pathlib import Path
import subprocess
import tempfile
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
    source_xbe = work / Path(config['path']).name
    required = [source_xbe, executable, compiler / 'lld-link.exe', compiler / f'{name}.lib', work / 'exports.def']
    required += [work / obj for obj in objects]
    missing = [str(path) for path in required if not path.is_file()]
    if missing:
        raise RuntimeError('Build the port and guest code first. Missing:\n' + '\n'.join(missing))
    image_hash = hashlib.sha1(source_xbe.read_bytes()).hexdigest()
    if image_hash != hashlib.sha1((root / config['path']).read_bytes()).hexdigest():
        raise RuntimeError('The guest objects were translated from a different XBE. Rebuild them first.')
    generated = root / config['out']
    shared = ['exports.def', 'x86_recomp_shared.h']
    sources = [Path(obj).with_suffix('.cpp').name for obj in objects]
    for filename in shared + sources:
        if not (generated / filename).is_file() or not (work / filename).is_file() or not filecmp.cmp(
                generated / filename, work / filename, shallow=False):
            raise RuntimeError(f'{filename} differs from the developer build. Rebuild the guest objects first.')
    shared_time = (work / 'x86_recomp_shared.h').stat().st_mtime_ns
    for source, obj in zip(sources, objects):
        if (work / obj).stat().st_mtime_ns < max(shared_time, (work / source).stat().st_mtime_ns):
            raise RuntimeError(f'{obj} is older than its source. Rebuild the guest objects first.')
    dll = output / library
    stamp = dll.with_suffix(dll.suffix + '.stamp')
    host_hash = hashlib.sha1(executable.read_bytes()).hexdigest()
    with tempfile.TemporaryDirectory(prefix='relink-', dir=output) as directory:
        staged_dll = Path(directory) / dll.name
        staged_stamp = Path(directory) / stamp.name
        command = [str(compiler / 'lld-link.exe'), '/dll', '/noentry', '/nodefaultlib', '/machine:x64',
                   '/opt:noicf', '/def:exports.def', f'/implib:{Path(directory) / "code.lib"}',
                   f'/out:{staged_dll}', str(compiler / f'{name}.lib'), *objects]
        subprocess.run(command, cwd=work, check=True)
        dll_hash = hashlib.sha1(staged_dll.read_bytes()).hexdigest()
        staged_stamp.write_text(f'{host_hash} {image_hash} {dll_hash}\n', encoding='ascii')
        staged_dll.replace(dll)
        staged_stamp.replace(stamp)
    print(f'Relinked {library.name} against the current executable.')


if __name__ == '__main__':
    main()
