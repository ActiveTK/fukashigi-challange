#!/bin/bash
set -u
cd "$(dirname "$0")/.."
bin=./build/fukashigi-gpu
SIZES=${SIZES:-"21 22"}
GPUS=${GPUS:-"1 2"}
nvidia-smi --query-gpu=name,memory.total,driver_version,clocks.max.sm --format=csv,noheader
nvidia-smi topo -m | head -5
bench() {
  out=$(timeout 600 "$bin" "$@" 2>/dev/null)
  sec=$(echo "$out" | awk '$2 == "total_seconds" {print $3}')
  ans=$(echo "$out" | awk '$1 == "residue" {print $3}' | tr '\n' ',')
  echo "bench $* seconds=$sec residues=$ans"
}
for g in $GPUS; do
  echo "== correctness g=$g"
  timeout 300 tests/check.sh "$bin" 1 18 -l 8 -g "$g" | tr '\n' ' '
  echo
done
for n in $SIZES; do
  for g in $GPUS; do
    bench -n "$n" -b 16 -k 1 -l 1 -g "$g"
  done
done
if [ -n "${EXTRA:-}" ]; then
  echo "== extra"
  eval "$EXTRA"
fi
