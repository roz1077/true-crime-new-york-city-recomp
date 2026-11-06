#!/usr/bin/env bash
# Capture one frame at a given trace frame with the replacement pack in place.
set -u
frame="${1:-1050}"
secs="${2:-80}"
name="${3:-pack2}"
cd /root/tcnyc-linux-build || exit 1
caps=/mnt/c/Users/user/AppData/Local/Temp/opencode/caps
mkdir -p "$caps"
rm -rf logs run.out
export DISPLAY=:0 XDG_RUNTIME_DIR=/mnt/wslg/runtime-dir WAYLAND_DISPLAY=wayland-0
export PULSE_SERVER=unix:/mnt/wslg/PulseServer SDL_AUDIODRIVER=pulse XBE_VERBOSE=1 TC_SW_CAP=0
export TCNYC_SETTINGS=/root/tcnyc-pack.toml
export TCNYC_TRACE=1 TCNYC_TRACE_AT="$frame"
timeout "${secs}s" ./TrueCrimeNYC "/mnt/c/True Crime NYC Xbox Port/assets" --instance 1 > run.out 2>&1
echo "EXIT:$?"
for tga in logs/trace_${frame}*.tga; do
  [ -f "$tga" ] || continue
  base=$(basename "$tga" .tga)
  convert "$tga" "$caps/${name}_${base}.png" && echo "wrote ${name}_${base}.png"
done
grep -E 'renderer: frame' run.out | tail -3
echo -n 'VUID lines: '; grep -cE 'VUID|Validation Error' run.out
