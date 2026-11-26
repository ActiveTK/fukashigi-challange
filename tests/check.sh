#!/bin/bash
set -u
bin=$1
from=$2
to=$3
shift 3
dir=$(cd "$(dirname "$0")" && pwd)
fail=0
for n in $(seq "$from" "$to"); do
  want=$(awk -v w=$((n + 1)) '$1 == w {print $2}' "$dir/b007764.txt")
  out=$("$bin" -n "$n" "$@" 2>/dev/null)
  got=$(echo "$out" | awk '$1 == "answer" {print $2}')
  chk=$(echo "$out" | awk '$1 == "check" {print $2}')
  secs=$(echo "$out" | awk '$2 == "total_seconds" {print $3}')
  if [ "$got" = "$want" ] && [ "$chk" = "ok" ]; then
    echo "n=$n ok ${secs}s"
  else
    echo "n=$n NG got=$got want=$want check=$chk"
    fail=1
  fi
done
exit $fail
