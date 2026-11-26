#!/bin/bash
set -u
cd "$(dirname "$0")/.."
bin=./build/fukashigi-gpu
VARIANTS=${VARIANTS:-"1 2 3"}
CHECK=${CHECK:-"2 3"}
SIZES=${SIZES:-"20 21"}
SHAPE=${SHAPE:-"-T 512 -B 16"}
nvidia-smi --query-gpu=name,memory.total,driver_version,clocks.max.sm --format=csv,noheader
bench() {
  out=$(timeout 300 "$bin" "$@" 2>/dev/null)
  sec=$(echo "$out" | awk '$2 == "total_seconds" {print $3}')
  ans=$(echo "$out" | awk '$1 == "residue" {print $3}' | tr '\n' ',')
  echo "bench $* seconds=$sec residues=$ans"
}
for v in $CHECK; do
  echo "== correctness v$v"
  timeout 300 tests/check.sh "$bin" 1 19 -l 8 -b 16 -v "$v" $SHAPE | tr '\n' ' '
  echo
done
for n in $SIZES; do
  for v in $VARIANTS; do
    bench -n "$n" -b 16 -k 1 -l 1 -v "$v" $SHAPE
  done
done
if [ -n "${EXTRA:-}" ]; then
  echo "== extra"
  eval "$EXTRA"
fi
