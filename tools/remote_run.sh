#!/bin/bash
set -u
host=$1
port=$2
lres=$3
shift 3
key=${KEY:-/home/node/.ssh/vastai}
ruser=${RUSER:-root}
dir=${RDIR:-/root/fk}
bin=${BIN:-build/fukashigi-gpu}
opts=(-o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o LogLevel=ERROR)
if [ -s "$lres" ]; then
  timeout 30 scp -i "$key" -P "$port" "${opts[@]}" "$lres" "$ruser@$host:$dir/res.txt" || exit 1
fi
args=$(printf '%q ' "$@")
timeout 30 ssh -i "$key" -p "$port" "${opts[@]}" "$ruser@$host" "cd $dir && touch res.txt && nohup flock -n run.lock $bin $args --resume res.txt -o res.txt >> run.log 2>&1 < /dev/null & echo started \$!"
