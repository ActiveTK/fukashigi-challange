#!/bin/bash
set -u
cd "$(dirname "$0")/.."
bin=./build/fukashigi-gpu
nvidia-smi --query-gpu=index,name,memory.total,clocks.max.sm --format=csv,noheader
nvidia-smi topo -m | head -n 10
G=$(nvidia-smi --query-gpu=index --format=csv,noheader | wc -l)
bench() {
  out=$(timeout "$1" "$bin" "${@:2}" 2>&1)
  echo "bench ${*:2} seconds=$(echo "$out" | awk '$2 == "total_seconds" {print $3}') residues=$(echo "$out" | awk '$1 == "residue" {print $3}' | tr '\n' ',') $(echo "$out" | grep -m1 -E 'error|cannot|needs')"
}
echo "== check g=$G"
timeout 90 tests/check.sh "$bin" 1 16 -l 8 -g "$G" | tr '\n' ' '
echo
bench 120 -n 24 -k 1 -l 1 -g "$G"
for g in 1 2 4 8; do
  bench 60 -n 22 -k 1 -l 1 -g "$g"
done
bench 200 -n 25 -k 1 -l 1 -g "$G"
