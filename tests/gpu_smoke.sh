#!/bin/bash
set -u
cd "$(dirname "$0")/.."
bin=./build/fukashigi-gpu
nvidia-smi --query-gpu=name,memory.total,driver_version --format=csv,noheader
timeout 120 tests/check.sh "$bin" 1 16 -l 8 -b 16
for n in 17 18 19; do
  timeout 240 tests/check.sh "$bin" "$n" "$n" -l 8 -b 16
done
timeout 300 tests/check.sh "$bin" 1 18 -l 8 -b 32
for n in 19 20; do
  for b in 16 32; do
    for l in 1 8; do
      out=$(timeout 240 "$bin" -n "$n" -b "$b" -k "$l" -l "$l" 2>/dev/null)
      st=$(echo "$out" | awk '$2 == "states" {print $3}')
      sec=$(echo "$out" | awk '$2 == "total_seconds" {print $3}')
      echo "bench n=$n bits=$b lanes=$l states=$st seconds=$sec"
    done
  done
done
