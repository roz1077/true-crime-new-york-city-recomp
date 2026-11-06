#!/usr/bin/env bash
# Capture several presented frames during one run and convert them all to PNG.
# usage: _wsl_capture_many.sh <settings.toml> <every> <seconds> <prefix>
set -u
settings="${1:-/root/tcnyc-msaa/settings.toml}"
every="${2:-120}"
secs="${3:-95}"
name="${4:-capture}"

cd /root/tcnyc-linux-build || exit 1
caps=/mnt/c/Users/user/AppData/Local/Temp/opencode/caps
mkdir -p "$caps"
rm -rf logs cap run.out
export DISPLAY=:0 XDG_RUNTIME_DIR=/mnt/wslg/runtime-dir WAYLAND_DISPLAY=wayland-0
export PULSE_SERVER=unix:/mnt/wslg/PulseServer
export SDL_AUDIODRIVER=pulse XBE_VERBOSE=1 TC_SW_CAP=0
export TCNYC_SETTINGS="$settings"
export TCNYC_TRACE=1 TCNYC_TRACE_EVERY="$every"
timeout "${secs}s" ./TrueCrimeNYC "/mnt/c/True Crime NYC Xbox Port/assets" --instance 1 > run.out 2>&1
echo "EXIT:$?" >> run.out
for tga in logs/trace_*.tga; do
  [ -f "$tga" ] || continue
  base=$(basename "$tga" .tga)
  convert "$tga" "$caps/${name}_${base}.png" && echo "wrote ${name}_${base}.png"
done
echo "-- msaa"; grep -E 'MSAA' run.out | head -2
echo -n "VUID lines: "; grep -cE 'VUID|Validation Error' run.out
echo "-- tail"; tail -3 run.out
