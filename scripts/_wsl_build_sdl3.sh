#!/bin/bash
set -u
mkdir -p /root/tcnyc-repo
mountpoint -q /root/tcnyc-repo || mount --bind "/mnt/c/True Crime NYC Xbox Port" /root/tcnyc-repo
cmake -S /root/tcnyc-repo/out/build/release/_deps/sdl3-src -B /root/sdl3-build -G Ninja \
    -DCMAKE_BUILD_TYPE=Release \
    -DSDL_SHARED=OFF -DSDL_STATIC=ON -DSDL_TEST_LIBRARY=OFF \
    -DSDL_RENDER=OFF -DSDL_GPU=OFF \
    -DCMAKE_INSTALL_PREFIX=/root/sdl3-install > /root/sdl3-build.log 2>&1
status=$?
if [ "$status" -eq 0 ]; then
    cmake --build /root/sdl3-build -j"$(nproc)" >> /root/sdl3-build.log 2>&1
    status=$?
fi
if [ "$status" -eq 0 ]; then
    cmake --install /root/sdl3-build >> /root/sdl3-build.log 2>&1
    status=$?
fi
echo "SDL3_EXIT:$status" >> /root/sdl3-build.log
exit "$status"
