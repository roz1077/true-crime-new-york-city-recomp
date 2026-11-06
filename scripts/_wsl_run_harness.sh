#!/bin/bash
set -e
cd /root/tcnyc-harness
rm -rf logs hdd1 run.out
set +e
timeout 20s ./tcnyc "/mnt/c/True Crime NYC Xbox Port/assets" --instance 1 > run.out 2>&1
echo "EXIT:$?" >> run.out
echo "--- run.out:"
tail -30 run.out
echo "--- log:"
tail -30 logs/tcnyc.log 2>/dev/null
