#!/usr/bin/env bash
# Summarise Vulkan validation output from a run log.
log="${1:-/root/tcnyc-linux-build/run.out}"
echo -n 'VUID lines: '
grep -cE 'VUID|Validation Error' "$log"
echo '-- top VUIDs'
grep -oE 'VUID-[A-Za-z0-9_-]+' "$log" | sort | uniq -c | sort -rn | head -20
echo '-- first messages'
grep -nE 'VUID|Validation Error' "$log" | head -8 | cut -c1-320
echo '-- msaa log'
grep -E 'MSAA' "$log" | head -3
echo '-- tail'
tail -3 "$log"
