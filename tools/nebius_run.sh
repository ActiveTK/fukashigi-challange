#!/bin/bash
set -u
cd "$(dirname "$0")/.."
export PATH=$HOME/.nebius/bin:$PATH
P=${P:?project id}
SUB=${SUB:?subnet id}
NAME=${NAME:?vm name}
LRES=${LRES:?local residue file}
PP=${PP:-}
PLATFORM=${PLATFORM:-gpu-h200-sxm}
PRESET=${PRESET:-8gpu-128vcpu-1600gb}
IMAGE=${IMAGE:-ubuntu24.04-kernel-7.0-nvidia580-x86_64}
DISK_GB=${DISK_GB:-50}
KIT=${KIT:-build/kit.tgz}
BIN=${BIN:-build/fukashigi-gpu}
PRICE=${PRICE:?usd per hour}
BUDGET=${BUDGET:?usd}
START_WAIT=${START_WAIT:-900}
EVERY=${EVERY:-60}
GUARD_STALL=${GUARD_STALL:-3000}
MAX_RESTARTS=${MAX_RESTARTS:-20}
KEY=${KEY:-$HOME/.ssh/nebius_fk}
RDIR=/home/fk/fk
SSHO=(-i "$KEY" -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o ConnectTimeout=10 -o ServerAliveInterval=20 -o LogLevel=ERROR)
STATE=${LRES}.state
log() { echo "$(date +%T) $*"; }
spent=0
[ -f "$STATE" ] && spent=$(cat "$STATE")
add_spent() { spent=$(python3 -c "print(round($spent + $1 * $PRICE / 3600, 4))"); echo "$spent" > "$STATE"; }
over_budget() { python3 -c "import sys; sys.exit(0 if $spent >= $BUDGET else 1)"; }
find_vm() {
  timeout 30 nebius compute instance list --parent-id "$P" --format json 2>/dev/null | python3 -c "
import json,sys
d=json.load(sys.stdin)
for it in d.get('items',[]):
    if it['metadata'].get('name')=='$NAME': print(it['metadata']['id'])
" 2>/dev/null | head -n 1
}
vm_state() {
  timeout 30 nebius compute instance get --id "$1" --format json 2>/dev/null | python3 -c "
import json,sys
s=json.load(sys.stdin)['status']
ip=''
for n in s.get('network_interfaces',[]):
    a=n.get('public_ip_address',{}).get('address','')
    if a: ip=a.split('/')[0]
print(s.get('state','?'), ip or '-')
" 2>/dev/null || echo "? -"
}
delete_vm() {
  local id
  id=$(find_vm)
  [ -z "$id" ] && return 0
  log "deleting $id"
  timeout 600 nebius compute instance delete --id "$id" > /dev/null 2>&1
  log "deleted rc=$? instances_left=$(timeout 30 nebius compute instance list --parent-id "$P" 2>/dev/null | grep -c ' id: ')"
}
create_vm() {
  local UD extra=()
  UD=$(printf '#cloud-config\nusers:\n  - name: fk\n    sudo: ALL=(ALL) NOPASSWD:ALL\n    shell: /bin/bash\n    ssh_authorized_keys:\n      - %s\n' "$(cat "$KEY.pub")")
  [ -n "$PP" ] && extra=(--recovery-policy fail --preemptible-on-preemption stop --spot-pricing-policy-id "$PP")
  log "create $NAME"
  timeout 900 nebius compute instance create --parent-id "$P" --name "$NAME" \
    --resources-platform "$PLATFORM" --resources-preset "$PRESET" \
    --boot-disk-managed-disk-name "$NAME-boot" --boot-disk-managed-disk-type network_ssd \
    --boot-disk-managed-disk-size-gibibytes "$DISK_GB" \
    --boot-disk-managed-disk-source-image-family-image-family "$IMAGE" \
    --boot-disk-attach-mode READ_WRITE --cloud-init-user-data "$UD" \
    --network-interfaces "[{\"name\": \"eth0\", \"ip_address\": {}, \"public_ip_address\": {}, \"subnet_id\": \"$SUB\"}]" \
    "${extra[@]}" > /dev/null 2>&1 &
}
rsh() { timeout "${T:-30}" ssh "${SSHO[@]}" "fk@$IP" "$@"; }
prepare_vm() {
  if ! rsh "test -x $RDIR/$BIN" 2>/dev/null; then
    log "upload kit"
    timeout 120 scp "${SSHO[@]}" "$KIT" "fk@$IP:kit.tgz" || return 1
    rsh "mkdir -p $RDIR && tar -C $RDIR -xzf kit.tgz" || return 1
  fi
  rsh "cat > $RDIR/guard.sh <<'G'
#!/bin/bash
cd $RDIR
while sleep 60; do
  grep -q '^# check' res.txt 2>/dev/null && { sleep 600; sudo shutdown -h now; }
  [ -f res.txt ] && [ \$(( \$(date +%s) - \$(stat -c %Y res.txt) )) -gt $GUARD_STALL ] && sudo shutdown -h now
done
G
chmod +x $RDIR/guard.sh; cd $RDIR; if ! { [ -f guard.pid ] && kill -0 \$(cat guard.pid) 2>/dev/null; }; then setsid nohup ./guard.sh > /dev/null 2>&1 < /dev/null & echo \$! > guard.pid; fi" || return 1
  return 0
}
job_state() { T=60 rsh "if pgrep -x $(basename "$BIN") > /dev/null; then echo yes; else echo no; fi" 2>/dev/null; }
harvest() { RUSER=fk KEY="$KEY" ONCE=1 tools/harvest.sh "$IP" 22 "$RDIR/res.txt" "$RDIR/run.log" "$LRES"; }
done_local() { grep -q '^# check' "$LRES" 2>/dev/null; }
trap 'log "interrupted"; delete_vm; exit 130' INT TERM
restarts=0
last_tick=$(date +%s)
running=0
touch "$LRES"
while true; do
  now=$(date +%s)
  [ "$running" = 1 ] && add_spent $(( now - last_tick ))
  last_tick=$now
  if done_local; then
    log "done $(grep '^# check' "$LRES" | tail -n 1) spent=$spent"
    delete_vm
    exit 0
  fi
  if over_budget; then
    log "budget reached spent=$spent"
    delete_vm
    exit 3
  fi
  VM=$(find_vm)
  if [ -z "$VM" ]; then
    create_vm
    t=0
    while [ -z "$VM" ] && [ $t -lt 120 ]; do sleep 5; t=$((t + 5)); VM=$(find_vm); done
    [ -z "$VM" ] && { log "create failed"; sleep "$EVERY"; continue; }
    log "vm $VM"
    begin=$(date +%s)
  fi
  read -r ST IP < <(vm_state "$VM")
  case "$ST" in
    RUNNING)
      running=1
      if [ "$IP" = "-" ]; then sleep 10; continue; fi
      ;;
    STOPPED)
      running=0
      if [ $(( $(date +%s) - ${begin:-0} )) -lt 180 ]; then
        sleep 15
        continue
      fi
      restarts=$((restarts + 1))
      if [ "$restarts" -gt "$MAX_RESTARTS" ]; then
        log "too many restarts"
        delete_vm
        exit 4
      fi
      log "stopped, starting again ($restarts)"
      timeout 900 nebius compute instance start --id "$VM" > /dev/null 2>&1 &
      begin=$(date +%s)
      sleep "$EVERY"
      continue
      ;;
    *)
      running=1
      if [ $(( $(date +%s) - ${begin:-$now} )) -gt "$START_WAIT" ]; then
        log "state $ST for over ${START_WAIT}s, deleting to retry later"
        delete_vm
        running=0
        sleep 300
      else
        sleep 15
      fi
      continue
      ;;
  esac
  if ! rsh true 2>/dev/null; then
    log "ssh not ready ip=$IP"
    sleep 15
    continue
  fi
  prepare_vm || { log "prepare failed"; sleep 15; continue; }
  out=$(harvest)
  rc=$?
  [ -n "$out" ] && echo "$out" | while read -r l; do log "$l"; done
  if [ "$rc" = 4 ]; then
    log "job error, restarting job"
    T=60 rsh "pkill -x $(basename "$BIN"); sleep 2; mv $RDIR/run.log $RDIR/run.log.err.\$(date +%s)" 2>/dev/null
  fi
  done_local && continue
  js=$(job_state)
  if [ "$js" = "no" ]; then
    log "starting job: $*"
    RUSER=fk RDIR=$RDIR KEY="$KEY" BIN="$BIN" tools/remote_run.sh "$IP" 22 "$LRES" "$@" | while read -r l; do log "$l"; done
  fi
  log "tick state=$ST ip=$IP residues=$(grep -c '^residue ' "$LRES") spent=$spent"
  sleep "$EVERY"
done
