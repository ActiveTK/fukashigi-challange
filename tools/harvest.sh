#!/bin/bash
set -u
host=$1
port=$2
rres=$3
rlog=$4
lres=$5
key=${KEY:-/home/node/.ssh/vastai}
ruser=${RUSER:-root}
every=${EVERY:-60}
stall=${STALL:-1800}
maxfail=${MAXFAIL:-5}
opts=(-i "$key" -p "$port" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o LogLevel=ERROR)
touch "$lres"
count() { grep -c '^residue ' "$1" 2>/dev/null; }
last=$(count "$lres")
since=$(date +%s)
fails=0
echo "watch $host:$port local=$lres residues=$last"
while true; do
  if snap=$(timeout 30 ssh "${opts[@]}" "$ruser@$host" "cat '$rres' 2>/dev/null; echo '#--log'; tail -n 5 '$rlog' 2>/dev/null"); then
    fails=0
    res=${snap%%#--log*}
    log=${snap#*#--log}
    head=$( { grep -m1 '^# fukashigi' "$lres"; printf '%s\n' "$res" | grep -m1 '^# fukashigi'; } | head -n 1)
    {
      [ -n "$head" ] && echo "$head"
      { grep '^residue ' "$lres"; printf '%s\n' "$res" | grep '^residue '; } | sort -u
      printf '%s\n' "$res" | grep -E '^# (answer|check)'
    } > "$lres.tmp" && mv "$lres.tmp" "$lres"
    n=$(count "$lres")
    now=$(date +%s)
    if [ "$n" -gt "$last" ]; then
      echo "new $((n - last)) total $n"
      last=$n
      since=$now
    fi
    if printf '%s\n' "$res" | grep -q '^# check'; then
      echo "done $(printf '%s\n' "$res" | grep '^# check' | tail -n 1)"
      exit 0
    fi
    bad=$(printf '%s\n' "$log" | grep -E -m1 'cuda error|Segmentation|Aborted|Killed|cannot|needs .* GB')
    if [ -n "$bad" ]; then
      echo "error $bad"
      exit 4
    fi
    if [ $((now - since)) -ge "$stall" ]; then
      echo "stall $((now - since))s total $n"
      since=$now
    fi
  else
    fails=$((fails + 1))
    echo "sshfail $fails"
    if [ "$fails" -ge "$maxfail" ]; then
      echo "lost"
      exit 3
    fi
  fi
  sleep "$every"
done
