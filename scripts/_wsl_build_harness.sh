#!/bin/bash
# Temporary WSL bootstrap: bind-mount the repo to a space-free path and build
# the SDK headless harness against the generated game code.
set -e
cd "$(dirname "$0")/.."
mkdir -p /root/tcnyc-repo
mountpoint -q /root/tcnyc-repo || mount --bind "$PWD" /root/tcnyc-repo
rm -rf /root/harness /root/tcnyc-harness-build.log
cp -r /root/tcnyc-repo/sdk/harness /root/harness
sed -i 's/\r$//' /root/harness/*.sh
chmod +x /root/harness/build.sh /root/harness/run.sh
set +e
/root/harness/build.sh -r /root/tcnyc-repo -g /root/tcnyc-repo/generated/game -o /root/tcnyc-harness -n tcnyc -x "texturedumper" -x "graphics_options" > /root/tcnyc-harness-build.log 2>&1
echo "EXIT:$?" >> /root/tcnyc-harness-build.log
