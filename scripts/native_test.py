"""Compile a small Windows regression harness and call its exported run function."""
import ctypes
import os
from pathlib import Path
import shutil
import subprocess
import tempfile


def _compiler():
    installed = Path(os.environ.get('ProgramFiles', r'C:\Program Files')) / 'LLVM/bin/clang++.exe'
    compiler = str(installed) if installed.is_file() else shutil.which('clang++')
    if not compiler:
        raise RuntimeError('LLVM clang++ is required for the native regression checks.')
    return compiler


def _vcvars():
    program_files = Path(os.environ.get('ProgramFiles(x86)', r'C:\Program Files (x86)'))
    vswhere = program_files / 'Microsoft Visual Studio/Installer/vswhere.exe'
    result = subprocess.run([str(vswhere), '-latest', '-products', '*', '-requires',
                             'Microsoft.VisualStudio.Component.VC.Tools.x86.x64',
                             '-property', 'installationPath'], check=True, capture_output=True, text=True)
    installation = result.stdout.strip()
    if not installation:
        raise RuntimeError('Visual Studio C++ build tools are required for this check.')
    return Path(installation) / 'VC/Auxiliary/Build/vcvars64.bat'


def run_native(source, *, prefix, needs_crt=False, compiler_flags=()):
    if os.name != 'nt':
        raise RuntimeError('These native regression harnesses currently require Windows.')
    with tempfile.TemporaryDirectory(prefix=prefix) as temp:
        temp = Path(temp)
        cpp, dll = temp / 'test.cpp', temp / 'test.dll'
        cpp.write_text(source)
        command = [_compiler(), '--target=x86_64-pc-windows-msvc', '-std=c++20', '-shared', '-fuse-ld=lld']
        command += list(compiler_flags)
        if not needs_crt:
            command += ['-nostdlib', '-fno-builtin', '-Wl,/noentry']
        command += [str(cpp), '-o', str(dll)]
        if needs_crt:
            bat = temp / 'build.cmd'
            bat.write_text(f'@echo off\ncall "{_vcvars()}" >nul\nif errorlevel 1 exit /b %errorlevel%\n'
                           + subprocess.list2cmdline(command) + '\n')
            command = ['cmd', '/d', '/c', str(bat)]
        result = subprocess.run(command, capture_output=True, text=True)
        if result.returncode:
            raise RuntimeError(result.stdout + result.stderr)
        library = ctypes.CDLL(str(dll))
        try:
            return library.run()
        finally:
            # Loaded DLLs must be released before TemporaryDirectory removes them.
            ctypes.windll.kernel32.FreeLibrary(ctypes.c_void_p(library._handle))
