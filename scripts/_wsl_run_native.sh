#!/bin/bash
set -e
cd /root/tcnyc-linux-build
rm -rf logs hdd1 run.out
set +e
TCNYC_RENDERER=software SDL_VIDEODRIVER=dummy XBE_VERBOSE=1 timeout 15s ./TrueCrimeNYC "/mnt/c/True Crime NYC Xbox Port/assets" --instance 1 > run.out 2>&1
echo "EXIT:$?" >> run.out
echo "--- run.out:"
tail -40 run.out
echo "--- log:"
tail -40 logs/tcnyc.log 2>/dev/null
