#!/bin/bash
set -u
cd "$(dirname "$0")/.."
bin=./build/fukashigi-gpu
nvidia-smi --query-gpu=index,name,memory.total --format=csv,noheader
nvidia-smi topo -m | head -n 3
G=$(nvidia-smi --query-gpu=index --format=csv,noheader | wc -l)
echo "== rect g=$G"
timeout 120 tests/check_rect.sh "$bin" 1 12 -l 8 -g "$G" | tail -n 3
for g in 1 "$G"; do
  echo "fp g=$g $("$bin" -n 14 -k 2 -l 2 -g "$g" 2>/dev/null | grep '^fingerprint' | md5sum | cut -c1-32)"
done
echo "== n=27"
timeout "${N27_SECONDS:-200}" "$bin" -n 27 -k 1 -g "$G" 2>&1 | grep -E 'row|error|cannot|needs|states|bytes|gpu 0' | head -n 30
