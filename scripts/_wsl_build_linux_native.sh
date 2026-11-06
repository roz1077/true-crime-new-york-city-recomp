#!/bin/bash
set -u
mkdir -p /root/tcnyc-repo
mountpoint -q /root/tcnyc-repo || mount --bind "/mnt/c/True Crime NYC Xbox Port" /root/tcnyc-repo
cmake -S /root/tcnyc-repo/platform/linux -B /root/tcnyc-linux-build -G Ninja \
    -DCMAKE_PREFIX_PATH=/root/sdl3-install -DCMAKE_BUILD_TYPE=Release > /root/tcnyc-linux-build.log 2>&1
status=$?
if [ "$status" -eq 0 ]; then
    cmake --build /root/tcnyc-linux-build -j"$(nproc)" >> /root/tcnyc-linux-build.log 2>&1
    status=$?
fi
echo "NATIVE_EXIT:$status" >> /root/tcnyc-linux-build.log
exit "$status"
