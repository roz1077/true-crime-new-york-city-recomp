#!/usr/bin/env bash
# Smoke-test the packaged tarball binary: extract, run briefly, report.
set -u
tar=/mnt/c/True\ Crime\ NYC\ Xbox\ Port/dist/TrueCrimeNYC-linux-x86_64.tar.gz
work=/tmp/pkgtest
rm -rf "$work"
mkdir -p "$work"
tar -C "$work" -xzf "$tar" || exit 1
bin="$work/TrueCrimeNYC-linux-x86_64/TrueCrimeNYC"
export DISPLAY=:0 XDG_RUNTIME_DIR=/mnt/wslg/runtime-dir WAYLAND_DISPLAY=wayland-0
export PULSE_SERVER=unix:/mnt/wslg/PulseServer SDL_AUDIODRIVER=pulse XBE_VERBOSE=1
export TCNYC_SETTINGS=/root/tcnyc-all.toml
cd "$work/TrueCrimeNYC-linux-x86_64" || exit 1
timeout 25s "$bin" --assets "/mnt/c/True Crime NYC Xbox Port/assets" > pkgrun.out 2>&1
echo "EXIT:$?"
echo '-- renderer'
grep -E 'renderer: frame' pkgrun.out | tail -3
echo '-- vulkan'
grep -E 'vulkan:' pkgrun.out | head -6
echo '-- errors'
grep -iE 'error|failed' pkgrun.out | head -5
