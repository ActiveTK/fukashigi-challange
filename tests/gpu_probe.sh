#!/bin/bash
set -u
cd "$(dirname "$0")/.."
bin=./build/fukashigi-gpu
nvidia-smi --query-gpu=index,name,memory.total,clocks.max.sm,compute_cap --format=csv,noheader
nvidia-smi topo -m | head -n 10
G=$(nvidia-smi --query-gpu=index --format=csv,noheader | wc -l)
echo "== check g=1"
timeout 120 tests/check.sh "$bin" 1 16 -l 8 -g 1 | tr '\n' ' '
echo
echo "== check g=$G"
timeout 120 tests/check.sh "$bin" 1 16 -l 8 -g "$G" | tr '\n' ' '
echo
for g in 1 "$G"; do
  out=$(timeout 60 "$bin" -n 22 -b 16 -k 1 -l 1 -g "$g" 2>&1)
  echo "bench n=22 g=$g $(echo "$out" | awk '$2 == "total_seconds" {print $3}') $(echo "$out" | awk '$1 == "residue" {print $3}') $(echo "$out" | grep -m1 -E 'error|cannot')"
done
