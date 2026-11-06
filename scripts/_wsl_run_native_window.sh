#!/bin/bash
# Real WSLg window (X0). TC_SW_SECS limits the run for the dev loop.
cd /root/tcnyc-linux-build
rm -rf logs hdd1 cap run.out
export DISPLAY=:0 XDG_RUNTIME_DIR=/mnt/wslg/runtime-dir WAYLAND_DISPLAY=wayland-0
export PULSE_SERVER=unix:/mnt/wslg/PulseServer
export SDL_AUDIODRIVER=pulse
export XBE_VERBOSE=1 TC_SW_CAP=${TC_SW_CAP:-0}
timeout "${TC_SW_SECS:-40}s" ./TrueCrimeNYC "/mnt/c/True Crime NYC Xbox Port/assets" --instance 1 > run.out 2>&1
echo "EXIT:$?" >> run.out
echo "--- tail:"
tail -25 run.out
echo "--- renderer:"
grep -E "renderer: (frame|software)" run.out | tail -12
echo "--- caps:"
ls cap 2>/dev/null | tail -5
