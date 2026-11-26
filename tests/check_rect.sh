#!/bin/bash
set -u
bin=$1
from=$2
to=$3
shift 3
dir=$(cd "$(dirname "$0")" && pwd)
fail=0
for n in $(seq "$from" "$to"); do
  out=$("$bin" -n "$n" "$@" 2>/dev/null)
  bad=$(echo "$out" | awk -v bf="$dir/b064298.txt" '
    BEGIN { while ((getline line < bf) > 0) { split(line, f, " "); v[f[1]] = f[2] } }
    $1 == "rect" && NF == 5 {
      r = $2; c = $3; d = r + c - 1
      idx = (d - 1) * d / 2 + r
      if (idx in v) { total++; if (v[idx] != $4 || $5 != "ok") { bad++; print "rect " r "x" c " got " $4 " want " v[idx] } }
    }
    END { printf "checked %d bad %d\n", total, bad + 0 }')
  echo "n=$n $bad" | tail -n 1
  echo "$bad" | grep -q 'bad 0$' || fail=1
done
exit $fail
